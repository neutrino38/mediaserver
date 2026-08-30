/* 
 * File:   RTPEndpoint.cpp
 * Author: Sergio
 * 
 * Created on 7 de septiembre de 2011, 12:16
 */

#include <fcntl.h>
#include <signal.h>
#include "log.h"
#include "RTPEndpoint.h"
#include "rtpsession.h"
#include "medkit/codecs.h"

RTPEndpoint::RTPEndpoint(MediaFrame::Type type, MediaFrame::MediaRole role, MediaFrame::MediaProtocol proto) : Port(type, proto), RTPSession(type,this,role)
{
	//Not reset
	reseted = false;
	tsTransparency = false;
	//No time
	prevts = 0;
	timestamp = 0;
	//No source yet
	prevSSRC = 0;
        //No codec
        codec = NoCodec;
	//Aucun codec refusé pour l'instant
	unmappedCodec = NoCodec;
	unmappedTs    = 0;
	unmappedCount = 0;
	//Get freg
	switch(type)
	{
		case MediaFrame::Audio:
			//Set it
			freq = 8;
			break;
		case MediaFrame::Video:
			//Set it
			freq = 90;
			break;
		case MediaFrame::Text:
			//Set it
			freq = 1;
			break;
	}
}

RTPEndpoint::~RTPEndpoint()
{
        //Check
        if (portinited)
                //End it
                End();
}

int RTPEndpoint::Init()
{
        //Check
        if (portinited)
                //Exit
                return false;
        
        //Start rtp session
        RTPSession::Init();

        //Inited
        portinited = true;

	//Reset
	reseted = true;

	//No time
	timestamp = 0;
	
	//Init time
	getUpdDifTime(&prev);
	return 0;
}

int RTPEndpoint::End()
{
        //Chec
        if (!portinited)
                //Exit
                return 0;
        
        //Not inited anymore
        portinited = false;
	
        //Detach if joined
	//Detach();

	//ORDRE : arrêter le consommateur AVANT de détruire la source. StopReceiving
	//baisse `receiving`, réveille la boucle de démultiplexage (DeleteStreams) et
	//joint son thread ; ce n'est qu'ensuite que la session peut être démontée.
	//L'ordre inverse laissait le thread de démultiplexage appeler GetPacket() sur
	//une session en cours de destruction.
	if (receiving)
		//Stop it
		StopReceiving();

        //Stop
        RTPSession::End();

	return 0;
}

int RTPEndpoint::StartReceiving()
{
	//Check if inited
	if (!portinited)
		//Exit
		return Error("Not inited");
	
        //Check
        if (receiving)
                //Exit
                return Error("Already receiving");

        //Inited
        receiving = true;

	//P5 : ré-arme la notification « premier paquet reçu » pour ce cycle de réception
	//(un nouveau EndpointConnectedEvent sera émis quand le média recommencera à couler).
	ArmRTPReceivedNotification();

        //Create thread
	createPriorityThread(&thread,run,this,1);

	//Sedn on reset
	ResetStream();

	//Return listening port
	return 1;
}

int RTPEndpoint::StopReceiving()
{
        //Chec
        if (!receiving)
                //Exit
                return Error("Not receiving");

        //Not inited anymore
        receiving = false;

	//Cancel grab : RÉVEIL SEUL, rien n'est détruit. DeleteStreams réveillait ET
	//détruisait, donc les streams disparaissaient avant le join ci-dessous — le
	//thread lisait alors des objets libérés (tas corrompu, crash différé
	//ailleurs, souvent dans un free() sans rapport).
	CancelStreams();

	//NB : l'ancien pthread_kill(SIGIO) ici était MORT : le thread bloque dans
	//l'attente du jitter buffer (cv), pas dans poll ; c'est le Cancel des streams
	//(CancelStreams ci-dessus) qui le réveille réellement.

        //Y unimos
	pthread_join(thread,NULL);
	//Plus personne ne lit : on peut detruire.
	DeleteStreams();

	return 1;
}

