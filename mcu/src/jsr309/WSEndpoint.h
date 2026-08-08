#ifndef WSENDPOINT_H
#define	WSENDPOINT_H

#include <list>
#include <string>
#include <utility>
#include "RTPMultiplexer.h"
#include "Joinable.h"
#include "websocketserver.h"
#include "websockets.h"
#include "Endpoint.h"
#include "medkit/codecs.h"
#include "text.h"
#include "redcodec.h"

class WSEndpoint : 
	public Endpoint::Port,
	public Joinable::Listener,
	public WebSocket::Listener,
	public TextOutput
{
public :
	WSEndpoint(MediaFrame::Type type);
	
	virtual ~WSEndpoint();

	// Port interface
	virtual int Init() { return 0; };
	virtual int End();

	//Joinable interface
	virtual void Update() {};
	virtual void SetREMB(DWORD estimation) {};

	//Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream() { SendReplacementChar(true); };
	virtual void onEndStream() { SendReplacementChar(true); };
	//Le lien retour Endpoint::Port::joined est désormais un weak_ptr : le
	//Port::Detach le lock() et ne déréférence jamais une source détruite. Plus
	//besoin de notification onJoinableEnded (C-13, lien A).

	//Websocket::Listener
	virtual void onOpen(WebSocket *ws);
	virtual void onMessageStart(WebSocket *ws,const WebSocket::MessageType type,const DWORD length) ;
	virtual void onMessageData(WebSocket *ws,const BYTE* data, const DWORD size);
	virtual void onMessageEnd(WebSocket *ws);
	virtual void onError(WebSocket *ws);
	virtual void onClose(WebSocket *ws);
		
	static void SetLocalPort(int port);
	static void SetLocalHost(char* host);
	//wss:// ou ws:// ? Réglé une fois au démarrage depuis --websocket-secure
	//(main.cpp). Le serveur est le SEUL à le savoir : le contrôleur SIP construit
	//l'URL qu'il publie dans son SDP depuis GetMediaCandidates, et ne peut pas
	//deviner si nous écoutons en TLS — nous écoutons sur le même port dans les
	//deux cas.
	static void SetLocalSecure(bool secure);
	static bool IsLocalSecure();

	//Statiques : ils ne lisent que la configuration globale du serveur WS (un
	//seul port/host pour tout le binaire, posés par main.cpp). L'API
	//conférence (S5, MultiConf::ConfigureParticipantMediaConnection) les
	//appelle sans instance ; l'appel via une instance (Endpoint::Port::
	//GetLocalMediaHost) reste valide.
	static int  	GetLocalPort();
	static char*  	GetLocalHost();
	
	void SetUseRed(bool red){useRed = red;};
	void SetPrimaryPayloadType(BYTE pt){payloadType = pt;};
	
	virtual int SendFrame(TextFrame &frame);
	
private:
	WebSocket::MessageType msgType;
	MediaFrame * media;
	
	// to generate timestamp
	timeval clock;
	Joinable *joined;
	
	//weak_ptr : le WebSocket est possédé par le WebSocketServer. On le verrouille
	//avant chaque appel (SendMessage/Close) → sûr même si la connexion est détruite
	//de façon concurrente par le thread serveur.
	std::weak_ptr<WebSocket> _ws;
	bool		useRed;
	static int  	wsPort;
	static char* 	wsHost;
	static bool 	wsSecure;
	BYTE		payloadType;

	//Trames reçues du côté RTP avant que le navigateur n'ait ouvert son
	//WebSocket : entre le 200 OK et le handshake il s'écoule un aller-retour SDP,
	//et la première phrase est justement celle où l'appelant se présente. Bornée
	//dans les deux dimensions (§4.5 de jsr309_text_over_wss.md) : une file non
	//bornée sur un flux que personne ne viendra peut-être jamais lire est une
	//fuite.
	static const size_t maxPendingFrames = 32;
	static const QWORD  maxPendingAgeMs  = 5000;
	std::list<std::pair<QWORD,std::string>> pending;
	
	RedundentCodec* RedCodec;
	WORD pseudoSeqNum; 
	WORD pseudoSeqCycle; 
	
	void SendReplacementChar(bool toWsSide);
	void PacketToWs(TextFrame & frame);	
};
#endif	/* WSENDPOINT_H */
