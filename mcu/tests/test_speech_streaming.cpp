/**
 * test_speech_streaming.cpp — the speech-server streaming port (SPEC §7.1,
 * §7.2, §14.1): every code before the 101, the session bounds, the close codes,
 * dual stack, and a configuration swapped under open sessions.
 */
#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include "websocketserver.h"
#include "speech/streaminghandler.h"
#include "speech_test_util.h"

using json = nlohmann::json;
using speechtest::WsClient;

namespace
{

// Answers {"type":"ping"} with {"type":"pong"}; everything else goes to the base.
class PingSession : public SpeechSession
{
public:
	using SpeechSession::SpeechSession;

protected:
	void OnText(const json& message) override
	{
		if (message["type"] == "ping")
			Send(json{{"type", "pong"}});
		else
			SpeechSession::OnText(message);
	}
};

// A WebSocket that only records how it was closed.
class WsProbe : public WebSocket
{
public:
	void Accept(std::weak_ptr<Listener> listener) override	{}
	void Reject(const WORD code, const char* reason) override	{}
	void SendMessage(const std::string& message) override		{}
	void SendMessage(const BYTE* data, const DWORD size) override	{}
	void Close() override						{ closeCode = 1; }
	void Close(const WORD code, const char* reason) override	{ closeCode = code; }

	int closeCode = 0;
};

const char* BaseConfig = R"({"control": {"allow": ["127.0.0.0/8"]},
	"limits": {"maxSessions": {"asr": 1}},
	"voices": [{"alias": "fr-homme", "lang": "fr", "voiceId": "v-1"}]})";

class SpeechStreaming : public ::testing::Test
{
protected:
	void Start(const std::string& configText, bool withComp = true)
	{
		std::string error;
		std::shared_ptr<const SpeechConfig> parsed = SpeechConfig::Parse(configText, error);
		ASSERT_TRUE(parsed) << error;
		config = std::make_unique<SpeechConfigHolder>(parsed);
		streaming = std::make_unique<StreamingHandler>(*config, tokens);

		StreamingHandler::Factory factory = [](const SpeechSession::Params& params) {
			return std::make_shared<PingSession>(params);
		};
		streaming->SetFactory(SpeechFunction::Asr, factory);
		streaming->SetFactory(SpeechFunction::Tts, factory);
		if (withComp)
			streaming->SetFactory(SpeechFunction::Comp, factory);
		streaming->Start();

		port = speechtest::FreePort(AF_INET6);
		ASSERT_NE(0, port);
		server.AddHandler("/streaming/", streaming.get());
		ASSERT_TRUE(server.Init(IPAddress::Parse("::").To(port)));
	}

	void TearDown() override
	{
		if (!streaming)
			return;
		streaming->CloseAll(SpeechSession::CloseGoingAway, nullptr, 1000);
		streaming->Stop();
		server.End();
	}

	std::string Token(SpeechFunction function)
	{
		return tokens.Create(function, 60, "-", time(NULL)).value;
	}

	int Open(WsClient& client, const std::string& path, const std::string& address = "::1")
	{
		return client.Open(address, port, path);
	}

	TokenStore tokens;
	std::unique_ptr<SpeechConfigHolder> config;
	std::unique_ptr<StreamingHandler> streaming;
	WebSocketServer server;
	int port = 0;
};


}

TEST_F(SpeechStreaming, AStreamOpensWithReady)
{
	Start(BaseConfig);

	WsClient client;
	ASSERT_EQ(101, Open(client, "/streaming/comp/" + Token(SpeechFunction::Comp) + "?lang=fr"));

	const json ready = json::parse(client.ReadText());
	EXPECT_EQ("ready", ready["type"]);
	EXPECT_EQ("comp", ready["function"]);
	EXPECT_EQ(16000, ready["rate"]);
	EXPECT_EQ(1, streaming->GetCounters().open[(int)SpeechFunction::Comp]);

	client.SendText(R"({"type": "ping"})");
	EXPECT_EQ("pong", json::parse(client.ReadText())["type"]);
}

