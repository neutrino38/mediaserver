#include "speech/usage.h"

void UsageCounters::Register(const std::string& account)
{
	std::lock_guard<std::mutex> lock(mutex);
	accounts[account];
}

void UsageCounters::AddAsrAudio(const std::string& account, uint64_t ms)
{
	std::lock_guard<std::mutex> lock(mutex);
	accounts[account].asrAudioMs += ms;
}

void UsageCounters::AddTtsChars(const std::string& account, uint64_t chars)
{
	std::lock_guard<std::mutex> lock(mutex);
	accounts[account].ttsChars += chars;
}

void UsageCounters::AddCompTokens(const std::string& account, uint64_t tokens)
{
	std::lock_guard<std::mutex> lock(mutex);
	accounts[account].compTokens += tokens;
}

UsageCounters::Usage UsageCounters::Total() const
{
	std::lock_guard<std::mutex> lock(mutex);
	Usage total;
	for (const auto& entry : accounts)
	{
		total.asrAudioMs += entry.second.asrAudioMs;
		total.ttsChars += entry.second.ttsChars;
		total.compTokens += entry.second.compTokens;
	}
	return total;
}

std::map<std::string,UsageCounters::Usage> UsageCounters::ByAccount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return accounts;
}
