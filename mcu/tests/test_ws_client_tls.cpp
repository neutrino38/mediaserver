/**
 * test_ws_client_tls.cpp — le transport TLS CLIENT (lot 4 de
 * docs/conception/WS-CLIENT/SPEC.md).
 *
 * Le lot 3 refusait `wss://` plutôt que de partir en clair. Ici la jambe sortante
 * chiffrée existe pour de bon, et c'est le serveur lui-même qui tient l'autre bout
 * — un seul binaire, les deux rôles, comme la recette §7 en petit.
 *
 * Ce que ces tests tiennent :
 *
 *   - `wss://` s'ouvre et l'écho revient : le ClientHello part TOUT SEUL (rien ne
 *     lit encore ce socket, donc rien n'appellerait Recv), et la requête d'upgrade
 *     n'est émise qu'une fois le handshake fini — `Send` JETTE ce qu'on lui donne
 *     avant, et la requête serait perdue en silence (§4.4, pièges 2 et 3) ;
 *   - la vérification du pair N'EST PAS décorative : avec l'autorité qui a signé
 *     le certificat, la jambe s'ouvre ; sans elle, elle se ferme. Une
 *     vérification qui accepte tout se lirait comme un succès dans le premier
 *     test seul : il en faut deux ;
 *   - une adresse littérale se vérifie contre les SAN `iPAddress`, jamais contre
 *     le CN — sans quoi `wss://127.0.0.1/` échouerait toujours, et l'exploitant
 *     désarmerait la vérification pour de mauvaises raisons ;
 *   - un échec TLS se dit au listener, `onError` puis `onClose`. Sans cela le
 *     WSEndpoint attendrait indéfiniment un pair qui ne viendra jamais (§4.6).
 *
 * Les callbacks arrivent sur le thread du réacteur, jamais sur celui du test :
 * tout ce que le listener enregistre est protégé.
 *
 *     ./tests/runtests --gtest_filter='WsClientTls*'
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
#include "wstlsfixture.h"

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

//---- Le pair : le vrai serveur WebSocket du mcu, en wss:// -------------------

//Écho, plus ce que la requête d'upgrade portait : c'est la seule façon de voir ce
//que l'URL est devenue sur le fil, DANS le tunnel.
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

//WebSocketServer::Init() rend la main AVANT que son thread n'ait appelé listen() :
//une connexion immédiate serait refusée.
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

//Le réacteur tourne dans son propre thread : on attend, on ne pompe pas.
bool WaitFor(const std::function<bool()>& until, int timeoutMs = 5000)
{
	for (int waited = 0; waited < timeoutMs; waited += 10)
	{
		if (until())
			return true;
		usleep(10 * 1000);
	}
	return until();
}

//Un serveur WebSocket SÉCURISÉ démarré sur un port libre, avec son handler d'écho.
class TlsServerFixture
{
public:
	bool Start(int firstPort)
	{
		handler = std::make_shared<RecordingEchoHandler>();
		server.AddHandler("/echo", handler.get());
		//SetSecure ne concerne QUE les connexions entrantes : le schéma de l'URL
		//décide seul du transport des sortantes.
		server.SetSecure(true, WsTlsTestCertificate::CertFile().c_str(),
				       WsTlsTestCertificate::KeyFile().c_str());

		for (int p = firstPort; p < firstPort+50 && !port; ++p)
			if (server.Init(p))
				port = p;

		return port && WaitForListener(port);
	}

	~TlsServerFixture()
	{
		if (port)
			server.End();
	}

	WebSocketServer&	Server()	{ return server;	}
	RecordingEchoHandler&	Handler()	{ return *handler;	}
	int			Port() const	{ return port;		}

	std::string Url(const std::string& hostPart,const std::string& path = "/echo")
	{
		return "wss://" + hostPart + ":" + std::to_string(port) + path;
	}

private:
	WebSocketServer				server;
	std::shared_ptr<RecordingEchoHandler>	handler;
	int					port = 0;
};

} // namespace

//=============================================================================

//Le cas nominal : tout le chemin, du ClientHello à l'écho. Sans la vérification
//du pair — c'est le test suivant qui s'en charge.
TEST(WsClientTls, UneUrlWssSOuvreEtLEchoRevient)
{
	if (!WsTlsTestCertificate::Ensure())
		GTEST_SKIP() << "Certificat de test non generable";

	TlsServerFixture fixture;
	if (!fixture.Start(39500))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	WebSocketTlsTransport::SetClientConfig(false, "");

	auto listener = std::make_shared<SharedListener>();
	ASSERT_TRUE(fixture.Server().Connect(fixture.Url("127.0.0.1"), listener));

	ASSERT_TRUE(WaitFor([&]{ return listener->IsOpened() || listener->IsClosed(); }))
		<< "la jambe chiffrée n'a jamais abouti : le ClientHello est-il parti ?";
	ASSERT_TRUE(listener->IsOpened());
	EXPECT_EQ(0, listener->Errors());

	//Ce que le pair a lu vient bien de l'URL, et il l'a lu DANS le tunnel
	EXPECT_EQ("/echo", fixture.Handler().Uri());
	EXPECT_EQ("127.0.0.1:"+std::to_string(fixture.Port()), fixture.Handler().Host());

	ASSERT_TRUE(listener->Send("bonjour chiffre"));
	ASSERT_TRUE(WaitFor([&]{ return listener->Messages() > 0; })) << "pas d'écho reçu";
	EXPECT_EQ("bonjour chiffre", listener->Message());
}

//L'autorité de test n'est dans aucun magasin système : une jambe vérifiée DOIT la
//refuser. Sans ce test, une vérification qui accepte tout passerait pour bonne.
TEST(WsClientTls, UnCertificatInconnuFermeLaJambe)
{
	if (!WsTlsTestCertificate::Ensure())
		GTEST_SKIP() << "Certificat de test non generable";

	TlsServerFixture fixture;
	if (!fixture.Start(39550))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	//Magasin du système seul : notre autorité y est inconnue
	WebSocketTlsTransport::SetClientConfig(true, "");

	auto listener = std::make_shared<SharedListener>();
	//L'échec est ASYNCHRONE : la connexion TCP, elle, aboutit
	ASSERT_TRUE(fixture.Server().Connect(fixture.Url("127.0.0.1"), listener));

	ASSERT_TRUE(WaitFor([&]{ return listener->IsClosed(); }))
		<< "un certificat refusé n'a notifié personne";
	EXPECT_FALSE(listener->IsOpened()) << "un certificat non vérifiable ne doit JAMAIS ouvrir la jambe";
	EXPECT_EQ(1, listener->Errors());
	EXPECT_TRUE(listener->ErrorBeforeClose()) << "onError doit précéder onClose";
}

//Avec l'autorité (ce que `--websocket-client-ca` donne), la même jambe s'ouvre —
//et elle s'ouvre sur une ADRESSE, donc vérifiée contre les SAN iPAddress : un
//client qui comparerait au CN échouerait ici.
TEST(WsClientTls, LAutoriteDonneeValideUneAdresseLitterale)
{
	if (!WsTlsTestCertificate::Ensure())
		GTEST_SKIP() << "Certificat de test non generable";

	TlsServerFixture fixture;
	if (!fixture.Start(39600))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	WebSocketTlsTransport::SetClientConfig(true, WsTlsTestCertificate::CaFile());

	auto listener = std::make_shared<SharedListener>();
	ASSERT_TRUE(fixture.Server().Connect(fixture.Url("127.0.0.1"), listener));

	ASSERT_TRUE(WaitFor([&]{ return listener->IsOpened() || listener->IsClosed(); }));
	ASSERT_TRUE(listener->IsOpened())
		<< "l'autorité est fournie et le certificat porte IP:127.0.0.1 en SAN";
	EXPECT_EQ(0, listener->Errors());

	ASSERT_TRUE(listener->Send("bonjour verifie"));
	ASSERT_TRUE(WaitFor([&]{ return listener->Messages() > 0; }));
	EXPECT_EQ("bonjour verifie", listener->Message());
}

//Une autorité demandée et illisible ne doit pas se traiter en silence : la jambe
//serait refusée plus tard, et personne ne saurait que c'est ce fichier.
TEST(WsClientTls, UneAutoriteIllisibleEchoueTOutDeSuite)
{
	if (!WsTlsTestCertificate::Ensure())
		GTEST_SKIP() << "Certificat de test non generable";

	TlsServerFixture fixture;
	if (!fixture.Start(39650))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	WebSocketTlsTransport::SetClientConfig(true, "/nexistepas/ca.crt");

	auto listener = std::make_shared<SharedListener>();
	EXPECT_FALSE(fixture.Server().Connect(fixture.Url("127.0.0.1"), listener))
		<< "un contexte TLS inutilisable est un échec SYNCHRONE";
	EXPECT_FALSE(listener->IsOpened());
	EXPECT_EQ(0, listener->Errors()) << "rien n'a été ouvert, donc rien à notifier";

	//Ne pas laisser la configuration cassée aux tests suivants
	WebSocketTlsTransport::SetClientConfig(true, "");
}