int RTPEndpoint::StartSending()
{
	//Check if inited
	if (!portinited)
		//Exit
		return Error("Not initied");

	//Check if wer are joined
	if (std::shared_ptr<Joinable> j = joined.lock())
		//Rquest a FPU
		j->Update();
        //Send
	sending = true;

	return 1;
}

int RTPEndpoint::StopSending()
{
        //Not Send
	sending = false;

	return 1;
}

//Voir la déclaration : borne la retentative ET le journal. Retenter a du sens (une
//renégociation peut ajouter le PT à la rtpMap de sortie) ; le faire à chaque
//paquet non, puisque RTPSession::SetSendingCodec journalise une Error par appel —
//sur de la vidéo, c'est le journal noyé à 30 lignes par seconde et par flux.
bool RTPEndpoint::TrySendingCodec(DWORD wanted)
{
	struct timeval tv;
	gettimeofday(&tv,0);
	const QWORD nowMs = (QWORD)tv.tv_sec*1000 + tv.tv_usec/1000;

	//Déjà refusé il y a moins d'une seconde : on jette sans redemander.
	if (wanted == unmappedCodec && nowMs - unmappedTs < 1000)
	{
		unmappedCount++;
		return false;
	}

	if (RTPSession::SetSendingCodec(wanted))
	{
		//Sortie du trou : le dire, sinon la reprise après renégociation est
		//invisible alors que l'entrée dans le trou est bruyante.
		if (unmappedCodec != NoCodec)
			Log("-RTPEndpoint: %s desormais dans la rtpMap de sortie, emission reprise"
			    " apres %u paquet(s) jete(s)\n",
			    GetNameForCodec(type,wanted), unmappedCount);

		unmappedCodec = NoCodec;
		unmappedCount = 0;
		return true;
	}

	unmappedCount++;

	Log("-RTPEndpoint: %s hors de la rtpMap de sortie negociee, %u paquet(s) jete(s)."
	    " Les emettre sous le PT precedent ferait decoder du bruit au pair.\n",
	    GetNameForCodec(type,wanted), unmappedCount);

	unmappedCodec = wanted;
	unmappedTs    = nowMs;
	unmappedCount = 0;

	return false;
}

int  RTPEndpoint::TryCheckCodec(int codec)
{
    //Sonde d'arbitrage du pont : « absent de la rtpMap » est un résultat
    //NOMINAL (les deux pattes ne portent pas le même codec), pas une erreur.
    //Vérifier en silence avant de basculer réellement le codec d'émission —
    //SetSendingCodec journalise son échec en Error, ce qui alarmait la
    //supervision à chaque arbitrage retombant sur le transcodage.
    if ( !RTPSession::CanSendCodec(codec) )
        return -1;

    if ( RTPSession::SetSendingCodec(codec) )
    {
        return codec;
    }
    else
    {
        return -1;
    }
}


