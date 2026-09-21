/**
 * test_ws_client_connect.cpp — `WebSocketServer::Connect`, la connexion
 * SORTANTE pilotée par le réacteur (lot 3 de docs/conception/WS-CLIENT/SPEC.md).
 *
 * Le lot 2 a livré la machine à état cliente, mais c'est le TEST qui la pilotait.
 * Ici, c'est le réacteur du serveur — un thread, un `poll()` — qui la tient, et
 * l'appelant n'a plus qu'une URL à donner. Ce que ces tests tiennent :
 *
 *   - une URL `ws://` devient une jambe ouverte, dans les DEUX familles : le
 *     littéral v4 et le littéral v6 entre crochets (RFC 3986 §3.2.2), que le
 *     parseur d'URL rend sans ses crochets et qu'il faut savoir lui rendre ;
 *   - ce que le pair reçoit vient bien de l'URL : la ligne de requête porte le
 *     chemin ET la query, l'en-tête `Host` porte l'hôte et le port ;
 *   - un échec SYNCHRONE (schéma inconnu, hôte absent, nom introuvable) se dit
 *     par la valeur de retour et ne notifie personne — il n'y a pas de jambe ;
 *   - un échec ASYNCHRONE (port fermé) se dit au listener, `onError` puis
 *     `onClose`, depuis le réacteur. Sans cela le WSEndpoint attend
 *     indéfiniment un pair qui ne viendra jamais.
 *
 * Le schéma `wss://` a sa propre suite, `tests/test_ws_client_tls.cpp` (lot 4).
 *
 * Le serveur tient ici les DEUX bouts — c'est la recette §7 du SPEC en petit.
 * Les callbacks arrivent sur le thread du réacteur, jamais sur celui du test :
 * tout ce que le listener enregistre est protégé.
 *
 *     ./tests/runtests --gtest_filter='WsClientConnect*'
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

#include "websocketserver.h"

namespace {

//---- Ce que la jambe cliente notifie ----------------------------------------

//Les callbacks viennent du thread réacteur ; le test lit depuis le sien.
class SharedListener : public WebSocket::Listener
{
public:
	virtual void onOpen(WebSocket* ws)
	{
		std::lock_guard<std::mutex> lock(mutex);
		opened = true;
		//Même chemin que WSEndpoint : on ne garde jamais un pointeur nu sur une
		//connexion possédée par le serveur.
		socket = ws->GetWeakPtr();
	}
	virtual void onMessageStart(WebSocket*,WebSocket::MessageType,const DWORD)
	{
		std::lock_guard<std::mutex> lock(mutex);
		message.clear();
	}
	virtual void onMessageData(WebSocket*,const BYTE* data,const DWORD size)
	{
		std::lock_guard<std::mutex> lock(mutex);
		message.append((const char*)data,size);
	}
	virtual void onMessageEnd(WebSocket*)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++messages;
	}
	virtual void onError(WebSocket*)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++errors;
		errorBeforeClose = errorBeforeClose || !closed;
	}
	virtual void onClose(WebSocket*)
	{
		std::lock_guard<std::mutex> lock(mutex);
		closed = true;
		++closes;
	}

public:
	bool IsOpened()		{ std::lock_guard<std::mutex> lock(mutex); return opened;	}
	bool IsClosed()		{ std::lock_guard<std::mutex> lock(mutex); return closed;	}
	int  Errors()		{ std::lock_guard<std::mutex> lock(mutex); return errors;	}
	int  Closes()		{ std::lock_guard<std::mutex> lock(mutex); return closes;	}
	int  Messages()		{ std::lock_guard<std::mutex> lock(mutex); return messages;	}
	bool ErrorBeforeClose()	{ std::lock_guard<std::mutex> lock(mutex); return errorBeforeClose; }
	std::string Message()	{ std::lock_guard<std::mutex> lock(mutex); return message;	}

	//Émettre vers le pair, comme le ferait le WSEndpoint
	bool Send(const std::string& text)
	{
		std::weak_ptr<WebSocket> weak;
		{
			std::lock_guard<std::mutex> lock(mutex);
			weak = socket;
		}
		std::shared_ptr<WebSocket> ws = weak.lock();
		if (!ws)
			return false;
		ws->SendMessage(text);
		return true;
	}

private:
	std::mutex	mutex;
	bool		opened		= false;
	bool		closed		= false;
	bool		errorBeforeClose= false;
	int		errors		= 0;
	int		closes		= 0;
	int		messages	= 0;
	std::string	message;
	std::weak_ptr<WebSocket> socket;
};

//---- Le pair : le vrai serveur WebSocket du mcu ------------------------------

//Écho, plus ce que la requête d'upgrade portait : c'est la seule façon de voir
//ce que l'URL est devenue sur le fil.
class RecordingEchoHandler :
	public WebSocketServer::Handler,
	public WebSocket::Listener,
	public std::enable_shared_from_this<RecordingEchoHandler>
{
public:
	virtual void onWebSocketConnection(const HTTPRequest& request,WebSocket* ws)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			uri  = request.GetRequestURI();
			host = request.GetHeader("Host");
		}
		ws->Accept(weak_from_this());
	}
	virtual void onOpen(WebSocket*)						{ }
	virtual void onMessageStart(WebSocket*,WebSocket::MessageType,const DWORD)	{ }
	virtual void onMessageData(WebSocket* ws,const BYTE* data,const DWORD size)
	{
		ws->SendMessage(std::string((const char*)data,size));
	}
	virtual void onMessageEnd(WebSocket*)					{ }
	virtual void onError(WebSocket*)					{ }
	virtual void onClose(WebSocket*)					{ }

	std::string Uri()	{ std::lock_guard<std::mutex> lock(mutex); return uri;	}
	std::string Host()	{ std::lock_guard<std::mutex> lock(mutex); return host;	}

private:
	std::mutex	mutex;
	std::string	uri;
	std::string	host;
};

//---- Outillage ---------------------------------------------------------------

//WebSocketServer::Init() rend la main AVANT que son thread n'ait appelé
//listen() : une connexion immédiate serait refusée.
bool WaitForListener(int port)
{
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return false;

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = inet_addr("127.0.0.1");

		bool ok = connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
		close(fd);
		if (ok)
			return true;
		usleep(20 * 1000);
	}
	return false;
}

//La machine sait-elle joindre ::1 ? Répondre AVANT d'appeler Connect : sans
//cela, un mauvais choix de famille se lirait comme une absence d'IPv6, et le
//test se contenterait de sauter.
bool HasIPv6Loopback(int port)
{
	int fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_loopback;

	bool ok = connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0;
	close(fd);
	return ok;
}

//Un port éphémère réservé puis relâché : personne n'y écoute
int DeadPort()
{
	int probe = socket(AF_INET, SOCK_STREAM, 0);
	if (probe < 0)
		return 0;

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	int port = 0;
	socklen_t len = sizeof(addr);
	if (bind(probe, (sockaddr*)&addr, sizeof(addr)) == 0 &&
	    getsockname(probe, (sockaddr*)&addr, &len) == 0)
		port = ntohs(addr.sin_port);
	close(probe);
	return port;
}

//Le réacteur tourne dans son propre thread : on attend, on ne pompe pas.
bool WaitFor(const std::function<bool()>& until, int timeoutMs = 3000)
{
	for (int waited = 0; waited < timeoutMs; waited += 10)
	{
		if (until())
			return true;
		usleep(10 * 1000);
	}
	return until();
}

//Un serveur WebSocket démarré sur un port libre, avec son handler d'écho.
class ServerFixture
{
public:
	bool Start(int firstPort)
	{
		handler = std::make_shared<RecordingEchoHandler>();
		server.AddHandler("/echo", handler.get());

		for (int p = firstPort; p < firstPort+50 && !port; ++p)
			if (server.Init(p))
				port = p;

		return port && WaitForListener(port);
	}

	~ServerFixture()
	{
		if (port)
			server.End();
	}

	WebSocketServer&	Server()	{ return server;	}
	RecordingEchoHandler&	Handler()	{ return *handler;	}
	int			Port() const	{ return port;		}

	std::string Url(const std::string& hostPart,const std::string& path = "/echo")
	{
		return "ws://" + hostPart + ":" + std::to_string(port) + path;
	}

private:
	WebSocketServer				server;
	std::shared_ptr<RecordingEchoHandler>	handler;
	int					port = 0;
};

} // namespace

//=============================================================================

TEST(WsClientConnect, UneUrlV4SOuvreEtLEchoRevient)
{
	ServerFixture fixture;
	if (!fixture.Start(39200))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto listener = std::make_shared<SharedListener>();
	ASSERT_TRUE(fixture.Server().Connect(fixture.Url("127.0.0.1"), listener));

	ASSERT_TRUE(WaitFor([&]{ return listener->IsOpened() || listener->IsClosed(); }))
		<< "la connexion sortante n'a jamais abouti";
	ASSERT_TRUE(listener->IsOpened());
	EXPECT_EQ(0, listener->Errors());

	ASSERT_TRUE(listener->Send("bonjour par le reacteur"));
	ASSERT_TRUE(WaitFor([&]{ return listener->Messages() > 0; })) << "pas d'écho reçu";
	EXPECT_EQ("bonjour par le reacteur", listener->Message());
}

//Un littéral v6 s'écrit entre crochets dans une URL (RFC 3986 §3.2.2) ; le
//parseur les retire, l'en-tête Host doit les retrouver, et la socket doit être
//ouverte dans la BONNE famille.
TEST(WsClientConnect, UneUrlV6EntreCrochetsSOuvre)
{
	ServerFixture fixture;
	if (!fixture.Start(39250))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	if (!HasIPv6Loopback(fixture.Port()))
		GTEST_SKIP() << "Pas d'IPv6 sur cette machine";

	auto listener = std::make_shared<SharedListener>();
	ASSERT_TRUE(fixture.Server().Connect(fixture.Url("[::1]"), listener))
		<< "la socket doit être ouverte dans la famille de l'adresse";

	ASSERT_TRUE(WaitFor([&]{ return listener->IsOpened() || listener->IsClosed(); }));
	ASSERT_TRUE(listener->IsOpened()) << "le serveur écoute pourtant en dual-stack";

	EXPECT_EQ("[::1]:"+std::to_string(fixture.Port()), fixture.Handler().Host())
		<< "un littéral v6 sans crochets rend le port indissociable de l'adresse";

	ASSERT_TRUE(listener->Send("bonjour en v6"));
	ASSERT_TRUE(WaitFor([&]{ return listener->Messages() > 0; }));
	EXPECT_EQ("bonjour en v6", listener->Message());
}

//Ce que le pair reçoit vient de l'URL, et de rien d'autre : chemin, query, Host.
TEST(WsClientConnect, LeCheminEtLaQueryArriventAuPair)
{
	ServerFixture fixture;
	if (!fixture.Start(39300))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto listener = std::make_shared<SharedListener>();
	ASSERT_TRUE(fixture.Server().Connect(fixture.Url("127.0.0.1","/echo/t140?token=abc#ici"), listener));

	ASSERT_TRUE(WaitFor([&]{ return listener->IsOpened() || listener->IsClosed(); }));
	ASSERT_TRUE(listener->IsOpened());

	//Le fragment ne passe jamais sur le fil (RFC 3986 §3.5)
	EXPECT_EQ("/echo/t140?token=abc", fixture.Handler().Uri());
	EXPECT_EQ("127.0.0.1:"+std::to_string(fixture.Port()), fixture.Handler().Host());
}

//Le seul échec ASYNCHRONE du lot : le connect() a été accepté, c'est le pair
//qui n'est pas là. Il ne peut se dire qu'au listener.
TEST(WsClientConnect, UnPortFermeSeDitParOnErrorPuisOnClose)
{
	ServerFixture fixture;
	if (!fixture.Start(39350))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	const int dead = DeadPort();
	ASSERT_GT(dead, 0);

	auto listener = std::make_shared<SharedListener>();
	//L'URL est valide et l'hôte résoluble : Connect accepte, l'échec viendra après
	ASSERT_TRUE(fixture.Server().Connect("ws://127.0.0.1:"+std::to_string(dead)+"/echo", listener));

	ASSERT_TRUE(WaitFor([&]{ return listener->IsClosed(); }))
		<< "un port fermé n'a notifié personne";
	EXPECT_FALSE(listener->IsOpened());
	EXPECT_EQ(1, listener->Errors());
	EXPECT_TRUE(listener->ErrorBeforeClose()) << "onError doit précéder onClose";
	EXPECT_EQ(1, listener->Closes());
}

//Échecs SYNCHRONES : il n'y a pas de jambe, donc rien à notifier. Le contrôleur
//les apprend par la valeur de retour, tout de suite.
TEST(WsClientConnect, UneUrlInutilisableEchoueSansNotifier)
{
	ServerFixture fixture;
	if (!fixture.Start(39400))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto listener = std::make_shared<SharedListener>();

	EXPECT_FALSE(fixture.Server().Connect("http://127.0.0.1:8080/echo", listener))	<< "schéma non WebSocket";
	EXPECT_FALSE(fixture.Server().Connect("ws:///echo", listener))			<< "URL sans hôte";
	EXPECT_FALSE(fixture.Server().Connect("pas une url", listener))			<< "URL illisible";
	EXPECT_FALSE(fixture.Server().Connect("", listener))				<< "URL vide";
	EXPECT_FALSE(fixture.Server().Connect("ws://nexistepas.invalid/echo", listener))	<< "nom introuvable (.invalid, RFC 2606)";

	//Rien n'a été ouvert, donc rien n'a été notifié
	EXPECT_FALSE(listener->IsOpened());
	EXPECT_EQ(0, listener->Errors());
	EXPECT_EQ(0, listener->Closes());
}

//Sans réacteur, personne ne piloterait la connexion : elle dormirait dans la
//file, et le contrôleur attendrait un événement qui ne viendrait jamais.
TEST(WsClientConnect, ConnectSansServeurDemarreEchoue)
{
	WebSocketServer server;
	auto listener = std::make_shared<SharedListener>();

	EXPECT_FALSE(server.Connect("ws://127.0.0.1:9090/echo", listener));
}
