/**
 * test_speech_tts.cpp — tts (SPEC §7.3, §14.3) against a local fake of
 * Voxtral TTS. No network.
 */
#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include "websocketserver.h"
#include "xmlrpcserver.h"
#include "speech/mistralspeech.h"
#include "speech/streaminghandler.h"
#include "speech/ttssession.h"
#include "fake_mistral_http.h"
#include "speech_test_util.h"

using json = nlohmann::json;
using speechtest::WsClient;

namespace
{

// One phrase as the client saw it.
struct Spoken
{
	int		id = 0;
	std::string	text;
	std::string	pcm;
	bool		done = false;
	json		error;
};

class SpeechTts : public ::testing::Test
{
protected:
	void Start(const std::string& extraConfig = "")
	{
		fake = std::make_unique<FakeMistralHttp>(std::string(SPEECH_FIXTURES_DIR) + "/tts_stream_raw.bin");
		fakePort = speechtest::FreePort(AF_INET);
		fakeServer = std::make_unique<XmlRpcServer>(IPAddress::Parse("127.0.0.1").To(fakePort));
		fakeServer->AddHandler("/v1/audio/speech", fake.get());
		fakeThread = std::thread([this] { fakeServer->Start(); });
		const int probe = speechtest::Connect("127.0.0.1", fakePort);
		ASSERT_GE(probe, 0);
		close(probe);

		std::string error;
		auto parsed = SpeechConfig::Parse(R"({"control": {"allow": ["127.0.0.0/8"]},
			"mistral": {"url": "http://127.0.0.1:)" + std::to_string(fakePort) + R"("},
			"voices": [{"alias": "fr-homme", "lang": "fr", "voiceId": "v-1"}])" + extraConfig + "}", error);
		ASSERT_TRUE(parsed) << error;
		config = std::make_unique<SpeechConfigHolder>(parsed);

		backend = {&server, &tasks, "test-key", &usage, &health};
		streaming = std::make_unique<StreamingHandler>(*config, tokens);
		streaming->SetFactory(SpeechFunction::Tts, [this](const SpeechSession::Params& params) {
			return std::make_shared<TtsSession>(params, backend);
		});
		streaming->Start();

		port = speechtest::FreePort(AF_INET6);
		server.AddHandler("/streaming/", streaming.get());
		ASSERT_TRUE(server.Init(IPAddress::Parse("::").To(port)));
	}

	void TearDown() override
	{
		if (!streaming)
			return;
		streaming->CloseAll(SpeechSession::CloseGoingAway, nullptr, 1000);
		tasks.Stop();
		streaming->Stop();
		server.End();
		// An answered request proves ServerRun is running: Stop is safe then.
		speechtest::Http("127.0.0.1", fakePort, "GET", "/");
		fakeServer->Stop();
		fakeThread.join();
	}

	json Open(WsClient& client)
	{
		const std::string token = tokens.Create(SpeechFunction::Tts, 60, "clinique-nord", time(NULL)).value;
		EXPECT_EQ(101, client.Open("::1", port, "/streaming/tts/" + token));
		const std::string text = client.ReadText();
		return text.empty() ? json() : json::parse(text);
	}

	void Type(WsClient& client, const std::string& text)
	{
		client.SendText(json{{"type", "text"}, {"text", text}}.dump());
	}

	// Reads until `count` phrases ended (done or error), or a close.
	static std::vector<Spoken> Listen(WsClient& client, size_t count)
	{
		std::vector<Spoken> spoken;
		size_t ended = 0;
		while (ended < count)
		{
			const WsClient::Frame frame = client.Read();
			if (frame.opcode == WebSocketFrameHeader::BinaryFrame)
			{
				if (!spoken.empty())
					spoken.back().pcm += frame.payload;
				continue;
			}
			if (frame.opcode != WebSocketFrameHeader::TextFrame)
				break;
			const json message = json::parse(frame.payload);
			if (message["type"] == "say")
				spoken.push_back({message["id"], message["text"]});
			else if (message["type"] == "done" && !spoken.empty())
			{
				spoken.back().done = true;
				++ended;
			}
			else if (message["type"] == "error")
			{
				if (spoken.empty())
					spoken.push_back({});
				spoken.back().error = message;
				++ended;
				if (message["fatal"] == true)
					break;
			}
		}
		return spoken;
	}

