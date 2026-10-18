#include "speech/tokenstore.h"

#include <openssl/rand.h>

namespace
{

std::string Base64Url(const unsigned char* data, size_t size)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	unsigned int buffer = 0;
	int bits = 0;
	for (size_t i = 0; i < size; ++i)
	{
		buffer = (buffer << 8) | data[i];
		bits += 8;
		while (bits >= 6)
		{
			bits -= 6;
			out.push_back(alphabet[(buffer >> bits) & 0x3F]);
		}
	}
	if (bits)
		out.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
	return out;
}

}

TokenStore::Token TokenStore::Create(SpeechFunction function, int ttlSecs, const std::string& account, time_t now)
{
	unsigned char random[32];
	if (RAND_bytes(random, sizeof(random)) != 1)
		return Token{"", function, account, 0};

	Token token{Base64Url(random, sizeof(random)), function, account, now + ttlSecs};

	std::lock_guard<std::mutex> lock(mutex);
	Purge(now);
	tokens[token.value] = token;
	++issued;
	return token;
}

TokenStore::Refusal TokenStore::Consume(const std::string& value, SpeechFunction function, time_t now, Token& token)
{
	std::lock_guard<std::mutex> lock(mutex);
	Purge(now);

	auto it = tokens.find(value);
	if (it == tokens.end())
		return Refusal::Unknown;
	if (it->second.function != function)
		return Refusal::WrongFunction;

	token = it->second;
	tokens.erase(it);
	++consumed;
	return Refusal::None;
}

TokenStore::Counters TokenStore::GetCounters(time_t now)
{
	std::lock_guard<std::mutex> lock(mutex);
	Purge(now);
	return Counters{tokens.size(), issued, consumed, expiredUnused};
}

void TokenStore::Purge(time_t now)
{
	for (auto it = tokens.begin(); it != tokens.end(); )
	{
		if (it->second.expiresAt <= now)
		{
			it = tokens.erase(it);
			++expiredUnused;
		}
		else
			++it;
	}
}
