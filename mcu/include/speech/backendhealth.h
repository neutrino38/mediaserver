#ifndef SPEECH_BACKENDHEALTH_H
#define SPEECH_BACKENDHEALTH_H

#include <ctime>
#include <mutex>
#include <string>
#include "speech/speechfunction.h"

/**
 * Last success and last failure of each Mistral service, for /status/general
 * (SPEC §6.4). Thread-safe.
 */
class BackendHealth
{
public:
	struct Service
	{
		time_t		lastSuccess = 0;	// 0: never
		time_t		lastFailure = 0;
		std::string	lastError;
		uint64_t	discarded = 0;	// unreadable answers thrown away
	};

	void Success(SpeechFunction function, time_t now);
	void Failure(SpeechFunction function, time_t now, const std::string& error);
	void Discarded(SpeechFunction function);

	Service Get(SpeechFunction function) const;

private:
	mutable std::mutex	mutex;
	Service			services[SpeechFunctionCount];
};

#endif