	TokenStore tokens;
	UsageCounters usage;
	BackendHealth health;
	TaskPool tasks{2};
	SpeechBackend backend;
	std::unique_ptr<SpeechConfigHolder> config;
	std::unique_ptr<StreamingHandler> streaming;
	WebSocketServer server;
	std::unique_ptr<FakeMistralHttp> fake;
	std::unique_ptr<XmlRpcServer> fakeServer;
	std::thread fakeThread;
	int port = 0;
	int fakePort = 0;
};

// 0.6 s of 24 kHz float32 become 0.6 s of 16 kHz S16.
constexpr size_t ThreeDeltasPcmBytes = 3 * FakeMistralHttp::DeltaSamples * 2 / 3 * 2;

}

TEST_F(SpeechTts, ReadyNamesTheVoice)
{
	Start();
	WsClient client;
	const json ready = Open(client);

	EXPECT_EQ("ready", ready["type"]);
	EXPECT_EQ("fr-homme", ready["voice"]);
	EXPECT_EQ(16000, ready["rate"]);
}

TEST_F(SpeechTts, ABackspaceIsAppliedBeforeMistralSeesTheText)
{
	Start();
	WsClient client;
	Open(client);

	Type(client, "abc\bd.");
	client.SendText(R"({"type": "flush"})");
	const std::vector<Spoken> spoken = Listen(client, 1);

	ASSERT_EQ(1u, spoken.size());
	EXPECT_EQ("abd.", spoken[0].text);
	ASSERT_EQ(1u, fake->Requests().size());
	EXPECT_EQ("abd.", fake->Requests()[0].body["input"]);
}

TEST_F(SpeechTts, TwoPhrasesComeInOrderEachBetweenSayAndDone)
{
	Start();
	WsClient client;
	Open(client);

	Type(client, "Bonjour. Au revoir. ");
	const std::vector<Spoken> spoken = Listen(client, 2);

	ASSERT_EQ(2u, spoken.size());
	EXPECT_EQ(1, spoken[0].id);
	EXPECT_EQ("Bonjour.", spoken[0].text);
	EXPECT_EQ(2, spoken[1].id);
	EXPECT_EQ("Au revoir.", spoken[1].text);
	for (const Spoken& phrase : spoken)
	{
		EXPECT_TRUE(phrase.done);
		EXPECT_NEAR((double)ThreeDeltasPcmBytes, (double)phrase.pcm.size(), 64.0);

		// S16 at half the float amplitude.
		const int16_t* s = (const int16_t*)phrase.pcm.data();
		int peak = 0;
		for (size_t i = 0; i < phrase.pcm.size() / 2; ++i)
			peak = std::max(peak, std::abs((int)s[i]));
		EXPECT_NEAR(16384, peak, 600);
	}
	EXPECT_EQ(18u, usage.ByAccount()["clinique-nord"].ttsChars);
	EXPECT_NE(0, health.Get(SpeechFunction::Tts).lastSuccess);
}

TEST_F(SpeechTts, TheRequestCarriesKeyModelVoiceAndFormat)
{
	Start();
	WsClient client;
	Open(client);

	Type(client, "Bonjour. ");
	Listen(client, 1);

	ASSERT_EQ(1u, fake->Requests().size());
	const FakeMistralHttp::Received request = fake->Requests()[0];
	EXPECT_EQ("Bearer test-key", request.authorization);
	EXPECT_EQ("voxtral-mini-tts-2603", request.body["model"]);
	EXPECT_EQ("v-1", request.body["voice"]);
	EXPECT_EQ("pcm", request.body["response_format"]);
	EXPECT_EQ(true, request.body["stream"]);
}

