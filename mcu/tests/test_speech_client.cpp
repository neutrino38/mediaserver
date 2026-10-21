/**
 * test_speech_client.cpp — the speech-server client of the mediaserver
 * (SpeechProcessor SPEC §5, §11.1): SpeechService and SpeechFlow against the
 * real speech-server binary, itself in front of the fake Voxtral Realtime.
 *
 * The delays of the flow (keep-alive, maximum age, retries) and the limits of
 * the server (idleSecs, maxSessionSecs) are scaled down, so that what takes
 * an hour in a call takes seconds here.
 */
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "speech_test_util.h"
#include "fake_mistral_realtime.h"
#include "speech/speechflow.h"
#include "speech/speechservice.h"
#include "websocketserver.h"
#include "xmlrpcserver.h"

using json = nlohmann::json;

namespace
{

const std::string Fixture = std::string(SPEECH_FIXTURES_DIR) + "/asr_binary_d240_lNone_s0.jsonl";

// What a flow told its owner, in order.
class Recorder : public SpeechFlow::Listener
{
public:
	void onReady(SpeechFlow* flow) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		++readies;
	}
	void onText(SpeechFlow* flow, const std::string& text) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		this->text += text;
		++texts;
	}
	void onState(SpeechFlow* flow, bool up, const std::string& reason) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		states.push_back(up ? "up" : "down");
	}
	bool CanRotate(SpeechFlow* flow) override
	{
		return canRotate;
	}

	int Readies()			{ std::lock_guard<std::mutex> lock(mutex); return readies; }
	int Texts()			{ std::lock_guard<std::mutex> lock(mutex); return texts; }
	std::string Text()		{ std::lock_guard<std::mutex> lock(mutex); return text; }
	std::vector<std::string> States()	{ std::lock_guard<std::mutex> lock(mutex); return states; }

	std::atomic<bool>		canRotate{true};

private:
	std::mutex			mutex;
	int				readies = 0;
	int				texts = 0;
	std::string			text;
	std::vector<std::string>	states;
};

bool Eventually(std::function<bool()> done, int ms = 5000)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!done())
	{
		if (std::chrono::steady_clock::now() >= until)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return true;
}

// Answers POST /control/token after a delay: a slow control port.
class SlowTokenHandler : public Handler
{
public:
	int ProcessRequest(TRequestInfo* req, TSession* const ses) override
	{
		std::this_thread::sleep_for(std::chrono::seconds(3));
		const std::string answer = json{{"token", std::string(43, 'A')}}.dump();
		ResponseContentType(ses, (char*)"application/json");
		return XmlRpcServer::SendResponse(ses, 200, answer.c_str(), answer.size());
	}
};

class SpeechClient : public ::testing::Test
{
protected:
	void SetUp() override
	{
		char pattern[] = "/tmp/speech-client-test-XXXXXX";
		ASSERT_TRUE(mkdtemp(pattern));
		dir = pattern;
		const std::string key = dir + "/key";
		std::ofstream(key) << "test-key\n";
		chmod(key.c_str(), 0600);

		controlPort = speechtest::FreePort(AF_INET);
		streamingPort = speechtest::FreePort(AF_INET);

		// The fake Mistral the speech-server child connects to.
		fake = std::make_unique<FakeMistralRealtime>(Fixture, "test-key");
		fakePort = speechtest::FreePort(AF_INET);
		fakeServer.AddHandler("/v1/audio/transcriptions/realtime", fake.get());
		ASSERT_TRUE(fakeServer.Init(IPAddress::Parse("127.0.0.1").To(fakePort)));

		// The mediaserver side: the reactor its streams go through, with an
		// echo to prove the reactor stays free.
		echo = std::make_shared<TextEchoWebsocketHandler>();
		wsPort = speechtest::FreePort(AF_INET);
		server.AddHandler("/echo", echo.get());
		ASSERT_TRUE(server.Init(IPAddress::Parse("127.0.0.1").To(wsPort)));
	}

	void TearDown() override
	{
		if (service)
			service->End();
		StopServer();
		server.End();
		fakeServer.End();
		system(("rm -rf " + dir).c_str());
	}

