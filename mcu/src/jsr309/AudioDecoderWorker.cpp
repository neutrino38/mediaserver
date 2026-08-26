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
	AudioDecoder*	codec=NULL;

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
                //Le décodeur restitue à la fréquence native du flux, qu'il ne
                //connaît qu'après son premier paquet. Personne n'a plus à
                //l'annoncer : chaque trame décodée porte la sienne.
                if (output) output->StartPlaying(codec->GetRate());
            }
            else
            {
				Error("Failed to open %s audio decoder\n", AudioCodec::GetNameFor((AudioCodec::Type)packet->GetCodec()));
				delete packet;
				break;
            }
			
		}

		//Lo decodificamos
		codec->Decode(packet->GetMediaData(),packet->GetMediaLength());

		//Un paquet peut donner plusieurs trames : les publier TOUTES. L'ancien
		//appel unique en perdait — 53 % du débit utile mesuré le 2026-08-14.
		for (SamplesPtr samples = codec->GetFrame(); samples; samples = codec->GetFrame())
		{
			if (output != NULL) output->PlayFrame(samples);
			if (input  != NULL) input->PutFrame(samples);
		}

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
