#ifndef SPEECH_TOKENSTORE_H
#define SPEECH_TOKENSTORE_H

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include "speech/speechfunction.h"

/**
 * Streaming tokens (SPEC §6.2): opaque, single use, one function each, kept in
 * memory only. Thread-safe.
 */
class TokenStore
{
public:
	struct Token
	{
		std::string	value;		// 32 random bytes, base64url, 43 characters
		SpeechFunction	function;
		std::string	account;
		time_t		expiresAt;
	};

	// An expired token is purged first, hence Unknown.
	enum class Refusal { None, Unknown, WrongFunction };

	struct Counters
	{
		size_t		live;
		uint64_t	issued;
		uint64_t	consumed;
		uint64_t	expiredUnused;
	};

	static constexpr size_t ValueLength = 43;

	// `value` is empty when the random generator fails.
	Token Create(SpeechFunction function, int ttlSecs, const std::string& account, time_t now);

	// Removes the token on success: a second presentation is Unknown. A token
	// presented for another function stays usable for its own.
	Refusal Consume(const std::string& value, SpeechFunction function, time_t now, Token& token);

	Counters GetCounters(time_t now);

private:
	void Purge(time_t now);

	std::mutex			mutex;
	std::map<std::string,Token>	tokens;
	uint64_t			issued = 0;
	uint64_t			consumed = 0;
	uint64_t			expiredUnused = 0;
};

#endif
