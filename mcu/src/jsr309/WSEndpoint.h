#ifndef WSENDPOINT_H
#define	WSENDPOINT_H

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
	
	int  	GetLocalPort();
	char*  	GetLocalHost();
	
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
	BYTE		payloadType;
	
	RedundentCodec* RedCodec;
	WORD pseudoSeqNum; 
	WORD pseudoSeqCycle; 
	
	void SendReplacementChar(bool toWsSide);
	void PacketToWs(TextFrame & frame);	
};
#endif	/* WSENDPOINT_H */
