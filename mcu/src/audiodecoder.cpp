#include "audiodecoder.h"
#include "media.h"

AudioDecoderWorker::AudioDecoderWorker()
{
	//Nothing
	output = NULL;
	decoding = false;
}

AudioDecoderWorker::~AudioDecoderWorker()
{
	End();
}

int AudioDecoderWorker::Init(AudioOutput *output)
{
	//Store it
	this->output = output;
	return 0;
}

int AudioDecoderWorker::End()
{
	//Check if already decoding
	if (decoding)
		//Stop
		Stop();
	return 0;
}

int AudioDecoderWorker::Start()
{
	Log("-StartAudioDecoder\n");

	//Check
	if (!output)
		//Exit
		return Error("null audio output");

	//Check if need to restart
	if (decoding)
		//Stop first
		Stop();

	//Start decoding
	decoding = 1;

	//Rearmer la file apres un eventuel Stop (Cancel collant :
	//l'historique redemarrait sur une file annulee)
	packets.Reset();

	//launc thread
	StartThread();

	return 1;
}
int  AudioDecoderWorker::Stop()
{
	Log(">StopAudioDecoder\n");

	//If we were started
	if (decoding)
	{
		//Stop
		decoding=0;

		//Cancel any pending wait
		packets.Cancel();

		//Esperamos
		StopThread();
	}

	Log("<StopAudioDecoder\n");

	return 1;
}


int AudioDecoderWorker::Decode()
{
	AudioDecoder*	codec=NULL;

	Log(">DecodeAudio\n");

	//La fréquence réelle est celle que porte chaque trame décodée ; on
	//n'annonce ici qu'un défaut, pour l'adaptateur plat.
	output->StartPlaying(8000);

	//Mientras tengamos que capturar
	while(decoding)
	{
		//Obtenemos el paquete
		if (!packets.Wait(0))
			//Check condition again
			continue;

		//Get packet in queue
		RTPPacket* packet = packets.Pop();

		//Check
		if (!packet)
			//Check condition again
			continue;

		//Comprobamos el tipo
		if ((codec==NULL) || (packet->GetCodec()!=codec->type))
		{
			//Si habia uno nos lo cargamos
			if (codec!=NULL)
				delete codec;

			//Creamos uno dependiendo del tipo
			if (!(codec = AudioCodecFactory::CreateDecoder((AudioCodec::Type)packet->GetCodec())))
				continue;

		}

		//Lo decodificamos
		codec->Decode(packet->GetMediaData(),packet->GetMediaLength());

		//Un paquet peut donner plusieurs trames : les jouer TOUTES.
		for (SamplesPtr samples = codec->GetFrame(); samples; samples = codec->GetFrame())
			output->PlayFrame(samples);

		//Delete packet
		delete(packet);
	}

	//End reproducing
	output->StopPlaying();

	//Check codec
	if (codec!=NULL)
		//Delete object
		delete codec;


	Log("<DecodeAudio\n");

	//Exit
	pthread_exit(0);
}

void AudioDecoderWorker::onRTPPacket(RTPPacket &packet)
{
	//Put it on the queue
	packets.Add(packet.Clone());
}
