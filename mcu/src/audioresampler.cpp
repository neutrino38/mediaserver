#include "log.h"
#include "audioresampler.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

// Crée un rééchantillonneur mono S16 inputRate -> outputRate via libswresample.
// Retourne NULL si aucun rééchantillonnage n'est nécessaire (taux identiques)
// ou en cas d'erreur.
static SwrContext* OpenResampler(DWORD inputRate, DWORD outputRate)
{
	if (!inputRate || !outputRate || inputRate==outputRate)
		return NULL;

	SwrContext *swr = NULL;
	AVChannelLayout mono;
	av_channel_layout_default(&mono, 1);

	int err = swr_alloc_set_opts2(&swr,
		&mono, AV_SAMPLE_FMT_S16, (int)outputRate,	// sortie
		&mono, AV_SAMPLE_FMT_S16, (int)inputRate,	// entrée
		0, NULL);

	av_channel_layout_uninit(&mono);

	if (err < 0 || swr_init(swr) < 0)
	{
		Error("-AudioResampler: échec configuration resampler %u Hz -> %u Hz\n", inputRate, outputRate);
		if (swr) swr_free(&swr);
		return NULL;
	}
	return swr;
}

AudioResampler::AudioResampler()
{
	swr = NULL;
	inRate = 0;
	outRate = 0;
}

AudioResampler::~AudioResampler()
{
	if (swr) swr_free(&swr);
}

void AudioResampler::Reset()
{
	if (swr) swr_free(&swr);
	inRate = 0;
	outRate = 0;
}

SamplesPtr AudioResampler::Resample(SamplesPtr samples, DWORD outputRate)
{
	if (!samples)
		return nullptr;

	DWORD inputRate = samples->GetRate();

	// Fréquence cible inconnue ou identique : rien à faire, on transmet la
	// trame telle quelle (elle est immuable et partagée par référence).
	if (!outputRate || inputRate == outputRate)
		return samples;

	// La trame fait foi : si sa fréquence a changé, on rouvre le resampler.
	// C'est ce qui rend impossible le bug de fréquence périmée du 2026-08-14,
	// où le resampler restait ouvert sur la fréquence d'avant le premier paquet.
	if (!swr || inRate != inputRate || outRate != outputRate)
	{
		if (swr)
		{
			Log("-AudioResampler: %u -> %u Hz devient %u -> %u Hz, resampler rouvert\n",
				inRate, outRate, inputRate, outputRate);
			swr_free(&swr);
		}
		swr = OpenResampler(inputRate, outputRate);
		inRate = inputRate;
		outRate = outputRate;
		if (!swr)
			return nullptr;
	}

	const int cap = (int)av_rescale_rnd(
		swr_get_delay(swr, (int64_t)inputRate) + samples->GetNbSamples(),
		outputRate, (int64_t)inputRate, AV_ROUND_UP);

	SamplesPtr out = Samples::Alloc((DWORD)cap, outputRate);
	if (!out)
	{
		Error("-AudioResampler: could not allocate %d samples\n", cap);
		return nullptr;
	}

	AVFrame *dst = out->GetAVFrame();
	const uint8_t *src = (const uint8_t*)samples->GetData();
	int produced = swr_convert(swr, dst->data, cap, &src, (int)samples->GetNbSamples());
	if (produced < 0)
	{
		Error("-AudioResampler: swr_convert a echoue (%d) sur %u echantillons %u -> %u Hz\n",
		      produced, samples->GetNbSamples(), inputRate, outputRate);
		return nullptr;
	}

	//0 échantillon produit n'est PAS un échec : le convertisseur a mis
	//l'entrée en tampon parce qu'elle ne suffisait pas encore à une sortie.
	//C'est le cas d'une trame de quelques échantillons — le mixeur en écrit
	//quand deux ticks se suivent de près après un réveil tardif — et c'était
	//la source des « could not transrate » du journal (59 en 20 min le
	//2026-08-29). La trame vide dit à l'appelant « rien à faire cette fois ».
	dst->nb_samples = produced;
	dst->pts = samples->GetPTS();
	return out;
}
