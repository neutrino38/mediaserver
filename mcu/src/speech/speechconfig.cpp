#include "speech/speechconfig.h"

#include <fstream>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{

// Thrown with the message for the operator; never escapes Parse().
struct ConfigError
{
	std::string message;
};

[[noreturn]] void Fail(const std::string& message)
{
	throw ConfigError{message};
}

const json& Object(const json& value, const std::string& path)
{
	if (!value.is_object())
		Fail("'" + path + "' must be an object");
	return value;
}

void CheckKeys(const json& object, const std::string& path, const std::set<std::string>& known)
{
	for (auto it = object.begin(); it != object.end(); ++it)
		if (!known.count(it.key()))
			Fail("unknown key '" + (path.empty() ? "" : path + ".") + it.key() + "'");
}

std::string Join(const std::string& path, const std::string& key)
{
	return path.empty() ? key : path + "." + key;
}

void ReadInt(const json& object, const std::string& path, const std::string& key, int& out, int min, int max)
{
	if (!object.contains(key))
		return;
	const json& value = object[key];
	if (!value.is_number_integer() || value.get<long long>() < min || value.get<long long>() > max)
		Fail("'" + Join(path, key) + "' must be an integer from " + std::to_string(min) + " to " + std::to_string(max));
	out = value.get<int>();
}

void ReadBool(const json& object, const std::string& path, const std::string& key, bool& out)
{
	if (!object.contains(key))
		return;
	if (!object[key].is_boolean())
		Fail("'" + Join(path, key) + "' must be true or false");
	out = object[key].get<bool>();
}

void ReadString(const json& object, const std::string& path, const std::string& key, std::string& out)
{
	if (!object.contains(key))
		return;
	if (!object[key].is_string() || object[key].get<std::string>().empty())
		Fail("'" + Join(path, key) + "' must be a non-empty string");
	out = object[key].get<std::string>();
}

void ReadAddress(const json& object, const std::string& path, const std::string& key, IPAddress& out)
{
	std::string text;
	ReadString(object, path, key, text);
	if (text.empty())
		return;
	out = IPAddress::Parse(text);
	if (!out.IsSet())
		Fail("'" + Join(path, key) + "' must be an IP address literal");
}

bool IsToken(const std::string& text)
{
	for (char c : text)
		if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
			return false;
	return !text.empty();
}

bool IsLang(const std::string& text)
{
	if (text.size() < 2 || text.size() > 3)
		return false;
	for (char c : text)
		if (c < 'a' || c > 'z')
			return false;
	return true;
}

