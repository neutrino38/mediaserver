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
	//Creamos el mutex
	pthread_mutex_init(&mutex,0);

 	//Y la condicion
	pthread_cond_init(&cond,0);

	//Init
	inited = false;
	recording = false;
	canceled = false;
	swr = NULL;
}

PipeAudioInput::~PipeAudioInput()
{
	//Creamos el mutex
	pthread_mutex_destroy(&mutex);

 	//Y la condicion
	pthread_cond_destroy(&cond);

	//Libère le resampler éventuel
	if (swr) swr_free(&swr);
}

int PipeAudioInput::RecBuffer(SWORD *buffer,DWORD size)
{
	int len = 0;

	//Bloqueamos
	pthread_mutex_lock(&mutex);

	//Mientras no tengamos suficientes muestras
	while(recording && (fifoBuffer.length()<size))
	{
		//Esperamos la condicion
		pthread_cond_wait(&cond,&mutex);

		//If we have been canceled
		if (canceled)
		{
			//Remove flag
			canceled = false;
			//Exit
			Log("PipeAudioInput: RecBuffer cancelled.\n");
			//End
			goto end;
		}
	}

	//Get samples from queue
	len = fifoBuffer.pop(buffer,size);

end:
	//Desbloqueamos
	pthread_mutex_unlock(&mutex);

	return len;
}

int PipeAudioInput::StartRecording(DWORD rate)
{
	Log("-PipeAudioInput start recording [rate:%d Hz]\n",rate);

	//Bloqueamos
	pthread_mutex_lock(&mutex);

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
	//Desbloqueamos
	pthread_mutex_unlock(&mutex);

	return true;
}

int PipeAudioInput::StopRecording()
{
	Log("-PipeAudioInput stop recording\n");
	
	//Bloqueamos
	pthread_mutex_lock(&mutex);

	//Estamos grabando
	recording = false;

	//Se�alamos
	pthread_cond_signal(&cond);

	//Desbloqueamos
	pthread_mutex_unlock(&mutex);

	return true;
}

int PipeAudioInput::PutSamples(SWORD *buffer,DWORD size)
{
	SWORD resampled[4096];

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

	//Block
	pthread_mutex_lock(&mutex);

	//Si estamos reproduciendo
	if (recording)
	{
		//Si no cabe
		if (fifoBuffer.length()+size>fifoBuffer.size())
			//Limpiamos
			fifoBuffer.clear();

		//Encolamos
		fifoBuffer.push(buffer,size);

		//Se�alamos
		pthread_cond_signal(&cond);
	}

	//Desbloqueamos
	pthread_mutex_unlock(&mutex);

	//Salimos
	return true;

}

int PipeAudioInput::Init(DWORD rate)
{
	Log("-PipeAudioInput init [rate:%d]\n",rate);
	
	//Protegemos
	pthread_mutex_lock(&mutex);

	//Iniciamos
	inited = true;

	//Store native sample rate
	nativeRate = rate;
	
	//Desprotegemos
	pthread_mutex_unlock(&mutex);

	return true;
}

int PipeAudioInput::End()
{
	//Protegemos
	pthread_mutex_lock(&mutex);

	//No estamos iniciados
	inited = false;
	recording = false;
	fifoBuffer.clear();
	
	//Terminamos
	pthread_cond_signal(&cond);

	//Desprotegemos
	pthread_mutex_unlock(&mutex);

	if (swr) swr_free(&swr);

	//Salimos
	return true;
}

void  PipeAudioInput::CancelRecBuffer()
{
	//Protegemos
	pthread_mutex_lock(&mutex);

	//Cancel
	canceled = true;

	//Se�alamos
	pthread_cond_signal(&cond);

	//Unloco mutex
	pthread_mutex_unlock(&mutex);
}
