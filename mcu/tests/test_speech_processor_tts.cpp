/**
 * test_speech_processor_tts.cpp — la synthèse du SpeechProcessor
 * (SPEC SPEECH-PROCESSOR §7, §11.4), de bout en bout : le vrai speech-server
 * devant le faux Voxtral TTS (fake_mistral_http.h). Chaque phrase y donne
 * 0,6 s d'un sinus de 440 Hz ; « SLOW » en donne 2 s, livrées lentement ;
 * « REJECT » est refusée par la modération.
 *
 * L'audio qui sort des ports est décodé paquet par paquet, avec son heure
 * d'arrivée et son énergie.
 *
 *     ./tests/runtests --gtest_filter='SpeechProcessorTts*'
 */
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#include "speech_test_util.h"
#include "fake_mistral_http.h"
#include "audio.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "speech/speechservice.h"
#include "websocketserver.h"
#include "xmlrpcserver.h"
#include "../src/jsr309/RTPMultiplexer.h"
#include "../src/jsr309/SpeechProcessor.h"

using json = nlohmann::json;

namespace
{

const std::string BS = "\x08";
const std::string LS = "\xE2\x80\xA8";
// Au-dessus du bruit de confort, bien en dessous du sinus de synthèse
const double SpeechRms = 1000;

class FakeSource : public RTPMultiplexer
{
};

// L'audio d'un port : un échantillon par paquet, son heure et son énergie.
class AudioCollector : public Joinable::Listener
{
public:
	struct Packet
	{
		std::chrono::steady_clock::time_point	at;
		double					rms;
	};

	void onRTPPacket(RTPPacket& packet) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!decoder)
			decoder.reset(AudioCodecFactory::CreateDecoder((AudioCodec::Type)packet.GetCodec()));
		decoder->Decode(packet.GetMediaData(), packet.GetMediaLength());
		for (SamplesPtr frame = decoder->GetFrame(); frame; frame = decoder->GetFrame())
		{
			double sum = 0;
			for (DWORD i = 0; i < frame->GetNbSamples(); ++i)
				sum += (double)frame->GetData()[i] * frame->GetData()[i];
			pcm.insert(pcm.end(), frame->GetData(), frame->GetData() + frame->GetNbSamples());
			packets.push_back({std::chrono::steady_clock::now(), std::sqrt(sum / frame->GetNbSamples())});
		}
	}
	void onResetStream() override {}
	void onEndStream() override {}

	int Loud()
	{
		std::lock_guard<std::mutex> lock(mutex);
		int loud = 0;
		for (const Packet& p : packets)
			if (p.rms > SpeechRms)
				++loud;
		return loud;
	}

	int LoudAfter(std::chrono::steady_clock::time_point from)
	{
		std::lock_guard<std::mutex> lock(mutex);
		int loud = 0;
		for (const Packet& p : packets)
			if (p.at > from && p.rms > SpeechRms)
				++loud;
		return loud;
	}

	std::vector<SWORD> Pcm()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return pcm;
	}

private:
	std::mutex			mutex;
	std::unique_ptr<AudioDecoder>	decoder;
	std::vector<Packet>		packets;
	std::vector<SWORD>		pcm;
};

struct Event
{
	int				type = 0;
	int				port = -1;
	std::vector<std::string>	tail;
};

class Events
{
public:
	void Post(JSR309Event* event)
	{
		Event parsed;
		xmlrpc_env env;
		xmlrpc_env_init(&env);
		xmlrpc_value* value = event->GetXmlValue(&env);
		delete event;
		for (int i = 0; i < xmlrpc_array_size(&env, value); ++i)
		{
			xmlrpc_value* item = NULL;
			xmlrpc_array_read_item(&env, value, i, &item);
			if (xmlrpc_value_type(item) == XMLRPC_TYPE_INT)
			{
				int n = 0;
				xmlrpc_read_int(&env, item, &n);
				if (i == 0) parsed.type = n;
				else if (i == 4) parsed.port = n;
				else if (i > 4) parsed.tail.push_back(std::to_string(n));
			}
			else if (xmlrpc_value_type(item) == XMLRPC_TYPE_BOOL)
			{
				xmlrpc_bool b = 0;
				xmlrpc_read_bool(&env, item, &b);
				parsed.tail.push_back(b ? "true" : "false");
			}
			else if (xmlrpc_value_type(item) == XMLRPC_TYPE_STRING && i > 4)
			{
				const char* s = NULL;
				xmlrpc_read_string(&env, item, &s);
				parsed.tail.push_back(s ? s : "");
				free((void*)s);
			}
			xmlrpc_DECREF(item);
		}
		xmlrpc_DECREF(value);
		xmlrpc_env_clean(&env);
		std::lock_guard<std::mutex> lock(mutex);
		events.push_back(parsed);
	}

