#include "ipaddress.h"
#include <ctype.h>
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
WebSocketServer::WebSocketServer() : retrier(this)
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
	//par le profil d'adressage (NETWORK-CONFIGURATION.md).
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
	//Thread des reprises de jambes sortantes (ConnectLater)
	retrier.Start();

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

/**************************
 * GetPollTimeout
 * 	Délai de poll() : la plus proche échéance d'ouverture cliente
 **************************/
int WebSocketServer::GetPollTimeout()
{
	int timeout = -1;

	for (Connections::iterator it=connections.begin(); it!=connections.end(); ++it)
	{
		const int left = it->second->GetOpeningTimeLeft();
		if (left<0)
			continue;
		//0 signifie « déjà écoulée » : ne pas bloquer, le tour suivant la traite.
		if (timeout<0 || left<timeout)
			timeout = left;
	}

	return timeout;
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

		//Adopte les connexions SORTANTES demandées depuis un autre thread, avant
		//de bâtir le jeu de poll : elles y entrent dès ce tour-ci.
		DrainPendingConnects();

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

		//Wait for events. Le delai n'est PAS infini quand une jambe cliente
		//attend son ouverture : sans echeance, un pair qui accepte le TCP puis
		//se tait n'enverrait jamais l'evenement qui nous reveillerait (§9.4).
		int n = poll(ufds.data(), ufds.size(), GetPollTimeout());
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
		//(Close) sans événement poll associé, et ouvertures clientes dont
		//l'échéance est écoulée — celles-là n'ont produit aucun événement, c'est
		//exactement ce qui leur est reproché. Après le dispatch : une réponse
		//arrivée à l'instant a déjà ouvert la jambe.
		for (Connections::iterator it=connections.begin(); it!=connections.end(); ++it)
		{
			if (it->second->GetOpeningTimeLeft()==0)
				it->second->OnOpeningTimeout();
			if (it->second->IsFinished())
				toClose.insert(it->first);
		}

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

namespace {

//Un champ d'URL tel que le parseur l'a délimité, "" s'il est absent.
std::string UrlField(const std::string& url,const http_parser_url& parsed,int field)
{
	if (!(parsed.field_set & (1<<field)))
		return std::string();
	return url.substr(parsed.field_data[field].off,parsed.field_data[field].len);
}

std::string ToLower(const std::string& in)
{
	std::string out = in;
	for (size_t i=0;i<out.size();++i)
		out[i] = tolower(out[i]);
	return out;
}

} // namespace

/**************************
 * ParseWsUrl
 * 	Décompose une URL ws:// ou wss:// (cf. websocketserver.h)
 **************************/
bool WebSocketServer::ParseWsUrl(const std::string& url, Target& target)
{
	http_parser_url parsed;
	memset(&parsed,0,sizeof(parsed));
	if (http_parser_parse_url(url.c_str(),url.length(),0,&parsed))
		return Error("-WebSocketServer: cannot parse url [url:%s]\n",url.c_str());

	//Le schéma décide du transport ET du port par défaut (RFC 6455 §3), et lui
	//seul : le mode sécurisé du serveur ne concerne que ses connexions entrantes.
	const std::string schema = ToLower(UrlField(url,parsed,UF_SCHEMA));
	if (schema!="ws" && schema!="wss")
		return Error("-WebSocketServer: unsupported scheme \"%s\" [url:%s]\n",schema.c_str(),url.c_str());
	target.tls = schema=="wss";

	//Le parseur d'URL retire les crochets d'un littéral v6 (RFC 3986 §3.2.2) :
	//`host` est ici une adresse ou un nom, jamais "[...]".
	target.host = UrlField(url,parsed,UF_HOST);
	if (target.host.empty())
		return Error("-WebSocketServer: no host in url [url:%s]\n",url.c_str());

	const WORD defaultPort = target.tls ? 443 : 80;
	target.port = (parsed.field_set & (1<<UF_PORT)) ? parsed.port : defaultPort;

	//Une requête HTTP porte toujours un chemin absolu, query comprise. Le
	//fragment reste chez nous : il n'est jamais émis sur le fil (RFC 3986 §3.5).
	target.path = UrlField(url,parsed,UF_PATH);
	if (target.path.empty())
		target.path = "/";
	const std::string query = UrlField(url,parsed,UF_QUERY);
	if (!query.empty())
		target.path += "?" + query;

	//En-tête Host : l'hôte tel qu'il a été demandé, crochets rendus à un
	//littéral v6, et le port sauf s'il est celui du schéma (RFC 7230 §5.4).
	target.hostHeader = target.host.find(':')==std::string::npos ? target.host : "["+target.host+"]";
	if (target.port!=defaultPort)
		target.hostHeader += ":" + std::to_string(target.port);

	return true;
}

/**************************
 * Connect
 * 	Ouvre une connexion WebSocket sortante (cf. websocketserver.h)
 **************************/
bool WebSocketServer::Connect(const std::string& url, std::weak_ptr<WebSocket::Listener> listener)
{
	//Sans réacteur, personne ne piloterait la connexion : l'échouer tout de
	//suite vaut mieux que de la laisser dormir dans la file.
	if (!inited)
		return Error("-WebSocketServer::Connect: server is not running [url:%s]\n",url.c_str());

	Target target;
	if (!ParseWsUrl(url,target))
		return false;

	const bool tls		      = target.tls;
	const std::string& host	      = target.host;
	const WORD port		      = target.port;
	const std::string& path	      = target.path;
	const std::string& hostHeader = target.hostHeader;

	//DNS ICI, dans le thread appelant : A et AAAA, ordre déterministe.
	int err = 0;
	const std::list<IPAddress> addresses = IPAddress::Resolve(host,err,AF_INET);
	if (addresses.empty())
		return Error("-WebSocketServer::Connect: cannot resolve \"%s\" (errno %d) [url:%s]\n",host.c_str(),err,url.c_str());

	//Le transport est construit ICI, avant tout socket : un contexte TLS
	//inutilisable est alors un échec SYNCHRONE de plus, et non une jambe muette.
	//Le SNI et l'identité vérifiée portent l'hôte de l'URL, sans crochets ni port.
	std::unique_ptr<WebSocketTransport> transport;
	if (tls)
	{
		transport = WebSocketTlsTransport::CreateClient(host,WebSocketTlsTransport::GetClientVerifyPeer());
		if (!transport)
			return Error("-WebSocketServer::Connect: no TLS client transport [url:%s]\n",url.c_str());
	} else {
		transport = std::make_unique<WebSocketPlainTransport>();
	}

	//Le socket et le connect() non bloquant appartiennent à l'appelant : ainsi
	//TOUT échec synchrone se dit par la valeur de retour, et le réacteur n'a
	//qu'un descripteur à adopter.
	int fd = FD_INVALID;
	int lastErrno = 0;
	for (std::list<IPAddress>::const_iterator it=addresses.begin();it!=addresses.end() && fd==FD_INVALID;++it)
	{
		const IPEndpoint to = it->To(port);

		int sock = socket(it->Family(),SOCK_STREAM,0);
		if (sock<0)
		{
			lastErrno = errno;
			continue;
		}

		//Non bloquant AVANT connect() : le réacteur ne doit jamais attendre un pair
		int fsflags = fcntl(sock,F_GETFL,0);
		fcntl(sock,F_SETFL, fsflags | O_NONBLOCK);

		//EINPROGRESS est le cas NORMAL : la fin du connect() se lit au premier
		//POLLOUT, par SO_ERROR (WebSocketConnection::OnWritable).
		if (connect(sock,to,to.Len())<0 && errno!=EINPROGRESS)
		{
			lastErrno = errno;
			close(sock);
			continue;
		}

		Log("-Outgoing connection [fd:%d,to:%s,path:%s,%s]\n",sock,to.ToString().c_str(),path.c_str(),tls?"tls":"plain");
		fd = sock;
	}

	if (fd==FD_INVALID)
		return Error("-WebSocketServer::Connect: cannot connect to \"%s\" (errno %d) [url:%s]\n",host.c_str(),lastErrno,url.c_str());

	PendingConnect request;
	request.fd	  = fd;
	request.host	  = hostHeader;
	request.path	  = path;
	request.transport = std::move(transport);
	request.listener  = listener;

	{
		std::lock_guard<std::mutex> lock(pendingMutex);
		pendingConnects.push_back(std::move(request));
	}

	//Réveiller le réacteur : il adoptera le socket au prochain tour de boucle
	onWakeupNeeded();

	return true;
}

/**************************
 * ConnectLater / CancelConnectLater
 * 	Rythme de la reprise d'une jambe cliente (cf. websocketserver.h)
 **************************/
uint64_t WebSocketServer::ConnectLater(const std::string& url,
				       std::weak_ptr<WebSocket::Listener> listener, DWORD delayMs)
{
	if (!inited)
	{
		Error("-WebSocketServer::ConnectLater: server is not running [url:%s]\n",url.c_str());
		return 0;
	}

	return retrier.Schedule(url,listener,delayMs);
}

void WebSocketServer::CancelConnectLater(uint64_t id)
{
	retrier.Cancel(id);
}

uint64_t WebSocketServer::Retrier::Schedule(const std::string& url,
					    std::weak_ptr<WebSocket::Listener> listener, DWORD delayMs)
{
	Attempt attempt;

	{
		std::lock_guard<std::mutex> lock(mutex);
		attempt.id	 = ++nextId;
		attempt.dueMs	 = getTimeMS() + delayMs;
		//Le délai demandé est aussi le rythme des tentatives suivantes. Plancher
		//d'une seconde : un délai nul sur un échec synchrone répété ferait tourner
		//ce thread à plein régime.
		attempt.retryMs	 = delayMs<1000 ? 1000 : delayMs;
		attempt.url	 = url;
		attempt.listener = listener;
		attempts.push_back(attempt);
	}

	//Le thread dort peut-être jusqu'à une échéance plus lointaine que celle
	//qu'on vient de poser : le réveiller pour qu'il recalcule.
	wait.Signal();

	return attempt.id;
}

void WebSocketServer::Retrier::Cancel(uint64_t id)
{
	std::lock_guard<std::mutex> lock(mutex);

	for (std::list<Attempt>::iterator it=attempts.begin();it!=attempts.end();++it)
	{
		if (it->id==id)
		{
			attempts.erase(it);
			return;
		}
	}
}

void WebSocketServer::Retrier::Stop()
{
	StopThread();

	std::lock_guard<std::mutex> lock(mutex);
	attempts.clear();
}

int WebSocketServer::Retrier::Run()
{
	Log(">Run WebSocket outgoing retrier [%p]\n",this);

	while (IsThreadRunning())
	{
		std::list<Attempt> due;
		//Attente par défaut : rien à faire, on dort jusqu'au prochain Schedule.
		DWORD sleepMs = 1000;

		{
			std::lock_guard<std::mutex> lock(mutex);
			const QWORD now = getTimeMS();

			std::list<Attempt>::iterator it = attempts.begin();
			while (it!=attempts.end())
			{
				//Une jambe détruite ne se reconnecte pas : la demande meurt
				//avec son listener, et c'est ce qui borne la reprise.
				if (it->listener.expired())
				{
					it = attempts.erase(it);
					continue;
				}

				if (it->dueMs<=now)
				{
					due.push_back(*it);
					it = attempts.erase(it);
					continue;
				}

				const QWORD left = it->dueMs-now;
				if (left<sleepMs)
					sleepMs = (DWORD) left;
				++it;
			}
		}

		//HORS du verrou : Connect() résout un nom, ouvre un socket et peut
		//notifier — un callback qui rappellerait ConnectLater se bloquerait.
		for (std::list<Attempt>::iterator it=due.begin();it!=due.end();++it)
		{
			if (!IsThreadRunning())
				break;

			//La tentative suivante, elle, n'est PAS programmée ici : un échec
			//asynchrone se dit au listener par onClose, et c'est lui qui
			//redemande. Seul l'échec SYNCHRONE (DNS muet, pair injoignable
			//tout de suite) ne notifie personne, donc se replanifie ici.
			if (!server->Connect(it->url,it->listener))
			{
				std::lock_guard<std::mutex> lock(mutex);
				Attempt again = *it;
				again.dueMs = getTimeMS() + again.retryMs;
				attempts.push_back(again);
			}
		}

		wait.WaitSignal(sleepMs);
	}

	Log("<Run WebSocket outgoing retrier [%p]\n",this);
	return 0;
}

/**************************
 * DrainPendingConnects
 * 	Adopte les connexions sortantes demandées depuis un autre thread
 **************************/
void WebSocketServer::DrainPendingConnects()
{
	std::list<PendingConnect> requests;

	{
		std::lock_guard<std::mutex> lock(pendingMutex);
		//Sortir la file du verrou : la création d'une connexion notifie, et un
		//callback qui rappellerait Connect() se bloquerait lui-même.
		requests.swap(pendingConnects);
	}

	for (std::list<PendingConnect>::iterator it=requests.begin();it!=requests.end();++it)
		CreateClientConnection(*it);
}

/*************************
 * CreateClientConnection
 * 	Enveloppe un socket sortant dans une connexion pilotée par le réacteur
 *************************/
void WebSocketServer::CreateClientConnection(PendingConnect& request)
{
	//Identité stable (pas le fd, réutilisable)
	uint64_t id = ++nextConnId;

	std::shared_ptr<WebSocketConnection> conn = std::make_shared<WebSocketConnection>(this, id);

	Log("-Outgoing connection adopted [fd:%d,id:%llu,host:%s,path:%s]\n",
	    request.fd,(unsigned long long)id,request.host.c_str(),request.path.c_str());

	//Le listener est posé ICI : en mode client personne n'appelle Accept(), et
	//un échec avant le 101 doit déjà pouvoir se dire.
	conn->InitClient(request.fd, std::move(request.transport),
			 request.host, request.path, request.listener);

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

	//Les reprises d'abord : une tentative en vol appelle Connect(), qui pousse
	//dans la file du réacteur — la laisser tourner pendant l'arrêt ferait fuir
	//le socket qu'elle vient d'ouvrir.
	retrier.Stop();

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

	//Les sockets sortants que le réacteur n'a pas eu le temps d'adopter ne sont
	//possédés par aucune connexion : les fermer ici, sinon ils fuient.
	{
		std::lock_guard<std::mutex> lock(pendingMutex);
		for (std::list<PendingConnect>::iterator it=pendingConnects.begin();it!=pendingConnects.end();++it)
		{
			shutdown(it->fd,SHUT_RDWR);
			close(it->fd);
		}
		pendingConnects.clear();
	}

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
