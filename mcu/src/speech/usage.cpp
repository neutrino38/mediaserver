#include "speech/usage.h"

void UsageCounters::Register(const std::string& account)
{
	std::lock_guard<std::mutex> lock(mutex);
	accounts[account];
}

UsageCounters::Usage UsageCounters::Total() const
{
	std::lock_guard<std::mutex> lock(mutex);
	Usage total;
	for (const auto& entry : accounts)
	{
		total.asrAudioSecs += entry.second.asrAudioSecs;
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
