/* 
 * File:   Recorder.cpp
 * Author: Sergio
 * 
 * Created on 26 de febrero de 2012, 16:50
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "Recorder.h"
#include "log.h"

bool Recorder::Create(const char *filename)
{
	//Origine de l'axe temps de l'enregistrement (ancre du rebasage texte)
	gettimeofday(&recStart,NULL);
	textForwarder.Reset();

	//Pas de source vidéo attachée, ou vidéo non négociée (StartReceiving jamais
	//reçu) : inutile d'attendre une I-frame qui ne viendra pas — sans cela le
	//MP4 resterait vide (waitVideo jette audio et texte).
	if (!MediaIsActive(MediaFrame::Video))
	{
		Log("-Recorder: no negotiated video source, disabling waitVideo\n");
		SetWaitVideo(false);
	}

	return MP4Recorder::Create(filename);
}

bool Recorder::MediaIsActive(MediaFrame::Type media)
{
	//Attaché pour ce média ?
	JoinedMap::iterator it = joined.find(media);
	if (it==joined.end())
		return false;
	//Source encore vivante ?
	std::shared_ptr<Joinable> j = it->second.lock();
	if (!j)
		return false;
	//Négociée côté source ?
	return j->IsReceiving();
}

Recorder::Recorder(std::wstring tag) : textForwarder(*this)
{
	//Store tag
	this->tag = tag;
	//NO audio or video
	audio = NULL;
	video = NULL;
	//Transcodage AAC créé à la demande, sur le premier paquet audio
	audioDecoder = NULL;
	audioEncoder = NULL;
	audioRate = 0;
	audioSamples = 0;
}

Recorder::~Recorder()
{
	//On se désinscrit des sources encore attachées AVANT destruction : sinon
	//chaque source garderait un Listener* pendouillant dans son set et
	//planterait (« pure virtual method called ») au Multiplex/destruction
	//suivant (C-13, sens inverse — RecorderDelete ne détache pas avant de
	//libérer le recorder). Le lock() ignore les sources déjà détruites : leur
	//weak_ptr a expiré, elles ne sont plus dans aucun set (lien A).
	for (JoinedMap::iterator it = joined.begin(); it!=joined.end(); ++it)
		if (std::shared_ptr<Joinable> j = it->second.lock())
			j->RemoveListener(this);
	joined.clear();

	//If we have an audio depacketizer
	if (audio)
		//Delete it
		delete(audio);
	//If we have an video depacketizer
	if (video)
		//Delete it
		delete(video);
	//Chaîne de transcodage AAC éventuelle
	if (audioDecoder)
		delete(audioDecoder);
	if (audioEncoder)
		delete(audioEncoder);
}

void Recorder::onRTPPacket(RTPPacket &packet)
{
	//Check type
	switch(packet.GetMedia())
	{
		case MediaFrame::Audio:
			onAudioPacket(packet);
			break;
		case MediaFrame::Video:
			//Écho : repousse le paquet vers l'émetteur de la source.
			//Pour un RTPEndpoint, le Joinable de réception EST aussi
			//l'émetteur (Joinable::Listener) ; le dynamic_cast échoue
			//proprement pour les autres sources (ports de mixer).
			if (echoVideo)
			{
				if (std::shared_ptr<Joinable> j = videoSource.lock())
				{
					Joinable::Listener* sender = dynamic_cast<Joinable::Listener*>(j.get());
					if (sender)
						sender->onRTPPacket(packet);
				}
			}
			//Do we have video depacketizer
			if (!video)
				//Create new
				video = RTPDepacketizer::Create(packet.GetMedia(),packet.GetCodec());
			//Check again
			if (video)
			{
				//Append to frame
				VideoFrame *frame = (VideoFrame*)video->AddPacket(&packet);
				//Is it last
				if (packet.GetMark())
				{
					//If got frame
					if (frame)
					{
						//Le mcu horodate en MILLISECONDES (MP4Recorder
						//reconvertit vers l'horloge video 90 kHz de
						//libmedkit). Le depacketiseur laisse ici le
						//timestamp RTP brut (90 kHz, origine aleatoire) :
						//on le ramene en ms, comme le fait deja la voie
						//audio (/8) et la voie texte (rebasage).
						frame->SetTimestamp(frame->GetTimeStamp()/90);
						//Record frame
						onMediaFrame(*frame);
					}
					//Clear frame
					video->ResetFrame();
				}
			}
			break;
		case MediaFrame::Text:
			onTextPacket(packet);
			break;
	}

}

void Recorder::onAudioPacket(RTPPacket &packet)
{
	AudioCodec::Type codec = (AudioCodec::Type)packet.GetCodec();

	//PCMU/PCMA s'écrivent tels quels dans le MP4. Tout le reste (Opus, G722,
	//AMR...) est transcodé en AAC : le conteneur ne les accepte pas (ou pas
	//dans leur format de payload RTP). Une fois le transcodage engagé on y
	//reste, la piste MP4 est mono-codec.
	if ((codec==AudioCodec::PCMU || codec==AudioCodec::PCMA) && !audioEncoder)
	{
		//Do we have audio depacketizer
		if (!audio)
			audio = RTPDepacketizer::Create(MediaFrame::Audio,codec);
		if (!audio)
			return;
		//One RTP packet = one audio frame
		MediaFrame *frame = audio->AddPacket(&packet);
		if (frame)
		{
			//Mp4AudioTrack attend des timestamps en ms (horloge RTP 8 kHz ici)
			frame->SetTimestamp(packet.GetTimestamp()/8);
			onMediaFrame(*frame);
			audio->ResetFrame();
		}
		return;
	}

	//Transcodage -> AAC. (Re)crée le décodeur si le codec change en cours de route.
	if (audioDecoder && audioDecoder->type!=codec)
	{
		delete(audioDecoder);
		audioDecoder = NULL;
	}
	if (!audioDecoder)
	{
		audioDecoder = AudioCodecFactory::CreateDecoder(codec);
		if (!audioDecoder)
			//CreateDecoder a déjà loggué le codec fautif
			return;
	}

	SWORD pcm[4096];
	int len = audioDecoder->Decode(packet.GetMediaData(),packet.GetMediaLength(),pcm,sizeof(pcm)/sizeof(SWORD));
	if (len<=0)
		return;

	if (!audioEncoder)
	{
		//L'encodeur AAC travaille à la fréquence native du décodeur (Opus : 48 kHz),
		//son resampler interne ne convertit que le format S16 -> FLTP.
		audioRate = audioDecoder->GetRate();
		if (!audioRate)
			audioRate = 8000;
		Properties props;
		char rate[16];
		snprintf(rate,sizeof(rate),"%u",audioRate);
		props.SetProperty("aac.samplerate",rate);
		audioEncoder = AudioCodecFactory::CreateEncoder(AudioCodec::AAC,props);
		if (!audioEncoder)
			return;
		audioSamples = 0;
		Log("-Recorder audio transcoding [%s -> AAC, %u Hz]\n",AudioCodec::GetNameFor(codec),audioRate);
	}

	//L'encodeur ffmpeg exige une trame complète par appel (1024 échantillons
	//pour l'AAC) : on accumule le PCM décodé et on encode trame par trame,
	//soit un sample MP4 par trame AAC.
	const int frameSamples = audioEncoder->numFrameSamples>0 ? audioEncoder->numFrameSamples : 1024;
	pcmFifo.insert(pcmFifo.end(),pcm,pcm+len);
	size_t pos = 0;
	while (pcmFifo.size()-pos>=(size_t)frameSamples)
	{
		BYTE aac[2048];
		int encoded = audioEncoder->Encode(pcmFifo.data()+pos,frameSamples,aac,sizeof(aac));
		pos += frameSamples;
		if (encoded>0)
		{
			AudioFrame frame(AudioCodec::AAC,audioRate);
			frame.SetMedia(aac,encoded);
			//Timestamps en ms, dérivés du nombre de trames produites (cadence
			//exacte de 1024 échantillons, pas de dérive cumulée)
			frame.SetTimestamp((DWORD)(audioSamples*1000/audioRate));
			audioSamples += frameSamples;
			onMediaFrame(frame);
		}
	}
	pcmFifo.erase(pcmFifo.begin(),pcmFifo.begin()+pos);
}

void Recorder::onTextPacket(RTPPacket &packet)
{
	//T.140 avec redondance (RFC 4103) : extraire le payload primaire et
	//récupérer les paquets perdus depuis les niveaux redondants
	if ((TextCodec::Type)packet.GetCodec()==TextCodec::T140RED)
	{
		RTPRedundantPacket *red = (RTPRedundantPacket*)&packet;
		redCodec.Decode(red,&textForwarder);
	}
	else
	{
		//T.140 nu : le payload est le texte, l'horloge RTP (1 kHz) donne des ms
		TextFrame frame(packet.GetTimestamp(),packet.GetMediaData(),packet.GetMediaLength());
		textForwarder.SendFrame(frame);
	}
}

int Recorder::TextForwarder::SendFrame(TextFrame &frame)
{
	//Les keepalives T.140 (BOM UTF-8 seul, trames vides) ne sont pas du texte
	static const BYTE BOM[] = {0xEF,0xBB,0xBF};
	if (!frame.GetLength())
		return 0;
	if (frame.GetLength()==sizeof(BOM) && !memcmp(frame.GetData(),BOM,sizeof(BOM)))
		return 0;
	//Rebase le timestamp RTP (origine aléatoire) sur l'axe de l'enregistrement :
	//la première trame est ancrée au temps écoulé depuis Create(), les
	//suivantes suivent par delta RTP (Mp4TextTrack travaille en ms)
	if (!baseSet)
	{
		timeval now;
		gettimeofday(&now,NULL);
		base = frame.GetTimeStamp();
		offset = (now.tv_sec-rec.recStart.tv_sec)*1000 + (now.tv_usec-rec.recStart.tv_usec)/1000;
		baseSet = true;
	}
	frame.SetTimestamp(frame.GetTimeStamp()-base+offset);
	rec.onMediaFrame(frame);
	return 1;
}

void Recorder::onResetStream()
{
	//Do nothing by now
}

void Recorder::onEndStream()
{
	//Do nothing by now
}

//Attach
int Recorder::Attach(MediaFrame::Type media, const std::shared_ptr<Joinable> & join)
{
	Log("-Endpoint attaching [media:%d]\n",media);

	//Get joined
	JoinedMap::iterator it = joined.find(media);

	//Detach if joined — lock() : source encore vivante ?
	if (it!=joined.end())
	{
		//Remove ourself as listeners
		if (std::shared_ptr<Joinable> j = it->second.lock())
			j->RemoveListener(this);
		//Remove from map
		joined.erase(it);
	}

	//If it is not null
	if (join)
	{
		//Set in map (lien retour non possédant)
		joined[media] = join,
		//Join to the new one
		join->AddListener(this);
	}

	//Copie dédiée pour l'écho (lue par le thread RTP vidéo hors de la map)
	if (media==MediaFrame::Video)
		videoSource = join;

	return 1;
}

int Recorder::Dettach(MediaFrame::Type media)
{
	Log("-Endpoint detaching [media:%d]\n",media);

	//Plus d'écho vers une source qu'on quitte
	if (media==MediaFrame::Video)
		videoSource.reset();

	//Get joined
	JoinedMap::iterator it = joined.find(media);

	//Detach if joined — lock() : ne déréférence pas si la source a disparu
	if (it!=joined.end())
	{
		//Remove ourself as listeners
		if (std::shared_ptr<Joinable> j = it->second.lock())
			j->RemoveListener(this);
		//Remove from map
		joined.erase(it);
	}

	return 1;
}
