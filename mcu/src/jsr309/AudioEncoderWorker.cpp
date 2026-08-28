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
	input = NULL;
	pushed = false;
	encoding = false;
	codec = (AudioCodec::Type)-1;
	configDirty = false;
	audioEncoder = NULL;
	encoderRate = 0;
	frameTime = 0;
	multiplier = 1;
}

AudioEncoderMultiplexerWorker::~AudioEncoderMultiplexerWorker()
{
	//Contrat de Worker : le destructeur DÉRIVÉ arrête le thread, Run() n'existe
	//déjà plus quand ~Worker s'exécute.
	End();
}

int AudioEncoderMultiplexerWorker::Init(AudioInput *input)
{
	//Store it
	this->input = input;
	pushed = false;
	return 1;
}

int AudioEncoderMultiplexerWorker::Init()
{
	//Transcodeur : plus de PipeAudioInput entre le décodeur et nous. Les trames
	//arrivent par EncodeSamples, sur le thread de la source (lot 3).
	this->input = NULL;
	pushed = true;
	return 1;
}

int AudioEncoderMultiplexerWorker::SetCodec(AudioCodec::Type codec)
{
	{
		std::lock_guard<std::mutex> lock(configLock);
		this->codec = codec;
	}

	//Le chemin des paquets applique au tour suivant : il jette son encodeur et
	//le recrée. Le Stop()/Start() d'avant séparait l'ancien paramétrage du
	//nouveau par l'arrêt du thread — sans thread, c'est ce drapeau qui le fait
	//(§4.4 de jsr309_transcode_sans_thread.md).
	configDirty = true;

	//Check
	if (!listeners.empty() && !encoding)
		//Start
		Start();

	return 1;
}

//Phase 5 (nego_fmtp §6.3) : les bornes que la négociation SDP de la patte
//émettrice impose à l'encodeur. Les Properties ne sont lues qu'à CreateEncoder,
//donc des bornes qui changent sur un encodeur ouvert exigent de le rouvrir —
//par le même drapeau que SetCodec, et non plus par un Stop/Start qui joignait
//un thread depuis le thread XML-RPC (celui qui tient le verrou de la
//MediaSession).
void AudioEncoderMultiplexerWorker::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	{
		std::lock_guard<std::mutex> lock(configLock);

		//Bornes identiques : ne rien faire (chaque push
		//re-signalisation/attach/StartSending repasse ici).
		if (negotiated == byCodec)
			return;

		negotiated = byCodec;
	}

	Log("-AudioEncoder: negotiated codec properties updated [%d codec(s)]\n",
	    (int)byCodec.size());

	configDirty = true;

	if (!listeners.empty() && !encoding && codec != (AudioCodec::Type)-1)
	{
		Log("-AudioEncoder: started encoder with negotiated properties.\n");
		Start();
	}
}

int AudioEncoderMultiplexerWorker::Start()
{
	//Check
	if (!pushed && !input)
		//Exit
		return Error("null audio input");

	//Postcondition de Stop() : aucune poignée en vol, aucun encodeur ouvert.
	//Appelé sans condition — Encode() peut être sorti de lui-même sans que
	//`encoding` le dise.
	Stop();

	//Ouvrir le chemin
	encoding = true;

	//Mode poussé : rien à lancer, c'est la source qui appelle EncodeSamples.
	if (!pushed)
		StartThread();

	return 1;
}

int AudioEncoderMultiplexerWorker::Stop()
{
	Log(">Stop AudioEncoderMultiplexerWorker\n");

	//Fermer le chemin d'abord et sans condition : en mode poussé c'est tout ce
	//qui arrête l'encodage.
	encoding = false;

	if (!pushed)
	{
		//Stop any pending grab, puis joindre
		if (input) input->CancelRecFrame();
		StopThread();
		//Paramos de grabar por si acaso
		if (input) input->StopRecording();
	}

	{
		//Le thread est joint (mode tiré) ou la source est déjà retirée (mode
		//poussé, cf. §4.3 : RemoveListener est la barrière). Ce verrou couvre le
		//cas où le plan de contrôle arrive alors qu'une trame est en vol.
		std::lock_guard<std::mutex> lock(encodeLock);

		delete audioEncoder;
		audioEncoder = NULL;
		packet.reset();
		resampler.Reset();
		encoderRate = 0;
		frameTime = 0;
	}

	Log("<Stop AudioEncoderMultiplexerWorker\n");

	return 1;
}

int AudioEncoderMultiplexerWorker::End()
{
	Stop();

	//Set null
	input = NULL;
	return 0;
}

