/**
 * test_speech_processor_asr.cpp — la reconnaissance du SpeechProcessor
 * (SPEC SPEECH-PROCESSOR §6, §11.3), de bout en bout : la voix entre en PCMA
 * sur le port A, passe la VAD, part au vrai speech-server, qui la relaie au
 * faux Voxtral Realtime. Le faux rend un delta enregistré par trame audio
 * reçue : le texte attendu est donc connu.
 *
 * Les événements sont relus par leur sérialisation XML-RPC, celle que le
 * contrôleur reçoit.
 *
 *     ./tests/runtests --gtest_filter='SpeechProcessorAsr*'
 */
#include <gtest/gtest.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
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
#include "fake_mistral_realtime.h"
#include "audio.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "text.h"
#include "redcodec.h"
#include "speech/speechservice.h"
#include "websocketserver.h"
#include "../src/jsr309/RTPMultiplexer.h"
#include "../src/jsr309/SpeechProcessor.h"

using json = nlohmann::json;

namespace
{

const std::string Fixture = std::string(SPEECH_FIXTURES_DIR) + "/asr_binary_d240_lNone_s0.jsonl";
const std::string Voice = std::string(SPEECH_FIXTURES_DIR) + "/tts16k.wav";
const std::string LS = "\xE2\x80\xA8";

class FakeSource : public RTPMultiplexer
{
};

// Ce qui sort en texte d'un port.
class TextCollector : public Joinable::Listener, public TextOutput
{
public:
	void onRTPPacket(RTPPacket& packet) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (packet.GetCodec() == TextCodec::T140RED)
			red.Decode((RTPRedundantPacket*)&packet, this);
		else
			text.append((const char*)packet.GetMediaData(), packet.GetMediaLength());
	}
	void onResetStream() override {}
	void onEndStream() override {}
	int SendFrame(TextFrame& frame) override
	{
		text.append((const char*)frame.GetData(), frame.GetLength());
		return 1;
	}
	std::string Text()	{ std::lock_guard<std::mutex> lock(mutex); return text; }

private:
	std::mutex	mutex;
	RedundentCodec	red;
	std::string	text;
};

// Un événement tel que le contrôleur le lit : (type, sessionTag, spId,
// media, port, …), la queue en chaînes.
struct Event
{
	int				type = 0;
	int				media = -1;
	int				port = -1;
	std::vector<std::string>	tail;
	std::chrono::steady_clock::time_point at;
};

