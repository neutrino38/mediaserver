#ifndef SPEECH_MISTRALREALTIME_H
#define SPEECH_MISTRALREALTIME_H

#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "config.h"
#include "websocketserver.h"

/**
 * One Voxtral Realtime session (SPEC §4.2): an outgoing WebSocket, audio in
 * binary frames, JSON events back.
 *
 * Events reach the listener on the reactor thread. The send methods are
 * thread-safe. The listener is held weakly: an event that arrives after its
 * session died finds nobody.
 */
class MistralRealtimeClient :
	public WebSocket::Listener,
	public std::enable_shared_from_this<MistralRealtimeClient>
{
public:
	// Every event names its client: a listener that reopened a session can
	// tell the late events of the previous one.
	class Listener
	{
	public:
		virtual ~Listener() = default;
		// session.updated: the delay is set, audio may flow.
		virtual void OnRealtimeReady(MistralRealtimeClient* client) = 0;
		// transcription.text.delta: appended text, never corrected.
		virtual void OnRealtimeText(MistralRealtimeClient* client, const std::string& text) = 0;
		// transcription.done: the whole text of the segment, deltas included.
		virtual void OnRealtimeDone(MistralRealtimeClient* client, const std::string& text) = 0;
		virtual void OnRealtimeError(MistralRealtimeClient* client, int code, const std::string& message) = 0;
		// Once, whatever the cause, including a failed opening.
		virtual void OnRealtimeClosed(MistralRealtimeClient* client) = 0;
	};

	static constexpr size_t MaxMessageSize = 1024*1024;

	MistralRealtimeClient(std::weak_ptr<Listener> listener, int delayMs);

	// wss://…/v1/audio/transcriptions/realtime?model=… from mistral.url.
	static std::string Url(const std::string& mistralUrl, const std::string& model);

	// Never on the reactor thread: it resolves the host and connects. False on
	// a synchronous failure, and then the listener is not told.
	bool Open(WebSocketServer& server, const std::string& url, const std::string& apiKey);

	void SendAudio(const BYTE* data, DWORD size);
	void Flush();
	void End();
	void Close();

	//WebSocket::Listener
	virtual void onOpen(WebSocket* ws) override;
	virtual void onMessageStart(WebSocket* ws, const WebSocket::MessageType type, const DWORD length) override;
	virtual void onMessageData(WebSocket* ws, const BYTE* data, const DWORD size) override;
	virtual void onMessageEnd(WebSocket* ws) override;
	virtual void onError(WebSocket* ws) override;
	virtual void onClose(WebSocket* ws) override;

private:
	void Send(const nlohmann::json& message);
	void Dispatch(const nlohmann::json& event);

	std::weak_ptr<Listener>	listener;
	const int		delayMs;

	std::mutex		mutex;
	std::weak_ptr<WebSocket> ws;
	bool			closeRequested = false;

	// Reactor thread only.
	WebSocket::MessageType	messageType = WebSocket::Text;
	std::string		message;
	bool			messageTooBig = false;
};

#endif
