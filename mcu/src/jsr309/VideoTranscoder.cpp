/* 
 * File:   VideoTranscoder.cpp
 * Author: Sergio
 * 
 * Created on 19 de marzo de 2013, 12:32
 */

#include "VideoTranscoder.h"
#include "videopipe.h"

VideoTranscoder::VideoTranscoder(std::wstring &name)
{
	//Store tag
	this->tag = name;

	//Not inited
	inited = false;

	//Pas encore un paquet vu : le mode sera décidé sur le premier
	state = 0;
	recCodec = -1;
	allowBridging = false;
}

VideoTranscoder::~VideoTranscoder()
{
	//Check if ended properly
	if (inited)
		//End!!
		End();
}

int VideoTranscoder::Init(bool adaptative, bool allowBridging)
{
	Log("-Init VideoTranscoder [%ls,encoder:%p,decoder:%p,bridging:%d]\n",
	    tag.c_str(),&encoder,&decoder,allowBridging);

	//Init pipe
	pipe.Init();
	//Start encoder
	encoder.Init(&pipe);
	//Star decoder
	decoder.Init(&pipe);
	//Inited
	inited = true;
        encoder.UseInputSize(adaptative);
	//Mode pont autorisé ou non ; le mode reste à décider sur le premier paquet
	this->allowBridging = allowBridging;
	state = 0;
	recCodec = -1;
	//OK
	return 1;
}
int VideoTranscoder::SetCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod, Properties & properties)
{
	int ret;
	Log("-VideoTranscoder: set codec for transcodeur %ls.\n", tag.c_str());
        if (properties.HasProperty("useInputSize"))
        {
            int adpt =  properties.GetProperty("useInputSize", 0);
            encoder.UseInputSize(adpt != 0);
            properties.erase(std::string("useInputSize"));
        }
	ret = encoder.SetCodec(codec,mode,fps,bitrate,intraPeriod, properties);
	return ret;
}
int VideoTranscoder::End()
{
	Log("-End VideoTranscoder [%ls]\n",tag.c_str());
	//End encoder and decoder
	encoder.End();
	decoder.End();
	//End pipe
	pipe.End();
	//Not inited
	inited = false;
	//OK
	return 1;
}

void VideoTranscoder::AddListener(Joinable::Listener *listener)
{
	encoder.AddListener(listener);
}

void VideoTranscoder::Update()
{
	encoder.Update();
}

void VideoTranscoder::SetREMB(DWORD estimation)
{
	encoder.SetREMB(estimation);
}

void VideoTranscoder::RemoveListener(Joinable::Listener *listener)
{
	encoder.RemoveListener(listener);
}

//Même politique que AudioTranscoder::onRTPPacket : le mode est décidé sur le
//codec RÉELLEMENT reçu, pas sur ce que le plan de contrôle a annoncé, et il est
//rejugé dès que ce codec change. `RTPMultiplexer::TryCodec` interroge tous les
//puits attachés — et `RTPEndpoint::TryCheckCodec` bascule au passage le codec
//d'émission du puits — donc un « oui » signifie que le paquet peut sortir tel
//quel.
void VideoTranscoder::onRTPPacket(RTPPacket &packet)
{
	if (!allowBridging)
	{
		decoder.onRTPPacket(packet);
		return;
	}

	if (recCodec != packet.GetCodec() || state == 0)
	{
		int previous = state;
		int ret = encoder.TryCodec(packet.GetCodec());

		if (ret == packet.GetCodec())
		{
			state = 2;
			Log("-VideoTranscoder: switched to bridged mode for codec %s.\n",
			    VideoCodec::GetNameFor((VideoCodec::Type) packet.GetCodec()));
		}
		else
		{
			state = 1;
			Log("-VideoTranscoder: switched to transcoder mode for codec %s.\n",
			    VideoCodec::GetNameFor((VideoCodec::Type) packet.GetCodec()));

			//LA différence avec l'audio. Reprendre l'encodage en cours de flux ne
			//suffit pas pour de la vidéo : le puits vient de recevoir des paquets
			//relayés et attend la suite d'un flux qui change de source. Sans image
			//clé il affiche du bruit jusqu'à la prochaine intra périodique du
			//codeur — le gel classique. On force donc la FPU dès que l'encodeur
			//reprend la main, ce que fait déjà VideoTranscoderFPU par XML-RPC.
			//
			//Reste hors de notre portée : le DÉCODEUR a lui aussi besoin d'une
			//intra, mais de la SOURCE, et c'est l'endpoint amont qui la demande
			//(RTCP FIR/PLI). Sur un vrai changement de codec le pair en émet une
			//de lui-même ; la transition est loggée pour que le contraire se voie.
			if (previous != 0)
				encoder.Update();
		}

		recCodec = packet.GetCodec();
	}

	switch (state)
	{
		case 2: // pont : ni décodeur ni encodeur dans le chemin
			encoder.Multiplex(packet);
			break;

		case 1:
		default:
			decoder.onRTPPacket(packet);
			break;
	}
}
void VideoTranscoder::onResetStream()
{
	decoder.onResetStream();
}
void VideoTranscoder::onEndStream()
{
	decoder.onEndStream();
}

//Phase 5 (nego_fmtp §6.3) : l'endpoint écoute le transcodeur, mais c'est son
//encodeur qui produit — les bornes descendent d'un cran.
void VideoTranscoder::SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec)
{
	encoder.SetNegotiatedCodecProperties(byCodec);
}

//Returning 0 here made every VideoTranscoderAttachToEndpoint/Dettach XML-RPC
//call answer an error while the attach had in fact happened — a controller that
//checks the status tears the call down over a success.
int VideoTranscoder::Attach(const std::shared_ptr<Joinable> & join)
{
	decoder.Attach(join);
	return 1;
}

int VideoTranscoder::Dettach()
{
	decoder.Dettach();
	return 1;
}