class Events
{
public:
	void Post(JSR309Event* event)
	{
		Event parsed;
		parsed.at = std::chrono::steady_clock::now();
		xmlrpc_env env;
		xmlrpc_env_init(&env);
		xmlrpc_value* value = event->GetXmlValue(&env);
		delete event;
		const int size = xmlrpc_array_size(&env, value);
		for (int i = 0; i < size; ++i)
		{
			xmlrpc_value* item = NULL;
			xmlrpc_array_read_item(&env, value, i, &item);
			int number = 0;
			switch (xmlrpc_value_type(item))
			{
				case XMLRPC_TYPE_INT:
					xmlrpc_read_int(&env, item, &number);
					if (i == 0) parsed.type = number;
					else if (i == 3) parsed.media = number;
					else if (i == 4) parsed.port = number;
					else if (i > 4) parsed.tail.push_back(std::to_string(number));
					break;
				case XMLRPC_TYPE_BOOL:
				{
					xmlrpc_bool b = 0;
					xmlrpc_read_bool(&env, item, &b);
					parsed.tail.push_back(b ? "true" : "false");
					break;
				}
				case XMLRPC_TYPE_STRING:
				{
					const char* s = NULL;
					xmlrpc_read_string(&env, item, &s);
					if (i > 4)
						parsed.tail.push_back(s ? s : "");
					free((void*)s);
					break;
				}
				default:
					break;
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
		for (const Event& event : events)
			if (event.type == type)
				out.push_back(event);
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

std::vector<SWORD> ReadWav(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const size_t data = raw.find("data");
	if (data == std::string::npos)
		return {};
	const uint32_t size = *(const uint32_t*)&raw[data + 4];
	const SWORD* samples = (const SWORD*)&raw[data + 8];
	return std::vector<SWORD>(samples, samples + size / 2);
}

double Rms(const BYTE* data, size_t bytes)
{
	const SWORD* s = (const SWORD*)data;
	double sum = 0;
	for (size_t i = 0; i < bytes / 2; ++i)
		sum += (double)s[i] * s[i];
	return bytes ? std::sqrt(sum / (bytes / 2)) : 0;
}

class SpeechProcessorAsr : public ::testing::Test
{
protected:
	void SetUp() override
	{
		char pattern[] = "/tmp/speech-sp-asr-XXXXXX";
		ASSERT_TRUE(mkdtemp(pattern));
		dir = pattern;
		const std::string key = dir + "/key";
		std::ofstream(key) << "test-key\n";
		chmod(key.c_str(), 0600);

		controlPort = speechtest::FreePort(AF_INET);
		streamingPort = speechtest::FreePort(AF_INET);

		fake = std::make_unique<FakeMistralRealtime>(Fixture, "test-key");
		fakePort = speechtest::FreePort(AF_INET);
		fakeServer.AddHandler("/v1/audio/transcriptions/realtime", fake.get());
		ASSERT_TRUE(fakeServer.Init(IPAddress::Parse("127.0.0.1").To(fakePort)));
		ASSERT_TRUE(server.Init(IPAddress::Parse("127.0.0.1").To(speechtest::FreePort(AF_INET))));

		LaunchServer();
		service = std::make_unique<SpeechService>(server, "http://127.0.0.1:" + std::to_string(controlPort),
			"ws://127.0.0.1:" + std::to_string(streamingPort), "", 20);
		service->Start();

		encoder.reset(AudioCodecFactory::CreateEncoder(AudioCodec::PCMA));
		ASSERT_TRUE(encoder != nullptr);
		encoder->TrySetRate(8000);

		sp = std::make_shared<SpeechProcessor>(L"asr");
		sp->SetSpeechService(service.get());
		sp->SetEventPoster([this](JSR309Event* event) { events.Post(event); });
		ASSERT_TRUE(sp->Init(true));
		source = std::make_shared<FakeSource>();
		ASSERT_TRUE(sp->Attach(0, MediaFrame::Audio, source));
		sp->GetJoinable(1, MediaFrame::Text)->AddListener(&text);
	}

	void TearDown() override
	{
		if (sp)
		{
			sp->GetJoinable(1, MediaFrame::Text)->RemoveListener(&text);
			sp->End();
		}
		if (service)
			service->End();
		StopServer();
		server.End();
		fakeServer.End();
		system(("rm -rf " + dir).c_str());
	}

	void LaunchServer()
	{
		std::ofstream(dir + "/config.json") << json{
			{"control", {{"listen", "127.0.0.1"}, {"port", controlPort}, {"allow", {"127.0.0.0/8"}}}},
			{"streaming", {{"listen", "127.0.0.1"}, {"port", streamingPort}}},
			{"mistral", {{"url", "http://127.0.0.1:" + std::to_string(fakePort)}}}}.dump();

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

	void StopServer()
	{
		if (pid <= 0)
			return;
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
		pid = -1;
	}

	// Démarre la reconnaissance du port A et attend que le flux soit prêt.
	void StartRecognition(bool toEvents, int textPort)
	{
		ASSERT_TRUE(sp->StartRecognition(0, toEvents, textPort, false));
		ASSERT_TRUE(Eventually([&] { return !events.Of(JSR309Event::SpeechServiceEvent).empty(); }));
		const Event up = events.Of(JSR309Event::SpeechServiceEvent)[0];
		ASSERT_EQ((std::vector<std::string>{"asr", "up", ""}), up.tail);
	}

	// Pousse des échantillons 16 kHz en PCMA, à vitesse réelle.
	void Play(const std::vector<SWORD>& pcm)
	{
		auto next = std::chrono::steady_clock::now();
		for (size_t pos = 0; pos + 320 <= pcm.size(); pos += 320)
		{
			for (AudioFramePtr frame = encoder->EncodeFrame(Samples::FromBuffer(&pcm[pos], 320, 16000));
			     frame; frame = encoder->EncodeFrame(nullptr))
			{
				RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMA, AudioCodec::PCMA);
				packet.SetSeqNum(seq++);
				packet.SetTimestamp(timestamp);
				timestamp += frame->GetLength();
				memcpy(packet.GetMediaData(), frame->GetData(), frame->GetLength());
				packet.SetMediaLength(frame->GetLength());
				source->Multiplex(packet);
			}
			next += std::chrono::milliseconds(20);
			std::this_thread::sleep_until(next);
		}
	}

	void PlaySilence(int ms)
	{
		Play(std::vector<SWORD>(16 * ms, 0));
	}

	std::string AllDeltas()
	{
		std::string all;
		for (const std::string& delta : fake->deltas)
			all += delta;
		return all;
	}

	std::string dir;
	int controlPort = 0;
	int streamingPort = 0;
	int fakePort = 0;
	pid_t pid = -1;

	std::unique_ptr<FakeMistralRealtime> fake;
	WebSocketServer fakeServer;
	WebSocketServer server;
	std::unique_ptr<SpeechService> service;
	std::unique_ptr<AudioEncoder> encoder;
	std::shared_ptr<SpeechProcessor> sp;
	std::shared_ptr<FakeSource> source;
	TextCollector text;
	Events events;
	WORD seq = 0;
	DWORD timestamp = 0;
};

}

// §11.3 : la voix entrée sur A sort en texte sur B et en événements ; un seul
// final par énoncé, qui porte tout l'énoncé, et pas plus d'un partiel toutes
// les 250 ms.
TEST_F(SpeechProcessorAsr, LaVoixSortEnTexteEtEnEvenements)
{
	StartRecognition(true, 1);
	const std::vector<SWORD> voice = ReadWav(Voice);
	ASSERT_GT(voice.size(), 16000u);

	PlaySilence(500);
	Play(voice);
	PlaySilence(2000);

	const std::string expected = AllDeltas();
	ASSERT_TRUE(Eventually([&] {
		std::string finals;
		for (const Event& e : events.Of(JSR309Event::SpeechRecognizedEvent))
			if (e.tail[1] == "true")
				finals += e.tail[0];
		return finals == expected;
	}, 5000));

	std::vector<Event> recognized = events.Of(JSR309Event::SpeechRecognizedEvent);
	std::string partials;
	std::chrono::steady_clock::time_point lastPartial;
	int finals = 0;
	for (const Event& e : recognized)
	{
		EXPECT_EQ(MediaFrame::Text, e.media);
		EXPECT_EQ(0, e.port) << "le port de la voix";
		if (e.tail[1] == "true")
		{
			++finals;
			continue;
		}
		if (!partials.empty())
			EXPECT_GE(e.at - lastPartial, std::chrono::milliseconds(230));
		partials += e.tail[0];
		lastPartial = e.at;
	}
	// Les partiels suivent le texte ; les derniers deltas d'un énoncé peuvent
	// n'arriver que dans le final.
	EXPECT_EQ(0u, expected.find(partials)) << "les partiels sont un début du texte";

	// Un final par fin de parole détectée
	const std::vector<Event> activity = events.Of(JSR309Event::SpeechActivityEvent);
	int stops = 0;
	for (const Event& e : activity)
		if (e.tail[0] == "false")
			++stops;
	EXPECT_GE(finals, 1);
	EXPECT_EQ(stops, finals);
	ASSERT_FALSE(activity.empty());
	EXPECT_EQ("true", activity.front().tail[0]);

	// Le texte du port B : tout l'énoncé, fermé par un saut de ligne
	ASSERT_TRUE(Eventually([&] { return text.Text().find(LS) != std::string::npos; }, 3000));
	std::string shown = text.Text();
	for (size_t at; (at = shown.find(LS)) != std::string::npos; )
		shown.erase(at, LS.size());
	EXPECT_EQ(expected, shown);
}

// §11.3 : le silence ne part pas au fournisseur.
TEST_F(SpeechProcessorAsr, LeSilenceNEstPasEnvoye)
{
	StartRecognition(true, -1);
	PlaySilence(3000);

	ASSERT_EQ(1u, fake->Count());
	EXPECT_EQ(0, fake->Get(0).frames);
	EXPECT_TRUE(events.Of(JSR309Event::SpeechActivityEvent).empty());
	EXPECT_TRUE(events.Of(JSR309Event::SpeechRecognizedEvent).empty());
}

// §11.3 : le pré-roulement fait partir ce qui précède la détection, donc la
// première syllabe.
TEST_F(SpeechProcessorAsr, LePreRoulementPrecedeLaVoix)
{
	StartRecognition(true, -1);
	PlaySilence(1000);
	Play(ReadWav(Voice));
	PlaySilence(1000);

	ASSERT_TRUE(Eventually([&] { return fake->Get(0).flushes >= 1; }));
	const std::string audio = fake->Get(0).audio;
	const size_t preRoll = SpeechProcessor::PreRollMs * 32;
	ASSERT_GT(audio.size(), preRoll);
	// La plus vieille trame du pré-roulement est encore du silence : sans
	// lui, la première trame reçue serait déjà de la voix.
	EXPECT_LT(Rms((const BYTE*)audio.data(), 640), 100.0);
}

// Un souffle ou un clic dans le micro n'est pas une prise de parole : la VAD
// les marque, mais la parole n'est déclarée qu'après OnsetMs de voix. Sinon
// le barge-in d'un contrôleur coupe la synthèse (appel du 2026-10-20 : des
// salves de 60 à 160 ms coupaient les phrases longues du chatbot).
TEST_F(SpeechProcessorAsr, DesSalvesBrevesNeSontPasUneParole)
{
	StartRecognition(true, -1);
	const std::vector<SWORD> voice = ReadWav(Voice);

	// 120 ms de vraie voix : six trames bien au-dessus du silence
	size_t at = 0;
	for (size_t pos = 0; pos + 1920 <= voice.size(); pos += 320)
	{
		bool loud = true;
		for (size_t f = 0; f < 6 && loud; ++f)
			loud = Rms((const BYTE*)&voice[pos + f * 320], 640) > 1000.0;
		if (loud)
		{
			at = pos;
			break;
		}
	}
	ASSERT_GT(at, 0u);
	const std::vector<SWORD> burst(voice.begin() + at, voice.begin() + at + 1920);

	PlaySilence(500);
	for (int i = 0; i < 3; ++i)
	{
		Play(burst);
		PlaySilence(300);
	}
	PlaySilence(500);

	EXPECT_TRUE(events.Of(JSR309Event::SpeechActivityEvent).empty());
	EXPECT_EQ(0, fake->Get(0).frames);

	// La même voix, entière, est bien une parole
	Play(voice);
	ASSERT_TRUE(Eventually([&] { return !events.Of(JSR309Event::SpeechActivityEvent).empty(); }));
	EXPECT_EQ("true", events.Of(JSR309Event::SpeechActivityEvent).front().tail[0]);
}

// §5.3 : une panne de speech-server est dite une fois, sans couper le relais.
TEST_F(SpeechProcessorAsr, UnePanneDuServiceEstPubliee)
{
	StartRecognition(true, -1);
	StopServer();
	ASSERT_TRUE(Eventually([&] { return events.Of(JSR309Event::SpeechServiceEvent).size() >= 2; }));
	const Event down = events.Of(JSR309Event::SpeechServiceEvent)[1];
	EXPECT_EQ("asr", down.tail[0]);
	EXPECT_EQ("down", down.tail[1]);
	EXPECT_FALSE(down.tail[2].empty()) << "la raison";
}
