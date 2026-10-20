#ifndef SPEECH_SPEECHFLOW_H
#define SPEECH_SPEECHFLOW_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "config.h"
#include "speech/speechfunction.h"
#include "speech/speechtokenclient.h"

class SpeechService;

/**
 * One asr or tts stream to speech-server, kept open for a whole call
 * (SpeechProcessor SPEC §5.2, §5.3).
 *
 *   Idle → Token → Connecting → Ready ⇄ Down
 *
 * Every opening asks a new token (a token is used once), then opens
 * /streaming/<function>/<token> with WebSocketServer::Connect. The token and
 * the connect run in the service TaskPool; the clock (keep-alive, rotation,
 * retries, timeouts) is the service's Tick thread. No caller ever waits.
 *
 * - Keep-alive: a tts stream gets a `flush`, a paused asr stream a `pause`,
 *   after `keepAliveMs` without any frame from us. speech-server closes a
 *   stream that stays `idleSecs` silent.
 * - Rotation: after `maxAgeMs`, at the first moment the owner says
 *   CanRotate(), a second stream opens; once it is ready it takes over, and
 *   the old one ends (asr: `done`, which flushes its last text; tts: close).
 *   The owner sees onReady again, not onState.
 * - Failure: a stream that closes unasked, or an opening that fails, makes
 *   the flow Down; it retries after 1, 2, 5, 10, then 30 s forever.
 *   onState is told once per change, never once per attempt.
 *
 * Listener calls come from the reactor, the TaskPool or the Tick thread, never
 * under a flow lock. They must not block: they hand over to a queue.
 */
class SpeechFlow : public std::enable_shared_from_this<SpeechFlow>
{
public:
	enum class State { Idle, Token, Connecting, Ready, Down };

	struct Params
	{
		// Query of the opening: lang, voice (tts), delay (asr).
		std::map<std::string, std::string> query;
		int			keepAliveMs = 30000;
		int			maxAgeMs = 55*60*1000;
		// Token, connect and `ready` together.
		int			readyTimeoutMs = 15000;
		// An ended stream gets this long to say its last words.
		int			retiringTimeoutMs = 10000;
		std::vector<int>	retryMs = {1000, 2000, 5000, 10000, 30000};
	};

	class Listener
	{
	public:
		virtual ~Listener() = default;
		// A new stream serves the flow: first opening, reopening, rotation.
		// tts phrase ids restart with each stream.
		virtual void onReady(SpeechFlow* flow) {}
		// asr: appended text, never corrected.
		virtual void onText(SpeechFlow* flow, const std::string& text) {}
		// tts: S16LE mono 16 kHz, faster than real time.
		virtual void onPcm(SpeechFlow* flow, const BYTE* data, DWORD size) {}
		virtual void onSay(SpeechFlow* flow, int id, const std::string& text) {}
		virtual void onDone(SpeechFlow* flow, int id) {}
		virtual void onStopped(SpeechFlow* flow, const std::vector<int>& ids) {}
		// tts: the phrase will not be spoken (moderation); no done follows.
		virtual void onRejected(SpeechFlow* flow, int id) {}
		// Once per change: the service went away, or came back.
		virtual void onState(SpeechFlow* flow, bool up, const std::string& reason) {}
		// Asked on the Tick thread once the stream is old: true at a silence.
		virtual bool CanRotate(SpeechFlow* flow) { return true; }
	};

	SpeechFlow(SpeechService& service, SpeechFunction function, std::weak_ptr<Listener> listener, const Params& params);
	~SpeechFlow();

	SpeechFunction GetFunction() const	{ return function; }
	State GetState();

	void Start();
	// Closes every stream; no listener call follows. Idempotent.
	void Stop();

	// False when no stream is ready: what is sent then is lost.
	bool SendAudio(const BYTE* data, DWORD size);
	bool SendText(const std::string& text);
	bool Flush();
	bool StopSpeaking();
	// asr. The paused state survives reopenings and rotations.
	bool Pause();
	bool Resume();

	// The service clock.
	void Tick(uint64_t nowMs);

	static uint64_t NowMs();
	static const char* StateName(State state);

private:
	class Link;
	friend class Link;

	// What a locked section leaves to tell the listener.
	struct Notice
	{
		bool		ready = false;
		int		state = -1;	// -1: no change, 0: down, 1: up
		std::string	reason;
	};

	void BeginOpen(uint64_t nowMs);
	void OnToken(uint64_t attempt, const SpeechTokenClient::Result& result);
	void OpeningFailed(const std::string& reason, uint64_t nowMs, Notice& notice);
	void Abandon(const std::shared_ptr<Link>& link, uint64_t nowMs);
	bool SendJson(const nlohmann::json& message);
	void Tell(const Notice& notice);

	void OnLinkMessage(Link* link, const nlohmann::json& message);
	void OnLinkBinary(Link* link, const BYTE* data, DWORD size);
	void OnLinkClosed(Link* link);

	SpeechService&			service;
	const SpeechFunction		function;
	const std::weak_ptr<Listener>	listener;
	const Params			params;

	std::mutex			mutex;
	State				state = State::Idle;
	bool				stopped = false;
	int				reported = -1;	// last onState: -1 none, 0 down, 1 up
	// Bumped by each opening: a late token answer of an abandoned one is
	// recognised and dropped.
	uint64_t			attempt = 0;
	bool				opening = false;
	uint64_t			openingDeadlineMs = 0;
	std::shared_ptr<Link>		openingLink;
	std::shared_ptr<Link>		current;
	uint64_t			currentSinceMs = 0;
	uint64_t			lastSentMs = 0;
	std::shared_ptr<Link>		retiring;
	uint64_t			retiringDeadlineMs = 0;
	// Closed by us, kept alive until their close so the server hears it.
	std::vector<std::pair<std::shared_ptr<Link>, uint64_t>> abandoned;
	size_t				retryIndex = 0;
	uint64_t			nextAttemptMs = 0;
	uint64_t			nextRotationMs = 0;
	bool				paused = false;
};

#endif
