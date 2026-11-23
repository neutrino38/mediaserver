#include "speech/streaminghandler.h"

#include <cstdlib>
#include <ctime>
#include <set>
#include <thread>
#include "http.h"
#include "log.h"

namespace
{

const std::string Prefix = "/streaming/";

bool IsTokenChars(const std::string& text)
{
	for (char c : text)
		if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
			return false;
	return !text.empty();
}

bool IsLang(const std::string& text)
{
	if (text.size() < 2 || text.size() > 3)
		return false;
	for (char c : text)
		if (c < 'a' || c > 'z')
			return false;
	return true;
}

// Fills `params` from the query, or returns the reason of a 400.
std::string ParseQuery(const std::string& query, const SpeechConfig& config, SpeechSession::Params& params)
{
	std::set<std::string> seen;
	std::string voice;
	params.lang = "fr";
	params.delayMs = 1000;

	size_t pos = 0;
	while (pos < query.size())
	{
		size_t end = query.find('&', pos);
		if (end == std::string::npos)
			end = query.size();
		const std::string pair = query.substr(pos, end - pos);
		pos = end + 1;
		if (pair.empty())
			continue;

		const size_t equal = pair.find('=');
		const std::string key = pair.substr(0, equal);
		const std::string value = equal == std::string::npos ? "" : pair.substr(equal + 1);

		if (!seen.insert(key).second)
			return "duplicate parameter " + key;
		if (!IsTokenChars(value))
			return "invalid value for " + key;

		if (key == "lang")
		{
			if (!IsLang(value))
				return "invalid lang";
			params.lang = value;
		}
		else if (key == "voice" && params.function == SpeechFunction::Tts)
			voice = value;
		else if (key == "delay" && params.function == SpeechFunction::Asr)
		{
			char* last = nullptr;
			const long delay = strtol(value.c_str(), &last, 10);
			if (*last != '\0' || ((delay < 80 || delay > 1200) && delay != 2400))
				return "delay must be from 80 to 1200 ms, or 2400";
			params.delayMs = (int)delay;
		}
		else
			return "unknown parameter " + key;
	}

	if (params.function == SpeechFunction::Tts)
	{
		const SpeechConfig::Voice* resolved = voice.empty() ? config.DefaultVoice(params.lang) : config.FindVoice(voice);
		if (!resolved)
			return voice.empty() ? "no voice for lang " + params.lang : "unknown voice " + voice;
		params.voice = resolved->alias;
	}
	else if (params.function == SpeechFunction::Asr && params.lang.size() != 2)
		return "lang must be a two-letter code for asr";
	return "";
}

}

StreamingHandler::StreamingHandler(SpeechConfigHolder& config, TokenStore& tokens) :
	config(config),
	tokens(tokens)
{
}

StreamingHandler::~StreamingHandler()
{
	StopThread();
}

void StreamingHandler::SetFactory(SpeechFunction function, Factory factory)
{
	factories[(int)function] = factory;
}

void StreamingHandler::Start()
{
	StartThread();
}

void StreamingHandler::Stop()
{
	StopThread();
}

void StreamingHandler::onWebSocketConnection(const HTTPRequest& request, WebSocket* ws)
{
	const std::string uri = request.GetRequestURI();
	const size_t question = uri.find('?');
	const std::string path = uri.substr(0, question);
	const std::string query = question == std::string::npos ? "" : uri.substr(question + 1);

	// Never log the whole path: it carries the token.
	const size_t slash = path.find('/', Prefix.size());
	SpeechSession::Params params;
	if (path.rfind(Prefix, 0) != 0 || slash == std::string::npos
	    || !ParseSpeechFunction(path.substr(Prefix.size(), slash - Prefix.size()), params.function))
		return ws->Reject(404, "Not Found");

	const std::string token = path.substr(slash + 1);
	params.tokenPrefix = token.substr(0, 6);
	params.remote = ws->GetRemoteEndpoint();
	params.config = config.Get();

	const int index = (int)params.function;
	const char* name = SpeechFunctionName(params.function);

	if (!factories[index] || (params.function == SpeechFunction::Tts && params.config->voices.empty()))
	{
		Log("-StreamingHandler: 503 [function:%s,reason:%s]\n", name, factories[index] ? "no voice configured" : "not available");
		return ws->Reject(503, "Service Unavailable");
	}

	const std::string invalid = ParseQuery(query, *params.config, params);
	if (!invalid.empty())
	{
		Log("-StreamingHandler: 400 [function:%s,token:%s,reason:%s]\n", name, params.tokenPrefix.c_str(), invalid.c_str());
		return ws->Reject(400, "Bad Request");
	}


	// The reactor thread is the only one to insert: counting then inserting
	// under the same lock cannot overshoot.
	std::lock_guard<std::mutex> lock(mutex);

	int open = 0;
	for (const auto& entry : sessions)
		if (entry.second->GetFunction() == params.function)
			++open;
	if (open >= params.config->maxSessions[index])
	{
		++refused[index];
		Log("-StreamingHandler: 429 [function:%s,open:%d]\n", name, open);
		return ws->Reject(429, "Too Many Requests");
	}

	TokenStore::Token consumed;
	if (token.size() != TokenStore::ValueLength
	    || tokens.Consume(token, params.function, time(NULL), consumed) != TokenStore::Refusal::None)
	{
		Log("-StreamingHandler: 403 [function:%s,token:%s,peer:%s]\n", name, params.tokenPrefix.c_str(), params.remote.ToString().c_str());
		return ws->Reject(403, "Forbidden");
	}
	params.account = consumed.account;

	std::shared_ptr<SpeechSession> session = factories[index](params);
	if (!session)
		return ws->Reject(500, "Internal Server Error");

	session->Attach(ws->GetWeakPtr(), [this](SpeechSession* closed) { Remove(closed); });
	sessions[session.get()] = session;
	ws->Accept(session);
}

void StreamingHandler::Remove(SpeechSession* session)
{
	std::lock_guard<std::mutex> lock(mutex);
	sessions.erase(session);
}

std::vector<std::shared_ptr<SpeechSession>> StreamingHandler::Snapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<std::shared_ptr<SpeechSession>> all;
	for (const auto& entry : sessions)
		all.push_back(entry.second);
	return all;
}

void StreamingHandler::CloseAll(WORD code, const char* reason, int waitMs)
{
	for (const auto& session : Snapshot())
		session->Close(code, reason);

	const uint64_t deadline = SpeechSession::NowMs() + waitMs;
	while (SpeechSession::NowMs() < deadline)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (sessions.empty())
				return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

StreamingHandler::Counters StreamingHandler::GetCounters() const
{
	Counters counters = {};
	const std::shared_ptr<const SpeechConfig> current = config.Get();
	for (int i = 0; i < SpeechFunctionCount; ++i)
	{
		counters.max[i] = current->maxSessions[i];
		counters.refused[i] = refused[i];
	}
	for (const auto& session : Snapshot())
		++counters.open[(int)session->GetFunction()];
	return counters;
}

int StreamingHandler::Run()
{
	while (IsThreadRunning())
	{
		const uint64_t now = SpeechSession::NowMs();
		for (const auto& session : Snapshot())
			session->CheckLimits(now);
		wait.WaitSignal(250);
	}
	return 0;
}
