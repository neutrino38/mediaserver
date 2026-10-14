/**
 * test_speech_control.cpp — the speech-server control port (SPEC §6, §14.1):
 * IP filter, token creation, voices, status. A real Abyss server on loopback.
 */
#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include "xmlrpcserver.h"
#include "speech/controlhandler.h"
#include "speech/speechstatus.h"
#include "speech/streaminghandler.h"
#include "speech_test_util.h"

using json = nlohmann::json;
using speechtest::Http;

namespace
{

class SpeechControl : public ::testing::Test
{
protected:
	void Start(const std::string& configText)
	{
		std::string error;
		std::shared_ptr<const SpeechConfig> parsed = SpeechConfig::Parse(configText, error);
		ASSERT_TRUE(parsed) << error;

		config = std::make_unique<SpeechConfigHolder>(parsed);
		streaming = std::make_unique<StreamingHandler>(*config, tokens);
		status = std::make_unique<SpeechStatus>(time(NULL), true, health, *streaming, tokens, usage);
		control = std::make_unique<ControlHandler>(*config, tokens, usage, *status);

		port = speechtest::FreePort(AF_INET);
		ASSERT_NE(0, port);
		server = std::make_unique<XmlRpcServer>(IPAddress::Parse("127.0.0.1").To(port));
		server->AddHandler("/", control.get());
		thread = std::thread([this] { server->Start(); });
	}

	void TearDown() override
	{
		if (!server)
			return;
		// A request answered proves ServerRun is running: Stop is safe then.
		Http("127.0.0.1", port, "GET", "/status/general");
		server->Stop();
		thread.join();
	}

	speechtest::HttpResponse Token(const std::string& body)
	{
		return Http("127.0.0.1", port, "POST", "/control/token", "application/json", body);
	}

	TokenStore tokens;
	UsageCounters usage;
	BackendHealth health;
	std::unique_ptr<SpeechConfigHolder> config;
	std::unique_ptr<StreamingHandler> streaming;
	std::unique_ptr<SpeechStatus> status;
	std::unique_ptr<ControlHandler> control;
	std::unique_ptr<XmlRpcServer> server;
	std::thread thread;
	int port = 0;
};

const char* Allowed = R"({"control": {"allow": ["127.0.0.0/8"]},
	"voices": [{"alias": "fr-homme", "lang": "fr", "voiceId": "v-123"}]})";

}

TEST_F(SpeechControl, AnAllowedAddressGetsAToken)
{
	Start(Allowed);

	const speechtest::HttpResponse response = Token(R"({"function": "tts", "ttl": 60})");
	ASSERT_EQ(201, response.status) << response.body;

	const json body = json::parse(response.body);
	EXPECT_EQ(TokenStore::ValueLength, body["token"].get<std::string>().size());
	EXPECT_EQ("tts", body["function"]);
	EXPECT_EQ("-", body["account"]);
	EXPECT_EQ(20u, body["expiresAt"].get<std::string>().size());
	EXPECT_EQ(1u, tokens.GetCounters(time(NULL)).live);
}

TEST_F(SpeechControl, AnAddressOutsideTheListGets403Everywhere)
{
	Start(R"({"control": {"allow": ["10.0.0.0/8"]}})");

	EXPECT_EQ(403, Token(R"({"function": "tts"})").status);
	EXPECT_EQ(403, Http("127.0.0.1", port, "GET", "/control/voices").status);
	EXPECT_EQ(403, Http("127.0.0.1", port, "GET", "/status/general").status);
	EXPECT_EQ(0u, tokens.GetCounters(time(NULL)).issued);
}

TEST_F(SpeechControl, TheAccountIsEchoedAndCounted)
{
	Start(Allowed);

	const speechtest::HttpResponse response = Token(R"({"function": "asr", "account": "clinique-nord"})");
	ASSERT_EQ(201, response.status) << response.body;
	EXPECT_EQ("clinique-nord", json::parse(response.body)["account"]);

	const json statusBody = json::parse(Http("127.0.0.1", port, "GET", "/status/general").body);
	EXPECT_TRUE(statusBody["usage"]["byAccount"].contains("clinique-nord"));
	EXPECT_EQ(1, statusBody["tokens"]["issued"]);
}

TEST_F(SpeechControl, InvalidRequestsAreRefused)
{
	Start(Allowed);

	EXPECT_EQ(400, Token(R"({"function": "tts", "ttl": 3601})").status);
	EXPECT_EQ(400, Token(R"({"function": "tts", "ttl": 0})").status);
	EXPECT_EQ(400, Token(R"({"function": "stt"})").status);
	EXPECT_EQ(400, Token(R"({"ttl": 60})").status);
	EXPECT_EQ(400, Token(R"({"function": "tts", "account": "a b"})").status);
	EXPECT_EQ(400, Token(R"({"function": "tts", "account": ")" + std::string(65, 'a') + R"("})").status);
	EXPECT_EQ(400, Token(R"({"function": "tts", "extra": 1})").status);
	EXPECT_EQ(400, Token("{not json").status);
	EXPECT_EQ(415, Http("127.0.0.1", port, "POST", "/control/token", "text/plain", "x").status);
	EXPECT_EQ(413, Token(std::string(ControlHandler::MaxBodySize + 1, ' ')).status);
	EXPECT_EQ(405, Http("127.0.0.1", port, "GET", "/control/token").status);
	EXPECT_EQ(0u, tokens.GetCounters(time(NULL)).issued);
}

TEST_F(SpeechControl, ThereIsNoStopUri)
{
	Start(Allowed);

	EXPECT_EQ(404, Http("127.0.0.1", port, "GET", "/stop").status);
	EXPECT_EQ(200, Http("127.0.0.1", port, "GET", "/status/general").status);
}

TEST_F(SpeechControl, TheVoicesComeFromTheConfiguration)
{
	Start(Allowed);

	const speechtest::HttpResponse response = Http("127.0.0.1", port, "GET", "/control/voices");
	ASSERT_EQ(200, response.status);
	const json voices = json::parse(response.body)["voices"];
	ASSERT_EQ(1u, voices.size());
	EXPECT_EQ("fr-homme", voices[0]["alias"]);
	EXPECT_EQ("fr", voices[0]["lang"]);
	EXPECT_EQ("v-123", voices[0]["voiceId"]);
}

TEST_F(SpeechControl, TheStatusHasTwoRenderings)
{
	Start(Allowed);

	const json body = json::parse(Http("127.0.0.1", port, "GET", "/status/general").body);
	EXPECT_EQ("speech-server", body["server"]["product"]);
	EXPECT_EQ("present", body["backend"]["apiKey"]);
	EXPECT_EQ(20, body["sessions"]["tts"]["max"]);
	EXPECT_EQ(0, body["sessions"]["asr"]["open"]);

	const std::string text = Http("127.0.0.1", port, "GET", "/status/general?format=text").body;
	EXPECT_NE(std::string::npos, text.find("speech-server"));
	EXPECT_NE(std::string::npos, text.find("Tokens"));
}
