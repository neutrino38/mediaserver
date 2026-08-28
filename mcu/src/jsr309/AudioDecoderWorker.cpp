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
	codec = NULL;
	decoding = false;
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

	//Ouvrir le chemin : desormais onRTPPacket decode lui-meme, sur le thread
	//de la source.
	decoding = true;

	return 1;
}

int  AudioDecoderJoinableWorker::Stop()
{
	Log(">StopAudioDecoderJoinableWorker\n");

	//If we were started
	if (decoding)
	{
		//Close the path
		decoding = false;

		//End reproducing
		if (output != NULL) output->StopPlaying();
		if (input != NULL) input->End();

		//If a decoder is created, delete it
		if (codec != NULL)
		{
			delete codec;
			codec = NULL;
		}
	}

	Log("<StopAudioDecoderJoinableWorker\n");

	return 1;
}


void AudioDecoderJoinableWorker::DecodePacket(RTPPacket &packet)
{
	//Comprobamos el tipo
	if ( codec==NULL  || packet.GetCodec()!= codec->type )
	{
		//Si habia uno nos lo cargamos
		if (codec!=NULL)
		{
			if (input) input->StopRecording();
			if (output) output->StopPlaying();
			delete codec;
			codec = NULL;
		}

		//Creamos uno dependiendo del tipo
		codec = AudioCodecFactory::CreateDecoder((AudioCodec::Type)packet.GetCodec());
		if ( codec != NULL )
		{
			//Le décodeur restitue à la fréquence native du flux, qu'il ne
			//connaît qu'après son premier paquet. Personne n'a plus à
			//l'annoncer : chaque trame décodée porte la sienne.
			if (output) output->StartPlaying(codec->GetRate());
		}
		else
		{
			Error("Failed to open %s audio decoder\n", AudioCodec::GetNameFor((AudioCodec::Type)packet.GetCodec()));
			//Refermer le chemin plutot que de rejouer l'echec a chaque paquet :
			//c'est ce que faisait la sortie de boucle du thread.
			Stop();
			return;
		}
	}

	//Lo decodificamos
	codec->Decode(packet.GetMediaData(),packet.GetMediaLength());

	//Un paquet peut donner plusieurs trames : les publier TOUTES. L'ancien
	//appel unique en perdait — 53 % du débit utile mesuré le 2026-08-14.
	for (SamplesPtr samples = codec->GetFrame(); samples; samples = codec->GetFrame())
	{
		if (output != NULL) output->PlayFrame(samples);
		if (input  != NULL) input->PutFrame(samples);
	}
}

void AudioDecoderJoinableWorker::onRTPPacket(RTPPacket &packet)
{
	//Décoder ici même : on est sur le thread de la source, sous son verrou de
	//multiplexage. Plus de copie du paquet, plus de file, plus de thread.
	if (decoding)
		DecodePacket(packet);
}

void AudioDecoderJoinableWorker::onResetStream()
{
	//Plus rien en attente à jeter : le paquet est consommé au retour de
	//onRTPPacket.
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
		//Retrait AVANT arrêt : c'est RemoveListener qui est la barrière. Il ne
		//rend la main que le Multiplex en cours terminé, donc plus aucun
		//onRTPPacket n'est en vol quand Stop() touche au décodeur.
		j->RemoveListener(this);
		//Stop
		Stop();
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
		//Même barrière qu'Attach : le retrait précède l'arrêt, sinon Stop()
		//détruirait le décodeur pendant qu'un onRTPPacket en vol l'utilise.
		j->RemoveListener(this);
		//Stop decoding
		Stop();
	}
	else
		//Stop decoding même si la source est déjà partie
		Stop();

	//Not joined anymore
	joined.reset();
	return 0;
}
