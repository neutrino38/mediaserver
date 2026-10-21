/**
 * BackendProbe: whether Mistral answers, asked with GET /v1/models, against a
 * local fake whose answer the test chooses.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "xmlrpcserver.h"
#include "speech/backendprobe.h"
#include "speech_test_util.h"

namespace
{

class FakeModels : public Handler
{
public:
	int ProcessRequest(TRequestInfo* req, TSession* const ses) override
	{
		const char* auth = RequestHeaderValue(ses, (char*)"authorization");
		{
			std::lock_guard<std::mutex> lock(mutex);
			authorization = auth ? auth : "";
		}
		++requests;
		if (status != 200)
			return XmlRpcServer::SendError(ses, status);
		static const char body[] = R"({"object":"list","data":[]})";
		ResponseContentType(ses, (char*)"application/json");
		return XmlRpcServer::SendResponse(ses, 200, body, sizeof(body) - 1);
	}

	std::string Authorization()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return authorization;
	}

	std::atomic<int>	status{200};
	std::atomic<int>	requests{0};

private:
	std::mutex	mutex;
	std::string	authorization;
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

class SpeechProbe : public ::testing::Test
{
protected:
	void SetUp() override
	{
		port = speechtest::FreePort(AF_INET);
		server = std::make_unique<XmlRpcServer>(IPAddress::Parse("127.0.0.1").To(port));
		server->AddHandler("/v1/models", &fake);
		thread = std::thread([this] { server->Start(); });
		const int probe = speechtest::Connect("127.0.0.1", port);
		ASSERT_GE(probe, 0);
		close(probe);
	}

	void TearDown() override
	{
		speechtest::Http("127.0.0.1", port, "GET", "/");
		server->Stop();
		thread.join();
	}

	std::string Url() const	{ return "http://127.0.0.1:" + std::to_string(port); }

	int port = 0;
	FakeModels fake;
	std::unique_ptr<XmlRpcServer> server;
	std::thread thread;
};

}

TEST_F(SpeechProbe, AnAnsweringMistralIsUpAndGetsTheKey)
{
	BackendProbe probe(Url(), "the-key", 0);
	const BackendProbe::Result result = probe.ProbeOnce();
	EXPECT_EQ(BackendProbe::State::Up, result.state);
	EXPECT_EQ("", result.reason);
	EXPECT_NE(0, result.checkedAt);
	EXPECT_EQ("Bearer the-key", fake.Authorization());
}

TEST_F(SpeechProbe, ARefusedKeyIsDownAndSaysSo)
{
	fake.status = 401;
	const BackendProbe::Result result = BackendProbe(Url(), "bad", 0).ProbeOnce();
	EXPECT_EQ(BackendProbe::State::Down, result.state);
	EXPECT_EQ("Mistral refuses the API key (HTTP 401)", result.reason);
}

TEST_F(SpeechProbe, ARateLimitAndAServerErrorAreDown)
{
	fake.status = 429;
	EXPECT_EQ("Mistral rate limit reached (HTTP 429)", BackendProbe(Url(), "k", 0).ProbeOnce().reason);
	fake.status = 503;
	const BackendProbe::Result result = BackendProbe(Url(), "k", 0).ProbeOnce();
	EXPECT_EQ(BackendProbe::State::Down, result.state);
	EXPECT_EQ("Mistral answers HTTP 503", result.reason);
}

TEST_F(SpeechProbe, AnUnreachableMistralIsDown)
{
	const int closed = speechtest::FreePort(AF_INET);
	const BackendProbe::Result result = BackendProbe("http://127.0.0.1:" + std::to_string(closed), "k", 0).ProbeOnce();
	EXPECT_EQ(BackendProbe::State::Down, result.state);
	EXPECT_EQ(0u, result.reason.rfind("Mistral unreachable: ", 0)) << result.reason;
}

// Without any session, the probe still sees the outage begin and end.
TEST_F(SpeechProbe, TheThreadFollowsTheOutageWithoutTraffic)
{
	BackendProbe probe(Url(), "k", 1);
	EXPECT_EQ(BackendProbe::State::Unknown, probe.Get().state) << "before the first probe";
	probe.Start();
	ASSERT_TRUE(Eventually([&] { return probe.Get().state == BackendProbe::State::Up; }));

	fake.status = 500;
	ASSERT_TRUE(Eventually([&] { return probe.Get().state == BackendProbe::State::Down; }, 3000));
	EXPECT_EQ("Mistral answers HTTP 500", probe.Get().reason);

	fake.status = 200;
	ASSERT_TRUE(Eventually([&] { return probe.Get().state == BackendProbe::State::Up; }, 3000));
	probe.Stop();
}

TEST_F(SpeechProbe, APeriodOfZeroNeverProbes)
{
	BackendProbe probe(Url(), "k", 0);
	probe.Start();
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_EQ(0, fake.requests.load());
	EXPECT_EQ(BackendProbe::State::Unknown, probe.Get().state);
}
