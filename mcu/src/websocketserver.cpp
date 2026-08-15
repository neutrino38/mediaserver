#include "ipaddress.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <vector>
#include <set>
#include "tools.h"
#include "log.h"
#include "websocketserver.h"

/************************
* WebSocketServer
* 	Constructor
*************************/
WebSocketServer::WebSocketServer()
{
	//Y no tamos iniciados
	inited = 0;
	serverPort = 0;
	server = FD_INVALID;
	nextConnId = 0;
	secure = false;
}

void WebSocketServer::SetSecure(bool secure, const char* certfile, const char* keyfile)
{
	this->secure   = secure;
	this->certfile = certfile ? certfile : "";
	this->keyfile  = keyfile  ? keyfile  : "";
}


/************************
* ~ WebSocketServer
* 	Destructor
*************************/
WebSocketServer::~WebSocketServer()
{
	//Check we have been correctly ended
	if (inited)
		//End it anyway
		End();
}

void WebSocketServer::AddHandler(const std::string base,Handler* hnd)
{
	Log("-WebSocket handler on %s\n",base.c_str());

	//Add to the map
	handlers[base] = hnd;
}

/************************
* Init
* 	Open the listening server port
*************************/
int WebSocketServer::Init(int port)
{


	//Check not already inited
	if (inited)
		//Error
		return Error("-Init: WebSocket Server is already running.\n");

	Log("-Init WebSocket Server [%d]\n",port);

	//Save server port
	serverPort = port;

	//Mode sécurisé : initialiser le contexte TLS serveur
	if (secure)
	{
		if (!WebSocketTlsTransport::ClassInit(certfile, keyfile))
			return Error("-Init: cannot init TLS (cert:\"%s\",key:\"%s\")\n",
				     certfile.c_str(), keyfile.c_str());
		Log("-WebSocket Server: secure mode (wss://) enabled\n");
	}


	//Create socket. AF_INET6 + IPV6_V6ONLY=0 : UNE socket entend les deux
	//familles, un client v4 arrivant en ::ffff:a.b.c.d. Les plans de contrôle
	//doivent tout entendre — contrairement au média, dont la famille est choisie
	//par le profil d'adressage (§14.5 d'ipv6.md).
	server = socket(AF_INET6, SOCK_STREAM, 0);

	//Set SO_REUSEADDR on a socket to true (1):
	int optval = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	//Sans cela une socket v6 n'entend QUE de l'IPv6 : la bascule ferait perdre
	//tous les clients v4 d'un coup. Un échec n'est pas fatal (certains noyaux
	//imposent net.ipv6.bindv6only=1), mais il change le service rendu : il se
	//journalise.
	int v6only = 0;
	if (setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0)
		Error("-WebSocket Server: cannot clear IPV6_V6ONLY (errno %d) — IPv4 clients will not be served\n",errno);

	//Bind to first available port
	const IPEndpoint listenOn = IPAddress::Any(AF_INET6).To(serverPort);

	//Bind
     	if (bind(server, listenOn, listenOn.Len()) < 0)
		//Error
		return Error("Can't bind server socket. errno = %d.\n", errno);
	//I am inited
	inited = 1;

	//Create threads
	StartThread();

	//Return ok
	return 1;
}

/**************************
 * DrainWakeup
 * 	Vide le compteur de l'eventfd de réveil
 **************************/
void WebSocketServer::DrainWakeup()
{
	//eventfd du Wait hérité (Worker) : une lecture remet le compteur à 0
	wait.Drain();
}

/***************************
 * Run
 * 	Server running thread : un seul poll() pour toutes les connexions
 ***************************/
int WebSocketServer::Run()
{
init:
	//Log
	Log(">Run WebSocket Server [%p,port:%d]\n",this,serverPort);

	//Listen for connections
	if (listen(server,5)<0)
		//Error
		return Error("Can't listen on server socket. errno = %d\n", errno);

	//Set non blocking so accept() ne bloque pas et on récupère les erreurs
	int fsflags = fcntl(server,F_GETFL,0);
	fcntl(server,F_SETFL, fsflags | O_NONBLOCK);

	//Run until ended
	while(inited)
	{
		//Libère les connexions fermées au tour précédent (grâce d'un tour)
		recentlyClosed.clear();

		//Construit le jeu de poll : [écoute, réveil, connexions...]
		std::vector<pollfd>   ufds;
		std::vector<uint64_t> ids;   //connId aligné aux entrées « connexion »
		ufds.reserve(connections.size()+2);
		ids.reserve(connections.size());

		pollfd lu; lu.fd = server;   lu.events = POLLIN|POLLHUP|POLLERR; lu.revents = 0;
		ufds.push_back(lu);
		pollfd wu; wu.fd = wait.GetPollFd(); wu.events = POLLIN;         wu.revents = 0;
		ufds.push_back(wu);

		for (Connections::iterator it=connections.begin(); it!=connections.end(); ++it)
		{
			pollfd cu;
			cu.fd     = it->second->GetFd();
			cu.events = it->second->GetPollEvents();
			cu.revents= 0;
			ufds.push_back(cu);
			ids.push_back(it->first);
		}

		//Wait for events
		int n = poll(ufds.data(), ufds.size(), -1);
		if (n<0)
		{
			//EINTR n'est pas une erreur dure
			if (errno==EINTR)
				continue;
			Error("WebSocketServer: poll error [errno:%d]\n",errno);
			//Si on nous a arrêtés, sortir
			if (!inited)
				break;
			continue;
		}

		//Réveil inter-thread (SendMessage/Close depuis un autre thread, ou End)
		if (ufds[1].revents & POLLIN)
			DrainWakeup();

		//Nouvelles connexions entrantes
		if (ufds[0].revents & POLLIN)
		{
			//Accepte toutes les connexions en attente (socket non bloquant)
			while (true)
			{
				int fd = accept(server,NULL,0);
				if (fd<0)
					//Plus de connexion en attente (EAGAIN) ou erreur
					break;
				CreateConnection(fd);
			}
		}
		if (ufds[0].revents & (POLLHUP|POLLERR|POLLNVAL))
		{
			Error("WebSocketServer: listen socket error [revents:%d,errno:%d]\n",ufds[0].revents,errno);
			if (!inited)
				break;
		}

		//Traite chaque connexion
		std::set<uint64_t> toClose;
		for (size_t i=2; i<ufds.size(); ++i)
		{
			uint64_t id = ids[i-2];
			Connections::iterator it = connections.find(id);
			if (it==connections.end())
				continue;
			//Copie du shared_ptr : la connexion reste vivante pendant le dispatch
			std::shared_ptr<WebSocketConnection> conn = it->second;

			short re = ufds[i].revents;
			if (re & (POLLNVAL|POLLERR|POLLHUP))
			{
				toClose.insert(id);
				continue;
			}
			//Écrire d'abord la sortie en attente, puis lire l'entrée
			if (re & POLLOUT)
				conn->OnWritable();
			if (re & POLLIN)
				conn->OnReadable();
			if (conn->IsFinished())
				toClose.insert(id);
		}

		//Connexions dont la fermeture a été demandée depuis un autre thread
		//(Close) sans événement poll associé
		for (Connections::iterator it=connections.begin(); it!=connections.end(); ++it)
			if (it->second->IsFinished())
				toClose.insert(it->first);

		//Fermetures (après le dispatch, pour ne pas invalider l'itération)
		for (std::set<uint64_t>::iterator it=toClose.begin(); it!=toClose.end(); ++it)
			CloseConnection(*it);
	}

	Log("<Run WebSocket Server\n");

	return 0;
}

