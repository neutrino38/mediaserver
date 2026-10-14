#ifndef SPEECH_USAGE_H
#define SPEECH_USAGE_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

/**
 * What Mistral bills, since start-up, in its own units (SPEC §6.4): the
 * operator derives the cost without keeping a second ledger. Thread-safe.
 */
class UsageCounters
{
public:
	struct Usage
	{
		uint64_t	asrAudioMs = 0;
		uint64_t	ttsChars = 0;
		uint64_t	compTokens = 0;
	};

	// An account appears in the breakdown from its first token on.
	void Register(const std::string& account);

	void AddAsrAudio(const std::string& account, uint64_t ms);
	void AddTtsChars(const std::string& account, uint64_t chars);

	Usage Total() const;
	std::map<std::string,Usage> ByAccount() const;

private:
	mutable std::mutex		mutex;
	std::map<std::string,Usage>	accounts;
};

#endif
