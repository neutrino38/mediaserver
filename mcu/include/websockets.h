/* 
 * File:   websockets.h
 * Author: Sergio
 *
 * Created on 3 de enero de 2013, 12:40
 */

#ifndef WEBSOCKETS_H
#define	WEBSOCKETS_H

#include <memory>
#include "ipaddress.h"

class WebSocket
{
public:
	enum MessageType { Text = 0, Binary = 1 };
	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		virtual void onOpen(WebSocket *ws) = 0;
		virtual void onMessageStart(WebSocket *ws,const WebSocket::MessageType type,const DWORD length) = 0;
		virtual void onMessageData(WebSocket *ws,const BYTE* data, const DWORD size) = 0;
		virtual void onMessageEnd(WebSocket *ws) = 0;
		virtual void onError(WebSocket *ws) = 0;
		virtual void onClose(WebSocket *ws) = 0;
	};
public:
	virtual void Accept(std::weak_ptr<Listener> listener) = 0;
	virtual void Reject(const WORD code, const char* reason) = 0;
	virtual void SendMessage(const std::string& message) = 0;
	virtual void SendMessage(const BYTE* data, const DWORD size) = 0;
	virtual void Close() = 0;
	//RFC 6455 §5.5.1 closing: a Close frame carrying `code`, then the cut once
	//the output is flushed. Nothing is sent after it.
	virtual void Close(const WORD code, const char* reason) = 0;

	//Renvoie un weak_ptr vers ce WebSocket, pour qu'un consommateur (ex. WSEndpoint,
	//sur un autre thread) puisse verrouiller la référence avant chaque appel et
	//éviter tout usage-après-libération. Défaut : vide (surchargé par les
	//implémentations gérées par shared_ptr, comme WebSocketConnection).
	virtual std::weak_ptr<WebSocket> GetWeakPtr()	{ return std::weak_ptr<WebSocket>(); }

	//Peer address, empty when unknown.
	virtual IPEndpoint GetRemoteEndpoint() const	{ return IPEndpoint(); }

	virtual void* GetUserData()			{ return userData;		}
	virtual void  SetUserData(void* userData)	{ this->userData = userData;	}
	
private:
	void 		*userData;
};



#endif	/* WEBSOCKETS_H */

