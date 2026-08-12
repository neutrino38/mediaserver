/* 
 * File:   RTPEndpoint.h
 * Author: Sergio
 *
 * Created on 7 de septiembre de 2011, 12:16
 */

#ifndef RTPENDPOINT_H
#define	RTPENDPOINT_H

#include "rtpsession.h"
#include "RTPMultiplexer.h"
#include "Joinable.h"
#include "Endpoint.h"
#include "JSR309Event.h"

class RTPEndpoint :
	public RTPSession,
	public Endpoint::Port,
	public Joinable::Listener,
	public RTPSession::Listener
{
public:
	RTPEndpoint(MediaFrame::Type type, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	virtual ~RTPEndpoint();

	virtual int Init();
	virtual int RequestUpdate();
	virtual int StartReceiving();
	virtual int StopReceiving();
	virtual int StartSending();
	virtual int StopSending();
	virtual int End();

	MediaFrame::Type GetType() { return type; }

	//Attach/Dettach to joinables
	int Attach(const std::shared_ptr<Joinable> & join);
	int Detach();

	//Joinable interface
	virtual void Update();
	virtual void SetREMB(DWORD estimation);

	//Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();
        virtual int  TryCheckCodec(int codec);

	//RTPSession::Listener
	virtual void onFPURequested(RTPSession *session);
	virtual void onReceiverEstimatedMaxBitrate(RTPSession *session,DWORD bitrate);
	virtual void onTempMaxMediaStreamBitrateRequest(RTPSession *session,DWORD bitrate,DWORD overhead);
	//Watchdog d'inactivité RTP (gap 5) : publie EndpointDisconnectedEvent
	virtual void onRTPTimeout(RTPSession *session);
	//P5 : premier paquet RTP/SRTP reçu (DTLS terminé ou pas de DTLS) : publie
	//EndpointConnectedEvent une seule fois par cycle de réception.
	virtual void onRTPPacketReceived(RTPSession *session);
        void SetTsTransparency(bool transparent)
	{
		tsTransparency = transparent;
	}
	
private:
	//Corps du thread de démultiplexage (pthread créé par StartReceiving).
	//
	//NE PAS le renommer `Run()` : `RTPSession` dérive de `Worker`, dont `Run()`
	//est virtuel pur et porte la boucle poll des sockets RTP/RTCP. Un `Run()` ici
	//OVERRIDE celui de `RTPSession` — le thread du Worker exécutait alors cette
	//boucle-ci, et la boucle poll ne tournait JAMAIS pour un endpoint JSR-309
	//(aucun paquet RTP lu). C'était le cas jusqu'au 2026-08-12.
	int MultiplexLoop();

	//Bascule le PT d'émission sur `wanted`, ou rend false si la rtpMap de sortie
	//négociée ne le porte pas — auquel cas l'appelant ne DOIT pas émettre le
	//paquet. Borne le harcèlement : un codec refusé le reste jusqu'à une
	//renégociation, et RTPSession::SetSendingCodec journalise une Error par appel.
	bool TrySendingCodec(DWORD wanted);

	//Funciones propias
	static void *run(void *par);

private:
	//Sentinelle « aucun codec » de `codec` et d'`unmappedCodec` : c'est déjà celle
	//qu'onResetStream écrit.
	static const DWORD NoCodec = (DWORD)-1;

	pthread_t thread;
	DWORD codec;
	DWORD timestamp;
	DWORD freq;
	timeval prev;
	DWORD prevts;
	bool reseted;
	bool tsTransparency;
	//Dernier codec refusé par la rtpMap de sortie, l'instant du refus, et les
	//paquets jetés depuis le dernier journal.
	DWORD unmappedCodec;
	QWORD unmappedTs;
	DWORD unmappedCount;
};

class ExternalFIRRequestedEvent: public JSR309Event
{
public:
	ExternalFIRRequestedEvent()
	{

	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env);


};

/**
 * Déconnexion d'un endpoint détectée par le watchdog d'inactivité RTP (gap 5).
 * Sérialisation XML-RPC : (isiii) = {type, sessionTag, joinableId(endpointId),
 * media, role} — même forme que ExternalFIRRequestedEvent (FillEvent remplit
 * joinableId/media/role depuis le contexte).
 */
class EndpointDisconnectedEvent: public JSR309Event
{
public:
	EndpointDisconnectedEvent()
	{

	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env);
};

/**
 * Média établi sur un endpoint (P5) : émis UNE fois par média/cycle de réception,
 * lorsque le handshake DTLS est terminé (ou qu'il n'y a pas de DTLS) ET que le
 * premier paquet RTP/SRTP a été reçu et validé. Même sérialisation que les autres :
 * (isiii) = {type, sessionTag, joinableId(endpointId), media, role}.
 */
class EndpointConnectedEvent: public JSR309Event
{
public:
	EndpointConnectedEvent()
	{

	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env);
};

#endif	/* RTPENDPOINT_H */

