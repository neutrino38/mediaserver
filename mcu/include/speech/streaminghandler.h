#ifndef SPEECH_STREAMINGHANDLER_H
#define SPEECH_STREAMINGHANDLER_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include "websocketserver.h"
#include "worker.h"
#include "speech/speechconfig.h"
#include "speech/speechsession.h"
#include "speech/tokenstore.h"

/**
 * GET /streaming/<function>/<token>?lang=&voice=&delay= (SPEC §7.1).
 *
 * Answers 400/403/404/429/503 before the 101, otherwise hands the connection
 * to a session built by the function's factory, and owns that session until
 * it closes. Its thread enforces the idle and duration bounds.
 *
 * onWebSocketConnection runs on the reactor thread: it only takes short locks.
 */
class StreamingHandler :
	public WebSocketServer::Handler,
	public Worker
{
public:
	typedef std::function<std::shared_ptr<SpeechSession>(const SpeechSession::Params&)> Factory;

	struct Counters
	{
		int		open[SpeechFunctionCount];
		int		max[SpeechFunctionCount];
		uint64_t	refused[SpeechFunctionCount];	// 429
	};

	StreamingHandler(SpeechConfigHolder& config, TokenStore& tokens);
	virtual ~StreamingHandler();

	// Before Start. A function without a factory answers 503.
	void SetFactory(SpeechFunction function, Factory factory);

	void Start();
	void Stop();

	// Closes every session with `code`, then waits up to `waitMs` for them to go.
	void CloseAll(WORD code, const char* reason, int waitMs);

	Counters GetCounters() const;

	virtual void onWebSocketConnection(const HTTPRequest& request, WebSocket* ws) override;

protected:
	virtual int Run() override;

private:
	void Remove(SpeechSession* session);
	std::vector<std::shared_ptr<SpeechSession>> Snapshot() const;

	SpeechConfigHolder&	config;
	TokenStore&		tokens;
	Factory			factories[SpeechFunctionCount];

	mutable std::mutex	mutex;
	std::map<SpeechSession*,std::shared_ptr<SpeechSession>> sessions;
	std::atomic<uint64_t>	refused[SpeechFunctionCount] = {};
};

#endif
