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
//Moteur d'écriture de fichier média de libmedkit.
#include "medkit/ffmediafilewriter.h"

//Délai laissé à une piste annoncée pour se déclarer. libavformat fige l'en-tête
//à la première écriture : passé ce délai, le fichier s'écrit sans elle plutôt
//que de retenir indéfiniment les médias qui, eux, sont arrivés.
static const DWORD MaxTrackWaitMs = 2000;

//Horloge RTP du codec, celle qui horodate ses paquets (RFC 3551) : 8 kHz pour
//tous, sauf Opus (48) et AMR-WB (16). Le G722 est l'exception connue — il
//échantillonne à 16 kHz mais son horloge RTP reste annoncée à 8 kHz.
static DWORD RtpClockRateFor(AudioCodec::Type codec)
{
	switch(codec)
	{
		case AudioCodec::OPUS:	return 48000;
		case AudioCodec::AMRWB:	return 16000;
		default:		return 8000;
	}
}

bool Recorder::Create(const char *filename)
{
	Log("-Recorder: opening record [%s]\n",filename);

	//Origine de l'axe temps de l'enregistrement (ancre du rebasage texte)
	gettimeofday(&recStart,NULL);
	textForwarder.Reset();

	//Quels médias vont réellement alimenter le fichier : attaché ET négocié
	//côté source. C'est cette liste que le fichier doit connaître d'avance.
	const bool hasAudio = MediaIsActive(MediaFrame::Audio);
	const bool hasVideo = MediaIsActive(MediaFrame::Video);
	const bool hasText  = MediaIsActive(MediaFrame::Text);

	//Pas de source vidéo attachée, ou vidéo non négociée (StartReceiving jamais
	//reçu) : inutile d'attendre une I-frame qui ne viendra pas — sans cela le
	//fichier resterait vide (waitVideo jette audio et texte).
	if (!hasVideo)
	{
		Log("-Recorder: no negotiated video source, disabling waitVideo\n");
		waitVideo = false;
	}

	std::lock_guard<std::mutex> lock(mutex);

	//Un enregistrement en cours est clos (trailer écrit) avant d'en ouvrir un autre
	recording = false;
	writer.reset();
	audioTrackDone = false;
	audioTrackOk = false;
	videoTrackDone = false;

	//Le conteneur suit l'extension : elle peut être inconnue, ou le fichier
	//impossible à ouvrir.
	std::unique_ptr<FfMediaFileWriter> w(new FfMediaFileWriter(NULL,filename,waitVideo));
	if (!w->IsOpen())
		return Error("-Recorder: could not open [%s] for recording\n",filename);

	//Le texte se déclare tout de suite : son codec ne dépend pas de ce qui
	//arrivera, et une première frappe peut n'arriver que longtemps après
	//l'en-tête — trop tard pour ouvrir une piste. Nom vide : un nom de piste
	//produirait un sous-titre d'introduction « [nom] » dans l'enregistrement.
	if (hasText)
	{
		if (w->IsTextSupported())
			w->AddTrack(TextCodec::T140,"",-1);
		else
			Error("-Recorder: container %s carries no text track\n",w->GetContainerName());
	}

	//Audio et vidéo ne se déclarent qu'à leur première trame (le codec vient du
	//paquet RTP) : l'en-tête les attend, sinon il se fige sans elles.
	if (hasAudio)
		w->ExpectTrack(FfMediaFileWriter::TrackAudio,MaxTrackWaitMs);
	if (hasVideo)
		w->ExpectTrack(FfMediaFileWriter::TrackVideo,MaxTrackWaitMs);

	writer = std::move(w);
	return true;
}

bool Recorder::Record()
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!writer)
		return Error("-Recorder: no file opened for recording\n");

	recording = true;
	return true;
}

bool Recorder::Stop()
{
	std::lock_guard<std::mutex> lock(mutex);

	recording = false;
	return true;
}

bool Recorder::Close()
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!writer)
		return false;

	recording = false;
	//Le destructeur du writer tient le dernier sous-titre, écrit le trailer et
	//ferme le fichier
	writer.reset();
	audioTrackDone = false;
	audioTrackOk = false;
	videoTrackDone = false;
	return true;
}

void Recorder::onMediaFrame(MediaFrame &frame)
{
	bool askIntra = false;

	{
		std::lock_guard<std::mutex> lock(mutex);

		if (!recording || !writer)
			return;

		if (frame.GetType()==MediaFrame::Video)
		{
			VideoFrame &videoFrame = (VideoFrame&)frame;

			//La piste vidéo naît sur une image clé : c'est elle qui porte les
			//dimensions (en-tête VP8) et les paramètres de codec (SPS H264)
			//dont le conteneur a besoin pour son en-tête.
			if (!videoTrackDone)
			{
				if (!videoFrame.IsIntra())
					return;
				//Un refus (codec hors conteneur) est définitif : le writer
				//désarme alors lui-même waitVideo pour ne pas retenir l'audio.
				writer->AddTrack(videoFrame.GetCodec(),
						 videoFrame.GetWidth(),videoFrame.GetHeight(),
						 256,"video");
				videoTrackDone = true;
			}

			// FRONTIÈRE D'HORLOGE mcu <-> libmedkit : le mcu horodate toutes ses
			// trames en millisecondes, libmedkit attend l'horloge vidéo 90 kHz
			// sur la piste vidéo (ms sur l'audio et le texte). On restaure
			// l'horodatage d'origine derrière : la trame appartient au
			// producteur, qui la remet ensuite aux autres auditeurs.
			DWORD tsMs = frame.GetTimeStamp();
			frame.SetTimestamp(tsMs*90);
			askIntra = (writer->ProcessFrame(&frame)==-333);
			frame.SetTimestamp(tsMs);
		}
		else
		{
			writer->ProcessFrame(&frame);
		}
	}

	//Le writer réclame une image clé (attente de waitVideo, ou paramètres de
	//codec encore inconnus) : la demander à la source. Hors du verrou, pour que
	//celui du recorder reste une feuille (docs/reference/jsr309-ordre-verrous.md).
	if (askIntra)
		if (std::shared_ptr<Joinable> j = videoSource.lock())
			j->Update();
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

	//Plus aucune trame en vol : le fichier peut être fermé (trailer écrit)
	Close();

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

bool Recorder::IsRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	return recording && writer;
}

