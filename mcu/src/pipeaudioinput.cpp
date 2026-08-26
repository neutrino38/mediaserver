#include "log.h"
#include "pipeaudioinput.h"

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
		Error("-PipeAudioInput: échec configuration resampler %u Hz -> %u Hz\n", inputRate, outputRate);
		if (swr) swr_free(&swr);
		return NULL;
	}
	return swr;
}

PipeAudioInput::PipeAudioInput()
{
	inited = false;
	recording = false;
	canceled = false;
	swr = NULL;
	swrInRate = 0;
	recordRate = 0;
	nativeRate = 0;
}

PipeAudioInput::~PipeAudioInput()
{
	if (swr) swr_free(&swr);
}

DWORD PipeAudioInput::QueuedMs() const
{
	DWORD ms = 0;
	for (std::deque<SamplesPtr>::const_iterator it=queue.begin(); it!=queue.end(); ++it)
	{
		DWORD rate = (*it)->GetRate();
		if (rate) ms += (*it)->GetNbSamples() * 1000 / rate;
	}
	return ms;
}

SamplesPtr PipeAudioInput::Resample(SamplesPtr samples)
{
	DWORD inRate = samples->GetRate();

	// Fréquence cible inconnue ou identique : rien à faire, on transmet la
	// trame telle quelle (elle est immuable et partagée par référence).
	if (!recordRate || inRate == recordRate)
		return samples;

	// La trame fait foi : si sa fréquence a changé, on rouvre le resampler.
	// C'est ce qui rend impossible le bug de fréquence périmée du 2026-08-14,
	// où le resampler restait ouvert sur la fréquence d'avant le premier paquet.
	if (!swr || swrInRate != inRate)
	{
		if (swr)
		{
			Log("-PipeAudioInput: fréquence d'entrée %u -> %u Hz, resampler rouvert\n",
				swrInRate, inRate);
			swr_free(&swr);
		}
		swr = OpenResampler(inRate, recordRate);
		swrInRate = inRate;
		if (!swr)
			return NULL;
	}

	const int cap = (int)av_rescale_rnd(
		swr_get_delay(swr, (int64_t)inRate) + samples->GetNbSamples(),
		recordRate, (int64_t)inRate, AV_ROUND_UP);

	SamplesPtr out = Samples::Alloc((DWORD)cap, recordRate);
	if (!out)
	{
		Error("-PipeAudioInput: could not allocate %d samples\n", cap);
		return NULL;
	}

	AVFrame *dst = out->GetAVFrame();
	const uint8_t *src = (const uint8_t*)samples->GetData();
	int produced = swr_convert(swr, dst->data, cap, &src, (int)samples->GetNbSamples());
	if (produced <= 0)
		return NULL;

	dst->nb_samples = produced;
	dst->pts = samples->GetPTS();
	return out;
}

SamplesPtr PipeAudioInput::RecFrame(DWORD timeoutMs)
{
	std::unique_lock<std::mutex> lock(mutex);

	if (!cond.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			[this]{ return !recording || canceled || !queue.empty(); }))
		//Timeout : rien n'est arrivé
		return NULL;

	if (canceled)
	{
		canceled = false;
		Log("PipeAudioInput: RecFrame cancelled.\n");
		return NULL;
	}

	if (queue.empty())
		return NULL;

	SamplesPtr samples = queue.front();
	queue.pop_front();
	return samples;
}

int PipeAudioInput::StartRecording(DWORD rate)
{
	Log("-PipeAudioInput start recording [rate:%d Hz]\n",rate);

	std::lock_guard<std::mutex> lock(mutex);

	//Changer de fréquence de sortie invalide ce qui est en file et le resampler.
	if (recordRate != rate)
	{
		if (swr) swr_free(&swr);
		swrInRate = 0;
		queue.clear();
	}

	recordRate = rate;
	recording = true;

	return true;
}

int PipeAudioInput::StopRecording()
{
	Log("-PipeAudioInput stop recording\n");

	std::lock_guard<std::mutex> lock(mutex);

	recording = false;

	cond.notify_one();

	return true;
}

int PipeAudioInput::PutFrame(SamplesPtr samples)
{
	if (!samples || samples->GetNbSamples() == 0)
		return 0;

	std::lock_guard<std::mutex> lock(mutex);

	//Personne n'écoute : inutile de rééchantillonner.
	if (!recording)
		return true;

	SamplesPtr out = Resample(samples);
	if (!out)
		return Error("-PipeAudioInput could not transrate\n");

	//Débordement : on vide plutôt que de bloquer le producteur (politique
	//historique), la latence primant sur l'intégrité du flux.
	if (QueuedMs() + out->GetNbSamples()*1000/out->GetRate() > MaxQueuedMs)
	{
		Log("-PipeAudioInput: file pleine (%u ms), on la vide\n", QueuedMs());
		queue.clear();
	}

	queue.push_back(out);

	cond.notify_one();

	return true;
}

int PipeAudioInput::Init(DWORD rate)
{
	Log("-PipeAudioInput init [rate:%d]\n",rate);

	std::lock_guard<std::mutex> lock(mutex);

	inited = true;
	//Ne sert plus qu'à PutSamples, qui n'a pas d'autre moyen de dire sa
	//fréquence. Les producteurs migrés la portent sur leurs trames.
	nativeRate = rate;

	return true;
}

int PipeAudioInput::PutSamples(SWORD *buffer,DWORD size)
{
	if (!buffer || size == 0)
		return 0;

	DWORD rate = nativeRate;
	if (!rate)
		return Error("-PipeAudioInput: rate unknown, Init() missing\n");

	SamplesPtr samples = Samples::FromBuffer(buffer, size, rate);
	if (!samples)
		return Error("-PipeAudioInput: could not allocate %u samples\n", size);

	return PutFrame(samples);
}

int PipeAudioInput::End()
{
	{
		std::lock_guard<std::mutex> lock(mutex);

		inited = false;
		recording = false;
		queue.clear();

		//Libère le resampler sous mutex (PutFrame l'utilise sous ce même mutex)
		if (swr) swr_free(&swr);
		swrInRate = 0;

		cond.notify_one();
	}

	return true;
}

void PipeAudioInput::CancelRecFrame()
{
	std::lock_guard<std::mutex> lock(mutex);

	canceled = true;

	cond.notify_one();
}
