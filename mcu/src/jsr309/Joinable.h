/* 
 * File:   Joinable.h
 * Author: Sergio
 *
 * Created on 7 de septiembre de 2011, 0:59
 */

#ifndef JOINABLE_H
#define	JOINABLE_H
#include <map>
#include <memory>
#include "config.h"
#include "rtp.h"
#include "xmlstreaminghandler.h"
#include "JSR309Event.h"

class JSR309Manager;

class Joinable
{
public:

	Joinable();
	~Joinable();
	
	class Listener 
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		virtual void onRTPPacket(RTPPacket &packet) = 0;
		virtual void onResetStream() = 0;
		virtual void onEndStream() = 0;
        virtual int  TryCheckCodec(int codec) { return codec; }
	};
	
public:
	int	SetEventHandler(int sessionId,JSR309Manager* jsrManager);
	int	SetEventContextId(int eventContextId );
	int	GetEventContextId() {return eventContextId;};
	
	virtual void AddListener(Listener *listener) = 0;
	virtual void Update() = 0;
	virtual void SetREMB(DWORD estimation) = 0;
	virtual void RemoveListener(Listener *listener) = 0;

	//Cible du BWE émetteur LOCAL de la patte sortante (lot 6.3, bps) : à
	//composer par min() avec la limite du pair (SetREMB), jamais à écraser.
	//No-op par défaut : seul le producteur qui ENCODE la consomme ; en mode
	//relais, la propagation amont est l'affaire du lot 5, pas de ce canal.
	virtual void SetSenderEstimate(DWORD estimation) {}

	//Acquittement d'une trame de référence que le consommateur aval vient de
	//décoder (RPSI, RFC 4585 §6.3.3) — le symétrique positif d'Update(), et
	//le même chemin de retour vers la source. No-op par défaut : seule une
	//jambe RTP a un pair à qui l'adresser (mixers, players : rien à faire).
	virtual void AcknowledgeReferencePicture(WORD pictureId) {}

	//Média effectivement négocié côté source (StartReceiving reçu) — override
	//dans Endpoint::Port. Les sources toujours actives (mixers, transcoders,
	//players) gardent le défaut.
	virtual bool IsReceiving() const { return true; }

	//Bornes négociées de l'encodeur d'émission (phase 5 nego_fmtp §6.3) : la
	//patte qui émettra ce flux pousse ici, par code codec, les Properties que
	//SA négociation SDP impose au producteur — profil H.264 et
	//packetization-mode déclarés par le pair, que l'encodeur ne doit pas
	//dépasser. No-op par défaut : seuls les producteurs qui ENCODENT
	//(VideoEncoderMultiplexerWorker, relayé par VideoTranscoder) les
	//consomment — un player ou un relais B2B n'a pas d'encodeur à contraindre.
	//
	//Un producteur PARTAGÉ entre plusieurs pattes (port de mixer écouté par
	//plusieurs endpoints) reçoit les bornes de la dernière patte qui a poussé :
	//un même flux ne peut pas satisfaire deux bornes à la fois — des bornes par
	//patte exigent un producteur par patte, ce qu'un transcodeur donne.
	virtual void SetNegotiatedCodecProperties(const std::map<int,Properties>& byCodec) {}
protected:
	bool PostEvent( JSR309Event *event);
private:
	int sessionId;
	int eventContextId;
	JSR309Manager* jsrManager;
};


#endif	/* JOINABLE_H */