/*************************
 * CreateConnection
 * 	Create new WebSocket Connection for socket
 *************************/
void WebSocketServer::CreateConnection(int fd)
{
	//Identité stable (pas le fd, réutilisable)
	uint64_t id = ++nextConnId;

	//Choix du transport : TLS (wss://) ou clair (ws://)
	std::unique_ptr<WebSocketTransport> transport;
	if (secure)
	{
		transport = WebSocketTlsTransport::Create();
		if (!transport)
		{
			Error("-CreateConnection: TLS transport unavailable, dropping fd:%d\n",fd);
			shutdown(fd,SHUT_RDWR);
			close(fd);
			return;
		}
	}
	else
	{
		transport = std::make_unique<WebSocketPlainTransport>();
	}

	//Create new WebSocket connection (possédée par la map)
	std::shared_ptr<WebSocketConnection> conn = std::make_shared<WebSocketConnection>(this, id);

	Log("-Incoming connection [fd:%d,id:%llu,%s]\n",fd,(unsigned long long)id,secure?"tls":"plain");

	//Init connection (pas de thread : machine à état passive)
	conn->Init(fd, std::move(transport));

	//Store it
	connections[id] = conn;
}

/**************************
 * CloseConnection
 * 	Ferme et retire une connexion de la map
 **************************/
void WebSocketServer::CloseConnection(uint64_t connId)
{
	Connections::iterator it = connections.find(connId);
	if (it==connections.end())
		return;

	//Garder vivant pendant la fermeture
	std::shared_ptr<WebSocketConnection> conn = it->second;

	//Notifier le WebSocket::Listener (ex. WSEndpoint remet _ws=NULL)
	conn->NotifyClose();

	//Fermer le socket
	conn->End();

	//Retirer de la map : plus aucune nouvelle référence ne peut être distribuée
	connections.erase(it);

	//Garder l'objet vivant un tour de boucle de plus (appels externes en vol)
	recentlyClosed.push_back(conn);
}


/************************
* End
* 	End server and close all connections
*************************/
int WebSocketServer::End()
{
	Log(">End WebSocket Server\n");

	//Check we have been inited
	if (!inited)
		//Do nothing
		return 0;

	//Stop thread
	inited = 0;

	//Réveiller le thread serveur pour qu'il constate inited=0 (fiable même si le
	//close() du socket d'écoute ne fait pas sortir poll())
	onWakeupNeeded();

	//Close server socket
	shutdown(server,SHUT_RDWR);
	//Will cause poll function to exit
	close(server);
	//Invalidate
	server = FD_INVALID;

	//Wait for server thread to close (le Wait du Worker est aussi réveillé)
	StopThread();

	//Détruire les connexions restantes
	connections.clear();
	recentlyClosed.clear();

	Log("<End WebSocket Server\n");
	return 0;
}

void WebSocketServer::onUpgradeRequest(WebSocketConnection* conn)
{
	//Get request
	HTTPRequest *request = conn->GetRequest();
	//Get URL
	std::string uri = request->GetRequestURI();
	//For each registered handler in reverse order
	for (Handlers::reverse_iterator it=handlers.rbegin();it!=handlers.rend();it++)
	{
		//Si la uri empieza por la base del handler
		if (uri.find((*it).first)==0)
		{
			//Ejecutamos el handler
			it->second->onWebSocketConnection(*request,(WebSocket*)conn);
			//Found
			return;
		}
	}
	//reject it
	conn->Reject(404,"No handlers for that url found");
}

void WebSocketServer::onWakeupNeeded()
{
	//Réveille le thread serveur bloqué dans poll() via l'eventfd du Wait
	//hérité (Worker). Thread-safe.
	wait.Signal();
}