	void LaunchServer(int idleSecs = 60, int maxSessionSecs = 3600, const std::string& mistralUrl = "", int probeSecs = 0)
	{
		std::ofstream(dir + "/config.json") << json{
			{"control", {{"listen", "127.0.0.1"}, {"port", controlPort}, {"allow", {"127.0.0.0/8"}}}},
			{"streaming", {{"listen", "127.0.0.1"}, {"port", streamingPort}}},
			{"mistral", {{"url", mistralUrl.empty() ? "http://127.0.0.1:" + std::to_string(fakePort) : mistralUrl},
				     {"probeSecs", probeSecs}}},
			{"voices", {{{"alias", "fr-homme"}, {"lang", "fr"}, {"voiceId", "v-1"}}}},
			{"limits", {{"idleSecs", idleSecs}, {"maxSessionSecs", maxSessionSecs}}}}.dump();

		pid = fork();
		if (pid == 0)
		{
			const std::string config = dir + "/config.json";
			const std::string key = dir + "/key";
			const std::string log = dir + "/log.txt";
			freopen(log.c_str(), "a", stdout);
			freopen(log.c_str(), "a", stderr);
			execl(SPEECH_SERVER_BIN, SPEECH_SERVER_BIN, "--config", config.c_str(), "--api-key-file", key.c_str(), (char*)nullptr);
			_exit(127);
		}
		// Up once the control port answers.
		ASSERT_EQ(200, speechtest::Http("127.0.0.1", controlPort, "GET", "/status/general").status);
	}

	void StopServer()
	{
		if (pid <= 0)
			return;
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
		pid = -1;
	}

	json ServerStatus()
	{
		const speechtest::HttpResponse response = speechtest::Http("127.0.0.1", controlPort, "GET", "/status/general");
		return response.status == 200 ? json::parse(response.body) : json();
	}

	void MakeService(const std::string& controlUrl = "", int probeMs = SpeechService::DefaultProbeMs)
	{
		service = std::make_unique<SpeechService>(server,
			controlUrl.empty() ? "http://127.0.0.1:" + std::to_string(controlPort) : controlUrl,
			"ws://127.0.0.1:" + std::to_string(streamingPort), "clinique-nord", 20, probeMs);
		service->Start();
	}

	static SpeechFlow::Params FastParams()
	{
		SpeechFlow::Params params;
		params.keepAliveMs = 500;
		params.readyTimeoutMs = 3000;
		params.retiringTimeoutMs = 2000;
		params.retryMs = {100, 200, 500};
		return params;
	}

	// 20 ms of 16 kHz S16 silence: the fake answers each frame with a delta.
	static void SendFrames(SpeechFlow& flow, int count)
	{
		const std::string frame(640, '\0');
		for (int i = 0; i < count; ++i)
		{
			flow.SendAudio((const BYTE*)frame.data(), frame.size());
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	std::string Deltas(size_t count)
	{
		std::string text;
		for (size_t i = 0; i < count && i < fake->deltas.size(); ++i)
			text += fake->deltas[i];
		return text;
	}

	std::string dir;
	int controlPort = 0;
	int streamingPort = 0;
	int fakePort = 0;
	int wsPort = 0;
	pid_t pid = -1;

	std::unique_ptr<FakeMistralRealtime> fake;
	WebSocketServer fakeServer;
	WebSocketServer server;
	std::shared_ptr<TextEchoWebsocketHandler> echo;
	std::unique_ptr<SpeechService> service;
};

}

TEST(SpeechServiceConfig, BothUrlsOrNone)
{
	std::string error;
	EXPECT_TRUE(SpeechService::CheckConfig("", "", "", error));
	EXPECT_TRUE(SpeechService::CheckConfig("http://speech:9200", "wss://speech.example.net:9201", "clinique-nord", error)) << error;

	EXPECT_FALSE(SpeechService::CheckConfig("http://speech:9200", "", "", error));
	EXPECT_FALSE(SpeechService::CheckConfig("", "ws://speech:9201", "", error));
	EXPECT_FALSE(SpeechService::CheckConfig("", "", "clinique-nord", error)) << "an account alone";
	EXPECT_FALSE(SpeechService::CheckConfig("ws://speech:9200", "ws://speech:9201", "", error)) << "control is HTTP";
	EXPECT_FALSE(SpeechService::CheckConfig("http://speech:9200", "http://speech:9201", "", error)) << "streaming is WebSocket";
	EXPECT_FALSE(SpeechService::CheckConfig("http://speech:9200", "ws://speech:9201", "clinique nord", error));
	EXPECT_FALSE(SpeechService::CheckConfig("http://speech:9200", "ws://speech:9201", std::string(65, 'a'), error));
}

TEST(SpeechServiceConfig, WithoutUrlsNoFlowIsMade)
{
	WebSocketServer server;
	SpeechService service(server, "", "", "");
	service.Start();
	EXPECT_FALSE(service.IsConfigured());
	EXPECT_EQ(nullptr, service.CreateFlow(SpeechFunction::Asr, std::weak_ptr<SpeechFlow::Listener>()));
	service.End();
}

// §11.1: a token before each opening, a new one for each reopening.
TEST_F(SpeechClient, EachOpeningAsksANewToken)
{
	LaunchServer();
	MakeService();
	auto recorder = std::make_shared<Recorder>();
	auto flow = service->CreateFlow(SpeechFunction::Asr, recorder, FastParams());
	flow->Start();

	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 1; }));
	EXPECT_EQ(SpeechFlow::State::Ready, flow->GetState());
	EXPECT_EQ(1, ServerStatus()["tokens"]["issued"]);
	EXPECT_TRUE(ServerStatus()["usage"]["byAccount"].contains("clinique-nord"));

	// Mistral drops: speech-server closes the stream with 1013.
	fake->DropAll(1011);
	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 2; }));

	EXPECT_EQ(2, ServerStatus()["tokens"]["issued"]);
	EXPECT_EQ(2, ServerStatus()["tokens"]["consumed"]);
	EXPECT_EQ((std::vector<std::string>{"up", "down", "up"}), recorder->States());
	EXPECT_EQ(1u, service->GetCounters().reopenings);
	flow->Stop();
}

