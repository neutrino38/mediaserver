#ifndef _WebSocketServer_H_
#define _WebSocketServer_H_
#include "worker.h"
#include "pthread.h"
#include <list>
#include <map>
#include <mutex>
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

	/**
	 * Connect — ouvre une connexion WebSocket SORTANTE : le mediaserver joue le
	 * navigateur (SPEC docs/conception/WS-CLIENT §4.5).
	 *
	 * Appelable depuis n'importe quel thread. Tout ce qui peut ATTENDRE se fait
	 * ICI, dans le thread appelant : analyse de l'URL, résolution DNS, création du
	 * socket et `connect()` non bloquant. Une résolution DNS lente dans le
	 * réacteur gèlerait TOUTES les jambes WebSocket du serveur — le même piège
	 * que le callback bloquant du réacteur RTP (docs/reference/threads-rtp.md).
	 * Seul le socket déjà en cours de connexion traverse, par une file protégée ;
	 * la map des connexions reste la propriété exclusive du thread réacteur.
	 *
	 * Rend false — et n'a alors notifié personne — pour tout échec SYNCHRONE :
	 * URL inutilisable, hôte introuvable, socket impossible. Tout ce qui suit est
	 * asynchrone et se dit au `listener` : `onOpen` à l'ouverture, `onError` puis
	 * `onClose` sur échec (refus TCP, code autre que 101, clé d'acceptation
	 * fausse).
	 *
	 * Le schéma de l'URL décide du transport, et lui seul : le mode `wss://` du
	 * serveur (SetSecure) ne concerne QUE ses connexions entrantes.
	 */
	bool Connect(const std::string& url, std::weak_ptr<WebSocket::Listener> listener);

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


	//Une connexion sortante demandée par un autre thread : le socket est DÉJÀ en
	//cours de connexion, il ne reste au réacteur qu'à l'adopter.
	struct PendingConnect
	{
		int		fd;
		std::string	host;	//valeur de l'en-tête Host (hôte[:port])
		std::string	path;	//chemin + query
		std::weak_ptr<WebSocket::Listener> listener;
	};

	void CreateConnection(int fd);
	void CreateClientConnection(const PendingConnect& request);
	void DrainPendingConnects();
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
	//Demandes de connexion sortante en attente d'adoption par le réacteur.
	//pendingMutex protège UNIQUEMENT cette file : c'est le seul état du serveur
	//qu'un autre thread touche.
	std::mutex			pendingMutex;
	std::list<PendingConnect>	pendingConnects;
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
