/**
 * test_audio_pipes.cpp — les pipes audio et les chaînes qui les traversent,
 * après le passage au transport AVFrame (design/audio-avframe.md).
 *
 * Le pipe portait le troisième bug du 2026-08-14 : son rééchantillonneur était
 * ouvert par StartRecording, donc AVANT que le décodeur ne découvre sa vraie
 * fréquence au premier paquet. Les échantillons 48 kHz étaient alors lus comme
 * du 16 kHz — audio 3x trop rapide, 83 paquets/s mesurés au lieu de 50.
 * La trame porte désormais sa fréquence, et c'est elle qui pilote le resampler.
 */
#include <gtest/gtest.h>
#include "log.h"
#include "pipeaudioinput.h"
#include "pipeaudiooutput.h"
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <memory>

namespace {

SamplesPtr Tone(int nb, int rate)
{
	std::vector<SWORD> pcm(nb);
	for (int i = 0; i < nb; i++)
		pcm[i] = (SWORD)(8000.0 * sin(2.0 * M_PI * 440.0 * i / rate));
	return Samples::FromBuffer(&pcm[0], (DWORD)nb, (DWORD)rate);
}

} // namespace

/* ------------------------------------------------------------------------- *
 *                              PipeAudioInput                               *
 * ------------------------------------------------------------------------- */

TEST(PipeAudioInput, UneTrameTraverseSansConversionQuandLesFrequencesCoincident)
{
	PipeAudioInput pipe;
	pipe.StartRecording(48000);

	ASSERT_TRUE(pipe.PutFrame(Tone(960, 48000)));

	SamplesPtr out = pipe.RecFrame(100);
	ASSERT_TRUE(out != nullptr);
	EXPECT_EQ(out->GetNbSamples(), 960u);
	EXPECT_EQ(out->GetRate(), 48000u);

	pipe.End();
}

TEST(PipeAudioInput, LaTrameEstRendueEntiereQuelleQueSoitSaTaille)
{
	PipeAudioInput pipe;
	pipe.StartRecording(48000);

	// 120 ms d'opus : 5760 échantillons. Aucun tampon fixe sur le chemin ne
	// doit les tronquer — c'est le défaut n°2 du 14/08 vu depuis le pipe.
	ASSERT_TRUE(pipe.PutFrame(Tone(5760, 48000)));

	SamplesPtr out = pipe.RecFrame(100);
	ASSERT_TRUE(out != nullptr);
	EXPECT_EQ(out->GetNbSamples(), 5760u);

	pipe.End();
}

// LE bug n°3 : le producteur change de fréquence après le StartRecording du
// consommateur. Personne ne prévient le pipe ; la trame le dit.
TEST(PipeAudioInput, UnChangementDeFrequenceDEcritureEstAbsorbe)
{
	PipeAudioInput pipe;
	pipe.StartRecording(8000);

	// D'abord du 8 kHz : passe-plat.
	ASSERT_TRUE(pipe.PutFrame(Tone(160, 8000)));
	SamplesPtr out = pipe.RecFrame(100);
	ASSERT_TRUE(out != nullptr);
	EXPECT_EQ(out->GetNbSamples(), 160u);
	EXPECT_EQ(out->GetRate(), 8000u);

	// Puis le décodeur découvre son vrai 48 kHz. Le pipe rééchantillonne, et
	// ce qui sort est bien à 8 kHz : 960 échantillons à 48 kHz = 20 ms = 160.
	// Sans cette reprise, les 960 sortaient tels quels, lus comme du 8 kHz.
	DWORD total = 0;
	for (int i = 0; i < 10; i++)
	{
		ASSERT_TRUE(pipe.PutFrame(Tone(960, 48000)));
		SamplesPtr s = pipe.RecFrame(100);
		ASSERT_TRUE(s != nullptr);
		EXPECT_EQ(s->GetRate(), 8000u);
		total += s->GetNbSamples();
	}
	// 9600 échantillons à 48 kHz = 1600 à 8 kHz, à l'amorçage du resampler près.
	EXPECT_GE(total, 1550u);
	EXPECT_LE(total, 1600u);

	pipe.End();
}

TEST(PipeAudioInput, RecFrameRendNullApresLeDelai)
{
	PipeAudioInput pipe;
	pipe.StartRecording(8000);

	// Rien n'a été publié : on ne bloque pas indéfiniment.
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	EXPECT_TRUE(pipe.RecFrame(50) == nullptr);
	std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0);
	EXPECT_GE(elapsed.count(), 40);
	EXPECT_LT(elapsed.count(), 2000);

	pipe.End();
}

