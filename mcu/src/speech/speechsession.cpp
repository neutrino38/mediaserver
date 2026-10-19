#include "speech/speechsession.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include "log.h"

using json = nlohmann::json;

SpeechSession::SpeechSession(const Params& params) :
	params(params),
	openedAtMs(NowMs()),
	lastActivityMs(openedAtMs)
{
}

uint64_t SpeechSession::NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SpeechSession::Attach(std::weak_ptr<WebSocket> ws, std::function<void(SpeechSession*)> onClosed)
{
	this->ws = ws;
	this->onClosed = onClosed;
}

namespace
{

// `nowMs` is read before the session is looked at: activity may be later.
uint64_t Elapsed(uint64_t nowMs, uint64_t sinceMs)
{
	return nowMs > sinceMs ? nowMs - sinceMs : 0;
}

}

void SpeechSession::CheckLimits(uint64_t nowMs)
{
	if (Elapsed(nowMs, lastActivityMs.load()) >= (uint64_t)params.config->idleSecs*1000)
		Close(CloseNormal, "idle");
	else if (Elapsed(nowMs, openedAtMs) >= (uint64_t)params.config->maxSessionSecs*1000)
		Close(CloseNormal, "max duration");
	else if (!closing)
		OnTick(nowMs);
}

void SpeechSession::Close(WORD code, const char* reason)
{
	if (closing.exchange(true))
		return;

	if (reason)
		SendError(reason, true);

	if (std::shared_ptr<WebSocket> socket = ws.lock())
		socket->Close(code, reason ? reason : "");
}

void SpeechSession::Send(const json& message)
{
	if (std::shared_ptr<WebSocket> socket = ws.lock())
		socket->SendMessage(message.dump());
}

void SpeechSession::SendAudio(const std::string& pcm)
{
	if (std::shared_ptr<WebSocket> socket = ws.lock())
		socket->SendMessage((const BYTE*)pcm.data(), pcm.size());
}

void SpeechSession::SendError(const std::string& reason, bool fatal)
{
	Send(json{{"type", "error"}, {"reason", reason}, {"fatal", fatal}});
}

void SpeechSession::onOpen(WebSocket* ws)
{
	Log("-SpeechSession: open [function:%s,account:%s,token:%s,peer:%s]\n",
	    SpeechFunctionName(params.function), params.account.c_str(),
	    params.tokenPrefix.c_str(), params.remote.ToString().c_str());

	OnOpen();
}

void SpeechSession::SendReady()
{
	json ready = {{"type", "ready"}, {"function", SpeechFunctionName(params.function)}, {"rate", 16000}};
	FillReady(ready);
	Send(ready);
}

void SpeechSession::onMessageStart(WebSocket* ws, const WebSocket::MessageType type, const DWORD length)
{
	messageType = type;
	message.clear();
	messageTooBig = false;
}

void SpeechSession::onMessageData(WebSocket* ws, const BYTE* data, const DWORD size)
{
	if (messageTooBig)
		return;
	if (message.size() + size > MaxMessageSize)
	{
		messageTooBig = true;
		message.clear();
		return;
	}
	message.append((const char*)data, size);
}

void SpeechSession::onMessageEnd(WebSocket* ws)
{
	lastActivityMs = NowMs();

	if (messageTooBig)
	{
		Close(CloseTooBig, "message too big");
		return;
	}

	if (messageType == WebSocket::Binary)
	{
		OnBinary((const BYTE*)message.data(), message.size());
		return;
	}

	const json parsed = json::parse(message, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object())
		SendError("invalid JSON", false);
	else if (!parsed.contains("type") || !parsed["type"].is_string())
		SendError("missing type", false);
	else
		OnText(parsed);
}

void SpeechSession::OnText(const json& message)
{
	SendError("unexpected message", false);
}

void SpeechSession::OnBinary(const BYTE* data, DWORD size)
{
	SendError("unexpected binary frame", false);
}

void SpeechSession::onError(WebSocket* ws)
{
	Error("-SpeechSession: transport error [function:%s,token:%s]\n",
	      SpeechFunctionName(params.function), params.tokenPrefix.c_str());
}

void SpeechSession::onClose(WebSocket* ws)
{
	OnClosed();

	Log("-SpeechSession: closed [function:%s,account:%s,token:%s,peer:%s,duration:%llus]\n",
	    SpeechFunctionName(params.function), params.account.c_str(),
	    params.tokenPrefix.c_str(), params.remote.ToString().c_str(),
	    (unsigned long long)((NowMs() - openedAtMs)/1000));

	if (onClosed)
		onClosed(this);
}
