#ifndef SPEECH_BACKENDPROBE_H
#define SPEECH_BACKENDPROBE_H

#include <ctime>
#include <mutex>
#include <string>
#include "worker.h"

/**
 * Whether Mistral answers now, asked every `periodSecs` with a GET /v1/models.
 *
 * BackendHealth only sees the sessions: with no traffic it knows nothing, and
 * a controller that stops calling during an outage would never see it end.
 * The probe answers even then, for /status/general.
 */
class BackendProbe : public Worker
{
public:
	static constexpr long TimeoutSecs = 10;

	enum class State { Unknown, Up, Down };

	struct Result
	{
		State		state = State::Unknown;
		std::string	reason;		// empty when up
		time_t		checkedAt = 0;	// 0: never
	};

	BackendProbe(const std::string& url, const std::string& apiKey, int periodSecs);
	~BackendProbe();

	// No thread when periodSecs is 0: the state stays Unknown.
	void Start();
	void Stop();

	Result Get() const;

	// One probe, in the calling thread.
	Result ProbeOnce() const;

	static const char* StateName(State state);

protected:
	virtual int Run() override;

private:
	const std::string	url;
	const std::string	apiKey;
	const int		periodSecs;

	mutable std::mutex	mutex;
	Result			last;
};

#endif