TEST(PipeAudioInput, UneLectureAnnuleeRendLaMain)
{
	PipeAudioInput pipe;
	pipe.StartRecording(8000);

	std::thread canceller([&pipe]{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		pipe.CancelRecFrame();
	});

	EXPECT_TRUE(pipe.RecFrame(5000) == nullptr);
	canceller.join();

	pipe.End();
}

TEST(PipeAudioInput, RienNEstMisEnFileTantQuOnNEnregistrePas)
{
	PipeAudioInput pipe;

	//StartRecording jamais appelé : la trame est jetée, pas accumulée.
	ASSERT_TRUE(pipe.PutFrame(Tone(160, 8000)));
	EXPECT_TRUE(pipe.RecFrame(20) == nullptr);

	pipe.End();
}

TEST(PipeAudioInput, LaFileEstBorneeEnDuree)
{
	PipeAudioInput pipe;
	pipe.StartRecording(8000);

	// 100 trames de 20 ms = 2 s, très au-delà de la profondeur admise : la
	// politique historique est de vider plutôt que de bloquer le producteur.
	for (int i = 0; i < 100; i++)
		ASSERT_TRUE(pipe.PutFrame(Tone(160, 8000)));

	int drained = 0;
	while (pipe.RecFrame(0) != nullptr)
		drained++;

	// Bien moins que les 100 publiées, et la latence reste sous le demi-second.
	EXPECT_LT(drained, 30);
	EXPECT_GT(drained, 0);

	pipe.End();
}

TEST(PipeAudioInput, LAdaptateurPlatRendExactementCeQuOnLuiDemande)
{
	PipeAudioInput pipe;
	pipe.StartRecording(8000);

	// L'appelant historique demande 160 échantillons ; le producteur en publie
	// 60 à la fois. L'adaptateur réassemble sans rien perdre.
	for (int i = 0; i < 4; i++)
		ASSERT_TRUE(pipe.PutFrame(Tone(60, 8000)));

	SWORD buffer[160];
	EXPECT_EQ(pipe.RecBuffer(buffer, 160), 160);

	pipe.End();
}

TEST(PipeAudioInput, LAdaptateurPlatDEcritureDeclareSaFrequenceParInit)
{
	PipeAudioInput pipe;
	pipe.Init(16000);
	pipe.StartRecording(8000);

	std::vector<SWORD> pcm(320, 0);
	ASSERT_TRUE(pipe.PutSamples(&pcm[0], 320));

	SamplesPtr out = pipe.RecFrame(100);
	ASSERT_TRUE(out != nullptr);
	EXPECT_EQ(out->GetRate(), 8000u);

	pipe.End();
}

/* ------------------------------------------------------------------------- *
 *                             PipeAudioOutput                               *
 * ------------------------------------------------------------------------- */

TEST(PipeAudioOutput, UneTrameEstEmpileeTelleQuelleSansConversion)
{
	PipeAudioOutput pipe(false);
	pipe.Init(8000);
	pipe.StartPlaying(8000);

	ASSERT_GT(pipe.PlayFrame(Tone(160, 8000)), 0);

	SWORD buffer[160];
	EXPECT_EQ(pipe.GetSamples(buffer, 160), 160);

	pipe.End();
}

// Même défaut n°3, côté sortie : la fréquence de la trame prime sur ce que
// StartPlaying a annoncé, qui peut le précéder.
TEST(PipeAudioOutput, LaFrequenceDeLaTramePiloteLeResampler)
{
	PipeAudioOutput pipe(false);
	pipe.Init(8000);		// fréquence de mixage
	pipe.StartPlaying(8000);	// annonce périmée

	// La trame arrive en réalité à 48 kHz : 960 échantillons = 20 ms = 160 à 8 kHz.
	ASSERT_GE(pipe.PlayFrame(Tone(960, 48000)), 0);

	SWORD buffer[4096];
	int got = pipe.GetSamples(buffer, 4096, true);
	EXPECT_GT(got, 0);
	EXPECT_LE(got, 160);	// et surtout PAS 960

	pipe.End();
}

TEST(PipeAudioOutput, LAdaptateurPlatUtiliseLaFrequenceAnnoncee)
{
	PipeAudioOutput pipe(false);
	pipe.Init(8000);
	pipe.StartPlaying(8000);

	std::vector<SWORD> pcm(160, 0);
	EXPECT_GT(pipe.PlayBuffer(&pcm[0], 160, 0), 0);

	SWORD buffer[160];
	EXPECT_EQ(pipe.GetSamples(buffer, 160), 160);

	pipe.End();
}

