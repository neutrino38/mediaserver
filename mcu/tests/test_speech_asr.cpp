/**
 * test_speech_asr.cpp — asr (SPEC §7.4, §14.2) against a local fake of Voxtral
 * Realtime replaying the lot 0 recording. No network.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include "websocketserver.h"
#include "speech/asrsession.h"
#include "speech/streaminghandler.h"
#include "fake_mistral_realtime.h"
#include "speech_test_util.h"

using json = nlohmann::json;
using speechtest::WsClient;

namespace
{

const std::string Fixture = std::string(SPEECH_FIXTURES_DIR) + "/asr_binary_d240_lNone_s0.jsonl";

// Polls `condition` for up to `ms`.
template <typename Condition>
bool WaitFor(Condition condition, int ms = 3000)
{
	for (int i = 0; i < ms/10; ++i)
	{
		if (condition())
			return true;
		usleep(10*1000);
	}
	return condition();
}

class SpeechAsr : public ::testing::Test
{
protected:
	void Start(const std::string& extraConfig = "", const std::string& apiKey = "test-key")
	{
		fakePort = speechtest::FreePort(AF_INET);
		fake = std::make_unique<FakeMistralRealtime>(Fixture, "test-key");
		ASSERT_FALSE(fake->deltas.empty());
		fakeServer.AddHandler("/v1/audio/transcriptions/realtime", fake.get());
		ASSERT_TRUE(fakeServer.Init(IPAddress::Parse("127.0.0.1").To(fakePort)));
		// Init returns before its thread calls listen().
		const int probe = speechtest::Connect("127.0.0.1", fakePort);
		ASSERT_GE(probe, 0);
		close(probe);

		Configure(extraConfig);
		backend = {&server, &tasks, apiKey, &usage, &health};
		streaming = std::make_unique<StreamingHandler>(*config, tokens);
		streaming->SetFactory(SpeechFunction::Asr, [this](const SpeechSession::Params& params) {
			return std::make_shared<AsrSession>(params, backend);
		});
		streaming->Start();

		port = speechtest::FreePort(AF_INET6);
		server.AddHandler("/streaming/", streaming.get());
		ASSERT_TRUE(server.Init(IPAddress::Parse("::").To(port)));
	}

	void Configure(const std::string& extra, const std::string& url = "")
	{
		std::string error;
		auto parsed = SpeechConfig::Parse(R"({"control": {"allow": ["127.0.0.0/8"]},
			"mistral": {"url": ")" + (url.empty() ? "http://127.0.0.1:" + std::to_string(fakePort) : url) + R"("})"
			+ extra + "}", error);
		ASSERT_TRUE(parsed) << error;
		if (config)
			config->Set(parsed);
		else
			config = std::make_unique<SpeechConfigHolder>(parsed);
	}

	void TearDown() override
	{
		if (!streaming)
			return;
		streaming->CloseAll(SpeechSession::CloseGoingAway, nullptr, 1000);
		tasks.Stop();
		streaming->Stop();
		server.End();
		fakeServer.End();
	}

	// Opens an asr stream and reads its `ready`.
	json Open(WsClient& client, const std::string& query = "")
	{
		const std::string token = tokens.Create(SpeechFunction::Asr, 60, "clinique-nord", time(NULL)).value;
		EXPECT_EQ(101, client.Open("::1", port, "/streaming/asr/" + token + query));
		const std::string text = client.ReadText();
		return text.empty() ? json() : json::parse(text);
	}

	static std::string Frame(char fill)	{ return std::string(640, fill); }	// 20 ms

	TokenStore tokens;
	UsageCounters usage;
	BackendHealth health;
	TaskPool tasks{2};
	SpeechBackend backend;
	std::unique_ptr<SpeechConfigHolder> config;
	std::unique_ptr<StreamingHandler> streaming;
	WebSocketServer server;
	WebSocketServer fakeServer;
	std::unique_ptr<FakeMistralRealtime> fake;
	int port = 0;
	int fakePort = 0;
};

}

TEST_F(SpeechAsr, ReadyComesOnceMistralTookTheDelay)
{
	Start();
	WsClient client;
	const json ready = Open(client, "?delay=480");

	EXPECT_EQ("ready", ready["type"]);
	EXPECT_EQ("fr", ready["lang"]);
	EXPECT_EQ(480, ready["delay"]);
	EXPECT_EQ(480, fake->Get(0).delay);
	EXPECT_EQ("fr", fake->Get(0).language);
}

TEST_F(SpeechAsr, TheLanguageAskedIsForcedOnMistral)
{
	Start();
	WsClient client;
	const json ready = Open(client, "?lang=en");

	EXPECT_EQ("en", ready["lang"]);
	EXPECT_EQ("en", fake->Get(0).language);
}

TEST_F(SpeechAsr, AudioReachesMistralUnchangedAndTextComesBackInOrder)
{
	Start();
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	std::string sent;
	for (char fill = 'a'; fill < 'f'; ++fill)
	{
		client.SendBinary(Frame(fill));
		sent += Frame(fill);
	}

	for (size_t i = 0; i < 5; ++i)
	{
		const json text = json::parse(client.ReadText());
		EXPECT_EQ("text", text["type"]);
		EXPECT_EQ(fake->deltas[i], text["text"]);
	}
	EXPECT_EQ(sent, fake->Get(0).audio);
	EXPECT_EQ(100u, usage.ByAccount()["clinique-nord"].asrAudioMs);
	EXPECT_NE(0, health.Get(SpeechFunction::Asr).lastSuccess);
}

TEST_F(SpeechAsr, PauseStopsTheRelayAndFlushes)
{
	Start();
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	client.SendBinary(Frame('a'));
	ASSERT_EQ("text", json::parse(client.ReadText())["type"]);

	client.SendText(R"({"type": "pause"})");
	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).flushes == 1; }));
	client.SendBinary(Frame('x'));

	client.SendText(R"({"type": "resume"})");
	client.SendBinary(Frame('b'));

	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).frames == 2; }));
	EXPECT_EQ(Frame('a') + Frame('b'), fake->Get(0).audio);
}

// A caller silent from the start: the client pauses before any audio. Mistral
// refuses a flush with nothing to flush, and closes its session (2026-10-20).
TEST_F(SpeechAsr, APauseBeforeAnyAudioDoesNotFlush)
{
	Start();
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	client.SendText(R"({"type": "pause"})");
	client.SendText(R"({"type": "resume"})");
	client.SendBinary(Frame('a'));

	EXPECT_EQ(fake->deltas[0], json::parse(client.ReadText())["text"]);
	EXPECT_EQ(0, fake->Get(0).refusedFlushes);
	EXPECT_EQ(0, fake->Get(0).flushes);
	EXPECT_FALSE(fake->Get(0).closed);

	// A second pause after audio flushes, a third with none since does not.
	client.SendText(R"({"type": "pause"})");
	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).flushes == 1; }));
	client.SendText(R"({"type": "resume"})");
	client.SendText(R"({"type": "pause"})");
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_EQ(0, fake->Get(0).refusedFlushes);
	EXPECT_FALSE(fake->Get(0).closed);
}

TEST_F(SpeechAsr, ALongPauseClosesMistralAndResumeReopensIt)
{
	Start(R"(, "asr": {"pauseCloseSecs": 1})");
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	client.SendText(R"({"type": "pause"})");
	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).closed; }, 3000));

	client.SendText(R"({"type": "resume"})");
	client.SendBinary(Frame('r'));

	ASSERT_TRUE(WaitFor([&] { return fake->Count() == 2 && fake->Get(1).frames == 1; }));
	EXPECT_EQ(Frame('r'), fake->Get(1).audio);
	EXPECT_EQ(fake->deltas[0], json::parse(client.ReadText())["text"]);
}

TEST_F(SpeechAsr, MistralDroppingTheSessionCloses1013)
{
	Start();
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	fake->DropAll(1011);

	const json error = json::parse(client.ReadText());
	EXPECT_EQ("backend closed", error["reason"]);
	EXPECT_EQ(true, error["fatal"]);
	EXPECT_EQ(SpeechSession::CloseTryLater, client.Read().CloseCode());
	EXPECT_NE(0, health.Get(SpeechFunction::Asr).lastFailure);
}

// A rotation: the client pauses at a silence, then ends the old stream. The
// pause already flushed; a second flush with no audio since is refused by
// Mistral, which closes before the end, and the session never ends cleanly.
TEST_F(SpeechAsr, DoneRightAfterAPauseEndsWithoutASecondFlush)
{
	Start();
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	client.SendBinary(Frame('a'));
	ASSERT_EQ(fake->deltas[0], json::parse(client.ReadText())["text"]);

	client.SendText(R"({"type": "pause"})");
	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).flushes == 1; }));
	client.SendText(R"({"type": "done"})");

	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).ends == 1; }));
	EXPECT_EQ(0, fake->Get(0).refusedFlushes);
	EXPECT_EQ(1, fake->Get(0).flushes);
}

TEST_F(SpeechAsr, DoneSendsOnlyTheMissingTextThenCloses1000)
{
	Start();
	fake->doneTail = " fin.";
	WsClient client;
	ASSERT_EQ("ready", Open(client)["type"]);

	client.SendBinary(Frame('a'));
	ASSERT_EQ(fake->deltas[0], json::parse(client.ReadText())["text"]);

	client.SendText(R"({"type": "done"})");

	EXPECT_EQ(" fin.", json::parse(client.ReadText())["text"]);
	EXPECT_EQ(SpeechSession::CloseNormal, client.Read().CloseCode());
	EXPECT_EQ(1, fake->Get(0).flushes);
	EXPECT_EQ(1, fake->Get(0).ends);
}

TEST_F(SpeechAsr, AWrongKeyClosesWithoutReady)
{
	Start("", "wrong-key");
	WsClient client;
	const json error = Open(client);

	EXPECT_EQ("error", error["type"]);
	EXPECT_EQ(true, error["fatal"]);
	EXPECT_EQ(SpeechSession::CloseTryLater, client.Read().CloseCode());
}

TEST_F(SpeechAsr, AnUnreachableMistralClosesWithoutReady)
{
	Start();
	Configure("", "http://127.0.0.1:" + std::to_string(speechtest::FreePort(AF_INET)));
	WsClient client;
	const json error = Open(client);

	EXPECT_EQ("error", error["type"]);
	EXPECT_EQ(SpeechSession::CloseTryLater, client.Read().CloseCode());
}

TEST_F(SpeechAsr, ASilentMistralDoesNotDelayAnotherSession)
{
	Start(R"(, "asr": {"model": "hang"})");
	WsClient stuck;
	const std::string token = tokens.Create(SpeechFunction::Asr, 60, "-", time(NULL)).value;
	ASSERT_EQ(101, stuck.Open("::1", port, "/streaming/asr/" + token));

	Configure("");
	WsClient other;
	ASSERT_EQ("ready", Open(other)["type"]);
	other.SendBinary(Frame('a'));
	EXPECT_EQ(fake->deltas[0], json::parse(other.ReadText())["text"]);
}

TEST_F(SpeechAsr, AMuteMistralTimesOutWith1013)
{
	Start(R"(, "asr": {"model": "mute"})");
	WsClient client;
	const std::string token = tokens.Create(SpeechFunction::Asr, 60, "-", time(NULL)).value;
	ASSERT_EQ(101, client.Open("::1", port, "/streaming/asr/" + token));

	// Past the opening bound, and well within the read timeout of the client.
	client.SetReadTimeout(AsrSession::OpenTimeoutMs/1000 + 3);
	const json error = json::parse(client.ReadText());
	EXPECT_EQ("backend timeout", error["reason"]);
	EXPECT_EQ(SpeechSession::CloseTryLater, client.Read().CloseCode());
}

namespace
{

class CountingListener : public MistralRealtimeClient::Listener
{
public:
	void OnRealtimeReady(MistralRealtimeClient*) override				{ ++ready; }
	void OnRealtimeText(MistralRealtimeClient*, const std::string&) override	{ ++texts; }
	void OnRealtimeDone(MistralRealtimeClient*, const std::string&) override	{}
	void OnRealtimeError(MistralRealtimeClient*, int, const std::string&) override	{}
	void OnRealtimeClosed(MistralRealtimeClient*) override			{}

	std::atomic<int> ready{0};
	std::atomic<int> texts{0};
};

}

TEST_F(SpeechAsr, AnEventForADeadListenerFindsNobody)
{
	Start();
	auto listener = std::make_shared<CountingListener>();
	auto client = std::make_shared<MistralRealtimeClient>(listener, 1000, "fr");
	ASSERT_TRUE(client->Open(server, MistralRealtimeClient::Url("http://127.0.0.1:" + std::to_string(fakePort), "m"), "test-key"));
	ASSERT_TRUE(WaitFor([&] { return listener->ready == 1; }));

	std::weak_ptr<CountingListener> watch = listener;
	listener.reset();
	ASSERT_TRUE(watch.expired());

	// The Mistral connection is still open: these events reach a dead listener.
	fake->SendDeltaToAll("late");
	fake->DropAll(1011);
	ASSERT_TRUE(WaitFor([&] { return fake->Get(0).closed; }));

	WsClient next;
	ASSERT_EQ("ready", Open(next)["type"]);
}
