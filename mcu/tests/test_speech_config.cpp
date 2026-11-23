/**
 * test_speech_config.cpp — speech-config.json (SPEC §8.2): defaults, strictness,
 * and the IPv4-only control port.
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include "speech/speechconfig.h"

namespace
{

const std::string Minimal = R"({"control": {"allow": ["10.0.0.0/8"]}})";

std::shared_ptr<const SpeechConfig> Parse(const std::string& text, std::string& error)
{
	return SpeechConfig::Parse(text, error);
}

void ExpectRejected(const std::string& text, const std::string& expectedInError)
{
	std::string error;
	EXPECT_FALSE(Parse(text, error)) << text;
	EXPECT_NE(std::string::npos, error.find(expectedInError)) << "error was: " << error;
}

}

TEST(SpeechConfig, AMinimalFileTakesEveryDefault)
{
	std::string error;
	auto config = Parse(Minimal, error);
	ASSERT_TRUE(config) << error;

	EXPECT_EQ("0.0.0.0", config->controlListen.ToString());
	EXPECT_EQ(9200, config->controlPort);
	EXPECT_EQ("::", config->streamingListen.ToString());
	EXPECT_EQ(9201, config->streamingPort);
	EXPECT_FALSE(config->streamingSecure);
	EXPECT_EQ("https://api.mistral.ai", config->mistralUrl);
	EXPECT_EQ(10, config->mistralWorkers);
	for (int i = 0; i < SpeechFunctionCount; ++i)
		EXPECT_EQ(20, config->maxSessions[i]);
	EXPECT_EQ(60, config->idleSecs);
	EXPECT_EQ(3600, config->maxSessionSecs);
	EXPECT_TRUE(config->voices.empty());
	EXPECT_EQ(1500, config->ttsPhraseSilenceMs);
	EXPECT_EQ("voxtral-mini-tts-3-0", config->ttsModel);
	EXPECT_EQ(30, config->asrPauseCloseSecs);
	EXPECT_EQ("voxtral-mini-transcribe-realtime-3-0", config->asrModel);
	EXPECT_EQ("mistral-small-latest", config->compModel);
}

TEST(SpeechConfig, TheSpecExampleIsAccepted)
{
	std::string error;
	auto config = Parse(R"({
		"control": { "listen": "192.0.2.1", "port": 9300, "allow": ["10.0.0.0/8", "192.0.2.7"] },
		"streaming": { "listen": "2001:db8::1", "port": 9301, "secure": true, "cert": "/c", "key": "/k" },
		"mistral": { "url": "http://127.0.0.1:8080", "workers": 2 },
		"limits": { "maxSessions": { "asr": 5, "tts": 6, "comp": 7 }, "idleSecs": 10, "maxSessionSecs": 100 },
		"voices": [ { "alias": "fr-homme", "lang": "fr", "voiceId": "abc" } ],
		"tts": { "phraseSilenceMs": 1000 },
		"asr": { "model": "voxtral-next", "vad": true, "silenceMs": 900, "pauseCloseSecs": 20 },
		"comp": { "model": "ministral-3b-latest" }
	})", error);
	ASSERT_TRUE(config) << error;

	EXPECT_EQ(9300, config->controlPort);
	EXPECT_TRUE(config->controlAllow.Allows(IPAddress::Parse("192.0.2.7")));
	EXPECT_EQ("2001:db8::1", config->streamingListen.ToString());
	EXPECT_TRUE(config->streamingSecure);
	EXPECT_EQ(7, config->maxSessions[(int)SpeechFunction::Comp]);
	ASSERT_EQ(1u, config->voices.size());
	EXPECT_EQ("abc", config->FindVoice("fr-homme")->voiceId);
	EXPECT_EQ("fr-homme", config->DefaultVoice("fr")->alias);
	EXPECT_EQ(nullptr, config->DefaultVoice("en"));
	EXPECT_TRUE(config->asrVad);
	EXPECT_EQ("voxtral-next", config->asrModel);
	EXPECT_EQ("ministral-3b-latest", config->compModel);
}

TEST(SpeechConfig, AnUnknownKeyIsAnError)
{
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "limit": {}})", "unknown key 'limit'");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"], "prot": 1}})", "unknown key 'control.prot'");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "limits": {"maxSessions": {"stt": 1}}})",
		       "unknown key 'limits.maxSessions.stt'");
}

TEST(SpeechConfig, TheAllowListIsMandatoryAndNotEmpty)
{
	ExpectRejected(R"({})", "control.allow");
	ExpectRejected(R"({"control": {}})", "control.allow");
	ExpectRejected(R"({"control": {"allow": []}})", "control.allow");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/33"]}})", "prefix length");
	ExpectRejected(R"({"control": {"allow": ["example.com"]}})", "not an IP address");
}

TEST(SpeechConfig, TheControlPortIsIPv4Only)
{
	ExpectRejected(R"({"control": {"allow": ["2001:db8::/32"]}})", "IPv4 only");
	ExpectRejected(R"({"control": {"listen": "::", "allow": ["10.0.0.0/8"]}})", "IPv4");
}

TEST(SpeechConfig, TypesAndBoundsAreChecked)
{
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"], "port": "9200"}})", "control.port");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"], "port": 70000}})", "control.port");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "streaming": {"secure": 1}})", "streaming.secure");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "asr": {"pauseCloseSecs": 60}})", "asr.pauseCloseSecs");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "mistral": {"url": "ftp://x"}})", "mistral.url");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"], "port": 9201}})", "must differ");
}

TEST(SpeechConfig, VoicesAreChecked)
{
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "voices": [{"alias": "a", "lang": "french", "voiceId": "x"}]})", "lang");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "voices": [{"alias": "a b", "lang": "fr", "voiceId": "x"}]})", "alias");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "voices": [{"alias": "a", "lang": "fr"}]})", "voiceId");
	ExpectRejected(R"({"control": {"allow": ["10.0.0.0/8"]}, "voices": [
		{"alias": "a", "lang": "fr", "voiceId": "x"}, {"alias": "a", "lang": "en", "voiceId": "y"}]})", "twice");
}

TEST(SpeechConfig, InvalidJSONIsAnError)
{
	ExpectRejected("{", "invalid JSON");
	ExpectRejected("[]", "must be an object");
}

TEST(SpeechConfig, AMissingFileIsAnError)
{
	std::string error;
	EXPECT_FALSE(SpeechConfig::Load("/nonexistent/speech-config.json", error));
	EXPECT_NE(std::string::npos, error.find("cannot read"));
}

TEST(SpeechConfig, OnlyPortsAddressesTlsAndMistralNeedARestart)
{
	std::string error;
	auto base = Parse(Minimal, error);
	auto voices = Parse(R"({"control": {"allow": ["10.0.0.0/8", "192.0.2.0/24"]},
		"limits": {"idleSecs": 5}, "voices": [{"alias": "a", "lang": "fr", "voiceId": "x"}]})", error);
	auto port = Parse(R"({"control": {"allow": ["10.0.0.0/8"]}, "streaming": {"port": 9500}})", error);
	auto mistral = Parse(R"({"control": {"allow": ["10.0.0.0/8"]}, "mistral": {"workers": 8}})", error);
	ASSERT_TRUE(base && voices && port && mistral);

	EXPECT_FALSE(base->NeedsRestartToApply(*voices));
	EXPECT_TRUE(base->NeedsRestartToApply(*port));
	EXPECT_TRUE(base->NeedsRestartToApply(*mistral));
}