	std::vector<Event> Of(int type)
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<Event> out;
		for (const Event& e : events)
			if (e.type == type)
				out.push_back(e);
		return out;
	}

private:
	std::mutex		mutex;
	std::vector<Event>	events;
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

class SpeechProcessorTts : public ::testing::Test
{
protected:
	void SetUp() override
	{
		char pattern[] = "/tmp/speech-sp-tts-XXXXXX";
		ASSERT_TRUE(mkdtemp(pattern));
		dir = pattern;
		const std::string key = dir + "/key";
		std::ofstream(key) << "test-key\n";
		chmod(key.c_str(), 0600);

		fake = std::make_unique<FakeMistralHttp>(std::string(SPEECH_FIXTURES_DIR) + "/tts_stream_raw.bin");
		fakePort = speechtest::FreePort(AF_INET);
		fakeServer = std::make_unique<XmlRpcServer>(IPAddress::Parse("127.0.0.1").To(fakePort));
		fakeServer->AddHandler("/v1/audio/speech", fake.get());
		fakeThread = std::thread([this] { fakeServer->Start(); });
		const int probe = speechtest::Connect("127.0.0.1", fakePort);
		ASSERT_GE(probe, 0);
		close(probe);

		controlPort = speechtest::FreePort(AF_INET);
		streamingPort = speechtest::FreePort(AF_INET);
		LaunchServer();

		ASSERT_TRUE(server.Init(IPAddress::Parse("127.0.0.1").To(speechtest::FreePort(AF_INET))));
		service = std::make_unique<SpeechService>(server, "http://127.0.0.1:" + std::to_string(controlPort),
			"ws://127.0.0.1:" + std::to_string(streamingPort), "", 20);
		service->Start();

		sp = std::make_shared<SpeechProcessor>(L"tts");
		sp->SetSpeechService(service.get());
		sp->SetEventPoster([this](JSR309Event* event) { events.Post(event); });
		ASSERT_TRUE(sp->Init(true));
		for (int port = 0; port < SpeechProcessor::Ports; ++port)
		{
			ASSERT_TRUE(sp->SetPortCodec(port, AudioCodec::PCMU));
			ASSERT_TRUE(sp->SetComfortNoise(port, 0));
			sp->GetJoinable(port, MediaFrame::Audio)->AddListener(&audio[port]);
		}
	}

	void TearDown() override
	{
		if (sp)
		{
			for (int port = 0; port < SpeechProcessor::Ports; ++port)
				sp->GetJoinable(port, MediaFrame::Audio)->RemoveListener(&audio[port]);
			sp->End();
		}
		if (service)
			service->End();
		if (pid > 0)
		{
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
		}
		server.End();
		if (fakeServer)
		{
			speechtest::Http("127.0.0.1", fakePort, "GET", "/");
			fakeServer->Stop();
			fakeThread.join();
		}
		system(("rm -rf " + dir).c_str());
	}

	void LaunchServer()
	{
		std::ofstream(dir + "/config.json") << json{
			{"control", {{"listen", "127.0.0.1"}, {"port", controlPort}, {"allow", {"127.0.0.0/8"}}}},
			{"streaming", {{"listen", "127.0.0.1"}, {"port", streamingPort}}},
			{"mistral", {{"url", "http://127.0.0.1:" + std::to_string(fakePort)}}},
			{"voices", {{{"alias", "fr-homme"}, {"lang", "fr"}, {"voiceId", "v-1"}}}}}.dump();

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
		ASSERT_EQ(200, speechtest::Http("127.0.0.1", controlPort, "GET", "/status/general").status);
	}

	// Démarre la synthèse vers `port` et attend que le flux soit prêt.
	void StartSynthesis(int port, int textSourcePort, bool announce = true)
	{
		const size_t before = events.Of(JSR309Event::SpeechServiceEvent).size();
		ASSERT_TRUE(sp->StartSynthesis(port, textSourcePort, "fr-homme", announce));
		ASSERT_TRUE(Eventually([&] { return events.Of(JSR309Event::SpeechServiceEvent).size() > before; }));
		ASSERT_EQ((std::vector<std::string>{"tts", "up", ""}), events.Of(JSR309Event::SpeechServiceEvent).back().tail);
	}

	std::vector<std::string> Inputs()
	{
		std::vector<std::string> inputs;
		for (const FakeMistralHttp::Received& request : fake->Requests())
			inputs.push_back(request.body.value("input", ""));
		return inputs;
	}

