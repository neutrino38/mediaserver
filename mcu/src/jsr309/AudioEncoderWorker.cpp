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
		input->CancelRecFrame();

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

                //L'encodeur dit la fréquence à laquelle il travaille, le pipe
                //convertit vers elle. Plus personne n'a à connaître la
                //fréquence NATIVE du producteur, qui n'est de toute façon
                //connue qu'après le premier paquet décodé.
                rate = encoder->GetRate();
                if (rate == 0)
                {
                    Error("-AudioEncoder: %s failed to open, no working rate\n",
                          AudioCodec::GetNameFor(codec));
                    delete encoder;
                    return 0;
                }

                input->StartRecording(rate);
                Log("-JSR309 AudioEncoder: Started audio encoder %s at %d Hz.\n",
                    AudioCodec::GetNameFor(codec), rate);
                clock = encoder->GetClockRate();
                packet.SetClockRate(clock);

                multiplier = (float) clock/ (float) rate;

                //Nouvel encodeur = nouvelle base de temps (frameTime repart de
                //zéro à chaque run, et l'horloge peut changer avec le codec) :
                //on l'annonce comme le veut la RFC 3550, par un SSRC neuf.
                //En aval, RTPSession::SendPacket tire alors un sendSSRC neuf et
                //le pair resynchronise proprement — au re-INVITE du 2026-08-14,
                //la base sautait de ±125k unités DANS le même flux et le jitter
                //buffer du pair décrochait (audio haché mesuré en capture).
                packet.SetSSRC(random());
            }
		//Capturamos. La trame arrive à la taille que le producteur a écrite ;
		//c'est l'encodeur qui la redécoupe à numFrameSamples, personne d'autre.
		SamplesPtr samples = input->RecFrame(1000);
		if (!samples)
			continue;

		//Une trame d'entrée peut en remplir plusieurs : on purge à chaque fois.
		for (AudioFrame* frame = encoder->EncodeFrame(samples);
		     frame; frame = encoder->EncodeFrame(NULL))
		{
			if (frame->GetLength() > packet.GetMaxMediaLength())
			{
				Error("-AudioEncoder: frame of %u bytes exceeds the RTP payload\n",
				      frame->GetLength());
				continue;
			}

			memcpy(packet.GetMediaData(),frame->GetData(),frame->GetLength());
			//Set frame time
			packet.SetTimestamp(frameTime);
			//Set length
			packet.SetMediaLength(frame->GetLength());

			//Multiplex it
			Multiplex(packet);

			//L'horloge n'avance que sur ce qui est RÉELLEMENT émis : elle
			//courait auparavant même quand l'encodage ne produisait rien.
			frameTime += encoder->numFrameSamples*multiplier;
		}
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