/* ------------------------------------------------------------------------- *
 *                        La chaîne de la conférence                         *
 * ------------------------------------------------------------------------- */

// Le mixeur écrit des tranches de 10 ms calées sur SON horloge, jamais sur la
// taille de trame du codec. L'encodeur les réassemble : c'est le seul à
// connaître numFrameSamples. Avant, l'appelant demandait numFrameSamples dans
// un tampon de 512 — 960 pour l'opus 48 kHz, soit un écrasement de pile à
// chaque trame, toujours vivant dans audiostream.cpp avant cette phase.
TEST(AudioConferenceChain, LeMixeurEcritDesTranchesDeDixMsEtLEncodeurLesReassemble)
{
	if (!AudioCodec::IsSupported(AudioCodec::OPUS))
		GTEST_SKIP() << "OPUS indisponible dans ffmpeg";

	Properties props;
	std::unique_ptr<AudioEncoder> enc(AudioCodecFactory::CreateEncoder(AudioCodec::OPUS, props));
	ASSERT_TRUE(enc != nullptr);
	ASSERT_EQ(enc->TrySetRate(48000), 48000u);
	ASSERT_EQ(enc->numFrameSamples, 960);	// 20 ms

	PipeAudioInput pipe;
	pipe.Init(8000);			// fréquence de mixage
	pipe.StartRecording(enc->GetRate());	// l'encodeur veut du 48 kHz

	// 100 tranches de 10 ms à 8 kHz = 1 s : le mixeur écrit à plat.
	std::vector<SWORD> slice(80, 0);
	int emitted = 0;

	for (int i = 0; i < 100; i++)
	{
		ASSERT_TRUE(pipe.PutSamples(&slice[0], 80));

		for (SamplesPtr s = pipe.RecFrame(0); s; s = pipe.RecFrame(0))
		{
			// La trame sort à la fréquence de l'encodeur, pas à celle du mixeur.
			EXPECT_EQ(s->GetRate(), 48000u);
			for (AudioFrame *f = enc->EncodeFrame(s); f; f = enc->EncodeFrame(NULL))
				emitted++;
		}
	}

	// 1 s de mixage = 50 trames de 20 ms, à l'amorçage du resampler près.
	EXPECT_GE(emitted, 48);
	EXPECT_LE(emitted, 50);

	pipe.End();
}

/* ------------------------------------------------------------------------- *
 *                     La chaîne du 14/08, de bout en bout                   *
 * ------------------------------------------------------------------------- */

// RTP -> décodeur opus 48 kHz -> PipeAudioInput -> encodeur speex 16 kHz -> RTP.
// C'est l'appel qui a cassé les trois contrats à la fois. Le débit doit se
// conserver : une trame de 20 ms entrée = une trame de 20 ms sortie.
TEST(AudioTranscodeChain, OpusQuaranteHuitVersSpeexSeize)
{
	if (!AudioCodec::IsSupported(AudioCodec::OPUS) ||
	    !AudioCodec::IsSupported(AudioCodec::SPEEX16))
		GTEST_SKIP() << "OPUS ou SPEEX16 indisponible dans ffmpeg";

	Properties props;
	std::unique_ptr<AudioEncoder> opusEnc(AudioCodecFactory::CreateEncoder(AudioCodec::OPUS, props));
	std::unique_ptr<AudioDecoder> opusDec(AudioCodecFactory::CreateDecoder(AudioCodec::OPUS));
	std::unique_ptr<AudioEncoder> spxEnc(AudioCodecFactory::CreateEncoder(AudioCodec::SPEEX16, props));
	ASSERT_TRUE(opusEnc && opusDec && spxEnc);
	ASSERT_EQ(opusEnc->TrySetRate(48000), 48000u);

	PipeAudioInput pipe;
	// L'encodeur speex déclare sa fréquence, le pipe convertit vers elle.
	// Personne ne connaît, ni n'a besoin de connaître, le 48 kHz de l'amont.
	DWORD spxRate = spxEnc->GetRate();
	ASSERT_EQ(spxRate, 16000u);
	pipe.StartRecording(spxRate);

	// 50 trames de 20 ms = 1 s de parole.
	const int kFrames = 50;
	int sent = 0, produced = 0;

	for (int i = 0; i < kFrames; i++)
	{
		AudioFrame *rtp = opusEnc->EncodeFrame(Tone(960, 48000));
		if (!rtp)
			continue;
		sent++;

		// Côté récepteur : décoder puis publier TOUTES les trames.
		ASSERT_GT(opusDec->Decode(rtp->GetData(), (int)rtp->GetLength()), 0);
		for (SamplesPtr s = opusDec->GetFrame(); s; s = opusDec->GetFrame())
			ASSERT_TRUE(pipe.PutFrame(s));

		// Côté émetteur : lire et réencoder.
		for (SamplesPtr s = pipe.RecFrame(0); s; s = pipe.RecFrame(0))
			for (AudioFrame *out = spxEnc->EncodeFrame(s); out; out = spxEnc->EncodeFrame(NULL))
			{
				EXPECT_GT(out->GetLength(), 0u);
				produced++;
			}
	}

	EXPECT_EQ(sent, kFrames);
	// Le débit se conserve : 50 trames entrées, 50 sorties à l'amorçage près.
	// Le 2026-08-14, cette même chaîne n'en rendait que 26 (53 %).
	EXPECT_GE(produced, kFrames - 2);
	EXPECT_LE(produced, kFrames);

	pipe.End();
}