	void Type(int port, const std::string& text)
	{
		if (!textSources[port])
		{
			textSources[port] = std::make_shared<FakeSource>();
			ASSERT_TRUE(sp->Attach(port, MediaFrame::Text, textSources[port]));
		}
		RTPPacket packet(MediaFrame::Text, TextCodec::T140);
		packet.SetPayload((BYTE*)text.data(), text.size());
		textSources[port]->Multiplex(packet);
	}

	std::string dir;
	int controlPort = 0;
	int streamingPort = 0;
	int fakePort = 0;
	pid_t pid = -1;

	std::unique_ptr<FakeMistralHttp> fake;
	std::unique_ptr<XmlRpcServer> fakeServer;
	std::thread fakeThread;
	WebSocketServer server;
	std::unique_ptr<SpeechService> service;
	std::shared_ptr<SpeechProcessor> sp;
	AudioCollector audio[SpeechProcessor::Ports];
	std::shared_ptr<FakeSource> textSources[SpeechProcessor::Ports];
	Events events;
};

}

// §11.4 : Say("A. B.") donne un Started, puis un Done après la seconde phrase,
// et le PCM sort à vitesse réelle.
TEST_F(SpeechProcessorTts, UnSayDeDeuxPhrasesDonneUnStartedEtUnDone)
{
	StartSynthesis(1, -1);
	const int sayId = sp->Say(1, "Un. Deux.");
	ASSERT_GT(sayId, 0);

	ASSERT_TRUE(Eventually([&] { return !events.Of(JSR309Event::SpeechSynthesisDoneEvent).empty(); }));
	EXPECT_EQ((std::vector<std::string>{"Un.", "Deux."}), Inputs());

	const std::vector<Event> started = events.Of(JSR309Event::SpeechSynthesisStartedEvent);
	ASSERT_EQ(1u, started.size());
	EXPECT_EQ(std::to_string(sayId), started[0].tail[0]);
	EXPECT_EQ(1, started[0].port);
	const std::vector<Event> ended = events.Of(JSR309Event::SpeechSynthesisDoneEvent);
	ASSERT_EQ(1u, ended.size());
	EXPECT_EQ((std::vector<std::string>{std::to_string(sayId), "false"}), ended[0].tail);

	// 2 × 0,6 s de synthèse : une soixantaine de paquets de 20 ms, au rythme
	// du tour, pas en rafale
	ASSERT_TRUE(Eventually([&] { return audio[1].Loud() >= 55; }, 3000));
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_LE(audio[1].Loud(), 62);
	EXPECT_EQ(0, audio[0].Loud()) << "rien vers l'autre port";
}

// §11.4 : StopSpeaking au milieu, plus aucun échantillon de synthèse dans les
// 20 ms qui suivent, et Done avec interrupted=true.
TEST_F(SpeechProcessorTts, StopSpeakingCoupeTout)
{
	StartSynthesis(1, -1);
	const int sayId = sp->Say(1, "SLOW une longue phrase");
	ASSERT_TRUE(Eventually([&] { return audio[1].Loud() >= 10; }));

	ASSERT_TRUE(sp->StopSpeaking(1));
	const auto stopped = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));

	// Un tour en vol au plus, plus le paquet que l'encodeur rend en retard
	EXPECT_EQ(0, audio[1].LoudAfter(stopped + std::chrono::milliseconds(60)));
	const std::vector<Event> ended = events.Of(JSR309Event::SpeechSynthesisDoneEvent);
	ASSERT_EQ(1u, ended.size());
	EXPECT_EQ((std::vector<std::string>{std::to_string(sayId), "true"}), ended[0].tail);

	// Le flux sert encore
	const int next = sp->Say(1, "Encore.");
	ASSERT_TRUE(Eventually([&] { return events.Of(JSR309Event::SpeechSynthesisDoneEvent).size() == 2; }));
	EXPECT_EQ((std::vector<std::string>{std::to_string(next), "false"}), events.Of(JSR309Event::SpeechSynthesisDoneEvent)[1].tail);
}

// §11.4 : le texte tapé sur A est dit sur B, et seulement sur B.
TEST_F(SpeechProcessorTts, LaFrappeDeAEstDiteSurB)
{
	StartSynthesis(1, 0);
	Type(0, "Bonjour. ");

	ASSERT_TRUE(Eventually([&] { return !events.Of(JSR309Event::SpeechSynthesisDoneEvent).empty(); }));
	EXPECT_EQ((std::vector<std::string>{"Bonjour."}), Inputs());
	EXPECT_EQ("0", events.Of(JSR309Event::SpeechSynthesisStartedEvent).at(0).tail[0]) << "sayId 0 : la frappe";
	ASSERT_TRUE(Eventually([&] { return audio[1].Loud() >= 25; }, 2000));
	EXPECT_EQ(0, audio[0].Loud());

	// Une phrase sans ponctuation part sur le silence de frappe
	Type(0, "a demain");
	ASSERT_TRUE(Eventually([&] { return Inputs().size() == 2; }, 3000));
	EXPECT_EQ("a demain", Inputs()[1]);
}

