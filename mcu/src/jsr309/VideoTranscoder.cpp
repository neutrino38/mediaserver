/* 
 * File:   VideoTranscoder.cpp
 * Author: Sergio
 * 
 * Created on 19 de marzo de 2013, 12:32
 */

#include "VideoTranscoder.h"
#include "videopipe.h"
#include "tools.h"

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
	//Aucune demande d'intra relayée pour l'instant
	setZeroTime(&lastSourceFPU);
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
	//AVANT d'arrêter quoi que ce soit : en mode pont la source tient un
	//Joinable::Listener* sur NOUS. VideoTranscoderDelete appelle End() sans
	//passer par Dettach(), et le shared_ptr détruit l'objet en sortie de portée
	//— la source publierait alors dans un objet libéré. La sûreté mémoire ne
	//doit pas dépendre de l'ordre des appels du contrôleur.
	UnlistenSource();
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
	//Armer l'encodeur dans tous les cas : en transcodage c'est lui qui absorbe
	//la demande (N FIR aval → une seule intra, à la prochaine image), et si le
	//pont retombe en transcodage l'intra en attente servira à la reprise.
	encoder.Update();

	//Hors transcodage établi, l'encodeur n'est pas (ou pas encore) dans le
	//chemin : en pont (state 2) seule la SOURCE peut produire l'intra que le
	//puits réclame, et en probing (state 0) le StartSending du puits arrive
	//avant le premier paquet — demander l'intra à la source sert aux deux modes
	//futurs. En transcodage (state 1) on ne relaie PAS : l'absorption est la
	//valeur ajoutée du transcodeur. En mode transcodage seul (!allowBridging),
	//joined est vide et le relais est un no-op.
	if (state != 1)
		RequestSourceFPU();
}

void VideoTranscoder::SetREMB(DWORD estimation)
{
	//En mode pont, l'encodeur n'est pas dans le chemin : seule la source peut
	//baisser le débit du flux relayé. La demande du puits (TMMBR/REMB, en bps)
	//remonte donc à l'amont, bornée par la consigne négociée de la patte
	//émettrice — le puits ne peut pas « autoriser » plus que sa négociation.
	if (state == 2)
	{
		DWORD cap = ((DWORD)encoder.GetBitrate())*1000;	//kbps -> bps
		if (cap && estimation > cap)
			estimation = cap;

		if (std::shared_ptr<Joinable> j = joined.lock())
			j->SetREMB(estimation);
		return;
	}

	//Transcodage (ou mode encore inconnu) : l'encodeur absorbe la limite.
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

			//Le puits recevait le flux de l'encodeur (ou rien du tout) : il
			//voit maintenant la continuation d'un flux qu'il n'a jamais vu,
			//illisible avant la prochaine intra périodique de la source — la
			//demander tout de suite. L'anti-tempête déduplique si le
			//StartSending du puits vient de le faire (probing).
			RequestSourceFPU();
		}
		else
		{
			state = 1;
			auto outCodec = encoder.GetCodec();
			Log("-VideoTranscoder: transcoding %s -> %s .\n",
				VideoCodec::GetNameFor(outCodec),
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

void VideoTranscoder::UnlistenSource()
{
	//lock() : la source est-elle encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
		j->RemoveListener(this);

	joined.reset();
}

void VideoTranscoder::RequestSourceFPU()
{
	//Borne : une demande par seconde, même cadence que lastFPURequest du
	//décodeur. Le compteur n'avance que si la demande part réellement.
	if (getDifTime(&lastSourceFPU)<1000000)
		return;

	//lock() : la source est-elle encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
	{
		Log("-VideoTranscoder: requesting FPU from source [%ls]\n",tag.c_str());
		getUpdDifTime(&lastSourceFPU);
		j->Update();
	}
}

//Returning 0 here made every VideoTranscoderAttachToEndpoint/Dettach XML-RPC
//call answer an error while the attach had in fact happened — a controller that
//checks the status tears the call down over a success.
//
//Même forme qu'AudioTranscoder::Attach, et pour la même raison : en mode pont
//c'est le TRANSCODEUR qui doit voir chaque paquet, puisque c'est onRTPPacket qui
//arbitre relais ou transcodage sur le codec réellement reçu. Brancher la source
//directement sur le décodeur, comme le faisait cette fonction, ne « désactive »
//pas le pont — il rend l'arbitrage inatteignable : onRTPPacket n'est jamais
//appelé, TryCodec jamais interrogé, `state` reste à 0, et tout le chemin pont
//est du code mort. Le symptôme n'est pas une perte de performance mais une perte
//de média : le 2026-08-12, un appel AV1 ↔ AV1 (les deux pattes s'accordant sur
//AV1, donc relayable tel quel) a décodé en libdav1d un flux qu'aucun
//dépaquetiseur AV1 ne préparait — « Unknown OBU type 11 », pas une image
//décodée, pas une image ré-encodée, appel établi et écran noir des deux côtés,
//avec en prime deux encodeurs SVT-AV1 ouverts pour rien. L'audio, lui, passait :
//AudioTranscoder::Attach honore allowBridging depuis toujours.
int VideoTranscoder::Attach(const std::shared_ptr<Joinable> & join)
{
	//Transcodage seul : la source alimente le décodeur, comme avant.
	if (!allowBridging)
	{
		decoder.Attach(join);
		return 1;
	}

	//Une source précédente ne doit pas continuer à nous publier des paquets.
	UnlistenSource();

	joined = join;

	//Le mode se rejuge sur le premier paquet de la NOUVELLE source : son codec
	//n'a aucune raison d'être celui de la précédente.
	state = 0;
	recCodec = -1;
	//Nouvelle source = nouveau flux : ne pas bloquer sa première demande
	//d'intra sur le compteur de la précédente.
	setZeroTime(&lastSourceFPU);

	//Le décodeur n'est plus alimenté par la source mais à la main, depuis
	//onRTPPacket, quand l'arbitrage retombe sur le transcodage. Il faut donc
	//démarrer son worker sans l'attacher (exactement ce que fait l'audio).
	decoder.Start();

	//... mais il doit connaître la source : c'est à ELLE que ses demandes de
	//FPU s'adressent (perte de paquets, erreur de décodage). Sans ce lien,
	//joined restait vide côté décodeur et les demandes échouaient en silence —
	//plus aucune FIR vers l'amont en mode transcodage, gel jusqu'à l'intra
	//périodique de la source.
	decoder.SetSource(join);

	if (join)
		join->AddListener(this);

	return 1;
}

int VideoTranscoder::Dettach()
{
	//En mode pont, c'est nous qui sommes inscrit auprès de la source : sans ce
	//retrait elle garderait un pointeur sur cet objet, et continuerait à publier
	//dedans après le détachement.
	UnlistenSource();

	//En mode transcodage seul, c'est le décodeur qui était inscrit et qui se
	//retire ; en mode pont, il n'était pas attaché et Dettach() se réduit à
	//l'arrêt de son worker — ce qu'on veut dans les deux cas.
	decoder.Dettach();
	return 1;
}