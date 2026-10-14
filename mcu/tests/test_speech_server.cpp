/**
 * test_speech_server.cpp — the speech-server binary itself (SPEC §8, §14.1):
 * what stops it from starting, SIGHUP, SIGTERM. `make check` builds the binary
 * first and passes its path in SPEECH_SERVER_BIN.
 */
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "speech_test_util.h"

using json = nlohmann::json;

namespace
{

class SpeechServerBinary : public ::testing::Test
{
protected:
	void SetUp() override
	{
		char pattern[] = "/tmp/speech-server-test-XXXXXX";
		ASSERT_TRUE(mkdtemp(pattern));
		dir = pattern;
		controlPort = speechtest::FreePort(AF_INET);
		streamingPort = speechtest::FreePort(AF_INET6);
		WriteKey("test-key\n", 0600);
	}

	void TearDown() override
	{
		if (pid > 0)
		{
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
		}
		system(("rm -rf " + dir).c_str());
	}

	std::string Config(const std::string& voices = "[]", const std::string& allow = R"(["127.0.0.0/8"])")
	{
		return R"({"control": {"listen": "127.0.0.1", "port": )" + std::to_string(controlPort)
			+ R"(, "allow": )" + allow + R"(}, "streaming": {"port": )" + std::to_string(streamingPort)
			+ R"(}, "voices": )" + voices + "}";
	}

	void WriteConfig(const std::string& text)
	{
		std::ofstream(dir + "/config.json") << text;
	}

	void WriteKey(const std::string& text, mode_t mode)
	{
		const std::string path = dir + "/key";
		std::ofstream(path) << text;
		chmod(path.c_str(), mode);
	}

	// Exit status of a run that must stop by itself.
	int RunToExit(const std::string& extra = "")
	{
		const std::string command = std::string(SPEECH_SERVER_BIN) + " --config " + dir + "/config.json --api-key-file "
			+ dir + "/key " + extra + " > " + dir + "/out.txt 2>&1";
		const int status = system(command.c_str());
		return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	}

	void Launch()
	{
		pid = fork();
		if (pid == 0)
		{
			const std::string config = dir + "/config.json";
			const std::string key = dir + "/key";
			const std::string log = dir + "/log.txt";
			freopen(log.c_str(), "w", stdout);
			freopen(log.c_str(), "a", stderr);
			execl(SPEECH_SERVER_BIN, SPEECH_SERVER_BIN, "--config", config.c_str(), "--api-key-file", key.c_str(), (char*)nullptr);
			_exit(127);
		}
	}

	int Stop()
	{
		kill(pid, SIGTERM);
		int status = 0;
		waitpid(pid, &status, 0);
		pid = -1;
		return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	}

	json Voices()
	{
		const speechtest::HttpResponse response = speechtest::Http("127.0.0.1", controlPort, "GET", "/control/voices");
		return response.status == 200 ? json::parse(response.body)["voices"] : json();
	}

	std::string dir;
	int controlPort = 0;
	int streamingPort = 0;
	pid_t pid = -1;
};

}

TEST_F(SpeechServerBinary, StartsAnswersAndStopsOnSigterm)
{
	WriteConfig(Config());
	Launch();

	const speechtest::HttpResponse status = speechtest::Http("127.0.0.1", controlPort, "GET", "/status/general");
	ASSERT_EQ(200, status.status);
	EXPECT_EQ("present", json::parse(status.body)["backend"]["apiKey"]);

	speechtest::WsClient client;
	EXPECT_EQ(403, client.Open("::1", streamingPort, "/streaming/asr/" + std::string(43, 'A')));

	EXPECT_EQ(0, Stop());
}

TEST_F(SpeechServerBinary, CheckValidatesWithoutListening)
{
	WriteConfig(Config());
	EXPECT_EQ(0, RunToExit("--check"));
}

TEST_F(SpeechServerBinary, RefusesToStartWithoutAValidSetup)
{
	EXPECT_EQ(1, RunToExit()) << "missing configuration file";

	WriteConfig(Config("[]", "[]"));
	EXPECT_EQ(1, RunToExit()) << "empty allow list";

	WriteConfig(R"({"control": {"allow": ["127.0.0.0/8"]}, "typo": 1})");
	EXPECT_EQ(1, RunToExit()) << "unknown key";

	WriteConfig(Config());
	WriteKey("test-key\n", 0644);
	EXPECT_EQ(1, RunToExit()) << "key readable by others";

	WriteKey("\n", 0600);
	EXPECT_EQ(1, RunToExit()) << "empty key";

	unlink((dir + "/key").c_str());
	EXPECT_EQ(1, RunToExit()) << "missing key";
}

TEST_F(SpeechServerBinary, SighupAppliesAValidFileAndIgnoresAnInvalidOne)
{
	WriteConfig(Config());
	Launch();
	ASSERT_EQ(0u, Voices().size());

	WriteConfig(Config(R"([{"alias": "fr-homme", "lang": "fr", "voiceId": "v-1"}])"));
	kill(pid, SIGHUP);
	json voices;
	for (int i = 0; i < 50 && voices.size() != 1; ++i, usleep(20*1000))
		voices = Voices();
	ASSERT_EQ(1u, voices.size());

	WriteConfig("{ broken");
	kill(pid, SIGHUP);
	usleep(200*1000);
	EXPECT_EQ(1u, Voices().size());

	EXPECT_EQ(0, Stop());
}
