#ifndef DCENDPOINT_H
#define DCENDPOINT_H

#include <string>
#include "RTPEndpoint.h"
#include "medkit/codecs.h"
#include "redcodec.h"
#include "sctptransport.h"
#include "t140datachannel.h"
#include "text.h"

/*
 * La jambe texte d'un endpoint JSR-309, portée par un data channel WebRTC.
 *
 * Le miroir de `WSEndpoint`, avec une différence qui change tout : le WebSocket
 * est une deuxième connexion, hors de la `RTCPeerConnection`, avec son port et
 * son URL ; le data channel est DEDANS. Cette jambe est donc ICE + DTLS + UDP
 * comme n'importe quelle jambe RTP — et c'est pour cela qu'elle DÉRIVE de
 * `RTPEndpoint` au lieu de réécrire un transport : port local, profil
 * d'adressage, latch, deux rôles DTLS, watchdog, boucle poll, tout est déjà là.
 * Seul change ce qui voyage dedans : du SCTP au lieu du RTP.
 *
 *   jambe pontée ──onRTPPacket──► RedCodec ──► T140DataChannel ──► SCTP ──► DTLS
 *   jambe pontée ◄───Multiplex──── RedCodec ◄── T140DataChannel ◄── SCTP ◄── DTLS
 *
 * Conception : docs/conception/T140-DC/SPEC.md §6.
 */
class DCEndpoint :
	public RTPEndpoint,
	public RTPSession::ApplicationListener,
	public SCTPTransport::Listener,
	public T140DataChannel::Listener,
	public TextOutput
{
public:
	//Le `a=sctp-port` par défaut de RFC 8841, celui qu'utilisent les navigateurs.
	static constexpr WORD DefaultSCTPPort = 5000;
	//Cadence prêtée à la pile SCTP par la boucle de la session, en ms.
	static constexpr DWORD TickMs = 10;

	DCEndpoint(MediaFrame::Type type);
	virtual ~DCEndpoint();

	//Endpoint::Port
	virtual int Init();
	virtual int End();
	//Pas de thread de démultiplexage : il n'arrive aucun RTP sur cette jambe. La
	//boucle poll de la session, démarrée à Init, porte tout — ICE, DTLS, et la
	//cadence de la pile SCTP.
	virtual int StartReceiving();
	virtual int StopReceiving();

	//Joinable::Listener — la jambe pontée parle, en T140 ou T140RED
	virtual void onRTPPacket(RTPPacket& packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//TextOutput — sortie du décodeur de redondance
	virtual int SendFrame(TextFrame& frame);

	//RTPSession::ApplicationListener
	virtual void onDTLSApplicationData(const BYTE* data,DWORD size);
	virtual DWORD GetApplicationTickMs() { return TickMs; }
	virtual void onApplicationTick(DWORD elapsedMs);

	//SCTPTransport::Listener
	virtual void onSCTPOutboundReady();
	virtual void onSCTPMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size);
	virtual void onSCTPAssociationUp();
	virtual void onSCTPAssociationDown();

	//T140DataChannel::Listener
	virtual void onT140Block(const BYTE* data,DWORD size);
	virtual void onT140ChannelOpen();
	virtual void onT140ChannelLost();

	//Le `a=sctp-port` du pair. L'association n'est montée qu'une fois le
	//handshake DTLS terminé — sans quoi son premier INIT serait perdu et
	//attendrait une retransmission d'une seconde.
	void  SetRemoteSCTPPort(WORD port);
	WORD  GetLocalSCTPPort() const	{ return localSCTPPort; }
	DWORD GetMaxMessageSize() const	{ return SCTPTransport::MaxMessageSize; }
	//Le flux du canal, ou -1 tant qu'il n'y en a pas : le contrôleur n'en a
	//besoin que s'il veut émettre un `a=dcmap`.
	int   GetStreamId() const;

	//Ouvre le canal NOUS-MÊMES (DATA_CHANNEL_OPEN), au lieu d'attendre celui du
	//pair. Le cas nominal ne l'appelle pas : nous répondons `a=setup:passive`,
	//donc c'est le navigateur qui crée le canal — comme c'est lui qui se connecte
	//à notre WebSocket. `streamId` doit respecter la parité de RFC 8832 §6 :
	//IMPAIR pour le serveur DTLS, le rôle que nous tenons.
	int   OpenDataChannel(WORD streamId);

	//Ce que la jambe pontée attend, lu dans sa rtpMap par Endpoint::StartReceiving :
	//la redondance de RFC 4103 n'a aucun sens SUR le canal (SCTP est fiable), mais
	//elle en a sur la patte RTP d'en face, et c'est nous qui la produisons.
	void SetUseRed(bool red)		{ useRed = red; }
	void SetPrimaryPayloadType(BYTE pt)	{ payloadType = pt; }

private:
	void FlushSCTP();

	SCTPTransport	sctp;
	T140DataChannel	t140;
	RedundentCodec	redCodec;

	WORD	localSCTPPort;
	WORD	remoteSCTPPort;
	//L'association est demandée mais pas encore montée : la boucle attend la fin
	//du handshake DTLS pour l'ouvrir.
	bool	associationWanted;

	bool	useRed;
	BYTE	payloadType;

	//Horloge et numéros de séquence synthétiques des paquets qu'on fabrique pour
	//la jambe pontée : le texte n'a pas de contrainte de gigue, et rien ne relie
	//ces numéros à ceux de l'autre patte.
	timeval	clock;
	WORD	pseudoSeqNum;
	WORD	pseudoSeqCycle;
};

#endif /* DCENDPOINT_H */
