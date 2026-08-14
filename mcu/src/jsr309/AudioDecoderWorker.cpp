/* 
 * File:   AudioDecoderWorker.cpp
 * Author: Sergio
 * 
 * Created on 4 de octubre de 2011, 20:06
 */
#include "log.h"
#include "AudioDecoderWorker.h"
#include "rtp.h"

AudioDecoderJoinableWorker::AudioDecoderJoinableWorker()
{
	//Nothing
	output = NULL;
    input = NULL;
}

AudioDecoderJoinableWorker::~AudioDecoderJoinableWorker()
{
	End();
}

int AudioDecoderJoinableWorker::Init(AudioOutput *output)
{
	//Store it
	this->output = output;
        if (input)
        {
            input->StopRecording();
            input = NULL;
        }
        return 1;
}

int AudioDecoderJoinableWorker::Init(PipeAudioInput * input)
{
    if (input)
    {
        // We switch to use pipe audio input instead of audio output
        if (output)
        {
            output->StopPlaying();
            output = NULL;
        }
        //Store it
		this->input = input;
        return input->Init(16000);
    }
    else
    {
        // We do not tuch the output but we stop the previous pipe if needed
        if (this->input)
        {
            this->input->StopRecording();
            this->input = NULL;
        }
        return 1;
    }
}

int AudioDecoderJoinableWorker::End()
{
	//Dettach
	Dettach();

	//Check if already decoding
	if (IsThreadRunning()) Stop();

	//Set null
	output = NULL;
	return 0;
}

int AudioDecoderJoinableWorker::Start()
{
	Log("-StartAudioDecoderJoinableWorker\n");

	//Check
	if (!output && !input)
		//Exit
		return Error("null audio outputi/input not starting");


	//Stop first
	Stop();


	//Rearmer la file apres un eventuel Stop (Cancel collant)
	packets.Reset();

	//launc thread
	StartThread();

	return 1;
}

int  AudioDecoderJoinableWorker::Stop()
{
	Log(">StopAudioDecoderJoinableWorker\n");

	//If we were started
	if (IsThreadRunning())
	{
		//Cancel any pending wait
		packets.Cancel();

		//Esperamos
		StopThread();
	}

	Log("<StopAudioDecoderJoinableWorker\n");

	return 1;
}


int AudioDecoderJoinableWorker::Decode()
{
	//8192 : une trame de 20 ms à 48 kHz fait 960 échantillons, 120 ms (opus)
	//jusqu'à 5760. L'ancien 512 tronquait chaque trame 48 kHz : Decode borne à
	//outLen et retient le reste en fifo, donc seuls 512 échantillons sur 960
	//sortaient par paquet — débit utile à 53 %, latence croissante, et l'aval
	//famélique (mesuré le 2026-08-14 : 25 paquets/s au lieu de 50 vers le pair).
	SWORD		raw[8192];
	DWORD		rawSize=8192;
	AudioDecoder*	codec=NULL;
	DWORD		frameTime=0;
	DWORD		lastTime=0;

	Log(">JSR309 DecodeAudio\n");


	//Mientras tengamos que capturar
	while (IsThreadRunning())
	{
		//Get packet in queue
		RTPPacket* packet = packets.Pop(5000);
		
		//Check
		if (!packet)
        {
			if (packets.IsCanceled()) break;
			//Timeout : continue
			continue;
		}

		//Comprobamos el tipo
		if ( codec==NULL  || packet->GetCodec()!= codec->type )
		{
			//Si habia uno nos lo cargamos
			if (codec!=NULL)
            {
                if (input) input->StopRecording();
                if (output) output->StopPlaying();
			    delete codec;
            }

			//Creamos uno dependiendo del tipo
            codec = AudioCodecFactory::CreateDecoder((AudioCodec::Type)packet->GetCodec());
			if ( codec != NULL )
            {
                DWORD rate = (output) ? output->GetNativeRate() : 16000;
                rate = codec->TrySetRate(rate);
                
				if (input) input->Init(rate);
				if (output) output->StartPlaying(rate);
            }
            else
            {
				Error("Failed to open %s audio decoder\n", AudioCodec::GetNameFor((AudioCodec::Type)packet->GetCodec()));
				delete packet;
				break;
            }
			
		}

		//Lo decodificamos
		int len = codec->Decode(packet->GetMediaData(),packet->GetMediaLength(),raw,rawSize);

		//Obtenemos el tiempo del frame
		frameTime = packet->GetTimestamp() - lastTime;

		//Actualizamos el ultimo envio
		lastTime = packet->GetTimestamp();

		//Y lo reproducimos
		if (output != NULL) output->PlayBuffer(raw,len,frameTime);
        if (input != NULL) input->PutSamples(raw, len);

		//Delete packet
		delete(packet);
	}

	//End reproducing
	if (output != NULL) output->StopPlaying();
    if (input != NULL) input->End();

	//If a decoder is created, delete it
	if (codec!=NULL) delete codec;		
	
	Log("<DecodeAudio\n");
	return 0;
}

void AudioDecoderJoinableWorker::onRTPPacket(RTPPacket &packet)
{
	//Put it on the queue
	packets.Add(packet.Clone());
}

void AudioDecoderJoinableWorker::onResetStream()
{
	//Clean all packets
	packets.Clear();
}

void AudioDecoderJoinableWorker::onEndStream()
{
	//Stop decoding
	Stop();
	//Not joined anymore
	joined.reset();
}

int AudioDecoderJoinableWorker::Attach(const std::shared_ptr<Joinable> & join)
{
	//Detach if joined — lock() : source encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		//Stop
		Stop();
		//Remove ourself as listeners
		j->RemoveListener(this);
	}
	//Store new one (lien retour non possédant)
	joined = join;
	//If it is not null
	if (join)
	{
		//Start
		Start();
		//Join to the new one
		join->AddListener(this);
	}
	//OK
	return 1;
}

int AudioDecoderJoinableWorker::Dettach()
{
        //Detach if joined — lock() : ne déréférence pas si la source a disparu
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		//Stop decoding
		Stop();
		//Remove ourself as listeners
		j->RemoveListener(this);
	}
	else
		//Stop decoding même si la source est déjà partie
		Stop();

	//Not joined anymore
	joined.reset();
	return 0;
}