void RTPEndpoint::onRTPPacket(RTPPacket &packet)
{
	//Check
	if (!sending)
	{
		//Exit
		Log("-RTPEndpoint: trying to send packet on an inactive RTP EP.\n");
		return;
	}
	
	//Un paquet sans média n'est pas une image : ne rien émettre.
	//
	//RTPPacket::SetData retire le bourrage (RFC 3550 §5.1) et rend donc une longueur
	//NULLE pour une sonde de débit WebRTC, qui est entièrement en bourrage sur le SSRC
	//média. Relayée telle quelle, elle part vers le pair comme un paquet RTP vide qui
	//porte l'horodatage de l'image en cours et consomme un numéro de séquence : le
	//dépaquetiseur d'en face y cherche un descripteur VP8, ou un NAL H.264, et ne
	//trouve rien. L'image entière est déclarée invalide.
	//
	//Mesuré sur la capture du 2026-08-21 20:09 (Chrome -> Linphone, VP8 relayé) : sur
	//les 1368 paquets d'Alice, 1356 sont relayés à l'octet près et les 12 qui portent
	//P=1 avec 255 octets de bourrage arrivent chez Bob avec une charge utile de ZÉRO.
	//Trois d'entre eux tombent dans la toute première image — l'intra — donc l'image
	//est corrompue dès le décroché, et comme rien ne renvoie d'intra ensuite elle ne
	//se rétablit jamais. Côté Linphone : `Vp8RtpFmtUnpackerCtx: sequence inconsistency
	//detected`, `VP8 invalid frame`, et en H.264 un décodeur qui ne trouve jamais de
	//jeu de paramètres (`DecodeFrame2 failed: 0x10`).
	//
	//Le jeter ICI et pas à la réception : la sonde doit rester comptée par la patte
	//qui la reçoit (séquence, pertes, transport-cc), sans quoi nous rapporterions à
	//l'émetteur la perte de ses propres sondes — l'inverse de ce que sert le
	//mécanisme.
	if (packet.GetMediaLength()==0)
		return;

        //Get type
        MediaFrame::Type packetType = packet.GetMedia();
        //Check types
        if (type!=packetType)
	{
		Error("-RTPEndpoint: packet contains media %d and Endpoint is for media %d."
		      " packet will not be sent.\n", packetType, type );
                //Exit
                return;
	}

        //Check type
        if (packet.GetCodec()!=codec)
        {
		//Un endpoint ne peut étiqueter que ce que sa rtpMap de sortie NÉGOCIÉE
		//porte. Le verdict de SetSendingCodec était ignoré ici : en cas d'échec il
		//laisse le PT PRÉCÉDENT dans l'en-tête, et le paquet partait quand même —
		//des octets d'un codec sous l'étiquette d'un autre. Le pair ne voit aucune
		//erreur : il lit le PT, croit savoir ce qu'il décode, et décode du bruit.
		//
		//Et le pire n'était pas là : `codec` était mis à jour AVANT l'appel, donc
		//dès le paquet suivant `packet.GetCodec() == codec` faisait sauter tout le
		//bloc. L'échec était journalisé UNE fois, puis plus rien — ni retentative
		//après une renégociation qui ajouterait le PT, ni trace des milliers de
		//paquets mal étiquetés qui suivaient.
		if (!TrySendingCodec(packet.GetCodec()))
			//Rien de juste à émettre : ne pas mentir sur l'étiquette.
			return;

                //Store it
                codec = packet.GetCodec();
	}

	//Get diference from latest frame
	QWORD dif = getUpdDifTime(&prev);

	//La source a changé de SSRC : encodeur relancé par une renégociation, ou
	//pair amont qui a lui-même changé de source. Sa base de timestamps lui est
	//propre (RFC 3550), le delta inter-bases ne veut rien dire — on repart au
	//temps mur comme après onResetStream. En aval, SendPacket tire un sendSSRC
	//neuf sur ce même changement, et le pair resynchronise proprement.
	if (packet.GetSSRC()!=prevSSRC)
	{
		if (prevSSRC)
			Log("-RTPEndpoint: source SSRC changed [%x->%x], rebasing %s timestamp on wall clock\n",
			    prevSSRC,packet.GetSSRC(),MediaFrame::TypeToString(packet.GetMedia()));
		prevSSRC = packet.GetSSRC();
		reseted = true;
	}

	//If was reseted
	if (reseted)
	{
		//Get new time
		timestamp += dif*freq/1000;
		//Not reseted
		reseted = false;

	} else {
		//Get dif from packet timestamp
		timestamp += packet.GetTimestamp()-prevts;
	}

	//Update prev rtp ts
	prevts = packet.GetTimestamp();

        //Send it
        if (tsTransparency)
		RTPSession::SendPacket(packet,prevts);
	else
		RTPSession::SendPacket(packet,timestamp);
}

void RTPEndpoint::onResetStream()
{
	//Reseted
	reseted = true;

	//Send emptu packet
	RTPSession::SendEmptyPacket();

	//Remove codec
	codec = -1;
}

