#ifndef SPEECH_SPEECHSERVICE_H
#define SPEECH_SPEECHSERVICE_H

#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "worker.h"
#include "speech/speechflow.h"
#include "speech/speechfunction.h"
#include "speech/speechtokenclient.h"
#include "speech/taskpool.h"

class WebSocketServer;

/**
 * The speech-server client of the mediaserver (SpeechProcessor SPEC §5).
 * One per process, owned by main().
 *
 * It holds where speech-server lives (`--speech-url` for the control port,
 * `--speech-streaming-url` for the streaming port), the account the tokens
 * are asked for, the TaskPool that runs the token requests and the connects,
 * and the clock thread that ticks every flow. It makes the SpeechFlows, and
 * keeps the counters that /status/general publishes.
 *
 * Without URLs the service is not configured: it makes no flow, and the
 * relays of a SpeechProcessor work alone.
 */
class SpeechService : public Worker
{
public:
	static constexpr int TaskThreads = 2;
	static constexpr int DefaultTickMs = 100;
	static constexpr int DefaultProbeMs = 5000;
	static constexpr long ProbeTimeoutSecs = 3;

	struct Counters
	{
		int		open[SpeechFunctionCount] = {};	// streams ready now
		uint64_t	reopenings = 0;	// streams back after a failure
		uint64_t	rotations = 0;
		uint64_t	failures = 0;
		time_t		lastFailureAt = 0;
		std::string	lastFailureFunction;
		std::string	lastFailureReason;
	};

	// Whether speech-server answers, and what it says of Mistral. Asked every
	// probeMs: a controller reads it before it offers speech to a call.
	struct Availability
	{
		bool		available = false;
		std::string	reason;		// empty when available
		time_t		checkedAt = 0;	// 0: not asked yet
	};

	SpeechService(WebSocketServer& server, const std::string& controlUrl, const std::string& streamingUrl,
		      const std::string& account, int tickMs = DefaultTickMs, int probeMs = DefaultProbeMs);
	~SpeechService();

	// Both URLs or none; control in http(s)://, streaming in ws(s)://.
	static bool CheckConfig(const std::string& controlUrl, const std::string& streamingUrl,
				const std::string& account, std::string& error);

	bool IsConfigured() const		{ return tasks != nullptr; }
	const std::string& GetControlUrl() const	{ return controlUrl; }
	const std::string& GetStreamingUrl() const	{ return streamingUrl; }
	const std::string& GetAccount() const	{ return account; }

	void Start();
	// Stops every flow, then the clock and the pool. Idempotent.
	void End();

	// nullptr when the service is not configured. The flow is not started.
	std::shared_ptr<SpeechFlow> CreateFlow(SpeechFunction function, std::weak_ptr<SpeechFlow::Listener> listener,
					       const SpeechFlow::Params& params = SpeechFlow::Params());

	Counters GetCounters();
	Availability GetAvailability();

	// For the flows.
	WebSocketServer& GetServer()		{ return server; }
	SpeechTokenClient& GetTokens()		{ return *tokens; }
	void RecordFailure(SpeechFunction function, const std::string& reason);
	void RecordReopening();
	void RecordRotation();

protected:
	virtual int Run() override;

private:
	WebSocketServer&		server;
	const std::string		controlUrl;
	const std::string		streamingUrl;
	const std::string		account;
	const int			tickMs;
	const int			probeMs;

	std::unique_ptr<TaskPool>		tasks;
	std::unique_ptr<SpeechTokenClient>	tokens;

	std::mutex				mutex;
	std::vector<std::weak_ptr<SpeechFlow>>	flows;
	Counters				counters;
	Availability				availability;
	bool					probing = false;
	uint64_t				nextProbeMs = 0;

	Availability Probe() const;
	void Probed(const Availability& result);
};

#endif
