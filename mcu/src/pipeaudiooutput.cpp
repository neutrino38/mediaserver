#include "log.h"
#include "pipeaudiooutput.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

// Crée un rééchantillonneur mono S16 inputRate -> outputRate via libswresample
// (remplace l'ancien AudioTransrater/speexdsp). Retourne NULL si aucun
// rééchantillonnage n'est nécessaire (taux identiques) ou en cas d'erreur.
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
		Error("-PipeAudioOutput: échec configuration resampler %u Hz -> %u Hz\n", inputRate, outputRate);
		if (swr) swr_free(&swr);
		return NULL;
	}
	return swr;
}

PipeAudioOutput::PipeAudioOutput(bool calcVAD)
{
	//Store vad flag
	this->calcVAD = calcVAD;
	//No vad score acumulated
	acu = 0;
	//No rates yet
	nativeRate = 0;
	playRate = 0;
	//No resampler yet
	swr = NULL;
	//(le mutex est construit par la std lib)
}

PipeAudioOutput::~PipeAudioOutput()
{
	//Libère le resampler éventuel
	if (swr) swr_free(&swr);
}

int PipeAudioOutput::PlayBuffer(SWORD *buffer,DWORD size,DWORD frameTime)
{
	SWORD resampled[4096];
	int v = -1;

	//Bloqueamos (protège aussi swr/playRate contre StartPlaying/StopPlaying concurrents)
	std::lock_guard<std::mutex> lock(mutex);

	//Check if we need to calculate it
	if (calcVAD && vad.IsRateSupported(playRate))
		//Calculate vad
		v = vad.CalcVad(buffer,size,playRate)*size;

	//Check if we are transtrating
	if (swr)
	{
		//Resample (mono S16) via libswresample
		uint8_t *outp = (uint8_t*)resampled;
		const uint8_t *inp = (const uint8_t*)buffer;
		int produced = swr_convert(swr, &outp, 4096, &inp, (int)size);
		if (produced < 0)
			//Error
			return Error("-PipeAudioOutput could not transrate\n");

		//Check if we need to calculate it
		if (calcVAD && v<0 && vad.IsRateSupported(nativeRate))
			//Calculate vad
			v = vad.CalcVad(resampled,produced,nativeRate)*produced;

		//Update parameters
		buffer = resampled;
		size = (DWORD)produced;
	}

	//Get left space
	int left = fifoBuffer.size()-fifoBuffer.length();

	//if not enought
	if (size>left)
	{
		Log("-PipeAudioOutput: too much data in audio buffer. buf len = %d, to add = %lu\n", fifoBuffer.length(), size);
		//Free space
		fifoBuffer.remove(size-left);
	}

	//Get initial bump
	if (!acu && v)
		//Two seconds minimum
		acu+=16000;
	//Acumule VAD
	acu += v;

	//Check max
	if (acu>48000)
		//Limit so it can timeout faster
		acu = 48000;

	//Metemos en la fifo
	fifoBuffer.push(buffer,size);

	return (size <= left) ? size : -2;
}

int PipeAudioOutput::StartPlaying(DWORD rate)
{
	Log("-PipeAudioOutput start playing [rate:%d Hz]\n",rate);

	//Lock
	std::lock_guard<std::mutex> lock(mutex);

	//Store play rate
	playRate = rate;

	//If we already had an open resampler
	if (swr)
		//Close it
		swr_free(&swr);

	//if rates are different (OpenResampler retourne NULL si égaux)
	swr = OpenResampler(playRate,nativeRate);

	//Exit
	return true;
}

int PipeAudioOutput::StopPlaying()
{
	Log("-PipeAudioOutput stop playing\n");

	//Lock
	std::lock_guard<std::mutex> lock(mutex);
	//Close resampler
	if (swr) swr_free(&swr);

	//Exit
	return true;
}

int PipeAudioOutput::GetSamples(SWORD *buffer,DWORD num,bool min_len)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Obtenemos la longitud
	int len = fifoBuffer.length();

	//Miramos si hay suficientes
	if (len > num)
		len = num;
	else if (len < num && !min_len)
		//Le lock est relâché par le RAII
		return 0;

	//OBtenemos las muestras
	fifoBuffer.pop(buffer,len);

	//Salimos
	return len;
}
int PipeAudioOutput::Init(DWORD rate)
{
	Log("-PipeAudioOutput init [rate:%d Hz]\n",rate);

	//Protegemos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Store play rate
		nativeRate = rate;

		//Iniciamos
		inited = true;
	}

	return true;
}

int PipeAudioOutput::End()
{
	//Protegemos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Terminamos
		inited = false;
	}

	return true;
}

DWORD PipeAudioOutput::GetVAD(DWORD numSamples)
{
	//Protegemos
	std::lock_guard<std::mutex> lock(mutex);
	//Get vad value
	DWORD r = acu;
	//Check
	if (acu<numSamples)
		//No vad
		acu = 0;
	else
		//Remove cumulative value
		acu -= numSamples;

	//Return
	return r;
}