TEST_F(SpeechStreaming, EveryRefusalHappensBeforeThe101)
{
	Start(BaseConfig, false);
	WsClient a, b, c, d, e, f, g, h, i, j, k;

	EXPECT_EQ(404, Open(a, "/streaming/stt/" + Token(SpeechFunction::Asr)));
	EXPECT_EQ(404, Open(b, "/streaming/asr"));
	EXPECT_EQ(403, Open(c, "/streaming/asr/" + std::string(TokenStore::ValueLength, 'A')));
	EXPECT_EQ(403, Open(d, "/streaming/asr/" + Token(SpeechFunction::Tts)));
	EXPECT_EQ(400, Open(e, "/streaming/asr/" + Token(SpeechFunction::Asr) + "?speed=2"));
	EXPECT_EQ(400, Open(f, "/streaming/asr/" + Token(SpeechFunction::Asr) + "?delay=100"));
	EXPECT_EQ(400, Open(g, "/streaming/asr/" + Token(SpeechFunction::Asr) + "?voice=fr-homme"));
	EXPECT_EQ(400, Open(h, "/streaming/tts/" + Token(SpeechFunction::Tts) + "?voice=claire"));
	EXPECT_EQ(400, Open(i, "/streaming/tts/" + Token(SpeechFunction::Tts) + "?lang=en"));
	EXPECT_EQ(400, Open(j, "/streaming/tts/" + Token(SpeechFunction::Tts) + "?lang=FR"));
	EXPECT_EQ(503, Open(k, "/streaming/comp/" + Token(SpeechFunction::Comp)));
	EXPECT_EQ(0u, tokens.GetCounters(time(NULL)).consumed);
}

TEST_F(SpeechStreaming, ATokenServesOneStreamOnly)
{
	Start(BaseConfig);
	const std::string token = Token(SpeechFunction::Tts);

	WsClient first, second;
	EXPECT_EQ(101, Open(first, "/streaming/tts/" + token + "?voice=fr-homme"));
	EXPECT_EQ(403, Open(second, "/streaming/tts/" + token + "?voice=fr-homme"));
}

TEST_F(SpeechStreaming, TtsWithoutAnyVoiceIs503)
{
	Start(R"({"control": {"allow": ["127.0.0.0/8"]}})");

	WsClient client;
	EXPECT_EQ(503, Open(client, "/streaming/tts/" + Token(SpeechFunction::Tts)));
}

TEST_F(SpeechStreaming, TheSessionLimitAnswers429AndKeepsTheToken)
{
	Start(BaseConfig);

	WsClient first, second;
	ASSERT_EQ(101, Open(first, "/streaming/asr/" + Token(SpeechFunction::Asr)));
	const std::string token = Token(SpeechFunction::Asr);
	EXPECT_EQ(429, Open(second, "/streaming/asr/" + token));
	EXPECT_EQ(1u, streaming->GetCounters().refused[(int)SpeechFunction::Asr]);

	TokenStore::Token kept;
	EXPECT_EQ(TokenStore::Refusal::None, tokens.Consume(token, SpeechFunction::Asr, time(NULL), kept));
}

TEST_F(SpeechStreaming, AnIdleStreamIsClosedWith1000)
{
	Start(R"({"control": {"allow": ["127.0.0.0/8"]}, "limits": {"idleSecs": 1}})");

	WsClient client;
	ASSERT_EQ(101, Open(client, "/streaming/comp/" + Token(SpeechFunction::Comp)));
	ASSERT_EQ("ready", json::parse(client.ReadText())["type"]);

	// Activity keeps it open past the idle bound...
	for (int i = 0; i < 4; ++i)
	{
		usleep(400*1000);
		client.SendText(R"({"type": "ping"})");
		ASSERT_EQ("pong", json::parse(client.ReadText())["type"]);
	}

	// ...silence closes it.
	const json error = json::parse(client.ReadText());
	EXPECT_EQ("error", error["type"]);
	EXPECT_EQ("idle", error["reason"]);
	EXPECT_EQ(true, error["fatal"]);

	const WsClient::Frame close = client.Read();
	EXPECT_EQ(WebSocketFrameHeader::Close, close.opcode);
	EXPECT_EQ(SpeechSession::CloseNormal, close.CloseCode());
}

