/**
 * test_audio_encoder_inline.cpp — l'encodeur audio JSR309 encode sur le thread
 * de la source.
 *
 * Lot 3 de `jsr309_transcode_sans_thread.md` : dans un transcodeur,
 * `AudioEncoderMultiplexerWorker` n'a plus ni thread ni `PipeAudioInput` entre
 * lui et le décodeur. La propriété nouvelle, celle que ces tests vérifient, est
 * qu'il n'y a plus rien à attendre — les paquets encodés sont là au retour de
 * `Multiplex`. Aucun test ici ne dort en espérant qu'un thread ait tourné.
 *
 * Le port de MIXEUR, lui, garde son thread : c'est le mixeur qui cadence
 * (§3.4). Rien ici ne l'exerce.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "rtp.h"
#include "../src/jsr309/AudioTranscoder.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

// Puits instrumenté : il ne relaie aucun codec, donc le transcodeur reste sur
// le chemin décodage/encodage.
class CountingSink : public Joinable::Listener
{
public:
	void onRTPPacket(RTPPacket &packet) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		count++;
		codecs.insert(packet.GetCodec());
		ssrcs.insert(packet.GetSSRC());
	}

	void onResetStream() override {}
	void onEndStream() override {}
	int TryCheckCodec(int codec) override { return -1; }

	int Count()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return count;
	}

	std::set<int> Codecs()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return codecs;
	}

	std::set<DWORD> Ssrcs()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return ssrcs;
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		count = 0;
		codecs.clear();
		ssrcs.clear();
	}

private:
	std::mutex	mutex;
	int		count = 0;
	std::set<int>	codecs;
	std::set<DWORD>	ssrcs;
};

// Silence PCMU : 0xFF est le zéro de la loi µ. 160 octets = 20 ms à 8 kHz.
// Livré par rappel, jamais copié : `RTPPacket` porte un `header` qui pointe
// dans son propre `buffer` et n'a pas de constructeur de copie.
template <typename Sink>
void SendPcmu(WORD seq, Sink sink)
{
	RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMU);
	packet.SetSeqNum(seq);
	packet.SetTimestamp((DWORD)seq * 160);
	memset(packet.GetMediaData(), 0xFF, 160);
	packet.SetMediaLength(160);
	sink(packet);
}

// Nombre de threads du processus, lu dans /proc : c'est la seule preuve directe
// qu'aucun thread n'est né.
int CountThreads()
{
	DIR* dir = opendir("/proc/self/task");
	if (!dir)
		return -1;

	int count = 0;
	while (struct dirent* entry = readdir(dir))
		if (entry->d_name[0] != '.')
			count++;

	closedir(dir);
	return count;
}

// La chaîne complète — décodage, rééchantillonnage, encodage, multiplexage —
// tourne sur le thread qui a livré le paquet. Pas d'attente, pas de sommeil :
// si ce compte est nul, c'est qu'un thread est encore dans le chemin.
TEST(AudioEncoderInline, LesPaquetsEncodesSontLaAuRetourDeMultiplex)
{
	const int kPackets = 10;

	std::wstring name = L"inline-audio";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	for (WORD seq = 1; seq <= kPackets; ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });

	// 20 ms de PCMU valent 20 ms d'Opus. Le rééchantillonneur retient au plus
	// une trame partielle au démarrage : d'où la tolérance de deux.
	EXPECT_GE(sink.Count(), kPackets - 2)
		<< "les paquets doivent etre encodes avant que Multiplex rende la main";

	std::set<int> codecs = sink.Codecs();
	ASSERT_EQ((size_t)1, codecs.size());
	EXPECT_EQ((int)AudioCodec::OPUS, *codecs.begin());

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// Le transcodeur audio ne porte plus AUCUN thread : ni décodeur (lot 1), ni
// encodeur (lot 3). Compté avant l'arrivée du premier paquet, donc avant que
// libopus n'existe.
TEST(AudioEncoderInline, LeTranscodeurAudioNeCreeAucunThread)
{
	std::wstring name = L"inline-audio-threads";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));

	const int before = CountThreads();
	ASSERT_GT(before, 0);

	CountingSink sink;
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	EXPECT_EQ(before, CountThreads())
		<< "le transcodeur audio ne doit plus porter de thread";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// §4.4 : SetCodec ne fait plus Stop/Start, il lève un drapeau que le chemin des
// paquets consomme. Ce que le puits doit voir : le nouveau codec, et un SSRC
// neuf — un nouvel encodeur, c'est une nouvelle base de temps (RFC 3550).
TEST(AudioEncoderInline, SetCodecEstAppliqueParLeCheminDesPaquets)
{
	std::wstring name = L"inline-audio-setcodec";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	WORD seq = 1;
	for (int n = 0; n < 5; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });
	ASSERT_GT(sink.Count(), 0);

	std::set<DWORD> firstRun = sink.Ssrcs();
	ASSERT_EQ((size_t)1, firstRun.size());

	// Renégociation depuis le plan de contrôle, encodeur en marche.
	const int threadsBefore = CountThreads();
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::PCMA));
	EXPECT_EQ(threadsBefore, CountThreads())
		<< "SetCodec ne doit ni lancer ni joindre de thread";

	sink.Reset();
	for (int n = 0; n < 5; ++n, ++seq)
		SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });

	ASSERT_GT(sink.Count(), 0) << "le nouveau codec doit produire des le paquet suivant";

	std::set<int> codecs = sink.Codecs();
	ASSERT_EQ((size_t)1, codecs.size());
	EXPECT_EQ((int)AudioCodec::PCMA, *codecs.begin());

	std::set<DWORD> secondRun = sink.Ssrcs();
	ASSERT_EQ((size_t)1, secondRun.size());
	EXPECT_NE(*firstRun.begin(), *secondRun.begin())
		<< "un nouveau run d'encodage annonce une nouvelle base de temps par un SSRC neuf";

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);
	transcoder.End();
}

// `Dettach` détruit décodeur et encodeur. Il ne peut le faire qu'une fois le
// `Multiplex` en cours terminé : après son retour, plus rien ne sort, et rien
// n'a travaillé sur un codec libéré (§4.3). À jouer aussi sous ASan/TSan.
TEST(AudioEncoderInline, DettachConcurrentNeLibereRienSousLaTrame)
{
	std::wstring name = L"inline-audio-race";
	AudioTranscoder transcoder(name);
	ASSERT_EQ(1, transcoder.Init(/*allowBriding=*/false));
	ASSERT_EQ(1, transcoder.SetCodec(AudioCodec::OPUS));

	CountingSink sink;
	transcoder.AddListener(&sink);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, transcoder.Attach(source));

	std::atomic<bool> stop { false };
	std::thread producer([&]{
		for (WORD seq = 1; !stop.load(); ++seq)
			SendPcmu(seq, [&](RTPPacket& p){ source->Multiplex(p); });
	});

	while (sink.Count() < 50)
		std::this_thread::yield();

	transcoder.Dettach();
	transcoder.RemoveListener(&sink);

	const int atDettach = sink.Count();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(atDettach, sink.Count())
		<< "RemoveListener est la barriere : plus rien ne doit passer apres";

	stop = true;
	producer.join();

	transcoder.End();
}

}	// namespace
