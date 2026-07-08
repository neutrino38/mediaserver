/* 
 * File:   Player.cpp
 * Author: Sergio
 * 
 * Created on 9 de septiembre de 2011, 0:11
 */
#include "log.h"
#include "Player.h"
#include "mp4streamer.h"

Player::Player(std::wstring tag) : MP4Streamer(this)
{
	//Store tag
	this->tag = tag;
}

void Player::SetListener(Player::Listener *listener,void* param)
{
	//Store values
	this->listener = listener;
	this->param = param;
}

Joinable* Player::GetJoinable(MediaFrame::Type media)
{
	switch(media)
	{
		case MediaFrame::Video:
			//Return multiplexer
			return &video;
		case MediaFrame::Audio:
			//Return multiplexer
			return &audio;
		case MediaFrame::Text:
			//Return multiplexer
			return &text;
	}
	return NULL;
}
	
void Player::NegotiateCodecs()
{
	// Ordre de préférence côté serveur ; on retient la 1ʳᵉ alternative que le
	// fichier possède ET que tous les endpoints attachés acceptent. Le chemin
	// Player→RTPEndpoint est en passthrough (pas de transcodage) : il faut donc
	// lire la piste dont le codec figure dans la rtpMap négociée avec le pair.
	static const AudioCodec::Type acodecs[] = {
		AudioCodec::PCMU, AudioCodec::PCMA, AudioCodec::G722,
		AudioCodec::AMR,  AudioCodec::OPUS, AudioCodec::GSM
	};
	// Phase 1 — passthrough : le fichier possède un codec accepté par le pair.
	bool audioDone = false;
	for (unsigned i = 0; i < sizeof(acodecs)/sizeof(acodecs[0]); i++)
	{
		if (HasAudioCodec(acodecs[i]) && audio.TryCodec((int)acodecs[i]) == (int)acodecs[i])
		{
			SetAudioCodec(acodecs[i]);
			audioDone = true;
			break;
		}
	}
	// Phase 2 — transcodage : aucun codec du fichier n'est jouable en l'état
	// (typiquement fichier AAC), on décode et on ré-encode vers un codec accepté
	// par le pair. SetAudioCodecTranscoded échoue si le fichier n'a aucune piste
	// audio décodable.
	//
	// v1 : cibles de transcodage limitées à PCMU/PCMA (validées : les encodeurs
	// G711 maison acceptent une tranche arbitraire). Les encodeurs ffmpeg
	// (G722/GSM/AMR/OPUS) ont une sémantique de taille de trame que le
	// découpage actuel ne satisfait pas encore -> exclus tant que non validés.
	static const AudioCodec::Type xcodecs[] = { AudioCodec::PCMU, AudioCodec::PCMA };
	if (!audioDone)
	{
		for (unsigned i = 0; i < sizeof(xcodecs)/sizeof(xcodecs[0]); i++)
		{
			if (audio.TryCodec((int)xcodecs[i]) == (int)xcodecs[i] &&
			    SetAudioCodecTranscoded(xcodecs[i]))
				break;
		}
	}

	static const VideoCodec::Type vcodecs[] = {
		VideoCodec::H264, VideoCodec::VP8,
		VideoCodec::H263_1998, VideoCodec::H263_1996
	};
	for (unsigned i = 0; i < sizeof(vcodecs)/sizeof(vcodecs[0]); i++)
	{
		if (HasVideoCodec(vcodecs[i]) && video.TryCodec((int)vcodecs[i]) == (int)vcodecs[i])
		{
			SetVideoCodec(vcodecs[i]);
			break;
		}
	}
}

void Player::onRTPPacket(RTPPacket &packet)
{
	switch(packet.GetMedia())
	{
		case MediaFrame::Video:
			//Multiplex
			video.Multiplex(packet);
			break;
		case MediaFrame::Audio:
			//Multiplex
			audio.Multiplex(packet);
			break;
	}
}

void Player::onTextFrame(TextFrame &frame)
{
	RTPPacket packet(MediaFrame::Text,TextCodec::T140,TextCodec::T140);
	//Set timestamp
	packet.SetTimestamp(frame.GetTimeStamp());
	//Copy
	packet.SetPayload(frame.GetData(),frame.GetLength());
	//Multiplex
	text.Multiplex(packet);
}

void Player::onEnd()
{
	//Reset audio stream
	audio.ResetStream();
	//Reset video stream
	video.ResetStream();
	//Reset text stream
	text.ResetStream();
	//Check for listener
	if (listener)
		//Send event
		listener->onEndOfFile(this,param);
}

void Player::onMediaFrame(MediaFrame &frame)
{
	
}

int Player::SetEventContextId( MediaFrame::Type media,  int ctxId )
{
	Joinable* j = GetJoinable(media);
	if (j != NULL)
		j->SetEventContextId(ctxId);
	return 0;
}


int Player::SetEventHandler( MediaFrame::Type media, int sessionId,	JSR309Manager* jsrManager)
{
	Joinable* j = GetJoinable(media);
	if (j != NULL)
		j->SetEventHandler(sessionId,jsrManager);

	return 0;
}

