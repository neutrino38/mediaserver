/**
 * test_speech_processor_media.cpp — le SpeechProcessor sans service speech
 * (SPEC docs/conception/SPEECH-PROCESSOR §11.2) : relais audio et texte, mix,
 * bruit de confort.
 *
 * Sans réseau : un faux Joinable injecte des RTPPacket dans un port, un faux
 * écouteur recueille et décode ce qui sort de l'autre. Le tour de 20 ms est
 * battu à la main (Init(false)), sauf pour la suppression pendant un relais.
 *
 *     ./tests/runtests --gtest_filter='SpeechProcessorMedia*'
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "redcodec.h"
#include "rtp.h"
#include "text.h"
#include "medkit/codecs.h"
#include "audio.h"
#include "../src/jsr309/RTPMultiplexer.h"
#include "../src/jsr309/SpeechProcessor.h"

namespace
{

// Ce qu'un endpoint enverrait au port.
class FakeSource : public RTPMultiplexer
{
};

// Ce qu'un endpoint recevrait du port : copie et décode.
class Collector : public Joinable::Listener, public TextOutput
{
public:
	explicit Collector(bool acceptRed = true) : acceptRed(acceptRed) {}

	void onRTPPacket(RTPPacket& packet) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		codecs.insert(packet.GetCodec());
		ssrcs.insert(packet.GetSSRC());
		timestamps.push_back(packet.GetTimestamp());
		++packets;

		if (packet.GetMedia() == MediaFrame::Text)
		{
			if (packet.GetCodec() == TextCodec::T140RED)
				red.Decode((RTPRedundantPacket*)&packet, this);
			else
				text.append((const char*)packet.GetMediaData(), packet.GetMediaLength());
			return;
		}

		if (!decoder || decoder->type != (AudioCodec::Type)packet.GetCodec())
		{
			decoder.reset(AudioCodecFactory::CreateDecoder((AudioCodec::Type)packet.GetCodec()));
			if (!decoder)
				return;
		}
		decoder->Decode(packet.GetMediaData(), packet.GetMediaLength());
		for (SamplesPtr frame = decoder->GetFrame(); frame; frame = decoder->GetFrame())
		{
			rate = frame->GetRate();
			pcm.insert(pcm.end(), frame->GetData(), frame->GetData() + frame->GetNbSamples());
		}
	}
	void onResetStream() override {}
	void onEndStream() override {}
	int TryCheckCodec(int codec) override
	{
		return codec == TextCodec::T140RED && !acceptRed ? -1 : codec;
	}

	int SendFrame(TextFrame& frame) override
	{
		text.append((const char*)frame.GetData(), frame.GetLength());
		return 1;
	}

	std::mutex			mutex;
	const bool			acceptRed;
	std::set<int>			codecs;
	std::set<DWORD>			ssrcs;
	std::vector<DWORD>		timestamps;
	int				packets = 0;
	std::unique_ptr<AudioDecoder>	decoder;
	std::vector<SWORD>		pcm;
	DWORD				rate = 0;
	RedundentCodec			red;
	std::string			text;
};

const double Pi = 3.14159265358979323846;

// 20 ms de sinus à 8 kHz, encodé dans le codec voulu.
class SineSource
{
public:
	SineSource(AudioCodec::Type codec, double hz, double amplitude) :
		codec(codec), hz(hz), amplitude(amplitude),
		encoder(AudioCodecFactory::CreateEncoder(codec))
	{
		if (encoder)
			encoder->TrySetRate(8000);
	}

	bool Ok() const	{ return encoder != nullptr; }

	void SendFrame(FakeSource& source)
	{
		std::vector<SWORD> slice(160);
		for (int i = 0; i < 160; ++i, ++n)
			slice[i] = (SWORD)(amplitude * std::sin(2 * Pi * hz * n / 8000.0));
		for (AudioFramePtr frame = encoder->EncodeFrame(Samples::FromBuffer(slice.data(), 160, 8000));
		     frame; frame = encoder->EncodeFrame(nullptr))
		{
			RTPPacket packet(MediaFrame::Audio, codec, codec);
			packet.SetSeqNum(seq++);
			packet.SetTimestamp(n);
			memcpy(packet.GetMediaData(), frame->GetData(), frame->GetLength());
			packet.SetMediaLength(frame->GetLength());
			source.Multiplex(packet);
		}
	}

private:
	AudioCodec::Type		codec;
	double				hz;
	double				amplitude;
	std::unique_ptr<AudioEncoder>	encoder;
	DWORD				n = 0;
	WORD				seq = 0;
};

double Rms(const std::vector<SWORD>& pcm, size_t from)
{
	double sum = 0;
	for (size_t i = from; i < pcm.size(); ++i)
		sum += (double)pcm[i] * pcm[i];
	return pcm.size() > from ? std::sqrt(sum / (pcm.size() - from)) : 0;
}

double Db(double ratio)
{
	return 20 * std::log10(ratio);
}

double Frequency(const std::vector<SWORD>& pcm, size_t from, DWORD rate)
{
	int crossings = 0;
	for (size_t i = from + 1; i < pcm.size(); ++i)
		if ((pcm[i - 1] < 0) != (pcm[i] < 0))
			++crossings;
	const double secs = (double)(pcm.size() - from) / rate;
	return crossings / 2.0 / secs;
}

// Un paquet T.140 RED, une génération de plus à chaque appel.
void SendRedText(FakeSource& source, RedundentCodec& red, WORD& seq, const std::string& text, DWORD ts, bool deliver = true)
{
	TextFrame frame(ts, (const BYTE*)text.data(), text.size());
	RTPRedundantPacket* packet = red.Encode(&frame, TextCodec::T140);
	packet->SetSeqNum(seq++);
	packet->SetSeqCycles(0);
	if (deliver)
		source.Multiplex(*packet);
	delete packet;
}

}

// §11.2 : un sinus de 1 kHz entre en PCMA sur A, sort en Opus sur B à la même
// fréquence, au même niveau à ±1 dB, avec un SSRC et des timestamps continus.
TEST(SpeechProcessorMedia, LAudioPasseDeAVersBEnChangeantDeCodec)
{
	if (!AudioCodec::IsEncodingSupported(AudioCodec::OPUS))
		GTEST_SKIP() << "OPUS indisponible dans ffmpeg";

	SpeechProcessor sp(L"relais");
	ASSERT_TRUE(sp.Init(false));
	ASSERT_TRUE(sp.SetPortCodec(1, AudioCodec::OPUS));
	ASSERT_TRUE(sp.SetComfortNoise(1, 0));

	auto source = std::make_shared<FakeSource>();
	ASSERT_TRUE(sp.Attach(0, MediaFrame::Audio, source));
	Collector collector;
	std::shared_ptr<Joinable> out = sp.GetJoinable(1, MediaFrame::Audio);
	out->AddListener(&collector);

	SineSource sine(AudioCodec::PCMA, 1000, 8000);
	ASSERT_TRUE(sine.Ok());
	uint64_t now = 0;
	for (int i = 0; i < 150; ++i, now += SpeechProcessor::FrameMs)
	{
		sine.SendFrame(*source);
		sp.Tick(now);
	}
	out->RemoveListener(&collector);
	sp.End();

	std::lock_guard<std::mutex> lock(collector.mutex);
	EXPECT_EQ(std::set<int>{AudioCodec::OPUS}, collector.codecs);
	EXPECT_EQ(1u, collector.ssrcs.size()) << "un seul flux sortant";
	ASSERT_GT(collector.timestamps.size(), 100u);
	const DWORD step = collector.timestamps[1] - collector.timestamps[0];
	EXPECT_GT(step, 0u);
	for (size_t i = 1; i < collector.timestamps.size(); ++i)
		ASSERT_EQ(step, collector.timestamps[i] - collector.timestamps[i - 1]) << "paquet " << i;

	// La dernière seconde, une fois le relais et le codec établis.
	ASSERT_GT(collector.rate, 0u);
	const size_t from = collector.pcm.size() - collector.rate;
	EXPECT_NEAR(1000.0, Frequency(collector.pcm, from, collector.rate), 20.0);
	EXPECT_NEAR(0.0, Db(Rms(collector.pcm, from) / (8000 / std::sqrt(2.0))), 1.0);
}

// §11.2 : rien en entrée, le port émet le bruit demandé, ou un silence exact.
TEST(SpeechProcessorMedia, SansEntreeLePortEmetLeBruitDemande)
{
	for (int dbov : {-40, 0})
	{
		SpeechProcessor sp(L"bruit");
		ASSERT_TRUE(sp.Init(false));
		ASSERT_TRUE(sp.SetPortCodec(1, AudioCodec::PCMU));
		ASSERT_TRUE(sp.SetComfortNoise(1, dbov));
		Collector collector;
		std::shared_ptr<Joinable> out = sp.GetJoinable(1, MediaFrame::Audio);
		out->AddListener(&collector);

		for (int i = 0; i < 100; ++i)
			sp.Tick(i * SpeechProcessor::FrameMs);
		out->RemoveListener(&collector);
		sp.End();

		std::lock_guard<std::mutex> lock(collector.mutex);
		ASSERT_GE(collector.packets, 99) << "le flux ne s'interrompt pas";
		// Le niveau est celui du mix, bruit blanc à MixRate. Le PCMU (8 kHz)
		// n'en garde que la moitié basse du spectre : la moitié de la
		// puissance, -3 dB.
		if (dbov)
			EXPECT_NEAR(dbov - 3.0, Db(Rms(collector.pcm, 0) / 32768.0), 1.0);
		else
			EXPECT_EQ(0.0, Rms(collector.pcm, 0)) << "silence exact";
	}
}

TEST(SpeechProcessorMedia, LeNiveauDeBruitEstBorne)
{
	SpeechProcessor sp(L"bornes");
	EXPECT_FALSE(sp.SetComfortNoise(0, 3));
	EXPECT_FALSE(sp.SetComfortNoise(0, -200));
	EXPECT_FALSE(sp.SetComfortNoise(2, -65));
	EXPECT_TRUE(sp.SetComfortNoise(0, -65));
}

// §11.2 : des horloges qui dérivent de 1 % ne font pas dériver la latence.
TEST(SpeechProcessorMedia, UneDeriveDHorlogeNeFaitPasDeriverLaLatence)
{
	for (int perHundred : {101, 99})
	{
		SpeechProcessor sp(L"derive");
		ASSERT_TRUE(sp.Init(false));
		ASSERT_TRUE(sp.SetPortCodec(1, AudioCodec::PCMU));
		auto source = std::make_shared<FakeSource>();
		ASSERT_TRUE(sp.Attach(0, MediaFrame::Audio, source));
		Collector collector;
		std::shared_ptr<Joinable> out = sp.GetJoinable(1, MediaFrame::Audio);
		out->AddListener(&collector);

		SineSource sine(AudioCodec::PCMU, 440, 4000);
		int sent = 0;
		int maxBuffered = 0;
		for (int tick = 1; tick <= 1000; ++tick)
		{
			// Le pair émet perHundred trames quand nous en consommons cent.
			while (sent * 100 < tick * perHundred)
			{
				sine.SendFrame(*source);
				++sent;
			}
			sp.Tick(tick * SpeechProcessor::FrameMs);
			maxBuffered = std::max(maxBuffered, sp.GetRelayBufferedMs(0));
		}
		out->RemoveListener(&collector);
		sp.End();

		EXPECT_LE(maxBuffered, SpeechProcessor::RelayMaxMs) << perHundred << " %";
		// Un tour de plus que l'anneau : sous les 80 ms de la SPEC.
		EXPECT_LT(maxBuffered + SpeechProcessor::FrameMs, 80 + 1);
		std::lock_guard<std::mutex> lock(collector.mutex);
		// Une trame par tour, quoi qu'il arrive en entrée. La première peut
		// manquer : le rééchantillonneur de l'encodeur garde quelques
		// échantillons de son premier appel.
		EXPECT_GE(collector.packets, 999);
		EXPECT_LE(collector.packets, 1000);
	}
}

// §11.2 : le texte RED entre sur A et sort en RED sur B, un paquet perdu en
// entrée est récupéré par la redondance.
TEST(SpeechProcessorMedia, LeTexteRedPasseEtUnePerteEstRecuperee)
{
	SpeechProcessor sp(L"texte");
	ASSERT_TRUE(sp.Init(false));
	auto source = std::make_shared<FakeSource>();
	ASSERT_TRUE(sp.Attach(0, MediaFrame::Text, source));
	Collector collector(true);
	std::shared_ptr<Joinable> out = sp.GetJoinable(1, MediaFrame::Text);
	out->AddListener(&collector);

	RedundentCodec red;
	WORD seq = 100;
	const std::string words[] = {"Bon", "jour", " à", " tous"};
	uint64_t now = 1000;
	for (int i = 0; i < 4; ++i)
	{
		// Le deuxième paquet est perdu : le suivant le porte en redondance.
		SendRedText(*source, red, seq, words[i], (DWORD)now, i != 1);
		for (int k = 0; k < 10; ++k, now += SpeechProcessor::FrameMs)
			sp.Tick(now);
	}
	for (int k = 0; k < 100; ++k, now += SpeechProcessor::FrameMs)
		sp.Tick(now);
	out->RemoveListener(&collector);
	sp.End();

	std::lock_guard<std::mutex> lock(collector.mutex);
	EXPECT_EQ("Bonjour à tous", collector.text);
	EXPECT_EQ(std::set<int>{TextCodec::T140RED}, collector.codecs);
}

// §11.2 : vers un endpoint qui ne prend pas le RED, le texte sort en T.140 nu.
TEST(SpeechProcessorMedia, LeTexteSortEnT140NuSiLEndpointRefuseLeRed)
{
	SpeechProcessor sp(L"texte");
	ASSERT_TRUE(sp.Init(false));
	auto source = std::make_shared<FakeSource>();
	ASSERT_TRUE(sp.Attach(1, MediaFrame::Text, source));
	Collector collector(false);
	std::shared_ptr<Joinable> out = sp.GetJoinable(0, MediaFrame::Text);
	out->AddListener(&collector);

	RedundentCodec red;
	WORD seq = 0;
	uint64_t now = 0;
	SendRedText(*source, red, seq, "salut", 0);
	for (int k = 0; k < 50; ++k, now += SpeechProcessor::FrameMs)
		sp.Tick(now);
	out->RemoveListener(&collector);
	sp.End();

	std::lock_guard<std::mutex> lock(collector.mutex);
	EXPECT_EQ("salut", collector.text);
	EXPECT_EQ(std::set<int>{TextCodec::T140}, collector.codecs);
}

// Le texte part par paquets espacés de 300 ms au plus, et pas un de plus.
TEST(SpeechProcessorMedia, LeTexteSortParPaquetsDe300Ms)
{
	SpeechProcessor sp(L"texte");
	ASSERT_TRUE(sp.Init(false));
	auto source = std::make_shared<FakeSource>();
	ASSERT_TRUE(sp.Attach(0, MediaFrame::Text, source));
	Collector collector(false);
	std::shared_ptr<Joinable> out = sp.GetJoinable(1, MediaFrame::Text);
	out->AddListener(&collector);

	// Une lettre toutes les 20 ms pendant 600 ms : deux ou trois paquets.
	RTPPacket packet(MediaFrame::Text, TextCodec::T140);
	uint64_t now = 0;
	for (int i = 0; i < 30; ++i, now += SpeechProcessor::FrameMs)
	{
		packet.SetPayload((BYTE*)"x", 1);
		source->Multiplex(packet);
		sp.Tick(now);
	}
	for (int k = 0; k < 50; ++k, now += SpeechProcessor::FrameMs)
		sp.Tick(now);
	out->RemoveListener(&collector);
	sp.End();

	std::lock_guard<std::mutex> lock(collector.mutex);
	EXPECT_EQ(std::string(30, 'x'), collector.text);
	EXPECT_LE(collector.packets, 3);
}

// §11.2 : la vidéo ne passe pas par un SpeechProcessor ; un port hors de {0, 1}
// n'existe pas.
TEST(SpeechProcessorMedia, LaVideoEtLesPortsInconnusSontRefuses)
{
	SpeechProcessor sp(L"refus");
	auto source = std::make_shared<FakeSource>();
	EXPECT_FALSE(sp.Attach(0, MediaFrame::Video, source));
	EXPECT_EQ(nullptr, sp.GetJoinable(0, MediaFrame::Video));
	EXPECT_FALSE(sp.Attach(2, MediaFrame::Audio, source));
	EXPECT_FALSE(sp.Attach(-1, MediaFrame::Text, source));
	EXPECT_EQ(nullptr, sp.GetJoinable(2, MediaFrame::Audio));
	EXPECT_FALSE(sp.Dettach(0, MediaFrame::Video));
	EXPECT_FALSE(sp.SetPortCodec(2, AudioCodec::PCMU));
}

// §11.2 : supprimer pendant un relais ne laisse ni accès libéré ni référence :
// le weak_ptr de l'endpoint attaché expire.
TEST(SpeechProcessorMedia, LaSuppressionPendantUnRelaisEstPropre)
{
	auto sp = std::make_shared<SpeechProcessor>(L"suppression");
	ASSERT_TRUE(sp->Init(true));
	ASSERT_TRUE(sp->SetPortCodec(1, AudioCodec::PCMU));
	auto audio = std::make_shared<FakeSource>();
	auto text = std::make_shared<FakeSource>();
	ASSERT_TRUE(sp->Attach(0, MediaFrame::Audio, audio));
	ASSERT_TRUE(sp->Attach(0, MediaFrame::Text, text));

	Collector collector;
	std::weak_ptr<Joinable> joined = sp->GetJoinable(1, MediaFrame::Audio);
	joined.lock()->AddListener(&collector);
	sp->GetJoinable(1, MediaFrame::Text)->AddListener(&collector);

	std::atomic<bool> running{true};
	std::thread feeder([&] {
		SineSource sine(AudioCodec::PCMU, 440, 4000);
		RTPPacket packet(MediaFrame::Text, TextCodec::T140);
		while (running)
		{
			sine.SendFrame(*audio);
			packet.SetPayload((BYTE*)"a", 1);
			text->Multiplex(packet);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	sp->End();
	sp.reset();
	EXPECT_TRUE(joined.expired());

	// Les sources continuent : plus personne ne les écoute.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	running = false;
	feeder.join();

	std::lock_guard<std::mutex> lock(collector.mutex);
	EXPECT_GT(collector.packets, 5);
}
