#include "speech/asrsession.h"

#include <algorithm>
#include <ctime>
#include <nlohmann/json.hpp>
#include "log.h"

using json = nlohmann::json;

namespace
{

// PCM S16LE mono 16 kHz: 32 bytes per millisecond.
constexpr size_t BytesPerMs = 32;

}

AsrSession::AsrSession(const Params& params, const SpeechBackend& backend) :
	SpeechSession(params),
	backend(backend)
{
}

void AsrSession::OnOpen()
{
	std::lock_guard<std::mutex> lock(mutex);
	OpenMistral();
}

void AsrSession::FillReady(json& ready)
{
	ready["langIgnored"] = true;
	ready["delay"] = params.delayMs;
}

void AsrSession::OpenMistral()
{
	mistral = std::make_shared<MistralRealtimeClient>(
		std::static_pointer_cast<AsrSession>(shared_from_this()), params.delayMs);
	mistralReady = false;
	audioSinceFlush = false;
	openingAtMs = NowMs();

	std::shared_ptr<MistralRealtimeClient> client = mistral;
	std::weak_ptr<SpeechSession> self = shared_from_this();
	const std::string url = MistralRealtimeClient::Url(params.config->mistralUrl, params.config->asrModel);
	const SpeechBackend& backend = this->backend;
	Log("-AsrSession: opening Mistral session [url:%s,token:%s]\n", url.c_str(), params.tokenPrefix.c_str());

	// Connect resolves the host: never on the reactor.
	backend.tasks->Post([client, self, url, &backend]() {
		if (self.expired() || client->Open(*backend.webSocket, url, backend.apiKey))
			return;
		backend.health->Failure(SpeechFunction::Asr, time(NULL), "cannot connect to " + url);
		if (std::shared_ptr<SpeechSession> session = self.lock())
			session->Close(CloseTryLater, "backend unavailable");
	});
}

void AsrSession::Relay(const BYTE* data, size_t size)
{
	// Held audio leaves in client-sized frames, not as one long message.
	const size_t frame = 100 * BytesPerMs;
	for (size_t pos = 0; pos < size; pos += frame)
		mistral->SendAudio(data + pos, std::min(frame, size - pos));
	if (size)
		audioSinceFlush = true;
	backend.usage->AddAsrAudio(params.account, size / BytesPerMs);
}

void AsrSession::OnBinary(const BYTE* data, DWORD size)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (paused || finishing)
		return;

	if (mistral && mistralReady)
	{
		Relay(data, size);
		return;
	}

	if (pending.size() + size > PendingAudioMs * BytesPerMs)
	{
		if (!droppedTold)
			SendError("audio dropped while the backend opens", false);
		droppedTold = true;
		return;
	}
	pending.append((const char*)data, size);
}

void AsrSession::OnText(const json& message)
{
	const std::string type = message["type"];

	std::lock_guard<std::mutex> lock(mutex);

	if (type == "pause")
	{
		if (paused || finishing)
			return;
		paused = true;
		pausedAtMs = NowMs();
		// The text still held by Mistral comes now, not after the pause.
		if (mistral && mistralReady && audioSinceFlush)
		{
			mistral->Flush();
			audioSinceFlush = false;
		}
	}
	else if (type == "resume")
	{
		if (!paused)
			return;
		paused = false;
		if (!mistral && !finishing)
			OpenMistral();
	}
	else if (type == "done")
	{
		if (finishing)
			return;
		finishing = true;
		finishingAtMs = NowMs();
		pending.clear();
		if (mistral && mistralReady)
		{
			if (audioSinceFlush)
				mistral->Flush();
			audioSinceFlush = false;
			mistral->End();
		}
		else if (!mistral)
			Close(CloseNormal, nullptr);
		else
			mistral->Close();
	}
	else
		SendError("unexpected message", false);
}

void AsrSession::OnRealtimeReady(MistralRealtimeClient* client)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (client != mistral.get())
		return;

	mistralReady = true;
	backend.health->Success(SpeechFunction::Asr, time(NULL));

	if (!pending.empty())
		Relay((const BYTE*)pending.data(), pending.size());
	pending.clear();
	droppedTold = false;

	if (!readySent)
	{
		readySent = true;
		SendReady();
	}
}

void AsrSession::OnRealtimeText(MistralRealtimeClient* client, const std::string& text)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (client != mistral.get())
		return;
	segment += text;
	Send(json{{"type", "text"}, {"text", text}});
}

void AsrSession::OnRealtimeDone(MistralRealtimeClient* client, const std::string& text)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (client != mistral.get())
		return;

	// The deltas already carried the start of `text`: only the rest goes out.
	if (text.size() > segment.size() && text.compare(0, segment.size(), segment) == 0)
		Send(json{{"type", "text"}, {"text", text.substr(segment.size())}});
	else if (!text.empty() && text != segment)
		Log("-AsrSession: final text differs from the deltas, kept the deltas [token:%s]\n", params.tokenPrefix.c_str());
	segment.clear();
}

void AsrSession::OnRealtimeError(MistralRealtimeClient* client, int code, const std::string& message)
{
	Error("-AsrSession: Mistral error %d: %s [token:%s]\n", code, message.c_str(), params.tokenPrefix.c_str());

	std::lock_guard<std::mutex> lock(mutex);
	// A silent paused session times out on Mistral's side: that is expected.
	if (client == mistral.get() && !paused)
		backend.health->Failure(SpeechFunction::Asr, time(NULL), std::to_string(code) + " " + message);
}

void AsrSession::OnRealtimeClosed(MistralRealtimeClient* client)
{
	std::lock_guard<std::mutex> lock(mutex);

	// A client closed for a pause, or replaced since: nothing to do.
	if (client != mistral.get())
		return;

	mistral.reset();
	mistralReady = false;

	if (finishing)
		Close(CloseNormal, nullptr);
	else if (!paused)
	{
		backend.health->Failure(SpeechFunction::Asr, time(NULL), "connection closed");
		Close(CloseTryLater, "backend closed");
	}
}

void AsrSession::OnTick(uint64_t nowMs)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (paused && mistral && nowMs - pausedAtMs >= (uint64_t)params.config->asrPauseCloseSecs*1000)
	{
		// Mistral would close a silent session by itself, after about 60 s.
		mistral->Close();
		mistral.reset();
		mistralReady = false;
	}

	if (mistral && !mistralReady && nowMs - openingAtMs >= OpenTimeoutMs)
	{
		backend.health->Failure(SpeechFunction::Asr, time(NULL), "no session.updated in time");
		Close(CloseTryLater, "backend timeout");
	}

	if (finishing && nowMs - finishingAtMs >= FinishTimeoutMs)
		Close(CloseNormal, nullptr);
}

void AsrSession::OnClosed()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (mistral)
		mistral->Close();
	mistral.reset();
}
