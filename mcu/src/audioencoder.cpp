#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <set>
#include "log.h"
#include "tools.h"
#include "audio.h"
#include "audioencoder.h"



/**********************************
* AudioEncoderWorker
*	Constructor
***********************************/
AudioEncoderWorker::AudioEncoderWorker()
{
	//Not encoding
	encodingAudio=0;
	//Set default codec to PCMU
	audioCodec=AudioCodec::PCMU;
	//Create mutex
}

/*******************************
* ~AudioEncoderWorker
*	Destructor.
********************************/
AudioEncoderWorker::~AudioEncoderWorker()
{
	//If still running
	if (encodingAudio)
		//End
		End();
	//Destroy mutex
}

/***************************************
* SetAudioCodec
*	Fija el codec de audio
***************************************/
int AudioEncoderWorker::SetAudioCodec(AudioCodec::Type codec)
{
	//Compromabos que soportamos el modo
	if (!(codec==AudioCodec::PCMA || codec==AudioCodec::GSM || codec==AudioCodec::PCMU))
		return 0;

	//Colocamos el tipo de audio
	audioCodec = codec;

	Log("-SetAudioCodec [%d,%s]\n",audioCodec,AudioCodec::GetNameFor(audioCodec));

	//Y salimos
	return 1;
}

/***************************************
* Init
*	Inicializa los devices
***************************************/
int AudioEncoderWorker::Init(AudioInput *input)
{
	Log(">Init audio encoder\n");

	//Nos quedamos con los puntericos
	audioInput  = input;

	//Y aun no estamos mandando nada
	encodingAudio=0;

	Log("<Init audio encoder\n");

	return 1;
}

/***************************************
* startencodingAudio
*	Helper function
***************************************/
/***************************************
* StartSending
*	Comienza a mandar a la ip y puertos especificados
***************************************/
int AudioEncoderWorker::StartEncoding()
{
	Log(">Start encoding audio\n");

	//Si estabamos mandando tenemos que parar
	if (encodingAudio)
		//paramos
		StopEncoding();

	encodingAudio=1;

	//Start thread
	StartThread();

	Log("<StartSending audio [%d]\n",encodingAudio);

	return 1;
}
/***************************************
* End
*	Termina la conferencia activa
***************************************/
int AudioEncoderWorker::End()
{
	//Terminamos de enviar
	StopEncoding();

	return 1;
}


/***************************************
* StopEncoding
* 	Termina el envio
****************************************/
int AudioEncoderWorker::StopEncoding()
{
	Log(">StopEncoding Audio\n");

	//Esperamos a que se cierren las threads de envio
	if (encodingAudio)
	{
		//paramos
		encodingAudio=0;

		//Cancel any pending audio
		audioInput->CancelRecFrame();

		//Y esperamos
		StopThread();
	}

	Log("<StopEncoding Audio\n");

	return 1;
}



/*******************************************
* Encode
*	Capturamos el audio y lo mandamos
*******************************************/
int AudioEncoderWorker::Encode()
{
        struct timeval 	before;
	AudioEncoder* 	codec;
	DWORD		frameTime=0;

	Log(">Encode Audio\n");

	//Obtenemos el tiempo ahora
	gettimeofday(&before,NULL);

	//Creamos el codec de audio
	if ((codec = AudioCodecFactory::CreateEncoder(audioCodec))==NULL)
	{
		Log("Error en el envio de audio,saliendo\n");
		return 0;
	}

	//Try to set native rate
	DWORD rate = codec->TrySetRate(audioInput->GetNativeRate());

	//Create audio frame
	AudioFrame frame(audioCodec,rate);

	//Empezamos a grabar
	audioInput->StartRecording(rate);

	//Mientras tengamos que capturar
	while(encodingAudio)
	{
		//Capturamos. La trame arrive à la taille que le mixeur a écrite ;
		//c'est l'encodeur qui la redécoupe. L'ancien tampon de 512 lisait
		//numFrameSamples échantillons alors qu'on n'en avait demandé que 160.
		SamplesPtr samples = audioInput->RecFrame(1000);
		if (!samples)
			//Skip and probably exit
			continue;

		if (!codec)
			continue;

		//Une trame d'entrée peut en remplir plusieurs : on purge à chaque fois.
		for (AudioFramePtr encoded = codec->EncodeFrame(samples);
		     encoded; encoded = codec->EncodeFrame(NULL))
		{
			if (encoded->GetLength() > frame.GetMaxMediaLength())
			{
				Error("-AudioEncoder: frame of %u bytes exceeds the media buffer\n",
				      encoded->GetLength());
				continue;
			}

			memcpy(frame.GetData(),encoded->GetData(),encoded->GetLength());

			//Set frame length
			frame.SetLength(encoded->GetLength());

			//Set frame time
			frame.SetTimestamp(frameTime);

			//Set frame duration
			frame.SetDuration(codec->numFrameSamples);

			//Clear rtp
			frame.ClearRTPPacketizationInfo();

			//Add rtp packet
			frame.AddRtpPacket(0,encoded->GetLength(),NULL,0,false);

			//L'horloge n'avance que sur ce qui est réellement produit.
			frameTime += codec->numFrameSamples;

			//Lock
			std::unique_lock<std::mutex> mutexLock(mutex);

			//For each listener
			for (Listeners::iterator it=listeners.begin(); it!=listeners.end(); ++it)
			{
				//Get listener
				MediaFrame::Listener* listener =  *it;
				//If was not null
				if (listener)
					//Call listener
					listener->onMediaFrame(frame);
			}

			//unlock
			mutexLock.unlock();
		}
	}

	Log("-Encode Audio cleanup[%d]\n",encodingAudio);

	//Paramos de grabar por si acaso
	audioInput->StopRecording();

	//Logeamos
	Log("-Deleting codec\n");

	//Borramos el codec
	delete codec;

	//Salimos
        Log("<Encode Audio\n");
	
	pthread_exit(0);
}

bool AudioEncoderWorker::AddListener(MediaFrame::Listener *listener)
{
	//Lock
	std::unique_lock<std::mutex> mutexLock(mutex);

	//Add to set
	listeners.insert(listener);

	//unlock
	mutexLock.unlock();

	return true;
}

bool AudioEncoderWorker::RemoveListener(MediaFrame::Listener *listener)
{
	//Lock
	std::unique_lock<std::mutex> mutexLock(mutex);

	//Search
	Listeners::iterator it = listeners.find(listener);

	//If found
	if (it!=listeners.end())
		//Erase it
		listeners.erase(it);

	//Unlock
	mutexLock.unlock();

	return true;
}