void ParseInto(const json& root, SpeechConfig& config)
{
	Object(root, "(root)");
	CheckKeys(root, "", {"control", "streaming", "mistral", "limits", "voices", "tts", "asr", "comp"});

	if (!root.contains("control"))
		Fail("'control.allow' is mandatory");
	const json& control = Object(root["control"], "control");
	CheckKeys(control, "control", {"listen", "port", "allow"});
	ReadAddress(control, "control", "listen", config.controlListen);
	if (!config.controlListen.IsV4())
		Fail("'control.listen' must be an IPv4 address: the control port does not serve IPv6");
	ReadInt(control, "control", "port", config.controlPort, 1, 65535);
	if (!control.contains("allow") || !control["allow"].is_array() || control["allow"].empty())
		Fail("'control.allow' must be a non-empty list: an open control port issues tokens to anyone");
	for (const json& entry : control["allow"])
	{
		std::string error;
		if (!entry.is_string() || !config.controlAllow.Add(entry.get<std::string>(), error))
			Fail("'control.allow': " + (entry.is_string() ? error : std::string("entries must be strings")));
	}
	if (config.controlAllow.HasIPv6())
		Fail("'control.allow' holds an IPv6 entry, which can never match: the control port is IPv4 only");

	if (root.contains("streaming"))
	{
		const json& streaming = Object(root["streaming"], "streaming");
		CheckKeys(streaming, "streaming", {"listen", "port", "secure", "cert", "key"});
		ReadAddress(streaming, "streaming", "listen", config.streamingListen);
		ReadInt(streaming, "streaming", "port", config.streamingPort, 1, 65535);
		ReadBool(streaming, "streaming", "secure", config.streamingSecure);
		ReadString(streaming, "streaming", "cert", config.streamingCert);
		ReadString(streaming, "streaming", "key", config.streamingKey);
	}
	if (config.streamingPort == config.controlPort)
		Fail("'control.port' and 'streaming.port' must differ");

	if (root.contains("mistral"))
	{
		const json& mistral = Object(root["mistral"], "mistral");
		CheckKeys(mistral, "mistral", {"url", "workers"});
		ReadString(mistral, "mistral", "url", config.mistralUrl);
		ReadInt(mistral, "mistral", "workers", config.mistralWorkers, 1, 64);
	}
	if (config.mistralUrl.rfind("https://", 0) != 0 && config.mistralUrl.rfind("http://", 0) != 0)
		Fail("'mistral.url' must start with https:// or http://");

	if (root.contains("limits"))
	{
		const json& limits = Object(root["limits"], "limits");
		CheckKeys(limits, "limits", {"maxSessions", "idleSecs", "maxSessionSecs"});
		if (limits.contains("maxSessions"))
		{
			const json& max = Object(limits["maxSessions"], "limits.maxSessions");
			CheckKeys(max, "limits.maxSessions", {"asr", "tts", "comp"});
			for (int i = 0; i < SpeechFunctionCount; ++i)
				ReadInt(max, "limits.maxSessions", SpeechFunctionName((SpeechFunction)i), config.maxSessions[i], 0, 100000);
		}
		ReadInt(limits, "limits", "idleSecs", config.idleSecs, 1, 86400);
		ReadInt(limits, "limits", "maxSessionSecs", config.maxSessionSecs, 1, 86400);
	}

	if (root.contains("voices"))
	{
		if (!root["voices"].is_array())
			Fail("'voices' must be a list");
		for (const json& entry : root["voices"])
		{
			Object(entry, "voices[]");
			CheckKeys(entry, "voices[]", {"alias", "lang", "voiceId"});
			SpeechConfig::Voice voice;
			ReadString(entry, "voices[]", "alias", voice.alias);
			ReadString(entry, "voices[]", "lang", voice.lang);
			ReadString(entry, "voices[]", "voiceId", voice.voiceId);
			if (!IsToken(voice.alias))
				Fail("'voices[].alias' must be made of [A-Za-z0-9._-]");
			if (!IsLang(voice.lang))
				Fail("'voices[].lang' must be a short language code such as 'fr'");
			if (voice.voiceId.empty())
				Fail("'voices[].voiceId' is mandatory");
			if (config.FindVoice(voice.alias))
				Fail("voice alias '" + voice.alias + "' is declared twice");
			config.voices.push_back(voice);
		}
	}

	if (root.contains("tts"))
	{
		const json& tts = Object(root["tts"], "tts");
		CheckKeys(tts, "tts", {"model", "phraseSilenceMs"});
		ReadString(tts, "tts", "model", config.ttsModel);
		ReadInt(tts, "tts", "phraseSilenceMs", config.ttsPhraseSilenceMs, 0, 60000);
	}

	if (root.contains("asr"))
	{
		const json& asr = Object(root["asr"], "asr");
		CheckKeys(asr, "asr", {"model", "vad", "silenceMs", "pauseCloseSecs"});
		ReadString(asr, "asr", "model", config.asrModel);
		ReadBool(asr, "asr", "vad", config.asrVad);
		ReadInt(asr, "asr", "silenceMs", config.asrSilenceMs, 0, 60000);
		// Mistral closes a silent session after about 60 s (SPEC §4.2).
		ReadInt(asr, "asr", "pauseCloseSecs", config.asrPauseCloseSecs, 1, 55);
	}

	if (root.contains("comp"))
	{
		const json& comp = Object(root["comp"], "comp");
		CheckKeys(comp, "comp", {"model", "debounceMs"});
		ReadString(comp, "comp", "model", config.compModel);
		ReadInt(comp, "comp", "debounceMs", config.compDebounceMs, 0, 10000);
	}
}

}

std::shared_ptr<const SpeechConfig> SpeechConfig::Parse(const std::string& text, std::string& error)
{
	auto config = std::make_shared<SpeechConfig>();
	try
	{
		ParseInto(json::parse(text), *config);
	}
	catch (const json::parse_error& e)
	{
		error = std::string("invalid JSON: ") + e.what();
		return nullptr;
	}
	catch (const ConfigError& e)
	{
		error = e.message;
		return nullptr;
	}
	return config;
}

std::shared_ptr<const SpeechConfig> SpeechConfig::Load(const std::string& path, std::string& error)
{
	std::ifstream file(path);
	if (!file)
	{
		error = "cannot read " + path;
		return nullptr;
	}
	std::stringstream text;
	text << file.rdbuf();

	std::shared_ptr<const SpeechConfig> config = Parse(text.str(), error);
	if (!config)
		error = path + ": " + error;
	return config;
}

const SpeechConfig::Voice* SpeechConfig::FindVoice(const std::string& alias) const
{
	for (const Voice& voice : voices)
		if (voice.alias == alias)
			return &voice;
	return nullptr;
}

const SpeechConfig::Voice* SpeechConfig::DefaultVoice(const std::string& lang) const
{
	for (const Voice& voice : voices)
		if (voice.lang == lang)
			return &voice;
	return nullptr;
}

bool SpeechConfig::NeedsRestartToApply(const SpeechConfig& next) const
{
	return controlListen != next.controlListen
		|| controlPort != next.controlPort
		|| streamingListen != next.streamingListen
		|| streamingPort != next.streamingPort
		|| streamingSecure != next.streamingSecure
		|| streamingCert != next.streamingCert
		|| streamingKey != next.streamingKey
		|| mistralUrl != next.mistralUrl
		|| mistralWorkers != next.mistralWorkers;
}