void RTPEndpoint::onEndStream()
{
	//Not joined anymore
	joined.reset();
}

int RTPEndpoint::MultiplexLoop()
{
        while(receiving)
        {
                //Get the packet
			RTPPacket* packet = RTPSession::GetPacket();
			//Check packet
			if (!packet)
			{
				//Next
				msleep(200);
				continue;
			}
			//Check type
			if (packet->GetCodec()==VideoCodec::RED)
			{

				//Get primary data
				RTPPacket *primary = ((RTPRedundantPacket*)packet)->CreatePrimaryPacket();
	//			Log("-RED %d %s\n",primary->GetType(),VideoCodec::GetNameFor((VideoCodec::Type)primary->GetCodec()));
				//Multiplex only primary data
				Multiplex(*primary);
				//Delete it
				delete(primary);
			} else {
	//			if (packet->GetMedia()==MediaFrame::Video) Log("-PRI %d %s\n",packet->GetType(),VideoCodec::GetNameFor((VideoCodec::Type)packet->GetCodec()));
				//Multiplex
				Multiplex(*packet);
			}
			//Delete ti
			delete(packet);
        }

        return 1;
}

void* RTPEndpoint::run(void *par)
{
        Log("RTPEndpointThread [%d]\n",getpid());
        //Get endpoint
	RTPEndpoint *end = (RTPEndpoint *)par;
        //Block signal in thread
	blocksignals();
	//Run : la boucle de démultiplexage, PAS Worker::Run() (cf. RTPEndpoint.h)
	end->MultiplexLoop();
	//Exit
	return NULL;
}

int RTPEndpoint::Attach(const std::shared_ptr<Joinable> & join)
{
	//Check if inited
	if (!portinited)
		//Error
		return Error("Not inited");

        //Detach if joined — lock() : source encore vivante ?
	if (std::shared_ptr<Joinable> j = joined.lock())
		//Remove ourself as listeners
		j->RemoveListener(this);
	//Store new one (lien retour non possédant)
	joined = join;
	//If it is not null
	if (join)
	{
		//Join to the new one
		join->AddListener(this);
		//Phase 5 : bornes négociées poussées au producteur fraîchement attaché
		//(l'ordre attach/négociation est libre côté contrôleur).
		PushNegotiatedProps();
	}

	//OK
	return 1;
}

int RTPEndpoint::Detach()
{
        //Detach if joined — lock() : ne déréférence pas si la source a disparu
	if (std::shared_ptr<Joinable> j = joined.lock())
		//Remove ourself as listeners
		j->RemoveListener(this);
	//Not joined anymore
	joined.reset();
	return 0;
}

void RTPEndpoint::onFPURequested(RTPSession *session)
{
	//Check if joined
	if (std::shared_ptr<Joinable> j = joined.lock())
		//Request update
		j->Update();
}

void RTPEndpoint::onReceiverEstimatedMaxBitrate(RTPSession *session,DWORD estimation)
{
	//Check if joined
       if (std::shared_ptr<Joinable> j = joined.lock())
               //Request update
               j->SetREMB(estimation);
}

void RTPEndpoint::onTempMaxMediaStreamBitrateRequest(RTPSession *session,DWORD estimation,DWORD overhead)
{
	//Check if joined
       if (std::shared_ptr<Joinable> j = joined.lock())
               //Request update
               j->SetREMB(estimation);
}

void RTPEndpoint::onSenderEstimatedBitrate(RTPSession *session,DWORD estimation)
{
	if (std::shared_ptr<Joinable> j = joined.lock())
		j->SetSenderEstimate(estimation);
}

void RTPEndpoint::onRTPTimeout(RTPSession *session)
{
	//Inactivité RTP prolongée détectée par le watchdog de RTPSession (appelé une
	//seule fois grâce à l'anti-rebond côté RTPSession). On notifie le contrôleur.
	Log("-RTPEndpoint::onRTPTimeout : publication EndpointDisconnectedEvent [%p]\n",this);
	PostEvent(new ::EndpointDisconnectedEvent());
}