bool AudioEncoderMultiplexerWorker::EnsureEncoder()
{
	//Consommer le drapeau AVANT de lire la configuration : une écriture qui se
	//glisse entre les deux laisse le drapeau levé, donc un tour de plus — jamais
	//une configuration perdue.
	const bool dirty = configDirty.exchange(false);

	AudioCodec::Type wanted;
	Properties effective;
	{
		std::lock_guard<std::mutex> lock(configLock);

		wanted = codec;

		//Phase 5 : les bornes négociées de la patte émettrice s'appliquent
		//à l'ouverture (Opus : FEC/DTX/CBR/maxaveragebitrate du pair).
		std::map<int,Properties>::const_iterator itNeg = negotiated.find((int)wanted);
		if (itNeg != negotiated.end())
			effective = itNeg->second;
	}

	if (wanted == (AudioCodec::Type)-1)
		return false;

	if (audioEncoder && !dirty && audioEncoder->type == wanted)
		return true;

	delete audioEncoder;
	audioEncoder = NULL;
	packet.reset();

	if (!effective.empty())
		Log("-AudioEncoder: opening with negotiated properties for %s [%d key(s)]\n",
		    AudioCodec::GetNameFor(wanted), (int)effective.size());

	audioEncoder = AudioCodecFactory::CreateEncoder(wanted, effective);
	if (audioEncoder == NULL)
	{
		Error("-AudioEncoder: could not create %s encoder\n", AudioCodec::GetNameFor(wanted));
		//Refermer le chemin plutôt que de rejouer l'échec à chaque trame : c'est
		//ce que faisait la sortie de boucle.
		encoding = false;
		return false;
	}

	//L'encodeur dit la fréquence à laquelle il travaille, le rééchantillonneur
	//convertit vers elle. Plus personne n'a à connaître la fréquence NATIVE du
	//producteur, qui n'est de toute façon connue qu'après le premier paquet
	//décodé.
	encoderRate = audioEncoder->GetRate();
	if (encoderRate == 0)
	{
		Error("-AudioEncoder: %s failed to open, no working rate\n",
		      AudioCodec::GetNameFor(wanted));
		delete audioEncoder;
		audioEncoder = NULL;
		encoding = false;
		return false;
	}

	//Nouvelle fréquence de sortie possible : le rééchantillonneur rouvre.
	resampler.Reset();

	//Mode tiré : c'est le producteur qui convertit, il faut le lui dire.
	if (input)
		input->StartRecording(encoderRate);

	const DWORD clock = audioEncoder->GetClockRate();
	multiplier = (float) clock / (float) encoderRate;

	packet.reset(new RTPPacket(MediaFrame::Audio, wanted, wanted));
	packet->SetClockRate(clock);

	//Nouvel encodeur = nouvelle base de temps (frameTime repart de zéro à chaque
	//run, et l'horloge peut changer avec le codec) : on l'annonce comme le veut
	//la RFC 3550, par un SSRC neuf. En aval, RTPSession::SendPacket tire alors un
	//sendSSRC neuf et le pair resynchronise proprement — au re-INVITE du
	//2026-08-14, la base sautait de ±125k unités DANS le même flux et le jitter
	//buffer du pair décrochait (audio haché mesuré en capture).
	packet->SetSSRC(random());
	frameTime = 0;

	Log("-JSR309 AudioEncoder: Started audio encoder %s at %d Hz.\n",
	    AudioCodec::GetNameFor(wanted), encoderRate);

	return true;
}

/*******************************************
* EncodeSamples
*	Encode une trame décodée et la multiplexe. Corps commun au port de mixeur
*	(appelé par la boucle) et au transcodeur (appelé par le décodeur, sur le
*	thread de la source).
*******************************************/
int AudioEncoderMultiplexerWorker::EncodeSamples(SamplesPtr samples)
{
	if (!samples)
		return 0;

	std::lock_guard<std::mutex> lock(encodeLock);

	if (!encoding)
		return 0;

	if (!EnsureEncoder())
		return 0;

	//La trame porte sa fréquence : la convertir vers celle de l'encodeur. En
	//mode tiré le producteur a déjà converti (StartRecording), et le
	//rééchantillonneur rend alors la trame telle quelle.
	SamplesPtr in = resampler.Resample(samples, encoderRate);
	if (!in)
		return Error("-AudioEncoder: could not resample to %u Hz\n", encoderRate);

	int sent = 0;

	//La trame arrive à la taille que le producteur a écrite ; c'est l'encodeur
	//qui la redécoupe à numFrameSamples, personne d'autre. Une trame d'entrée
	//peut en remplir plusieurs : on purge à chaque fois.
	for (AudioFrame* frame = audioEncoder->EncodeFrame(in);
	     frame; frame = audioEncoder->EncodeFrame(NULL))
	{
		if (frame->GetLength() > packet->GetMaxMediaLength())
		{
			Error("-AudioEncoder: frame of %u bytes exceeds the RTP payload\n",
			      frame->GetLength());
			continue;
		}

		memcpy(packet->GetMediaData(),frame->GetData(),frame->GetLength());
		//Set frame time
		packet->SetTimestamp(frameTime);
		//Set length
		packet->SetMediaLength(frame->GetLength());

		//Multiplex it
		Multiplex(*packet);
		sent++;

		//L'horloge n'avance que sur ce qui est RÉELLEMENT émis : elle courait
		//auparavant même quand l'encodage ne produisait rien.
		frameTime += audioEncoder->numFrameSamples*multiplier;
	}

	return sent;
}

/*******************************************
* Encode
*	Pompe du port de mixeur : c'est le mixeur qui cadence, ce thread ne fait que
*	tirer ce qu'il produit. Le transcodeur, lui, n'a plus de thread ici.
*******************************************/
int AudioEncoderMultiplexerWorker::Encode()
{
	Log(">Encode AudioEncoderMultiplexerWorker\n");

	while (encoding && IsThreadRunning())
	{
		//Ouvrir l'encodeur AVANT la première capture : c'est lui qui déclare au
		//producteur la fréquence voulue (StartRecording), donc rien n'arrive
		//tant qu'il n'est pas ouvert.
		{
			std::lock_guard<std::mutex> lock(encodeLock);
			EnsureEncoder();
		}

		//Création impossible : le chemin s'est refermé de lui-même.
		if (!encoding)
			break;

		//Capturamos
		SamplesPtr samples = input->RecFrame(1000);
		if (!samples)
			continue;

		EncodeSamples(samples);
	}

	Log("<Encode AudioEncoderMultiplexerWorker [%d]\n",(int)encoding);
	return 0;
}

void AudioEncoderMultiplexerWorker::AddListener(Listener *listener)
{
	//Check if we were already encoding
	if (listener && !encoding && codec!=(AudioCodec::Type)-1)
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
