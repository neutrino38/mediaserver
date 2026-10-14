#ifndef SPEECH_SPEECHSTATUS_H
#define SPEECH_SPEECHSTATUS_H

#include <ctime>
#include <map>
#include <string>
#include "speech/streaminghandler.h"
#include "speech/tokenstore.h"
#include "speech/usage.h"

/**
 * /status/general of speech-server (SPEC §6.4): one collection, two renderings,
 * like the mediaserver's StatusHandler.
 */
class SpeechStatus
{
public:
	struct Info
	{
		std::string	version;
		int		pid;
		time_t		startedAt;
		long		uptimeSecs;
		bool		apiKeyPresent;
		StreamingHandler::Counters	sessions;
		TokenStore::Counters		tokens;
		UsageCounters::Usage		usage;
		std::map<std::string,UsageCounters::Usage> usageByAccount;
	};

	SpeechStatus(time_t startedAt, bool apiKeyPresent, StreamingHandler& streaming, TokenStore& tokens, UsageCounters& usage);

	// `now` is injected so that the uptime stays testable.
	void Collect(Info& info, time_t now);

	static std::string RenderJSON(const Info& info);
	static std::string RenderText(const Info& info);

private:
	time_t			startedAt;
	bool			apiKeyPresent;
	StreamingHandler&	streaming;
	TokenStore&		tokens;
	UsageCounters&		usage;
};

std::string SpeechIsoUtc(time_t when);

#endif
