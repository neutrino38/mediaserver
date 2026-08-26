#ifndef T140DATACHANNEL_H
#define T140DATACHANNEL_H

#include <list>
#include <mutex>
#include <string>
#include <utility>
#include "config.h"
#include "datachannel.h"
#include "sctptransport.h"

/*
 * T.140 sur un data channel WebRTC — RFC 8865.
 *
 * La seule couche du chantier qui connaisse T.140, et la seule que les deux API
 * (JSR-309 et conférence) partagent. En dessous, une association SCTP ; au
 * dessus, des T140block. Elle ne connaît ni ICE, ni DTLS, ni UDP, ni RTP.
 *
 * Le contrat de RFC 8865 tient en trois lignes : le canal est FIABLE et ORDONNÉ,
 * son sous-protocole est `t140`, et un message SCTP porte exactement un
 * T140block. La redondance de RFC 4103 n'a donc aucun sens ici — c'est l'affaire
 * de la jambe RTP d'en face.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §5.5.
 */
class T140DataChannel
{
public:
	//PPID des data channels WebRTC (RFC 8831 §8).
	enum PPID
	{
		PPIDControl	= 50,	//DCEP
		PPIDString	= 51,	//un T140block
		PPIDBinaryPartial = 52,	//obsolète
		PPIDBinary	= 53,
		PPIDStringPartial = 54,	//obsolète
		PPIDStringEmpty	= 56,	//T140block vide
		PPIDBinaryEmpty	= 57,
	};

	//Le sous-protocole que RFC 8865 impose au canal.
	static const char* const SubProtocol;

	class Listener
	{
	public:
		virtual ~Listener(){};

		//Un T140block reçu du pair. Appelé depuis le thread qui a livré le
		//datagramme SCTP, et ce n'est pas toujours celui du porteur (les timers
		//de la pile sont globaux) : le destinataire doit être sûr en concurrence.
		virtual void onT140Block(const BYTE* data,DWORD size) = 0;

		//Le canal s'ouvre, ou disparaît. T.140 §5.3 demande un U+FFFD du côté
		//qui survit à une perte : c'est l'adaptateur qui l'émet, lui seul sait
		//vers quoi.
		virtual void onT140ChannelOpen() {}
		virtual void onT140ChannelLost() {}
	};

	T140DataChannel(SCTPTransport& sctp,Listener& listener);

	//Ouvre le canal nous-mêmes, au lieu d'attendre celui du pair. `streamId`
	//doit respecter la parité de RFC 8832 §6 : PAIR pour le client DTLS, IMPAIR
	//pour le serveur. Le cas nominal ne l'appelle pas — c'est le navigateur qui
	//crée le canal, comme c'est lui qui se connecte à notre WebSocket.
	int OpenChannel(WORD streamId);

	//À appeler depuis SCTPTransport::Listener, par le propriétaire.
	void OnMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size);
	void OnAssociationUp();
	void OnAssociationDown();

	//Émet un T140block. Sûr depuis n'importe quel thread : rien n'est chiffré
	//ici, la pile est verrouillée en interne et le datagramme finit dans la file
	//de sortie du transport. Tant que le canal n'est pas ouvert, la trame attend.
	int  SendText(const BYTE* data,DWORD size);

	bool IsOpen() const { return open; }
	WORD GetStreamId() const { return streamId; }

private:
	void Bind(WORD streamId);
	void FlushPending();

	SCTPTransport&	sctp;
	Listener&	listener;

	bool	bound;		//un flux est retenu
	bool	open;		//et le canal y est établi (OPEN vu, ou ACK reçu)
	WORD	streamId;

	//Texte émis avant que le canal ne soit là : entre le 200 OK et l'ouverture
	//il s'écoule un aller-retour SDP, un ICE et un handshake DTLS, et la
	//première phrase est justement celle où l'appelant se présente. Bornée dans
	//les DEUX dimensions : une file non bornée sur un flux que personne ne
	//viendra peut-être jamais lire est une fuite.
	static const size_t maxPendingFrames = 32;
	static const QWORD  maxPendingAgeMs  = 5000;

	std::mutex	mutex;
	timeval		clock;
	std::list<std::pair<QWORD,std::string>> pending;
};

#endif /* T140DATACHANNEL_H */