TEST_F(SpeechTts, NoAudioFollowsStopped)
{
	Start();
	WsClient client;
	Open(client);

	Type(client, "SLOW one. Second. ");

	// Wait for audio of the first phrase, then stop.
	WsClient::Frame frame;
	do
		frame = client.Read();
	while (frame.opcode != WebSocketFrameHeader::BinaryFrame && frame.opcode != -1);
	ASSERT_EQ(WebSocketFrameHeader::BinaryFrame, frame.opcode);
	client.SendText(R"({"type": "stop"})");

	json stopped;
	do
	{
		frame = client.Read();
		if (frame.opcode == WebSocketFrameHeader::TextFrame)
			stopped = json::parse(frame.payload);
	}
	while (frame.opcode != -1 && stopped.value("type", "") != "stopped");
	ASSERT_EQ("stopped", stopped.value("type", ""));
	EXPECT_EQ((json{1, 2}), stopped["ids"]);

	// The slow request keeps streaming on Mistral's side for 3 s: none of it
	// may reach the client.
	client.SetReadTimeout(2);
	frame = client.Read();
	EXPECT_EQ(-1, frame.opcode) << "a frame of type " << frame.opcode << " followed stopped";
}

TEST_F(SpeechTts, ARejectedPhraseDoesNotCloseTheSession)
{
	Start();
	WsClient client;
	Open(client);

	Type(client, "REJECT this. Next one. ");
	const std::vector<Spoken> spoken = Listen(client, 2);

	ASSERT_EQ(2u, spoken.size());
	EXPECT_EQ("rejected", spoken[0].error["reason"]);
	EXPECT_EQ(1, spoken[0].error["id"]);
	EXPECT_EQ(false, spoken[0].error["fatal"]);
	EXPECT_TRUE(spoken[1].done);
	EXPECT_EQ(0, health.Get(SpeechFunction::Tts).lastFailure);
}

TEST_F(SpeechTts, AFailingMistralCloses1013)
{
	Start();
	WsClient client;
	Open(client);

	Type(client, "FAIL now. ");
	const std::vector<Spoken> spoken = Listen(client, 1);

	ASSERT_EQ(1u, spoken.size());
	EXPECT_EQ(true, spoken[0].error["fatal"]);
	EXPECT_EQ(SpeechSession::CloseTryLater, client.Read().CloseCode());
	EXPECT_EQ("HTTP 500", health.Get(SpeechFunction::Tts).lastError);
}

TEST_F(SpeechTts, TypingSilenceSendsAPhraseWithoutPunctuation)
{
	Start(R"(, "tts": {"phraseSilenceMs": 200})");
	WsClient client;
	Open(client);

	Type(client, "sans ponctuation");
	const std::vector<Spoken> spoken = Listen(client, 1);

	ASSERT_EQ(1u, spoken.size());
	EXPECT_EQ("sans ponctuation", spoken[0].text);
}

TEST_F(SpeechTts, TheLot0RecordingDecodesWhateverTheCuts)
{
	Start();
	MistralSpeech::Request request = {"http://127.0.0.1:" + std::to_string(fakePort), "test-key", "m", "v", "FIXTURE"};
	size_t floatBytes = 0;
	const MistralSpeech::Result result = MistralSpeech::Synthesize(request, [&](const std::string& audio) {
		floatBytes += audio.size();
		return true;
	});

	EXPECT_EQ("", result.error);
	EXPECT_EQ(200, result.httpStatus);
	// tts.jsonl, lot 0: 499 200 bytes of audio in the stream.
	EXPECT_EQ(499200u, floatBytes);
}

TEST_F(SpeechTts, ASlowRequestDoesNotDelayAnotherSession)
{
	Start();
	WsClient slow, quick;
	Open(slow);
	Open(quick);

	Type(slow, "SLOW phrase. ");
	// The slow phrase is under way: its say came, its 3 s of audio did not.
	ASSERT_EQ("say", json::parse(slow.ReadText())["type"]);

	const auto start = std::chrono::steady_clock::now();
	Type(quick, "Vite. ");
	const std::vector<Spoken> spoken = Listen(quick, 1);
	const auto elapsed = std::chrono::steady_clock::now() - start;

	ASSERT_EQ(1u, spoken.size());
	EXPECT_TRUE(spoken[0].done);
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}
