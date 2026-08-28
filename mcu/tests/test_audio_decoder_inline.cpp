/**
 * test_audio_decoder_inline.cpp — le décodeur audio JSR309 décode sur le
 * thread de la source.
 *
 * Lot 1 de `jsr309_transcode_sans_thread.md` : `AudioDecoderJoinableWorker`
 * n'a plus de file ni de thread. La propriété nouvelle, celle que ces tests
 * vérifient, est qu'il n'y a plus rien à attendre — la sortie est là au retour
 * de `onRTPPacket`. Aucun test ici ne dort en espérant qu'un thread ait tourné.
 *
 * Le troisième test est le pendant de sûreté : `Dettach` pendant un
 * `onRTPPacket` concurrent ne peut pas libérer le décodeur sous le paquet,
 * parce que `RemoveListener` ne rend la main que le `Multiplex` en cours
 * terminé (§4.1 et §4.3 du plan). À jouer aussi sous ASan/TSan.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <memory>
#include <thread>

#include "rtp.h"
#include "pipeaudioinput.h"
#include "../src/jsr309/AudioDecoderWorker.h"
#include "../src/jsr309/RTPMultiplexer.h"

namespace {

// Puits audio instrumenté : il compte les trames décodées qu'on lui joue.
class CountingAudioOutput : public AudioOutput
{
public:
	DWORD GetNativeRate() override		{ return rate; }
	DWORD GetPlayingRate() override		{ return rate; }

	int PlayFrame(SamplesPtr samples) override
	{
		frames++;
		samplesPlayed += samples ? samples->GetNbSamples() : 0;
		return 1;
	}

	int StartPlaying(DWORD samplerate) override
	{
		rate = samplerate;
		playing = true;
		return 1;
	}

	int StopPlaying() override
	{
		playing = false;
		return 1;
	}

	std::atomic<int>	frames { 0 };
	std::atomic<DWORD>	samplesPlayed { 0 };
	std::atomic<bool>	playing { false };
	std::atomic<DWORD>	rate { 0 };
};

// Silence PCMU : 0xFF est le zéro de la loi µ. 160 octets = 20 ms à 8 kHz.
RTPPacket MakePcmuPacket(WORD seq)
{
	RTPPacket packet(MediaFrame::Audio, AudioCodec::PCMU);
	packet.SetSeqNum(seq);
	packet.SetTimestamp((DWORD)seq * 160);
	memset(packet.GetMediaData(), 0xFF, 160);
	packet.SetMediaLength(160);
	return packet;
}

// Nombre de threads du processus, lu dans /proc : c'est la seule preuve
// directe qu'aucun thread n'est né.
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

// L'appel de `Multiplex` d'un RTPMultiplexer tient son verrou pendant tout
// l'appel aux listeners : c'est la barrière sur laquelle le lot s'appuie.
TEST(AudioDecoderInline, LaTrameDecodeeEstLaAuRetourDeMultiplex)
{
	CountingAudioOutput output;
	AudioDecoderJoinableWorker decoder;
	ASSERT_EQ(1, decoder.Init(&output));

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	RTPPacket packet = MakePcmuPacket(1);
	source->Multiplex(packet);

	// Pas d'attente, pas de sommeil : si ce compte est nul, c'est qu'un thread
	// est encore dans le chemin.
	EXPECT_EQ(1, output.frames.load())
		<< "le paquet doit etre decode avant que Multiplex rende la main";
	EXPECT_EQ((DWORD)160, output.samplesPlayed.load());
	EXPECT_TRUE(output.playing.load());
	EXPECT_EQ((DWORD)8000, output.rate.load());

	decoder.Dettach();
	decoder.End();
}

// Même propriété pour l'autre puits, celui du transcodeur : le pipe a la trame
// tout de suite, `RecFrame(0)` n'attend rien.
TEST(AudioDecoderInline, LePipeALaTrameSansAttendre)
{
	PipeAudioInput pipe;
	AudioDecoderJoinableWorker decoder;
	ASSERT_TRUE(decoder.Init(&pipe));
	ASSERT_TRUE(pipe.StartRecording(8000));

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	RTPPacket packet = MakePcmuPacket(1);
	source->Multiplex(packet);

	SamplesPtr samples = pipe.RecFrame(0);
	ASSERT_TRUE((bool)samples) << "la trame doit deja etre en file";
	EXPECT_EQ((DWORD)160, samples->GetNbSamples());

	decoder.Dettach();
	decoder.End();
}

TEST(AudioDecoderInline, DemarrerLeDecodeurNeCreeAucunThread)
{
	CountingAudioOutput output;
	AudioDecoderJoinableWorker decoder;
	ASSERT_EQ(1, decoder.Init(&output));

	const int before = CountThreads();
	ASSERT_GT(before, 0);

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	RTPPacket packet = MakePcmuPacket(1);
	source->Multiplex(packet);

	EXPECT_EQ(before, CountThreads())
		<< "le decodeur audio ne doit plus porter de thread";

	decoder.Dettach();
	decoder.End();
}

// `Dettach` détruit le décodeur. Il ne peut le faire qu'une fois le
// `Multiplex` en cours terminé : après son retour, plus aucune trame n'arrive,
// et rien n'a travaillé sur un codec libéré.
TEST(AudioDecoderInline, DettachConcurrentNeLibereRienSousLePaquet)
{
	CountingAudioOutput output;
	AudioDecoderJoinableWorker decoder;
	ASSERT_EQ(1, decoder.Init(&output));

	auto source = std::make_shared<RTPMultiplexer>();
	ASSERT_EQ(1, decoder.Attach(source));

	std::atomic<bool> stop { false };
	std::thread producer([&]{
		for (WORD seq = 1; !stop.load(); ++seq)
		{
			RTPPacket packet = MakePcmuPacket(seq);
			source->Multiplex(packet);
		}
	});

	// Laisser le producteur entrer dans la boucle, puis détacher au milieu.
	while (output.frames.load() < 50)
		std::this_thread::yield();

	decoder.Dettach();

	const int atDettach = output.frames.load();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(atDettach, output.frames.load())
		<< "RemoveListener est la barriere : plus rien ne doit passer apres";

	stop = true;
	producer.join();

	decoder.End();
}

}	// namespace
