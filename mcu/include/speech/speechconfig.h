#ifndef SPEECH_SPEECHCONFIG_H
#define SPEECH_SPEECHCONFIG_H

#include <memory>
#include <string>
#include <vector>
#include "ipaddress.h"
#include "speech/allowlist.h"
#include "speech/speechfunction.h"

/**
 * speech-config.json (SPEC §8.2), immutable once parsed.
 *
 * A missing key takes its default; an unknown key is an error, so that a typo
 * never passes silently.
 */
struct SpeechConfig
{
	struct Voice
	{
		std::string	alias;
		std::string	lang;
		std::string	voiceId;
	};

	// Abyss truncates IPv6 peer addresses: the control port is IPv4 only.
	IPAddress	controlListen = IPAddress::Parse("0.0.0.0");
	int		controlPort = 9200;
	AllowList	controlAllow;

	IPAddress	streamingListen = IPAddress::Parse("::");
	int		streamingPort = 9201;
	bool		streamingSecure = false;
	std::string	streamingCert = "/etc/mediaserver/mcu.crt";
	std::string	streamingKey = "/etc/mediaserver/mcu.key";

	std::string	mistralUrl = "https://api.mistral.ai";
	int		mistralWorkers = 10;

	int		maxSessions[SpeechFunctionCount] = { 20, 20, 20 };
	int		idleSecs = 60;
	int		maxSessionSecs = 3600;

	std::vector<Voice> voices;

	std::string	ttsModel = "voxtral-mini-tts-2603";
	int		ttsPhraseSilenceMs = 1500;
	std::string	asrModel = "voxtral-mini-transcribe-realtime-2602";
	bool		asrVad = false;
	int		asrSilenceMs = 1500;
	int		asrPauseCloseSecs = 30;
	std::string	compModel = "mistral-small-latest";

	static std::shared_ptr<const SpeechConfig> Parse(const std::string& text, std::string& error);
	static std::shared_ptr<const SpeechConfig> Load(const std::string& path, std::string& error);

	const Voice* FindVoice(const std::string& alias) const;
	// First voice of the language, nullptr when none.
	const Voice* DefaultVoice(const std::string& lang) const;

	// Keys a SIGHUP cannot apply: ports, listen addresses, TLS, mistral.*.
	bool NeedsRestartToApply(const SpeechConfig& next) const;
};

/**
 * The configuration in force. SIGHUP swaps it; a session keeps the snapshot it
 * was opened with.
 */
class SpeechConfigHolder
{
public:
	explicit SpeechConfigHolder(std::shared_ptr<const SpeechConfig> config) : config(config) {}

	std::shared_ptr<const SpeechConfig> Get() const		{ return std::atomic_load(&config); }
	void Set(std::shared_ptr<const SpeechConfig> next)	{ std::atomic_store(&config, next); }

private:
	std::shared_ptr<const SpeechConfig> config;
};

#endif
