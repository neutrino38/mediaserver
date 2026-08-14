#include "log.h"
#include "pipeaudioinput.h"

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
		Error("-PipeAudioInput: échec configuration resampler %u Hz -> %u Hz\n", inputRate, outputRate);
		if (swr) swr_free(&swr);
		return NULL;
	}
	return swr;
}

PipeAudioInput::PipeAudioInput()
{
	//Init (le mutex et la condition sont construits par la std lib)
	inited = false;
	recording = false;
	canceled = false;
	swr = NULL;
}

PipeAudioInput::~PipeAudioInput()
{
	//Libère le resampler éventuel
	if (swr) swr_free(&swr);
}

int PipeAudioInput::RecBuffer(SWORD *buffer,DWORD size)
{
	int len = 0;

	//Bloqueamos
	std::unique_lock<std::mutex> lock(mutex);

	//Mientras no tengamos suficientes muestras
	while(recording && (fifoBuffer.length()<size))
	{
		//Esperamos la condicion
		cond.wait(lock);

		//If we have been canceled
		if (canceled)
		{
			//Remove flag
			canceled = false;
			//Exit
			Log("PipeAudioInput: RecBuffer cancelled.\n");
			//End (le lock est relâché par le RAII)
			return len;
		}
	}

	//Get samples from queue
	len = fifoBuffer.pop(buffer,size);

	return len;
}

int PipeAudioInput::StartRecording(DWORD rate)
{
	Log("-PipeAudioInput start recording [rate:%d Hz]\n",rate);

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

        if (swr)
        {
            swr_free(&swr);
            fifoBuffer.clear();
        }

	//Store recording rate
	recordRate = rate;
	//Open resampler (NULL si aucun rééchantillonnage nécessaire)
	swr = OpenResampler( nativeRate, recordRate );
	//Estamos grabando
	recording = true;

	return true;
}

int PipeAudioInput::StopRecording()
{
	Log("-PipeAudioInput stop recording\n");
	
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Estamos grabando
	recording = false;

	//Señalamos
	cond.notify_one();

	return true;
}

int PipeAudioInput::PutSamples(SWORD *buffer,DWORD size)
{
	SWORD resampled[4096];

	//Block (protège aussi swr contre StartRecording/End concurrents)
	std::lock_guard<std::mutex> lock(mutex);

	//If we need to transrate
	if (swr)
	{
		//Resample (mono S16) via libswresample
		uint8_t *outp = (uint8_t*)resampled;
		const uint8_t *inp = (const uint8_t*)buffer;
		int produced = swr_convert(swr, &outp, 4096, &inp, (int)size);
		if (produced < 0)
			//Error
			return Error("-PipeAudioInput could not transrate\n");
		//Swith input parameters to resample ones
		buffer = resampled;
		size = (DWORD)produced;
	}

	//Si estamos reproduciendo
	if (recording)
	{
		//Si no cabe
		if (fifoBuffer.length()+size>fifoBuffer.size())
			//Limpiamos
			fifoBuffer.clear();

		//Encolamos
		fifoBuffer.push(buffer,size);

		//Señalamos
		cond.notify_one();
	}

	//Salimos
	return true;

}

int PipeAudioInput::Init(DWORD rate)
{
	Log("-PipeAudioInput init [rate:%d]\n",rate);

	//Protegemos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Iniciamos
		inited = true;

		//Le décodeur ne connaît sa fréquence qu'au PREMIER paquet reçu, donc
		//souvent APRÈS le StartRecording de l'encodeur : le resampler a alors
		//été ouvert sur l'ancienne fréquence native. S'il ne suit pas, les
		//échantillons 48 kHz du décodeur sont lus comme du 16 kHz — audio 3x
		//trop rapide, mesuré en trafic le 2026-08-14. Un changement de
		//fréquence d'écriture en cours d'enregistrement rouvre le resampler.
		if (recording && nativeRate != rate)
		{
			if (swr)
				swr_free(&swr);
			fifoBuffer.clear();
			nativeRate = rate;
			swr = OpenResampler(nativeRate, recordRate);
		}
		else
			//Store native sample rate
			nativeRate = rate;
	}

	return true;
}

int PipeAudioInput::End()
{
	//Protegemos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//No estamos iniciados
		inited = false;
		recording = false;
		fifoBuffer.clear();

		//Libère le resampler sous mutex (PutSamples l'utilise sous ce même mutex)
		if (swr) swr_free(&swr);

		//Terminamos
		cond.notify_one();
	}

	//Salimos
	return true;
}

void  PipeAudioInput::CancelRecBuffer()
{
	//Protegemos
	std::lock_guard<std::mutex> lock(mutex);

	//Cancel
	canceled = true;

	//Señalamos
	cond.notify_one();
}
