/**
 * test_speech_comp.cpp — comp (SPEC §7.5, §14.4): the reading of the model's
 * answer, and the check/checked exchange against a local fake of the Mistral
 * chat. No network.
 */
#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include "websocketserver.h"
#include "xmlrpcserver.h"
#include "speech/compcheck.h"
#include "speech/compsession.h"
#include "speech/streaminghandler.h"
#include "fake_mistral_http.h"
#include "speech_test_util.h"

using json = nlohmann::json;
using speechtest::WsClient;

namespace
{

std::string Pairs(const json& pairs)
{
	return json{{"corrections", pairs}}.dump();
}

}

TEST(SpeechCompCheck, AWordIsReplacedAndLocatedInUtf16)
{
	const CompCheck::Result result = CompCheck::Apply("Bonjr je voudrait", Pairs({{{"from", "Bonjr"}, {"to", "Bonjour"}}, {{"from", "voudrait"}, {"to", "voudrais"}}}));

	ASSERT_TRUE(result.valid);
	EXPECT_EQ("Bonjour je voudrais", result.text);
	ASSERT_EQ(2u, result.changes.size());
	EXPECT_EQ(0u, result.changes[0].start);
	EXPECT_EQ(5u, result.changes[0].end);
	EXPECT_EQ(9u, result.changes[1].start);
	EXPECT_EQ(17u, result.changes[1].end);
}

TEST(SpeechCompCheck, PositionsCountUtf16UnitsNotBytes)
{
	// "é" is 2 bytes and 1 unit; "😀" is 4 bytes and 2 units.
	const CompCheck::Result result = CompCheck::Apply("été \xF0\x9F\x98\x80 piza", Pairs({{{"from", "piza"}, {"to", "pizza"}}}));

	ASSERT_EQ(1u, result.changes.size());
	EXPECT_EQ(7u, result.changes[0].start);
	EXPECT_EQ(11u, result.changes[0].end);
}

TEST(SpeechCompCheck, OnlyWholeWordsAreReplaced)
{
	const CompCheck::Result result = CompCheck::Apply("voilà la", Pairs({{{"from", "la"}, {"to", "là"}}}));

	EXPECT_EQ("voilà là", result.text);
	ASSERT_EQ(1u, result.changes.size());
	EXPECT_EQ(6u, result.changes[0].start);
}

TEST(SpeechCompCheck, PairsAreFoundLeftToRight)
{
	const CompCheck::Result result = CompCheck::Apply("la maison la", Pairs({{{"from", "maison"}, {"to", "maisons"}}, {{"from", "la"}, {"to", "là"}}}));

	EXPECT_EQ("la maisons là", result.text);
}

TEST(SpeechCompCheck, TwoWordsMayBecomeOne)
{
	const CompCheck::Result result = CompCheck::Apply("Est ce que", Pairs({{{"from", "Est ce"}, {"to", "Est-ce"}}}));

	EXPECT_EQ("Est-ce que", result.text);
}

TEST(SpeechCompCheck, AnElisionIsACorrection)
{
	const CompCheck::Result result = CompCheck::Apply("Jai recu", Pairs({{{"from", "Jai"}, {"to", "J'ai"}}, {{"from", "recu"}, {"to", "reçu"}}}));

	EXPECT_EQ("J'ai reçu", result.text);
}

TEST(SpeechCompCheck, ARewritingIsDropped)
{
	const CompCheck::Result result = CompCheck::Apply("Pouvez vous", Pairs({
		{{"from", "Pouvez"}, {"to", "Pouvez-vous"}},
		{{"from", "vous"}, {"to", "nous aussi"}}}));

	ASSERT_TRUE(result.valid);
	EXPECT_EQ("Pouvez vous", result.text);
	EXPECT_TRUE(result.changes.empty());
}

TEST(SpeechCompCheck, AWordNotInTheTextIsDropped)
{
	const CompCheck::Result result = CompCheck::Apply("une piza", Pairs({
		{{"from", "voudrait"}, {"to", "voudrais"}},
		{{"from", "piza"}, {"to", "pizza"}},
		{{"from", "une"}, {"to", "une"}}}));

	EXPECT_EQ("une pizza", result.text);
	EXPECT_EQ(1u, result.changes.size());
}

TEST(SpeechCompCheck, AnUnreadableAnswerIsInvalid)
{
	EXPECT_FALSE(CompCheck::Apply("abc", "not json").valid);
	EXPECT_FALSE(CompCheck::Apply("abc", R"({"text": "abc"})").valid);
	EXPECT_EQ("abc", CompCheck::Apply("abc", "not json").text);
	EXPECT_TRUE(CompCheck::Apply("abc", R"({"corrections": [42, {"from": 1}]})").valid);
}

namespace
{

class SpeechComp : public ::testing::Test
{
protected:
	void Start()
	{
		fake = std::make_unique<FakeMistralHttp>(std::string(SPEECH_FIXTURES_DIR) + "/tts_stream_raw.bin");
		fake->chat = [](const std::string& user) {
			// Corrects "piza" and, wrongly, "voudrais" which only the context holds.
			return Pairs({{{"from", "piza"}, {"to", "pizza"}}, {{"from", "voudrais"}, {"to", "voudrait"}}});
		};
		fakePort = speechtest::FreePort(AF_INET);
		fakeServer = std::make_unique<XmlRpcServer>(IPAddress::Parse("127.0.0.1").To(fakePort));
		fakeServer->AddHandler("/v1/chat/completions", fake.get());
		fakeThread = std::thread([this] { fakeServer->Start(); });
		const int probe = speechtest::Connect("127.0.0.1", fakePort);
		ASSERT_GE(probe, 0);
		close(probe);

		std::string error;
		auto parsed = SpeechConfig::Parse(R"({"control": {"allow": ["127.0.0.0/8"]},
			"mistral": {"url": "http://127.0.0.1:)" + std::to_string(fakePort) + R"("}})", error);
		ASSERT_TRUE(parsed) << error;
		config = std::make_unique<SpeechConfigHolder>(parsed);