// §11.1: a stopped speech-server makes the flow down once, the retries back
// off, and the restart brings it up once.
TEST_F(SpeechClient, ServerDownThenBackIsOneEventEach)
{
	LaunchServer();
	MakeService();
	auto recorder = std::make_shared<Recorder>();
	auto flow = service->CreateFlow(SpeechFunction::Tts, recorder, FastParams());
	flow->Start();
	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 1; }));
	EXPECT_EQ(1, service->GetCounters().open[(int)SpeechFunction::Tts]);

	StopServer();
	ASSERT_TRUE(Eventually([&] { return recorder->States().size() == 2; }));
	const uint64_t failuresAtDown = service->GetCounters().failures;

	// Retries at 100, 200, 500, 500 ms…: about four in 1.5 s, never a burst.
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	const uint64_t retries = service->GetCounters().failures - failuresAtDown;
	EXPECT_GE(retries, 2u);
	EXPECT_LE(retries, 6u);
	EXPECT_EQ(SpeechFlow::State::Down, flow->GetState());
	EXPECT_EQ(0, service->GetCounters().open[(int)SpeechFunction::Tts]);

	LaunchServer();
	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 2; }));
	EXPECT_EQ((std::vector<std::string>{"up", "down", "up"}), recorder->States());
	EXPECT_FALSE(service->GetCounters().lastFailureReason.empty());
	flow->Stop();
}

// The availability a controller reads before offering speech follows the
// server, with no flow open: down when it stops, back when it restarts.
TEST_F(SpeechClient, AvailabilityFollowsTheServerWithoutAnyFlow)
{
	LaunchServer();
	MakeService("", 200);
	ASSERT_TRUE(Eventually([&] { return service->GetAvailability().available; }));
	EXPECT_EQ("", service->GetAvailability().reason);

	StopServer();
	ASSERT_TRUE(Eventually([&] { return !service->GetAvailability().available; }));
	const std::string reason = service->GetAvailability().reason;
	EXPECT_EQ(0u, reason.rfind("speech-server unreachable at http://127.0.0.1:" + std::to_string(controlPort) + ": ", 0)) << reason;

	LaunchServer();
	ASSERT_TRUE(Eventually([&] { return service->GetAvailability().available; }));
}

// speech-server answers but Mistral does not: speech is unavailable, and the
// reason is speech-server's own.
TEST_F(SpeechClient, AMistralOutageMakesSpeechUnavailable)
{
	LaunchServer(60, 3600, "http://127.0.0.1:" + std::to_string(speechtest::FreePort(AF_INET)), 1);
	MakeService("", 200);
	ASSERT_TRUE(Eventually([&] { return !service->GetAvailability().reason.empty()
		&& service->GetAvailability().reason.rfind("Mistral", 0) == 0; }));
	EXPECT_FALSE(service->GetAvailability().available);
	EXPECT_EQ(0u, service->GetAvailability().reason.rfind("Mistral unreachable: ", 0)) << service->GetAvailability().reason;
	EXPECT_EQ("down", ServerStatus()["backend"]["mistral"]["state"]);
}

// §11.1: a silent tts and a paused asr outlive the server idle limit.
TEST_F(SpeechClient, KeepAliveHoldsSilentFlowsOpen)
{
	LaunchServer(2);
	MakeService();
	auto ttsRecorder = std::make_shared<Recorder>();
	auto asrRecorder = std::make_shared<Recorder>();
	auto tts = service->CreateFlow(SpeechFunction::Tts, ttsRecorder, FastParams());
	auto asr = service->CreateFlow(SpeechFunction::Asr, asrRecorder, FastParams());
	tts->Start();
	asr->Start();
	ASSERT_TRUE(Eventually([&] { return ttsRecorder->Readies() == 1; }));
	ASSERT_TRUE(Eventually([&] { return asrRecorder->Readies() == 1; }));
	asr->Pause();

	// Three times the idle limit.
	std::this_thread::sleep_for(std::chrono::seconds(6));

	EXPECT_EQ((std::vector<std::string>{"up"}), ttsRecorder->States());
	EXPECT_EQ((std::vector<std::string>{"up"}), asrRecorder->States());
	EXPECT_EQ(1, ServerStatus()["sessions"]["tts"]["open"]);
	EXPECT_EQ(1, ServerStatus()["sessions"]["asr"]["open"]);
	tts->Stop();
	asr->Stop();
}

