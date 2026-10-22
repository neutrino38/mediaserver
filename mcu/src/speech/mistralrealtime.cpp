#include "speech/mistralrealtime.h"

#include <nlohmann/json.hpp>
#include "log.h"

using json = nlohmann::json;

MistralRealtimeClient::MistralRealtimeClient(std::weak_ptr<Listener> listener, int delayMs, const std::string& lang) :
	listener(listener),
	delayMs(delayMs),
	lang(lang)
{
}

std::string MistralRealtimeClient::Url(const std::string& mistralUrl, const std::string& model)
{
	std::string base = mistralUrl;
	if (base.rfind("https://", 0) == 0)
		base = "wss://" + base.substr(8);
	else if (base.rfind("http://", 0) == 0)
		base = "ws://" + base.substr(7);
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	return base + "/v1/audio/transcriptions/realtime?model=" + model;
}

bool MistralRealtimeClient::Open(WebSocketServer& server, const std::string& url, const std::string& apiKey)
{
	return server.Connect(url, shared_from_this(), {{"Authorization", "Bearer " + apiKey}});
}

void MistralRealtimeClient::SendAudio(const BYTE* data, DWORD size)
{
	std::shared_ptr<WebSocket> socket;
	{
		std::lock_guard<std::mutex> lock(mutex);
		socket = ws.lock();
	}
	if (socket)
		socket->SendMessage(data, size);
}

void MistralRealtimeClient::Flush()
{
	Send(json{{"type", "input_audio.flush"}});
}

void MistralRealtimeClient::End()
{
	Send(json{{"type", "input_audio.end"}});
}

void MistralRealtimeClient::Close()
{
	std::shared_ptr<WebSocket> socket;
	{
		std::lock_guard<std::mutex> lock(mutex);
		closeRequested = true;
		socket = ws.lock();
	}
	if (socket)
		socket->Close(1000, "");
}

void MistralRealtimeClient::Send(const json& message)
{
	std::shared_ptr<WebSocket> socket;
	{
		std::lock_guard<std::mutex> lock(mutex);
		socket = ws.lock();
	}
	if (socket)
		socket->SendMessage(message.dump());
}

void MistralRealtimeClient::onOpen(WebSocket* socket)
{
	bool closeNow;
	{
		std::lock_guard<std::mutex> lock(mutex);
		ws = socket->GetWeakPtr();
		closeNow = closeRequested;
	}
	if (closeNow)
		socket->Close(1000, "");
}

void MistralRealtimeClient::onMessageStart(WebSocket* ws, const WebSocket::MessageType type, const DWORD length)
{
	messageType = type;
	message.clear();
	messageTooBig = false;
}

void MistralRealtimeClient::onMessageData(WebSocket* ws, const BYTE* data, const DWORD size)
{
	if (messageTooBig || message.size() + size > MaxMessageSize)
	{
		messageTooBig = true;
		message.clear();
		return;
	}
	message.append((const char*)data, size);
}

void MistralRealtimeClient::onMessageEnd(WebSocket* ws)
{
	if (messageTooBig || messageType != WebSocket::Text)
	{
		Error("-MistralRealtimeClient: unexpected %s message ignored\n", messageTooBig ? "oversized" : "binary");
		return;
	}

	const json event = json::parse(message, nullptr, false);
	if (event.is_discarded() || !event.is_object() || !event.contains("type") || !event["type"].is_string())
	{
		Error("-MistralRealtimeClient: unreadable event ignored\n");
		return;
	}
	Dispatch(event);
}

void MistralRealtimeClient::Dispatch(const json& event)
{
	std::shared_ptr<Listener> target = listener.lock();
	if (!target)
		return;

	const std::string type = event["type"];

	if (type == "session.created")
	{
		Send(json{{"type", "session.update"}, {"session", {{"target_streaming_delay_ms", delayMs}, {"language", lang}}}});
	}
	else if (type == "session.updated")
		target->OnRealtimeReady(this);
	else if (type == "transcription.text.delta")
		target->OnRealtimeText(this, event.value("text", ""));
	else if (type == "transcription.done")
		target->OnRealtimeDone(this, event.value("text", ""));
	else if (type == "error")
	{
		const json error = event.value("error", json::object());
		const int code = error.contains("code") && error["code"].is_number_integer() ? error["code"].get<int>() : 0;
		const std::string text = error.contains("message") && error["message"].is_string() ? error["message"].get<std::string>() : "";
		target->OnRealtimeError(this, code, text);
	}
	else
		Debug("-MistralRealtimeClient: event %s ignored\n", type.c_str());
}

void MistralRealtimeClient::onError(WebSocket* ws)
{
	Error("-MistralRealtimeClient: connection error\n");
}

void MistralRealtimeClient::onClose(WebSocket* ws)
{
	if (std::shared_ptr<Listener> target = listener.lock())
		target->OnRealtimeClosed(this);
}