		backend = {&server, &tasks, "test-key", &usage, &health};
		streaming = std::make_unique<StreamingHandler>(*config, tokens);
		streaming->SetFactory(SpeechFunction::Comp, [this](const SpeechSession::Params& params) {
			return std::make_shared<CompSession>(params, backend);
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
		speechtest::Http("127.0.0.1", fakePort, "GET", "/");
		fakeServer->Stop();
		fakeThread.join();
	}

	void Open(WsClient& client)
	{
		const std::string token = tokens.Create(SpeechFunction::Comp, 60, "clinique-nord", time(NULL)).value;
		ASSERT_EQ(101, client.Open("::1", port, "/streaming/comp/" + token + "?lang=fr"));
		ASSERT_EQ("ready", json::parse(client.ReadText())["type"]);
	}

	static void Check(WsClient& client, const json& id, const std::string& context, const std::string& text)
	{
		client.SendText(json{{"type", "check"}, {"id", id}, {"context", context}, {"text", text}}.dump());
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

}

TEST_F(SpeechComp, AFaultInTheTextComesBackCorrectedAndTheContextIsNeverTouched)
{
	Start();
	WsClient client;
	Open(client);

	Check(client, 7, "Bonjour je voudrais", " une piza svp");
	const json checked = json::parse(client.ReadText());

	EXPECT_EQ("checked", checked["type"]);
	EXPECT_EQ(7, checked["id"]);
	EXPECT_EQ(" une pizza svp", checked["text"]);
	ASSERT_EQ(1u, checked["changes"].size());
	EXPECT_EQ(5, checked["changes"][0]["start"]);
	EXPECT_EQ(9, checked["changes"][0]["end"]);
	EXPECT_EQ(320u, usage.ByAccount()["clinique-nord"].compTokens);
	EXPECT_NE(0, health.Get(SpeechFunction::Comp).lastSuccess);
}

TEST_F(SpeechComp, TheRequestCarriesPromptModelLanguageAndBothTexts)
{
	Start();
	WsClient client;
	Open(client);

	Check(client, "a", "Bonjour", " une piza");
	client.ReadText();

	ASSERT_EQ(1u, fake->Requests().size());
	const FakeMistralHttp::Received request = fake->Requests()[0];
	EXPECT_EQ("Bearer test-key", request.authorization);
	EXPECT_EQ("mistral-small-latest", request.body["model"]);
	EXPECT_EQ("json_object", request.body["response_format"]["type"]);
	EXPECT_EQ(CompSession::Prompt(), request.body["messages"][0]["content"]);
	const json user = json::parse(request.body["messages"][1]["content"].get<std::string>());
	EXPECT_EQ("fr", user["langue"]);
	EXPECT_EQ("Bonjour", user["contexte"]);
	EXPECT_EQ(" une piza", user["texte"]);
}

TEST_F(SpeechComp, AThirdCheckReplacesTheWaitingOne)
{
	Start();
	WsClient client;
	Open(client);

	Check(client, 1, "", "SLOW une piza");
	Check(client, 2, "", "deux piza");
	Check(client, 3, "", "trois piza");

	std::map<int, json> answers;
	for (int i = 0; i < 3; ++i)
	{
		const json answer = json::parse(client.ReadText());
		answers[answer["id"].get<int>()] = answer;
	}
	EXPECT_EQ("checked", answers[1]["type"]);
	EXPECT_EQ("superseded", answers[2]["reason"]);
	EXPECT_EQ(false, answers[2]["fatal"]);
	EXPECT_EQ("trois pizza", answers[3]["text"]);
	EXPECT_EQ(2u, fake->Requests().size());
}

TEST_F(SpeechComp, AFailingMistralIsNotFatal)
{
	Start();
	WsClient client;
	Open(client);

	Check(client, 1, "", "FAIL piza");
	const json error = json::parse(client.ReadText());
	EXPECT_EQ("error", error["type"]);
	EXPECT_EQ(1, error["id"]);
	EXPECT_EQ(false, error["fatal"]);
	EXPECT_EQ("HTTP 500", health.Get(SpeechFunction::Comp).lastError);

	Check(client, 2, "", "une piza");
	EXPECT_EQ("checked", json::parse(client.ReadText())["type"]);
}

TEST_F(SpeechComp, AnUnreadableAnswerIsDroppedAndCounted)
{
	Start();
	fake->chat = [](const std::string&) { return std::string("Voici les corrections : piza -> pizza"); };
	WsClient client;
	Open(client);

	Check(client, 1, "", "une piza");
	const json error = json::parse(client.ReadText());

	EXPECT_EQ("unreadable answer", error["reason"]);
	EXPECT_EQ(1u, health.Get(SpeechFunction::Comp).discarded);
}

TEST_F(SpeechComp, BadChecksAreRefusedWithoutAskingMistral)
{
	Start();
	WsClient client;
	Open(client);

	Check(client, 1, "", std::string(CompSession::MaxChars + 1, 'a'));
	EXPECT_EQ("too long", json::parse(client.ReadText())["reason"]);

	client.SendText(R"({"type": "check", "text": "sans id"})");
	EXPECT_EQ("error", json::parse(client.ReadText())["type"]);

	Check(client, 2, "", "   ");
	const json blank = json::parse(client.ReadText());
	EXPECT_EQ("checked", blank["type"]);
	EXPECT_EQ("   ", blank["text"]);

	EXPECT_TRUE(fake->Requests().empty());
}
