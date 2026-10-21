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

SpeechStatus::SpeechStatus(time_t startedAt, bool apiKeyPresent, BackendHealth& health, BackendProbe& probe, StreamingHandler& streaming, TokenStore& tokens, UsageCounters& usage) :
	startedAt(startedAt),
	apiKeyPresent(apiKeyPresent),
	health(health),
	probe(probe),
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
	for (int i = 0; i < SpeechFunctionCount; ++i)
		info.backend[i] = health.Get((SpeechFunction)i);
	info.mistral = probe.Get();
	info.sessions = streaming.GetCounters();
	info.tokens = tokens.GetCounters(now);
	info.usage = usage.Total();
	info.usageByAccount = usage.ByAccount();
}

namespace
{

json Instant(time_t when)
{
	return when ? json(SpeechIsoUtc(when)) : json(nullptr);
}

json UsageJSON(const UsageCounters::Usage& usage)
{
	return json{{"asrAudioSecs", usage.asrAudioMs/1000.0}, {"ttsChars", usage.ttsChars}, {"compTokens", usage.compTokens}};
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

	json backend = {{"apiKey", info.apiKeyPresent ? "present" : "absent"}};
	for (int i = 0; i < SpeechFunctionCount; ++i)
		backend[SpeechFunctionName((SpeechFunction)i)] = {
			{"lastSuccess", Instant(info.backend[i].lastSuccess)},
			{"lastFailure", Instant(info.backend[i].lastFailure)},
			{"lastError", info.backend[i].lastError},
			{"discarded", info.backend[i].discarded}};
	backend["mistral"] = {
		{"state", BackendProbe::StateName(info.mistral.state)},
		{"reason", info.mistral.reason.empty() ? json(nullptr) : json(info.mistral.reason)},
		{"checkedAt", Instant(info.mistral.checkedAt)}};

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
		{"backend", backend},
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
	snprintf(buf, sizeof(buf), "  Mistral        %s%s%s (checked %s)\n", BackendProbe::StateName(info.mistral.state),
		info.mistral.reason.empty() ? "" : " - ", info.mistral.reason.c_str(),
		info.mistral.checkedAt ? SpeechIsoUtc(info.mistral.checkedAt).c_str() : "never");
	out += buf;

	for (int i = 0; i < SpeechFunctionCount; ++i)
	{
		const BackendHealth::Service& service = info.backend[i];
		snprintf(buf, sizeof(buf), "  Mistral %-6s success %s, failure %s%s%s\n", SpeechFunctionName((SpeechFunction)i),
			service.lastSuccess ? SpeechIsoUtc(service.lastSuccess).c_str() : "never",
			service.lastFailure ? SpeechIsoUtc(service.lastFailure).c_str() : "never",
			service.lastError.empty() ? "" : " - ", service.lastError.c_str());
		out += buf;
	}

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
	snprintf(buf, sizeof(buf), "  %-14s %.1f / %llu / %llu\n", "(total)",
		info.usage.asrAudioMs/1000.0, (unsigned long long)info.usage.ttsChars,
		(unsigned long long)info.usage.compTokens);
	out += buf;
	for (const auto& entry : info.usageByAccount)
	{
		snprintf(buf, sizeof(buf), "  %-14s %.1f / %llu / %llu\n", entry.first.c_str(),
			entry.second.asrAudioMs/1000.0, (unsigned long long)entry.second.ttsChars,
			(unsigned long long)entry.second.compTokens);
		out += buf;
	}
	return out;
}
