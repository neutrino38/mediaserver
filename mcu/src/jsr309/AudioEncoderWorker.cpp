/* 
 * File:   AudioEncoderWorker.cpp
 * Author: Sergio
 * 
 * Created on 4 de octubre de 2011, 20:42
 */

#include <set>
#include "log.h"
#include "AudioEncoderWorker.h"

AudioEncoderMultiplexerWorker::AudioEncoderMultiplexerWorker()
{
	//Nothing
	input = NULL;
	encoding = false;
	codec = (AudioCodec::Type)-1;
}

AudioEncoderMultiplexerWorker::~AudioEncoderMultiplexerWorker()
{
	End();
}

int AudioEncoderMultiplexerWorker::Init(AudioInput *input)
{
	//Store it
	this->input = input;
        return 1;
}
int AudioEncoderMultiplexerWorker::SetCodec(AudioCodec::Type codec)
{
	//Colocamos el tipo de audio
	this->codec = codec;


	//Check
	if (!listeners.empty() && !encoding)
        {
		//Start
		Start();
        }

	return 1;
}

//Phase 5 (nego_fmtp §6.3) : mêmes règles que le worker vidéo — les Properties ne
//sont lues qu'à la création de l'encodeur, donc des bornes qui changent sur un
//encodeur ouvert exigent un cycle Stop/Start.
void AudioEncoderMultiplexerWorker::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	//Bornes identiques : ne pas redémarrer pour rien.
	if (negotiated == byCodec)
		return;

	negotiated = byCodec;

	Log("-AudioEncoder: negotiated codec properties updated [%d codec(s)]\n",
	    (int)byCodec.size());

	if (encoding)
	{
		Stop();
		if (!listeners.empty())
		{
			Log("-AudioEncoder: restarted encoder with negotiated properties.\n");
			Start();
		}
	}
}

int AudioEncoderMultiplexerWorker::Start()
{
	//Check
	if (!input)
		//Exit
		return Error("null audio input");

	//Check if need to restart
	if (encoding)
		//Stop first
		Stop();

	//Start decoding
	encoding = 1;

	//launc thread
	StartThread();

	return 1;
}

int AudioEncoderMultiplexerWorker::Stop()
{
	Log(">Stop AudioEncoderMultiplexerWorker\n");

	//If we were started
	if (encoding)
	{
		//Stop
		encoding=0;

		//Stop any pending grab
		input->CancelRecBuffer();

		//Esperamos
		StopThread();
	}

	Log("<Stop AudioEncoderMultiplexerWorker\n");

	return 1;
}

int AudioEncoderMultiplexerWorker::End()
{
	//Check if already decoding
	if (encoding)
		//Stop
		Stop();

	//Set null
	input = NULL;
	return 0;
}


/*******************************************
* SendAudio
*	Capturamos el audio y lo mandamos
*******************************************/
int AudioEncoderMultiplexerWorker::Encode()
{
	RTPPacket	packet(MediaFrame::Audio,codec,codec);
	SWORD 		recBuffer[512];
	//NULL explicite : ce pointeur était lu non initialisé au premier tour de boucle.
	AudioEncoder* 	encoder = NULL;
	DWORD		frameTime=0;

	Log(">Encode AudioEncoderMultiplexerWorker [%d,%s]\n",codec,AudioCodec::GetNameFor(codec));

	DWORD clock;
        DWORD rate;
	//Get ts multiplier
	float multiplier;

	//Mientras tengamos que capturar
	while(encoding)
	{
            if (encoder == NULL || encoder->type != codec)
            {
                if (encoder) delete encoder;

                //Phase 5 : les bornes négociées de la patte émettrice s'appliquent
                //à l'ouverture (Opus : FEC/DTX/CBR/maxaveragebitrate du pair).
                Properties effective;
                std::map<int,Properties>::const_iterator itNeg = negotiated.find((int)codec);
                if (itNeg != negotiated.end())
                {
                    effective = itNeg->second;
                    Log("-AudioEncoder: opening with negotiated properties for %s [%d key(s)]\n",
                        AudioCodec::GetNameFor(codec), (int)itNeg->second.size());
                }

                encoder = AudioCodecFactory::CreateEncoder(codec, effective);
                if (encoder == NULL)
                    return Error("Could not create codec\n");

                input->StartRecording(encoder->GetRate());
                Log("-JSR309 AudioEncoder: Started audio encoder %s at %d Hz.\n",
                    AudioCodec::GetNameFor(codec), encoder->GetRate());
                clock = encoder->GetClockRate();
                packet.SetClockRate(clock);
                
                rate = encoder->TrySetRate(input->GetNativeRate());
                multiplier = (float) clock/ (float) rate;
            }
		//Incrementamos el tiempo de envio
		frameTime += encoder->numFrameSamples*multiplier;

		//Capturamos
		if (input->RecBuffer(recBuffer,encoder->numFrameSamples)==0)
                {
                    msleep(1000);
                    continue;
                }


		//Lo codificamos
		int len = encoder->Encode(recBuffer,encoder->numFrameSamples,
                                          packet.GetMediaData(),packet.GetMaxMediaLength());

		//Comprobamos que ha sido correcto
		if(len<=0)
			continue;

		//Set frame time
		packet.SetTimestamp(frameTime);
		//Set length
		packet.SetMediaLength(len);
		
		//Multiplex it
		Multiplex(packet);
	}

	Log("-SendAudio cleanup[%d]\n",encoding);

	//Paramos de grabar por si acaso
	input->StopRecording();

	//Logeamos
	Log("-Deleting codec\n");

	//Borramos el codec
	delete encoder;

	//Salimos
        Log("<SendAudio\n");
	return 0;
}

void AudioEncoderMultiplexerWorker::AddListener(Listener *listener)
{
	//Check if we were already encoding
	if (listener && !encoding && codec!=-1)
		//Start encoding;
		Start();
	//Add the listener
	RTPMultiplexer::AddListener(listener);
}

void AudioEncoderMultiplexerWorker::RemoveListener(Listener *listener)
{
	//Remove the listener
	RTPMultiplexer::RemoveListener(listener);
	//If there are no more
	if (listeners.empty())
		//Stop encoding
		Stop();
}

void AudioEncoderMultiplexerWorker::Update()
{
}
