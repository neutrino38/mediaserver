#include "speech/speechstatus.h"

#include <unistd.h>
#include <nlohmann/json.hpp>
#include "version.h"

using json = nlohmann::json;

std::string SpeechIsoUtc(time_t when)
{
	char buf[32];
	struct tm tm;
	gmtime_r(&when, &tm);
	strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

SpeechStatus::SpeechStatus(time_t startedAt, bool apiKeyPresent, StreamingHandler& streaming, TokenStore& tokens, UsageCounters& usage) :
	startedAt(startedAt),
	apiKeyPresent(apiKeyPresent),
	streaming(streaming),
	tokens(tokens),
	usage(usage)
{
}

void SpeechStatus::Collect(Info& info, time_t now)
{
	info.version = MCUVERSION;
	info.pid = getpid();
	info.startedAt = startedAt;
	info.uptimeSecs = now - startedAt;
	info.apiKeyPresent = apiKeyPresent;
	info.sessions = streaming.GetCounters();
	info.tokens = tokens.GetCounters(now);
	info.usage = usage.Total();
	info.usageByAccount = usage.ByAccount();
}

namespace
{

json UsageJSON(const UsageCounters::Usage& usage)
{
	return json{{"asrAudioSecs", usage.asrAudioSecs}, {"ttsChars", usage.ttsChars}, {"compTokens", usage.compTokens}};
}

}

std::string SpeechStatus::RenderJSON(const Info& info)
{
	json sessions = json::object();
	for (int i = 0; i < SpeechFunctionCount; ++i)
		sessions[SpeechFunctionName((SpeechFunction)i)] = {
			{"open", info.sessions.open[i]},
			{"max", info.sessions.max[i]},
			{"refused", info.sessions.refused[i]}};

	json byAccount = json::object();
	for (const auto& entry : info.usageByAccount)
		byAccount[entry.first] = UsageJSON(entry.second);

	json usage = UsageJSON(info.usage);
	usage["byAccount"] = byAccount;

	const json status = {
		{"server", {
			{"product", "speech-server"},
			{"version", info.version},
			{"pid", info.pid},
			{"startedAt", SpeechIsoUtc(info.startedAt)},
			{"uptimeSecs", info.uptimeSecs}}},
		{"backend", {{"apiKey", info.apiKeyPresent ? "present" : "absent"}}},
		{"sessions", sessions},
		{"tokens", {
			{"live", info.tokens.live},
			{"issued", info.tokens.issued},
			{"consumed", info.tokens.consumed},
			{"expiredUnused", info.tokens.expiredUnused}}},
		{"usage", usage}};

	return status.dump(2) + "\n";
}

std::string SpeechStatus::RenderText(const Info& info)
{
	char buf[256];
	std::string out;

	snprintf(buf, sizeof(buf), "speech-server %s, pid %d\n", info.version.c_str(), info.pid);
	out += buf;
	snprintf(buf, sizeof(buf), "  started        %s (uptime %ld s)\n", SpeechIsoUtc(info.startedAt).c_str(), info.uptimeSecs);
	out += buf;
	snprintf(buf, sizeof(buf), "  Mistral key    %s\n", info.apiKeyPresent ? "present" : "absent");
	out += buf;

	out += "\nSessions         open / max / refused\n";
	for (int i = 0; i < SpeechFunctionCount; ++i)
	{
		snprintf(buf, sizeof(buf), "  %-14s %d / %d / %llu\n", SpeechFunctionName((SpeechFunction)i),
			info.sessions.open[i], info.sessions.max[i], (unsigned long long)info.sessions.refused[i]);
		out += buf;
	}

	snprintf(buf, sizeof(buf), "\nTokens           live %zu, issued %llu, consumed %llu, expired unused %llu\n",
		info.tokens.live, (unsigned long long)info.tokens.issued,
		(unsigned long long)info.tokens.consumed, (unsigned long long)info.tokens.expiredUnused);
	out += buf;

	out += "\nUsage            asr audio s / tts chars / comp tokens\n";
	snprintf(buf, sizeof(buf), "  %-14s %llu / %llu / %llu\n", "(total)",
		(unsigned long long)info.usage.asrAudioSecs, (unsigned long long)info.usage.ttsChars,
		(unsigned long long)info.usage.compTokens);
	out += buf;
	for (const auto& entry : info.usageByAccount)
	{
		snprintf(buf, sizeof(buf), "  %-14s %llu / %llu / %llu\n", entry.first.c_str(),
			(unsigned long long)entry.second.asrAudioSecs, (unsigned long long)entry.second.ttsChars,
			(unsigned long long)entry.second.compTokens);
		out += buf;
	}
	return out;
}