void Recorder::onRTPPacket(RTPPacket &packet)
{
	//Les sources restent attachées entre deux enregistrements : sans cette
	//sortie, chaque paquet audio ferait tourner le dépaquétisage et le
	//transcodage AAC pour un fichier qui n'existe pas.
	if (!IsRecording())
		return;

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
						//Le mcu horodate en MILLISECONDES (onMediaFrame
						//reconvertit vers l'horloge video 90 kHz du
						//fichier). Le depacketiseur laisse ici le
						//timestamp RTP brut (90 kHz, origine aleatoire) :
						//on le ramene en ms, comme le fait deja la voie
						//audio et la voie texte (rebasage).
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

bool Recorder::AudioFitsFile(AudioCodec::Type codec)
{
	//Enregistrer un flux RTP sans le transcoder suppose que sa charge utile RTP
	//SOIT la trame codée. C'est vrai des codecs par échantillon et d'Opus, pas
	//de l'AMR (en-tête CMR + TOC devant chaque trame), que le conteneur
	//accepterait pourtant : le fichier serait illisible.
	switch(codec)
	{
		case AudioCodec::PCMU:
		case AudioCodec::PCMA:
		case AudioCodec::SLIN:
		case AudioCodec::G722:
		case AudioCodec::OPUS:
			break;
		default:
			return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	//Et le conteneur choisi par l'appelant doit le porter : le muxer mp4 refuse
	//tous les codecs télécom, là où Matroska les prend tous.
	return writer && writer->IsCodecSupported(codec);
}

bool Recorder::EnsureAudioTrack(AudioCodec::Type codec,DWORD rate)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!writer)
		return false;

	if (!audioTrackDone)
	{
		//AddTrack rend 0 si la piste existe déjà, <0 si le conteneur refuse le
		//codec — et dans ce cas il ne faut pas le redemander à chaque paquet.
		audioTrackOk = writer->AddTrack(codec,rate,"audio")>=0;
		audioTrackDone = true;
	}
	return audioTrackOk;
}

void Recorder::onAudioPacket(RTPPacket &packet)
{
	AudioCodec::Type codec = (AudioCodec::Type)packet.GetCodec();

	//Codec que le conteneur porte tel quel : enregistré sans transcodage. Tout
	//le reste passe par l'AAC, que tous les conteneurs de fichier acceptent.
	//Une fois le transcodage engagé on y reste : la piste est mono-codec.
	if (!audioEncoder && AudioFitsFile(codec))
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
			//La piste audio du fichier attend des timestamps en ms ; le
			//dépaquétiseur laisse ici l'horloge RTP du codec.
			const DWORD clock = RtpClockRateFor(codec);
			frame->SetTimestamp(packet.GetTimestamp()/(clock/1000));
			if (EnsureAudioTrack(codec,clock))
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

	audioDecoder->Decode(packet.GetMediaData(),packet.GetMediaLength());

	//Un paquet peut donner plusieurs trames : les transcoder TOUTES.
	for (SamplesPtr samples = audioDecoder->GetFrame(); samples;
	     samples = audioDecoder->GetFrame())
	{
		if (!audioEncoder)
		{
			//L'encodeur AAC travaille à la fréquence que porte la trame
			//(Opus : 48 kHz), et non à une fréquence annoncée d'avance.
			audioRate = samples->GetRate();
			if (!audioRate)
				continue;
			Properties props;
			char rate[16];
			snprintf(rate,sizeof(rate),"%u",audioRate);
			props.SetProperty("aac.samplerate",rate);
			audioEncoder = AudioCodecFactory::CreateEncoder(AudioCodec::AAC,props);
			if (!audioEncoder)
				return;
			//La fréquence demandée n'est pas forcément celle retenue (l'encodeur
			//rééchantillonne si son codec ne la porte pas) : c'est la sienne qui
			//horodate les trames et qui déclare la piste du fichier.
			if (audioEncoder->GetRate())
				audioRate = audioEncoder->GetRate();
			audioSamples = 0;
			Log("-Recorder audio transcoding [%s -> AAC, %u Hz]\n",AudioCodec::GetNameFor(codec),audioRate);
		}

		if (!EnsureAudioTrack(AudioCodec::AAC,audioRate))
			return;

		//L'encodeur accumule lui-même jusqu'à sa trame complète (1024
		//échantillons pour l'AAC) : plus de fifo ni de découpage ici.
		for (AudioFramePtr encoded = audioEncoder->EncodeFrame(samples);
		     encoded; encoded = audioEncoder->EncodeFrame(NULL))
		{
			AudioFrame frame(AudioCodec::AAC,audioRate);
			frame.SetMedia(encoded->GetData(),encoded->GetLength());
			//Timestamps en ms, dérivés du nombre de trames produites (cadence
			//exacte de 1024 échantillons, pas de dérive cumulée)
			frame.SetTimestamp((DWORD)(audioSamples*1000/audioRate));
			audioSamples += audioEncoder->numFrameSamples;
			onMediaFrame(frame);
		}
	}
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
	//suivantes suivent par delta RTP (la piste texte travaille en ms)
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
