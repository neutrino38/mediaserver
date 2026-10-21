#ifndef SPEECH_SPEECHTOKENCLIENT_H
#define SPEECH_SPEECHTOKENCLIENT_H

#include <functional>
#include <string>
#include "speech/speechfunction.h"
#include "speech/taskpool.h"

/**
 * The mediaserver side of POST /control/token (speech-server SPEC §6.2): one
 * token per stream opening, since a token is used once.
 *
 * The request is a blocking libcurl call: it runs in the TaskPool, never on
 * the WebSocket reactor nor on a media clock (SpeechProcessor SPEC §5.2). The
 * answer comes back on that pool thread.
 */
class SpeechTokenClient
{
public:
	struct Result
	{
		std::string	token;	// empty on failure
		std::string	error;	// why there is no token
	};

	SpeechTokenClient(TaskPool& tasks, const std::string& controlUrl, const std::string& account);

	void Request(SpeechFunction function, std::function<void(const Result&)> done);

	// The blocking call itself.
	static Result Fetch(const std::string& controlUrl, const std::string& account, SpeechFunction function);

private:
	TaskPool&		tasks;
	const std::string	controlUrl;
	const std::string	account;
};

#endif
