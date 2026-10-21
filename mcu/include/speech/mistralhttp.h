#ifndef SPEECH_MISTRALHTTP_H
#define SPEECH_MISTRALHTTP_H

#include <functional>
#include <string>

/**
 * One blocking request to Mistral with libcurl: JSON in, the body handed out as
 * it arrives. It runs in a TaskPool thread, never on the reactor. TLS is
 * verified against the system store. An empty key sends no Authorization
 * header: the mediaserver asks speech-server for its tokens this way.
 */
class MistralHttp
{
public:
	struct Result
	{
		long		httpStatus = 0;	// 0: no answer
		bool		cancelled = false;
		std::string	error;		// empty on success
	};

	// `onBody` sees the body of a 2xx answer only (a token is a 201);
	// returning false cancels.
	// An error body is never read: it may quote the user's text.
	static Result Post(const std::string& baseUrl, const std::string& path, const std::string& apiKey,
			   const std::string& json, const std::string& accept,
			   std::function<bool(const char* data, size_t size)> onBody);

	// A GET with no body, done within `timeoutSecs` (connect included).
	static Result Get(const std::string& baseUrl, const std::string& path, const std::string& apiKey,
			  long timeoutSecs, std::function<bool(const char* data, size_t size)> onBody);
};

#endif