void RTPEndpoint::onRTPPacketReceived(RTPSession *session)
{
	//P5 : premier paquet RTP/SRTP reçu et validé pour ce média. Le succès du
	//déchiffrement (côté RTPSession) garantit que le handshake DTLS est terminé
	//(cas SRTP/DTLS) ou qu'il n'y a pas de crypto : les deux conditions P5 sont donc
	//satisfaites. L'anti-rebond one-shot est tenu par RTPSession (ré-armé à
	//StartReceiving), donc ce callback n'arrive qu'une fois par cycle de réception.
	Log("-RTPEndpoint::onRTPPacketReceived : publication EndpointConnectedEvent [%p]\n",this);
	PostEvent(new ::EndpointConnectedEvent());
}

void RTPEndpoint::Update()
{
	//Update	
	//send External FIR
	
	if (UseExtFIR() )
	{
		PostEvent(new ::ExternalFIRRequestedEvent());
	}	
	//send RTCP FIR
	if (UseRtcpFIR() )
		RequestFPU();
}

void RTPEndpoint::SetREMB(DWORD estimation)
{
	//Check if we have an estimator
	if (remoteRateEstimator)
		//Update temporal limit : le feedback spontané de l'estimateur (s'il est
		//activé par la propriété "tmmbr") restera cohérent avec la borne.
		remoteRateEstimator->SetTemporalMaxLimit(estimation);

	//Demande EXPLICITE venue de l'aval (mode pont : TMMBR/REMB du puits relayé,
	//ou consigne négociée poussée au basculement). L'émettre sur le fil tout de
	//suite : le feedback spontané ci-dessus est verrouillé par la négociation
	//(défaut : aucun) et SetTemporalMaxLimit seul ne produit AUCUN paquet — la
	//limite restait lettre morte. SetMaxReceiveBitrate la compose avec
	//l'estimation locale, l'amortit (une baisse part tout de suite, une hausse
	//attend 200 ms) et l'émet dans le dialecte négocié — un navigateur ne
	//comprend que REMB. La retransmission tant que le TMMBN du pair n'arrive
	//pas est assurée par SendSenderReport (pendingTMBR).
	SetMaxReceiveBitrate(estimation);
}

int RTPEndpoint::RequestUpdate()
{
	//Request FIR
	RequestFPU();
	return 0;
}

void RTPEndpoint::AcknowledgeReferencePicture(WORD pictureId)
{
	//Le RPSI répond au flux ENTRANT de cette jambe : flux par défaut (ssrc=0)
	SendReferencePictureSelectionIndication(0,pictureId);
}

 xmlrpc_value* ExternalFIRRequestedEvent::GetXmlValue(xmlrpc_env *env)
{
	 BYTE sessTag[1024];
	UTF8Parser sessTagParser(sessionTag);
	DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
	sessTag[sessLen] = 0;
    return xmlrpc_build_value(env,"(isiii)",(int)JSR309Event::ExternalFIRRequestedEvent,sessTag,this->joinableId,(int)this->media,(int)this->role);

}

 xmlrpc_value* EndpointDisconnectedEvent::GetXmlValue(xmlrpc_env *env)
{
	BYTE sessTag[1024];
	UTF8Parser sessTagParser(sessionTag);
	DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
	sessTag[sessLen] = 0;
    return xmlrpc_build_value(env,"(isiii)",(int)JSR309Event::EndpointDisconnectedEvent,sessTag,this->joinableId,(int)this->media,(int)this->role);

}

 xmlrpc_value* EndpointConnectedEvent::GetXmlValue(xmlrpc_env *env)
{
	BYTE sessTag[1024];
	UTF8Parser sessTagParser(sessionTag);
	DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
	sessTag[sessLen] = 0;
    return xmlrpc_build_value(env,"(isiii)",(int)JSR309Event::EndpointConnectedEvent,sessTag,this->joinableId,(int)this->media,(int)this->role);

}