// The control case: without the keep-alive the server does close.
TEST_F(SpeechClient, WithoutKeepAliveTheServerCloses)
{
	LaunchServer(1);
	MakeService();
	auto recorder = std::make_shared<Recorder>();
	SpeechFlow::Params params = FastParams();
	params.keepAliveMs = 60000;
	auto tts = service->CreateFlow(SpeechFunction::Tts, recorder, params);
	tts->Start();
	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 1; }));
	EXPECT_TRUE(Eventually([&] { return recorder->States().size() >= 2; }, 4000));
	tts->Stop();
}

// §11.1: an old stream is replaced at the first silence, and the text
// neither loses nor repeats anything.
TEST_F(SpeechClient, RotationWaitsForSilenceAndKeepsTheText)
{
	LaunchServer();
	MakeService();
	auto recorder = std::make_shared<Recorder>();
	recorder->canRotate = false;
	SpeechFlow::Params params = FastParams();
	params.maxAgeMs = 1000;
	auto flow = service->CreateFlow(SpeechFunction::Asr, recorder, params);
	flow->Start();
	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 1; }));

	SendFrames(*flow, 5);
	ASSERT_TRUE(Eventually([&] { return recorder->Texts() == 5; }));

	// Old, but speaking: no rotation.
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	EXPECT_EQ(1, recorder->Readies());

	flow->Pause();
	recorder->canRotate = true;
	ASSERT_TRUE(Eventually([&] { return recorder->Readies() == 2; }));
	EXPECT_EQ(1u, service->GetCounters().rotations);

	flow->Resume();
	SendFrames(*flow, 3);
	ASSERT_TRUE(Eventually([&] { return recorder->Texts() == 8; }));

	// The old stream ended by `done`: speech-server flushed then ended Mistral.
	EXPECT_TRUE(Eventually([&] { return fake->Get(0).ends == 1; }, 3000));
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_EQ(Deltas(5) + Deltas(3), recorder->Text());
	EXPECT_EQ((std::vector<std::string>{"up"}), recorder->States());
	EXPECT_EQ(2, ServerStatus()["tokens"]["issued"]);
	flow->Stop();
}

// §11.1: a token that takes 3 s blocks neither the reactor nor a media clock.
TEST_F(SpeechClient, ASlowTokenBlocksNeitherReactorNorClock)
{
	SlowTokenHandler slow;
	const int slowPort = speechtest::FreePort(AF_INET);
	XmlRpcServer slowServer(IPAddress::Parse("127.0.0.1").To(slowPort));
	slowServer.AddHandler("/control/token", &slow);
	std::thread slowThread([&] { slowServer.Start(); });
	const int probe = speechtest::Connect("127.0.0.1", slowPort);
	ASSERT_GE(probe, 0);
	close(probe);

	MakeService("http://127.0.0.1:" + std::to_string(slowPort));
	auto recorder = std::make_shared<Recorder>();
	auto flow = service->CreateFlow(SpeechFunction::Tts, recorder, FastParams());

	// A 20 ms clock that starts the flow on its first beat.
	std::atomic<bool> running{true};
	std::atomic<long> startUs{0};
	std::atomic<long> maxGapMs{0};
	std::thread clock([&] {
		auto last = std::chrono::steady_clock::now();
		bool started = false;
		while (running)
		{
			if (!started)
			{
				const auto before = std::chrono::steady_clock::now();
				flow->Start();
				startUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before).count();
				started = true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			const auto now = std::chrono::steady_clock::now();
			maxGapMs = std::max<long>(maxGapMs, std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
			last = now;
		}
	});

	// Meanwhile the reactor echoes at once.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	speechtest::WsClient client;
	ASSERT_EQ(101, client.Open("127.0.0.1", wsPort, "/echo"));
	for (int i = 0; i < 5; ++i)
	{
		const auto before = std::chrono::steady_clock::now();
		client.SendText("ping");
		EXPECT_EQ("ping", client.ReadText());
		EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(200));
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
	EXPECT_EQ(SpeechFlow::State::Token, flow->GetState()) << "the token is still awaited";

	std::this_thread::sleep_for(std::chrono::milliseconds(1800));
	running = false;
	clock.join();

	EXPECT_LT(startUs.load(), 5000) << "Start() waited";
	EXPECT_LT(maxGapMs.load(), 80) << "the clock missed beats";

	flow->Stop();
	service->End();
	speechtest::Http("127.0.0.1", slowPort, "GET", "/");
	slowServer.Stop();
	slowThread.join();
}