// §11.4 : un retour arrière dans du texte déjà dit envoie stop, puis
// « Correction : » suivi de la ligne retapée.
TEST_F(SpeechProcessorTts, UneCorrectionEstAnnonceeEtRedite)
{
	StartSynthesis(1, 0);
	Type(0, "deux pizas. ");
	ASSERT_TRUE(Eventually([&] { return Inputs().size() == 1; }));

	Type(0, BS + BS + BS + BS + BS + BS + BS);
	Type(0, "pizzas pour ce soir." + LS);
	ASSERT_TRUE(Eventually([&] { return Inputs().size() == 2; }));
	EXPECT_EQ("Correction : deux pizzas pour ce soir.", Inputs()[1]);
}

// §11.4 : la voix relayée de A et la synthèse sortent ensemble sur B, sans
// que l'addition déborde.
TEST_F(SpeechProcessorTts, LaSyntheseSeMeleAuRelaisSansDebordement)
{
	auto source = std::make_shared<FakeSource>();
	ASSERT_TRUE(sp->Attach(0, MediaFrame::Audio, source));
	StartSynthesis(1, -1);
	sp->Say(1, "SLOW une phrase");

	// Un 1 kHz presque à pleine échelle entre sur A pendant la synthèse
	std::unique_ptr<AudioEncoder> pcmu(AudioCodecFactory::CreateEncoder(AudioCodec::PCMU));
	pcmu->TrySetRate(8000);
	auto next = std::chrono::steady_clock::now();
	WORD seq = 0;
	DWORD n = 0;
	for (int frame = 0; frame < 100; ++frame)
	{
		std::vector<SWORD> slice(160);
		for (int i = 0; i < 160; ++i, ++n)
			slice[i] = (SWORD)(30000 * std::sin(2 * M_PI * 1000 * n / 8000.0));
		for (AudioFramePtr f = pcmu->EncodeFrame(Samples::FromBuffer(slice.data(), 160, 8000)); f; f = pcmu->EncodeFrame(nullptr))
		{
			RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMU, AudioCodec::PCMU);
			packet.SetSeqNum(seq++);
			packet.SetTimestamp(n);
			memcpy(packet.GetMediaData(), f->GetData(), f->GetLength());
			packet.SetMediaLength(f->GetLength());
			source->Multiplex(packet);
		}
		next += std::chrono::milliseconds(20);
		std::this_thread::sleep_until(next);
	}

	const std::vector<SWORD> pcm = audio[1].Pcm();
	int peak = 0;
	int jump = 0;
	for (size_t i = 1; i < pcm.size(); ++i)
	{
		peak = std::max(peak, std::abs((int)pcm[i]));
		jump = std::max(jump, std::abs((int)pcm[i] - (int)pcm[i - 1]));
	}
	EXPECT_GT(peak, 30000) << "les deux sources sont dans le mix";
	// Un débordement inverserait le signe : un saut de toute l'échelle
	EXPECT_LT(jump, 40000);
}

// Une phrase refusée par la modération finit sans audio, et le Say avec elle.
TEST_F(SpeechProcessorTts, UnePhraseRefuseeFinitLeSay)
{
	StartSynthesis(1, -1);
	const int sayId = sp->Say(1, "REJECT ceci");
	ASSERT_TRUE(Eventually([&] { return !events.Of(JSR309Event::SpeechSynthesisDoneEvent).empty(); }));
	EXPECT_EQ(std::to_string(sayId), events.Of(JSR309Event::SpeechSynthesisDoneEvent)[0].tail[0]);
	EXPECT_EQ(0, audio[1].Loud());
}

// Sans synthèse active, un Say est refusé ; un texte vide aussi.
TEST_F(SpeechProcessorTts, UnSaySansSyntheseEstRefuse)
{
	EXPECT_EQ(0, sp->Say(1, "Bonjour."));
	StartSynthesis(1, -1);
	EXPECT_EQ(0, sp->Say(1, "   "));
	EXPECT_EQ(0, sp->Say(2, "Bonjour."));
	EXPECT_GT(sp->Say(1, "Bonjour."), 0);
}
