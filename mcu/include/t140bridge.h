#ifndef T140BRIDGE_H
#define T140BRIDGE_H

#include "config.h"
#include "rtpsession.h"
#include "sctptransport.h"
#include "t140datachannel.h"

/*
 * Le collage entre une `RTPSession` et la pile T.140 sur data channel.
 *
 * Les deux API du serveur portent le même texte sur le même transport, et le
 * câblage est le même des deux côtés : livrer à la pile les données applicatives
 * que le DTLS déchiffre, lui prêter la cadence de la boucle poll, et vider sa
 * file de sortie depuis le thread de cette boucle — jamais ailleurs, l'objet
 * `SSL` n'étant pas concurrent. Ce qui diffère est au-dessus : la jambe JSR-309
 * (`DCEndpoint`) convertit en RTP T140/T140RED pour la jambe pontée, le flux de
 * conférence (`TextStream`) écrit dans les pipes du mixeur.
 *
 * Le porteur reste la session : ICE, DTLS, socket, port local, profil
 * d'adressage, latch et watchdog sont les siens. Cette classe n'en connaît que
 * trois choses — chiffrer, se réveiller, et dire si le handshake est fini.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §5.6.
 */
class T140Bridge :
	public RTPSession::ApplicationListener,
	public SCTPTransport::Listener
{
public:
	//Le `a=sctp-port` par défaut de RFC 8841, celui qu'utilisent les navigateurs.
	static constexpr WORD  DefaultSCTPPort = 5000;
	//Cadence prêtée à la pile SCTP par la boucle de la session, en ms.
	static constexpr DWORD TickMs = 10;

	T140Bridge(RTPSession& session,T140DataChannel::Listener& listener);
	~T140Bridge();

	//Se branche sur la session et demande l'association. Celle-ci ne s'ouvre
	//qu'une fois le handshake DTLS terminé : une jambe qui ne porte pas de RTP ne
	//négocie aucun profil SRTP, donc `onDTLSSetup` ne lui dit jamais rien, et un
	//INIT émis trop tôt coûterait une retransmission d'une seconde.
	int  Start();
	//Se débranche, ferme l'association et signale la perte du canal. L'appelant
	//doit avoir arrêté la boucle de la session AVANT : elle bat la cadence de la
	//pile et vide sa file, démonter l'une sous l'autre est une course.
	int  Stop();

	//Émet un T140block. Sûr depuis n'importe quel thread : rien n'est chiffré
	//ici. Tant que le canal n'est pas ouvert, la trame attend (bornée).
	int  SendText(const BYTE* data,DWORD size);
	//Ouvre le canal nous-mêmes. Le cas nominal ne l'appelle pas : nous répondons
	//`a=setup:passive`, donc c'est le pair qui crée le canal.
	int  OpenChannel(WORD streamId);

	void  SetRemoteSCTPPort(WORD port);
	WORD  GetLocalSCTPPort() const	{ return localSCTPPort; }
	DWORD GetMaxMessageSize() const	{ return SCTPTransport::MaxMessageSize; }
	bool  IsOpen() const		{ return t140.IsOpen(); }
	//Le flux du canal, ou -1 tant qu'il n'y en a pas.
	int   GetStreamId() const;

	//RTPSession::ApplicationListener
	virtual void onDTLSApplicationData(const BYTE* data,DWORD size);
	virtual DWORD GetApplicationTickMs() { return TickMs; }
	virtual void onApplicationTick(DWORD elapsedMs);

	//SCTPTransport::Listener
	virtual void onSCTPOutboundReady();
	virtual void onSCTPMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size);
	virtual void onSCTPAssociationUp();
	virtual void onSCTPAssociationDown();

private:
	void Flush();

	RTPSession&	session;
	SCTPTransport	sctp;
	T140DataChannel	t140;

	WORD	localSCTPPort;
	WORD	remoteSCTPPort;
	//L'association est demandée mais pas encore montée : la boucle attend la fin
	//du handshake DTLS pour l'ouvrir.
	bool	wanted;
};

#endif /* T140BRIDGE_H */
