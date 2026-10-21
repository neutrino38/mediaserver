#include "speech/ttssession.h"

#include <ctime>
#include <nlohmann/json.hpp>
#include "log.h"
#include "speech/mistralspeech.h"
#include "speech/pcmconverter.h"

using json = nlohmann::json;

namespace
{

// Mistral bills characters, not bytes.
uint64_t CountChars(const std::string& utf8)
{
	uint64_t count = 0;
	for (unsigned char c : utf8)
		if ((c & 0xC0) != 0x80)
			++count;
	return count;
}

}

// TRACE_EXCHANGE=yes: the text of the conversation in the log, controls
// visible. Never in a binary in service.
#ifdef TRACE_EXCHANGE
namespace
{

std::string ExchangeEscape(const std::string& text)
{
	std::string out;
	char buf[8];
	for (unsigned char c : text)
		if (c < 0x20 || c == 0x7f)
		{
			snprintf(buf, sizeof(buf), "\\x%02x", c);
			out += buf;
		}
		else
			out += (char)c;
	return out;
}

}
#define TraceExchange(...) Log(__VA_ARGS__)
#else
#define TraceExchange(...) ((void)0)
#endif

TtsSession::TtsSession(const Params& params, const SpeechBackend& backend) :
	SpeechSession(params),
	backend(backend),
	voiceId(params.config->FindVoice(params.voice) ? params.config->FindVoice(params.voice)->voiceId : ""),
	splitter(params.config->ttsPhraseSilenceMs)
{
}

void TtsSession::FillReady(json& ready)
{
	ready["voice"] = params.voice;
	ready["lang"] = params.lang;
}

void TtsSession::OnText(const json& message)
{
	const std::string type = message["type"];

	std::lock_guard<std::mutex> lock(mutex);

	if (type == "text")
	{
		if (!message.contains("text") || !message["text"].is_string())
			return SendError("text must be a string", false);
		TraceExchange("-TtsSession: exchange text in [token:%s] \"%s\"\n", params.tokenPrefix.c_str(), ExchangeEscape(message["text"].get<std::string>()).c_str());
		Enqueue(splitter.Add(message["text"], NowMs()));
	}
	else if (type == "flush")
	{
		TraceExchange("-TtsSession: exchange flush [token:%s]\n", params.tokenPrefix.c_str());
		Enqueue({splitter.Flush()});
	}
	else if (type == "stop")
	{
		json ids = json::array();
		if (speakingId)
			ids.push_back(speakingId);
		for (const Phrase& phrase : queue)
			ids.push_back(phrase.id);

		if (cancel)
			*cancel = true;
		cancel.reset();
		speakingId = 0;
		queue.clear();
		splitter.Clear();

		TraceExchange("-TtsSession: exchange stop [token:%s,ids:%s]\n", params.tokenPrefix.c_str(), ids.dump().c_str());
		Send(json{{"type", "stopped"}, {"ids", ids}});
	}
	else
		SendError("unexpected message", false);
}

void TtsSession::OnTick(uint64_t nowMs)
{
	std::lock_guard<std::mutex> lock(mutex);
	Enqueue({splitter.Tick(nowMs)});
}

void TtsSession::OnClosed()
{
	std::lock_guard<std::mutex> lock(mutex);
	closed = true;
	if (cancel)
		*cancel = true;
	queue.clear();
}

void TtsSession::Enqueue(const std::vector<std::string>& phrases)
{
	for (const std::string& text : phrases)
		if (!text.empty())
		{
			TraceExchange("-TtsSession: exchange phrase queued [token:%s,phrase:%d,queue:%zu] \"%s\"\n", params.tokenPrefix.c_str(), nextId, queue.size() + 1, ExchangeEscape(text).c_str());
			queue.push_back({nextId++, text});
		}
	Pump();
}

void TtsSession::Pump()
{
	if (closed || speakingId || queue.empty())
		return;

	const Phrase phrase = queue.front();
	queue.pop_front();
	speakingId = phrase.id;
	cancel = std::make_shared<std::atomic<bool>>(false);

	// `say` leaves before the request is posted: it precedes every audio frame.
	Send(json{{"type", "say"}, {"id", phrase.id}, {"text", phrase.text}});

	std::weak_ptr<SpeechSession> self = shared_from_this();
	std::shared_ptr<std::atomic<bool>> token = cancel;
	backend.tasks->Post([self, phrase, token]() {
		std::shared_ptr<SpeechSession> session = self.lock();
		if (session && !*token)
			std::static_pointer_cast<TtsSession>(session)->Speak(phrase, token);
	});
}

void TtsSession::Speak(const Phrase& phrase, std::shared_ptr<std::atomic<bool>> token)
{
	backend.usage->AddTtsChars(params.account, CountChars(phrase.text));

	PcmConverter converter(MistralSpeech::Rate);
	const MistralSpeech::Request request = {
		params.config->mistralUrl, backend.apiKey, params.config->ttsModel, voiceId, phrase.text};

#ifdef TRACE_EXCHANGE
	size_t pcmBytes = 0;
	const uint64_t startedMs = NowMs();
#endif
	TraceExchange("-TtsSession: exchange phrase to Mistral [token:%s,phrase:%d] \"%s\"\n", params.tokenPrefix.c_str(), phrase.id, ExchangeEscape(phrase.text).c_str());
	const MistralSpeech::Result result = MistralSpeech::Synthesize(request, [&](const std::string& float32) {
		const std::string pcm = converter.Convert(float32);
#ifdef TRACE_EXCHANGE
		pcmBytes += pcm.size();
#endif
		std::lock_guard<std::mutex> lock(mutex);
		if (*token)
			return false;
		if (!pcm.empty())
			SendAudio(pcm);
		return true;
	});

	std::lock_guard<std::mutex> lock(mutex);

	// Stopped, or the session closed: whatever happened is no longer told.
	if (*token)
		return;

	if (result.error.empty())
	{
		const std::string tail = converter.Drain();
		if (!tail.empty())
			SendAudio(tail);
		TraceExchange("-TtsSession: exchange phrase synthesized [token:%s,phrase:%d,pcmBytes:%zu,tookMs:%llu]\n", params.tokenPrefix.c_str(), phrase.id,
		    pcmBytes + tail.size(), (unsigned long long)(NowMs() - startedMs));
		Send(json{{"type", "done"}, {"id", phrase.id}});
		backend.health->Success(SpeechFunction::Tts, time(NULL));
	}
	else if (result.httpStatus == 403)
	{
		// Mistral moderation refused this phrase only.
		Log("-TtsSession: phrase %d rejected by Mistral [token:%s]\n", phrase.id, params.tokenPrefix.c_str());
		Send(json{{"type", "error"}, {"id", phrase.id}, {"reason", "rejected"}, {"fatal", false}});
	}
	else
	{
		Error("-TtsSession: Mistral TTS failed: %s [token:%s]\n", result.error.c_str(), params.tokenPrefix.c_str());
		backend.health->Failure(SpeechFunction::Tts, time(NULL), result.error);
		closed = true;
		queue.clear();
		Close(CloseTryLater, "backend unavailable");
		return;
	}

	cancel.reset();
	speakingId = 0;
	Pump();
}
