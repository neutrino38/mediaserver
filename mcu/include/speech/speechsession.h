#ifndef SPEECH_SPEECHSESSION_H
#define SPEECH_SPEECHSESSION_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "config.h"
#include "ipaddress.h"
#include "websockets.h"
#include "speech/speechconfig.h"
#include "speech/speechfunction.h"

/**
 * One streaming connection, one function (SPEC §7). The base carries what the
 * three functions share: `ready`, `error`, the idle and duration bounds, and
 * the close codes.
 *
 * The WebSocket::Listener callbacks run on the reactor thread and must never
 * block. Close() and the Send helpers are thread-safe.
 */
class SpeechSession :
	public WebSocket::Listener,
	public std::enable_shared_from_this<SpeechSession>
{
public:
	struct Params
	{
		SpeechFunction	function;
		std::string	account;
		std::string	tokenPrefix;	// never the whole token in a log
		IPEndpoint	remote;
		std::shared_ptr<const SpeechConfig> config;
		std::string	lang;
		std::string	voice;		// tts: resolved alias
		int		delayMs;	// asr
	};

	// RFC 6455 close codes used by the server.
	static constexpr WORD CloseNormal	= 1000;
	static constexpr WORD CloseGoingAway	= 1001;
	static constexpr WORD CloseTooBig	= 1009;
	static constexpr WORD CloseInternal	= 1011;
	static constexpr WORD CloseTryLater	= 1013;

	static constexpr size_t MaxMessageSize = 64*1024;

	explicit SpeechSession(const Params& params);
	virtual ~SpeechSession() = default;

	// Called once, before Accept: immutable afterwards.
	void Attach(std::weak_ptr<WebSocket> ws, std::function<void(SpeechSession*)> onClosed);

	SpeechFunction GetFunction() const	{ return params.function; }

	// Watchdog thread: closes the session once idle or too old, else ticks it.
	void CheckLimits(uint64_t nowMs);

	// Sends `reason` as a fatal error first, when set, then the Close frame.
	void Close(WORD code, const char* reason);

	static uint64_t NowMs();

	//WebSocket::Listener
	virtual void onOpen(WebSocket* ws) override;
	virtual void onMessageStart(WebSocket* ws, const WebSocket::MessageType type, const DWORD length) override;
	virtual void onMessageData(WebSocket* ws, const BYTE* data, const DWORD size) override;
	virtual void onMessageEnd(WebSocket* ws) override;
	virtual void onError(WebSocket* ws) override;
	virtual void onClose(WebSocket* ws) override;

protected:
	// Reactor thread, once the 101 is sent. Sends `ready` by default; a
	// session that needs its backend first sends it itself, later.
	virtual void OnOpen()				{ SendReady(); }
	virtual void FillReady(nlohmann::json& ready)	{}
	// Watchdog thread, about every 250 ms, until the session closes.
	virtual void OnTick(uint64_t nowMs)		{}
	// A text frame, already parsed, with a string `type`.
	virtual void OnText(const nlohmann::json& message);
	virtual void OnBinary(const BYTE* data, DWORD size);
	virtual void OnClosed()				{}

	void SendReady();
	void Send(const nlohmann::json& message);
	void SendAudio(const std::string& pcm);
	void SendError(const std::string& reason, bool fatal);

	const Params params;

private:
	std::weak_ptr<WebSocket>		ws;
	std::function<void(SpeechSession*)>	onClosed;

	const uint64_t		openedAtMs;
	std::atomic<uint64_t>	lastActivityMs;
	std::atomic<bool>	closing{false};

	// Reactor thread only.
	WebSocket::MessageType	messageType = WebSocket::Text;
	std::string		message;
	bool			messageTooBig = false;
};

#endif