/* ------------------------------------------------------------------------- *
 *                    La chaîne d'enregistrement (Recorder)                  *
 * ------------------------------------------------------------------------- */

// RTP opus 48 kHz -> décodeur -> encodeur AAC -> sample MP4. L'AAC exige des
// trames de 1024 échantillons, une taille qui ne correspond à aucune trame RTP :
// c'est l'encodeur qui accumule, plus le Recorder avec sa fifo à la main.
TEST(AudioRecorderChain, OpusVersAacCadenceParLEncodeur)
{
	if (!AudioCodec::IsSupported(AudioCodec::OPUS) ||
	    !AudioCodec::IsSupported(AudioCodec::AAC))
		GTEST_SKIP() << "OPUS ou AAC indisponible dans ffmpeg";

	Properties props;
	std::unique_ptr<AudioEncoder> opusEnc(AudioCodecFactory::CreateEncoder(AudioCodec::OPUS, props));
	std::unique_ptr<AudioDecoder> opusDec(AudioCodecFactory::CreateDecoder(AudioCodec::OPUS));
	ASSERT_TRUE(opusEnc && opusDec);
	ASSERT_EQ(opusEnc->TrySetRate(48000), 48000u);

	// L'encodeur AAC n'est créé qu'à la première trame décodée, à la fréquence
	// qu'elle PORTE : c'est le point que la phase 4 change dans le Recorder.
	std::unique_ptr<AudioEncoder> aacEnc;
	DWORD audioRate = 0;
	QWORD audioSamples = 0;
	int emitted = 0;
	DWORD lastTs = 0;

	// 1 s : 50 trames opus de 20 ms.
	for (int i = 0; i < 50; i++)
	{
		AudioFrame *rtp = opusEnc->EncodeFrame(Tone(960, 48000));
		if (!rtp)
			continue;

		ASSERT_GT(opusDec->Decode(rtp->GetData(), (int)rtp->GetLength()), 0);

		for (SamplesPtr s = opusDec->GetFrame(); s; s = opusDec->GetFrame())
		{
			if (!aacEnc)
			{
				audioRate = s->GetRate();
				ASSERT_EQ(audioRate, 48000u);
				char rate[16];
				snprintf(rate, sizeof(rate), "%u", audioRate);
				Properties aacProps;
				aacProps.SetProperty("aac.samplerate", rate);
				aacEnc.reset(AudioCodecFactory::CreateEncoder(AudioCodec::AAC, aacProps));
				ASSERT_TRUE(aacEnc != nullptr);
				EXPECT_EQ(aacEnc->numFrameSamples, 1024);
			}

			for (AudioFrame *f = aacEnc->EncodeFrame(s); f; f = aacEnc->EncodeFrame(NULL))
			{
				EXPECT_GT(f->GetLength(), 0u);
				lastTs = (DWORD)(audioSamples * 1000 / audioRate);
				audioSamples += aacEnc->numFrameSamples;
				emitted++;
			}
		}
	}

	// 48000 échantillons / 1024 = 46 trames AAC, au délai d'amorçage près.
	EXPECT_GE(emitted, 44);
	EXPECT_LE(emitted, 47);
	// Les horodatages couvrent bien près d'une seconde, sans dérive.
	EXPECT_GE(lastTs, 900u);
	EXPECT_LE(lastTs, 1000u);
}
