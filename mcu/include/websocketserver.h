#ifndef _WebSocketServer_H_
#define _WebSocketServer_H_
#include "worker.h"
#include "pthread.h"
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include "websocketconnection.h"	//apporte config.h (BYTE/DWORD) avant websockets.h
#include "websockets.h"


/**
 * WebSocketServer
 *
 * Un thread unique + un poll() unique gèrent TOUTES les connexions : le socket
 * d'écoute, un eventfd de réveil inter-thread, et le socket de chaque connexion.
 * Les WebSocketConnection sont des machines à état passives possédées ici via
 * shared_ptr (plus de thread ni de poll() par-connexion, plus de liste zombies).
 */
class WebSocketServer : public WebSocketConnection::Listener, public Worker
{
public:
	class Handler
	{
	public:
		virtual void onWebSocketConnection(const HTTPRequest& request,WebSocket *ws) = 0;

	};
public:
	/** Constructors */
	WebSocketServer();
	~WebSocketServer();

	int Init(int port);
	void AddHandler(const std::string base,Handler* hnd);
	int End();

	//Active le mode sécurisé (wss://). À appeler AVANT Init(). Les certificats
	//(PEM) sont chargés dans Init() via WebSocketTlsTransport::ClassInit().
	void SetSecure(bool secure, const char* certfile, const char* keyfile);

	//WebSocketConnection::Listener
	virtual void onUpgradeRequest(WebSocketConnection* conn);
	virtual void onWakeupNeeded();

public:
        int Run();

private:
	typedef std::map<std::string,Handler *> Handlers;
	//Map possédante : connId → connexion. Manipulée UNIQUEMENT par le thread serveur.
	typedef std::map<uint64_t,std::shared_ptr<WebSocketConnection>> Connections;


	void CreateConnection(int fd);
	void CloseConnection(uint64_t connId);
	void DrainWakeup();

private:
	int inited;
	int serverPort;
	int server;		//socket d'écoute
	uint64_t nextConnId;

	bool        secure;	//wss:// activé ?
	std::string certfile;
	std::string keyfile;

	Handlers handlers;
	Connections connections;
	//Connexions fermées au tour courant, gardées vivantes un tour de boucle de plus
	//pour laisser s'écouler d'éventuels appels externes en vol (cf. §3.3 du plan).
	std::vector<std::shared_ptr<WebSocketConnection>> recentlyClosed;

};



class TextEchoWebsocketHandler :
	public WebSocketServer::Handler,
	public WebSocket::Listener,
	public std::enable_shared_from_this<TextEchoWebsocketHandler>
{
public:
	virtual void onWebSocketConnection(const HTTPRequest& request,WebSocket *ws)
	{
		Debug("-onUpgradeRequest %s\n", request.GetRequestURI().c_str());
		ws->Accept(weak_from_this());
	}
	virtual void onOpen(WebSocket *ws)
	{
		Debug("-onOpened\n");
	}
	virtual void onMessageStart(WebSocket *ws,WebSocket::MessageType type,const DWORD length)
	{
		Debug("-onMessageStart\n");
	}
	virtual void onMessageData(WebSocket *ws,const BYTE* data, const DWORD size)
	{
		std::string str((char*)data,size);
		Debug("-onMessageData %s\n",str.c_str());
		//Dump(data,size);
		ws->SendMessage(str);
	}
	virtual void onMessageEnd(WebSocket *ws)
	{
		Debug("-onMessageEnd\n");
	}
	virtual void onError(WebSocket *ws)
	{
		Debug("-onError\n");
	}
	virtual void onClose(WebSocket *ws)
	{
		Debug("-onClose\n");
	}
};

#endif
