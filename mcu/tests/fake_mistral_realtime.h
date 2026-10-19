/**
 * fake_mistral_realtime.h — a local Voxtral Realtime for the asr tests. It
 * speaks the protocol recorded at lot 0 (tests/fixtures/speech) and replays
 * the recorded deltas, one per audio frame received.
 */
#ifndef FAKE_MISTRAL_REALTIME_H
#define FAKE_MISTRAL_REALTIME_H

#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "websocketserver.h"

class FakeMistralRealtime : public WebSocketServer::Handler
{
public:
	// One accepted Mistral session.
	class Session : public WebSocket::Listener
	{
	public:
		explicit Session(FakeMistralRealtime* fake) : fake(fake) {}

		void onOpen(WebSocket* ws) override
		{
			socket = ws->GetWeakPtr();
			if (mute)
				return;
			Send({{"type", "session.created"}, {"session", {{"target_streaming_delay_ms", nullptr}}}});
		}
		void onMessageStart(WebSocket* ws, const WebSocket::MessageType type, const DWORD length) override
		{
			binary = type == WebSocket::Binary;
			message.clear();
		}
		void onMessageData(WebSocket* ws, const BYTE* data, const DWORD size) override
		{
			message.append((const char*)data, size);
		}
		void onMessageEnd(WebSocket* ws) override
		{
			std::lock_guard<std::mutex> lock(fake->mutex);
			if (binary)
			{
				audio += message;
				++frames;
				if (next < fake->deltas.size())
				{
					sent += fake->deltas[next];
					SendLocked({{"type", "transcription.text.delta"}, {"text", fake->deltas[next++]}});
				}
				return;
			}
			const nlohmann::json event = nlohmann::json::parse(message);
			const std::string type = event["type"];
			if (type == "session.update")
			{
				delay = event["session"]["target_streaming_delay_ms"];
				SendLocked({{"type", "session.updated"}, {"session", event["session"]}});
			}
			else if (type == "input_audio.flush")
			{
				++flushes;
				SendLocked({{"type", "transcription.done"}, {"text", sent + fake->doneTail}});
				sent.clear();
			}
			else if (type == "input_audio.end")
			{
				++ends;
				SendLocked({{"type", "transcription.done"}, {"text", ""}});
				if (std::shared_ptr<WebSocket> ws = socket.lock())
					ws->Close(1000, "");
			}
		}
		void onError(WebSocket* ws) override {}
		void onClose(WebSocket* ws) override
		{
			std::lock_guard<std::mutex> lock(fake->mutex);
			closed = true;
		}

		void Send(const nlohmann::json& event)
		{
			if (std::shared_ptr<WebSocket> ws = socket.lock())
				ws->SendMessage(event.dump());
		}
		void SendLocked(const nlohmann::json& event)	{ Send(event); }

		FakeMistralRealtime*	fake;
		bool			mute = false;	// accepts, then never speaks
		std::weak_ptr<WebSocket> socket;
		bool			binary = false;
		std::string		message;
		// Guarded by fake->mutex.
		std::string		audio;
		int			frames = 0;
		size_t			next = 0;
		std::string		sent;
		int			delay = -1;
		int			flushes = 0;
		int			ends = 0;
		bool			closed = false;
	};

	FakeMistralRealtime(const std::string& fixture, const std::string& apiKey) : apiKey(apiKey)
	{
		std::ifstream file(fixture);
		std::string line;
		while (std::getline(file, line))
		{
			const nlohmann::json entry = nlohmann::json::parse(line);
			if (entry["kind"] == "rx" && entry["msg"]["type"] == "transcription.text.delta")
				deltas.push_back(entry["msg"]["text"]);
		}
	}

	void onWebSocketConnection(const HTTPRequest& request, WebSocket* ws) override
	{
		if (request.GetHeader("Authorization") != "Bearer " + apiKey)
			return ws->Reject(401, "Unauthorized");
		// A backend that never answers the upgrade.
		if (request.GetRequestURI().find("model=hang") != std::string::npos)
			return;
		auto session = std::make_shared<Session>(this);
		session->mute = request.GetRequestURI().find("model=mute") != std::string::npos;
		{
			std::lock_guard<std::mutex> lock(mutex);
			sessions.push_back(session);
		}
		ws->Accept(session);
	}

	// Snapshot of session `index`, under the lock.
	Session Get(size_t index)
	{
		std::lock_guard<std::mutex> lock(mutex);
		return index < sessions.size() ? *sessions[index] : Session(this);
	}

	size_t Count()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return sessions.size();
	}

	void DropAll(WORD code)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto& session : sessions)
			if (std::shared_ptr<WebSocket> ws = session->socket.lock())
				ws->Close(code, "dropped");
	}

	void SendDeltaToAll(const std::string& text)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto& session : sessions)
			session->Send({{"type", "transcription.text.delta"}, {"text", text}});
	}

	std::mutex				mutex;
	std::vector<std::string>		deltas;
	std::string				doneTail;	// appended to the done text
	const std::string			apiKey;
	std::vector<std::shared_ptr<Session>>	sessions;
};

#endif
