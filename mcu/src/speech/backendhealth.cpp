#include "speech/backendhealth.h"

void BackendHealth::Success(SpeechFunction function, time_t now)
{
	std::lock_guard<std::mutex> lock(mutex);
	services[(int)function].lastSuccess = now;
}

void BackendHealth::Failure(SpeechFunction function, time_t now, const std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex);
	services[(int)function].lastFailure = now;
	services[(int)function].lastError = error;
}

BackendHealth::Service BackendHealth::Get(SpeechFunction function) const
{
	std::lock_guard<std::mutex> lock(mutex);
	return services[(int)function];
}