TEST(SpeechSessionLimits, AnInstantReadBeforeTheLastActivityIsNotIdleness)
{
	std::string error;
	SpeechSession::Params params;
	params.function = SpeechFunction::Comp;
	params.config = SpeechConfig::Parse(R"({"control": {"allow": ["127.0.0.0/8"]}})", error);
	ASSERT_TRUE(params.config) << error;
	auto session = std::make_shared<PingSession>(params);

	// The watchdog read its clock just before this session was created.
	const uint64_t before = SpeechSession::NowMs() - 1;
	auto socket = std::make_shared<WsProbe>();
	session->Attach(socket, nullptr);
	session->CheckLimits(before);

	EXPECT_EQ(0, socket->closeCode);
}

TEST_F(SpeechStreaming, ShutdownClosesWith1001)
{
	Start(BaseConfig);

	WsClient client;
	ASSERT_EQ(101, Open(client, "/streaming/comp/" + Token(SpeechFunction::Comp)));
	ASSERT_EQ("ready", json::parse(client.ReadText())["type"]);

	streaming->CloseAll(SpeechSession::CloseGoingAway, "going away", 0);

	EXPECT_EQ("going away", json::parse(client.ReadText())["reason"]);
	EXPECT_EQ(SpeechSession::CloseGoingAway, client.Read().CloseCode());
}

TEST_F(SpeechStreaming, ABadMessageIsANonFatalError)
{
	Start(BaseConfig);

	WsClient client;
	ASSERT_EQ(101, Open(client, "/streaming/comp/" + Token(SpeechFunction::Comp)));
	ASSERT_EQ("ready", json::parse(client.ReadText())["type"]);

	client.SendText("not json");
	json error = json::parse(client.ReadText());
	EXPECT_EQ("invalid JSON", error["reason"]);
	EXPECT_EQ(false, error["fatal"]);

	client.SendText(R"({"text": "no type"})");
	EXPECT_EQ("missing type", json::parse(client.ReadText())["reason"]);

	client.SendText(R"({"type": "ping"})");
	EXPECT_EQ("pong", json::parse(client.ReadText())["type"]);
}

TEST_F(SpeechStreaming, AnOversizedMessageClosesWith1009)
{
	Start(BaseConfig);

	WsClient client;
	ASSERT_EQ(101, Open(client, "/streaming/comp/" + Token(SpeechFunction::Comp)));
	ASSERT_EQ("ready", json::parse(client.ReadText())["type"]);

	client.SendBinary(std::string(SpeechSession::MaxMessageSize + 1, 'x'));

	EXPECT_EQ(true, json::parse(client.ReadText())["fatal"]);
	EXPECT_EQ(SpeechSession::CloseTooBig, client.Read().CloseCode());
}

TEST_F(SpeechStreaming, BothFamiliesReachTheStreamingPort)
{
	Start(BaseConfig);

	WsClient v6, v4;
	EXPECT_EQ(101, Open(v6, "/streaming/comp/" + Token(SpeechFunction::Comp), "::1"));
	EXPECT_EQ(101, Open(v4, "/streaming/comp/" + Token(SpeechFunction::Comp), "127.0.0.1"));
}

TEST_F(SpeechStreaming, AReloadedVoiceServesTheNextStreamAndSparesTheOpenOne)
{
	Start(BaseConfig);

	WsClient open;
	ASSERT_EQ(101, Open(open, "/streaming/tts/" + Token(SpeechFunction::Tts)));
	ASSERT_EQ("ready", json::parse(open.ReadText())["type"]);

	std::string error;
	config->Set(SpeechConfig::Parse(R"({"control": {"allow": ["127.0.0.0/8"]},
		"voices": [{"alias": "fr-homme", "lang": "fr", "voiceId": "v-1"},
		           {"alias": "en-paul", "lang": "en", "voiceId": "en_paul_neutral"}]})", error));

	WsClient next;
	EXPECT_EQ(101, Open(next, "/streaming/tts/" + Token(SpeechFunction::Tts) + "?voice=en-paul"));

	open.SendText(R"({"type": "ping"})");
	EXPECT_EQ("pong", json::parse(open.ReadText())["type"]);
}
