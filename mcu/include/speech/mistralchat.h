#ifndef SPEECH_MISTRALCHAT_H
#define SPEECH_MISTRALCHAT_H

#include <cstdint>
#include <string>

/**
 * One chat completion asking for a JSON object (SPEC §4.2, COMP): POST
 * /v1/chat/completions, not streamed. Blocking: it runs in the TaskPool.
 */
class MistralChat
{
public:
	struct Request
	{
		std::string	baseUrl;	// mistral.url
		std::string	apiKey;
		std::string	model;
		std::string	system;
		std::string	user;
	};

	struct Result
	{
		long		httpStatus = 0;
		std::string	error;		// empty on success
		std::string	content;	// the model's message
		uint64_t	tokens = 0;	// billed: usage.total_tokens
	};

	static Result Complete(const Request& request);
};

#endif
