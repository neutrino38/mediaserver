#ifndef SPEECH_SPEECHBACKEND_H
#define SPEECH_SPEECHBACKEND_H

#include <string>
#include "websocketserver.h"
#include "speech/backendhealth.h"
#include "speech/taskpool.h"
#include "speech/usage.h"

/**
 * What a session needs to reach Mistral and to account for it. Owned by
 * main(), outlives every session.
 */
struct SpeechBackend
{
	WebSocketServer*	webSocket;	// the reactor, also for outgoing connections
	TaskPool*		tasks;		// what may wait: DNS, HTTPS
	std::string		apiKey;
	UsageCounters*		usage;
	BackendHealth*		health;
};

#endif
