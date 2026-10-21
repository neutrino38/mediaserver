#include "speech/backendprobe.h"

#include "log.h"
#include "speech/mistralhttp.h"

BackendProbe::BackendProbe(const std::string& url, const std::string& apiKey, int periodSecs) :
	url(url),
	apiKey(apiKey),
	periodSecs(periodSecs)
{
}

BackendProbe::~BackendProbe()
{
	Stop();
}

void BackendProbe::Start()
{
	if (periodSecs > 0)
		StartThread();
}

void BackendProbe::Stop()
{
	StopThread();
}

BackendProbe::Result BackendProbe::Get() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return last;
}

BackendProbe::Result BackendProbe::ProbeOnce() const
{
	// The list of models is not read: its size is Mistral's business.
	const MistralHttp::Result http = MistralHttp::Get(url, "/v1/models", apiKey, TimeoutSecs,
		[](const char*, size_t) { return true; });

	Result result;
	result.checkedAt = time(NULL);
	if (http.error.empty())
	{
		result.state = State::Up;
		return result;
	}
	result.state = State::Down;
	if (http.httpStatus == 401 || http.httpStatus == 403)
		result.reason = "Mistral refuses the API key (" + http.error + ")";
	else if (http.httpStatus == 429)
		result.reason = "Mistral rate limit reached (" + http.error + ")";
	else if (http.httpStatus)
		result.reason = "Mistral answers " + http.error;
	else
		result.reason = "Mistral unreachable: " + http.error;
	return result;
}

const char* BackendProbe::StateName(State state)
{
	switch (state)
	{
		case State::Up:		return "up";
		case State::Down:	return "down";
		default:		return "unknown";
	}
}

int BackendProbe::Run()
{
	while (IsThreadRunning())
	{
		const Result result = ProbeOnce();
		State before;
		{
			std::lock_guard<std::mutex> lock(mutex);
			before = last.state;
			last = result;
		}
		if (result.state == State::Down && before != State::Down)
			Error("-BackendProbe: Mistral unavailable, speech processing will fail [url:%s,reason:%s]\n",
			      url.c_str(), result.reason.c_str());
		else if (result.state == State::Up && before != State::Up)
			Log("-BackendProbe: Mistral answers [url:%s]\n", url.c_str());

		wait.WaitSignal(periodSecs * 1000);
	}
	return 0;
}
