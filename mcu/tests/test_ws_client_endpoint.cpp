/**
 * test_ws_client_endpoint.cpp — la jambe texte JSR-309 en mode CLIENT : le
 * mediaserver joue le navigateur (lot 5 de docs/conception/WS-CLIENT/SPEC.md).
 *
 * Les lots 3 et 4 ont livré la connexion sortante ; ici c'est le `WSEndpoint`
 * qui la pilote, et le texte T.140 traverse pour de bon. Ce que ces tests
 * tiennent :
 *
 *   - le pontage dans les DEUX sens : un paquet RTP T.140 devient un message
 *     WebSocket chez le pair, et un message du pair revient en RTP ;
 *   - la coupure est une PERTE ANNONCÉE (arbitrage du 2026-09-21) : U+FFFD vers
 *     le pair RTP quand la connexion tombe, U+FFFD vers le pair WebSocket à la
 *     reconnexion, et le texte de la coupure est perdu — jamais rejoué ;
 *   - la reprise est indéfinie, toutes les 5 s, et s'arrête à `End()` — et à
 *     rien d'autre ;
 *   - une URL inutilisable est une faute IMMÉDIATE : elle n'arme rien, parce
 *     qu'elle ne se réparera pas en attendant ;
 *   - une jambe cliente ne publie PAS l'adresse d'écoute du serveur : elle
 *     publie la cible qu'on lui a donnée (§4.7) ;
 *   - une ouverture dont le pair accepte le TCP puis se tait est ABANDONNÉE
 *     (§9.4). Sans échéance, le réacteur attendrait un événement qui ne
 *     viendra jamais.
 *
 * Les callbacks arrivent sur le thread du réacteur, jamais sur celui du test :
 * tout ce qu'un pair ou un puits enregistre est protégé.
 *
 *     ./tests/runtests --gtest_filter='WsClientEndpoint*'
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
#include <vector>

#include "websocketserver.h"
#include "../src/jsr309/Endpoint.h"
#include "../src/jsr309/WSEndpoint.h"

namespace {

//U+FFFD en UTF-8 : la marque de perte de T.140 §5.3
const std::string FFFD = "\xEF\xBF\xBD";

//---- Le pair : le vrai serveur WebSocket du mcu ------------------------------

//Écho, plus la trace de ce qui est arrivé et de chaque (re)connexion. C'est
//cette trace qui dit si un texte a été rejoué ou perdu.
class PeerHandler :
	public WebSocketServer::Handler,
	public WebSocket::Listener,
	public std::enable_shared_from_this<PeerHandler>
{
public:
	virtual void onWebSocketConnection(const HTTPRequest& request,WebSocket* ws)
	{
		ws->Accept(weak_from_this());
	}
	virtual void onOpen(WebSocket* ws)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++opens;
		socket = ws->GetWeakPtr();
	}
	virtual void onMessageStart(WebSocket*,WebSocket::MessageType,const DWORD)
	{
		std::lock_guard<std::mutex> lock(mutex);
		current.clear();
	}
	virtual void onMessageData(WebSocket*,const BYTE* data,const DWORD size)
	{
		std::lock_guard<std::mutex> lock(mutex);
		current.append((const char*)data,size);
	}
	virtual void onMessageEnd(WebSocket*)
	{
		std::string echo;
		std::weak_ptr<WebSocket> weak;
		{
			std::lock_guard<std::mutex> lock(mutex);
			received.push_back(current);
			//L'U+FFFD n'est pas du dialogue : le renvoyer ferait croire à une
			//perte de l'autre côté.
			if (current!=FFFD)
				echo = current;
			weak = socket;
			current.clear();
		}

		if (!echo.empty())
			if (std::shared_ptr<WebSocket> ws = weak.lock())
				ws->SendMessage(echo);
	}
	virtual void onError(WebSocket*)	{ }
	virtual void onClose(WebSocket*)
	{
		std::lock_guard<std::mutex> lock(mutex);
		++closes;
	}

public:
	int Opens()	{ std::lock_guard<std::mutex> lock(mutex); return opens;	}
	int Closes()	{ std::lock_guard<std::mutex> lock(mutex); return closes;	}

	std::vector<std::string> Received()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return received;
	}

	int CountReceived(const std::string& text)
	{
		std::lock_guard<std::mutex> lock(mutex);
		int n = 0;
		for (size_t i=0;i<received.size();++i)
			if (received[i]==text)
				++n;
		return n;
	}

	//Couper la jambe depuis le pair : c'est le serveur distant qui raccroche.
	bool CloseCurrent()
	{
		std::weak_ptr<WebSocket> weak;
		{
			std::lock_guard<std::mutex> lock(mutex);
			weak = socket;
			socket.reset();
		}
		std::shared_ptr<WebSocket> ws = weak.lock();
		if (!ws)
			return false;
		ws->Close();
		return true;
	}

private:
	std::mutex			mutex;
	int				opens	= 0;
	int				closes	= 0;
	std::string			current;
	std::vector<std::string>	received;
	std::weak_ptr<WebSocket>	socket;
};

//---- Le côté RTP : ce que la jambe multiplexe vers le pair SIP ---------------

class RtpSink : public Joinable::Listener
{
public:
	virtual void onRTPPacket(RTPPacket& packet)
	{
		std::lock_guard<std::mutex> lock(mutex);
		payloads.push_back(std::string((const char*)packet.GetMediaData(),
					       packet.GetMediaLength()));
	}
	virtual void onResetStream()	{ }
	virtual void onEndStream()	{ }

	std::vector<std::string> Payloads()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return payloads;
	}

	int Count(const std::string& text)
	{
		std::lock_guard<std::mutex> lock(mutex);
		int n = 0;
		for (size_t i=0;i<payloads.size();++i)
			if (payloads[i]==text)
				++n;
		return n;
	}

	int Total()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return (int) payloads.size();
	}

private:
	std::mutex			mutex;
	std::vector<std::string>	payloads;
};

//---- Un listener nu, pour ce qui n'a pas besoin d'une jambe ------------------

class BareListener : public WebSocket::Listener
{
public:
	virtual void onOpen(WebSocket*)						{ std::lock_guard<std::mutex> lock(mutex); opened = true; }
	virtual void onMessageStart(WebSocket*,WebSocket::MessageType,const DWORD)	{ }
	virtual void onMessageData(WebSocket*,const BYTE*,const DWORD)		{ }
	virtual void onMessageEnd(WebSocket*)					{ }
	virtual void onError(WebSocket*)					{ std::lock_guard<std::mutex> lock(mutex); ++errors; }
	virtual void onClose(WebSocket*)					{ std::lock_guard<std::mutex> lock(mutex); closed = true; }

	bool IsOpened()	{ std::lock_guard<std::mutex> lock(mutex); return opened;	}
	bool IsClosed()	{ std::lock_guard<std::mutex> lock(mutex); return closed;	}
	int  Errors()	{ std::lock_guard<std::mutex> lock(mutex); return errors;	}

private:
	std::mutex	mutex;
	bool		opened	= false;
	bool		closed	= false;
	int		errors	= 0;
};

//---- Outillage ---------------------------------------------------------------

//WebSocketServer::Init() rend la main AVANT que son thread n'appelle listen() :
//une connexion immédiate serait refusée (piège du lot 2).
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

//Un paquet T.140 tel qu'une jambe RTP en délivre
void InjectText(WSEndpoint& endpoint,const std::string& text,WORD seq)
{
	RTPPacket packet(MediaFrame::Text, TextCodec::T140);
	packet.SetTimestamp(seq*100);
	packet.SetSeqNum(seq);
	packet.SetPayload((BYTE*)text.c_str(),text.length());
	endpoint.onRTPPacket(packet);
}

//Le serveur WebSocket qui tient le rôle du pair distant, et que le WSEndpoint
//utilise AUSSI pour ouvrir ses connexions sortantes : un seul réacteur tient
//les deux bouts, comme dans la recette §7.
class PeerFixture
{
public:
	bool Start(int firstPort)
	{
		handler = std::make_shared<PeerHandler>();
		server.AddHandler("/t140", handler.get());

		for (int p = firstPort; p < firstPort+50 && !port; ++p)
			if (server.Init(p))
				port = p;

		if (!port || !WaitForListener(port))
			return false;

		WSEndpoint::SetServer(&server);
		return true;
	}

	~PeerFixture()
	{
		WSEndpoint::SetServer(NULL);
		if (port)
			server.End();
	}

	WebSocketServer&	Server()	{ return server;	}
	PeerHandler&		Peer()		{ return *handler;	}
	int			Port() const	{ return port;		}

	std::string Url(const std::string& path = "/t140")
	{
		return "ws://127.0.0.1:" + std::to_string(port) + path;
	}

private:
	WebSocketServer				server;
	std::shared_ptr<PeerHandler>		handler;
	int					port = 0;
};

} // namespace

//=============================================================================

//Le pontage, dans les deux sens, sur une seule jambe sortante.
TEST(WsClientEndpoint, LeTexteTraverseDansLesDeuxSens)
{
	PeerFixture fixture;
	if (!fixture.Start(39400))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto leg = std::make_shared<WSEndpoint>(MediaFrame::Text);
	RtpSink sink;
	leg->AddListener(&sink);

	ASSERT_EQ(1, leg->Connect(fixture.Url()));
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().Opens() > 0; }))
		<< "la jambe sortante n'a jamais atteint le pair";

	//RTP -> WebSocket
	InjectText(*leg,"bonjour",1);
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().CountReceived("bonjour") > 0; }))
		<< "le texte T.140 n'est pas arrivé au pair WebSocket";

	//WebSocket -> RTP (le pair renvoie l'écho)
	ASSERT_TRUE(WaitFor([&]{ return sink.Count("bonjour") > 0; }))
		<< "le texte du pair n'est pas reparti en RTP";

	leg->End();
}

//Le texte arrivé AVANT la première ouverture n'est pas perdu : entre la
//configuration de la jambe et l'ouverture il s'écoule un aller-retour, et la
//première phrase est celle où l'appelant se présente.
TEST(WsClientEndpoint, LeTexteDAvantLaPremiereOuvertureEstRejoue)
{
	PeerFixture fixture;
	if (!fixture.Start(39450))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto leg = std::make_shared<WSEndpoint>(MediaFrame::Text);
	RtpSink sink;
	leg->AddListener(&sink);

	//AVANT tout Connect : la jambe n'a pas encore de pair
	InjectText(*leg,"je me presente",1);

	ASSERT_EQ(1, leg->Connect(fixture.Url()));

	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().CountReceived("je me presente") > 0; }))
		<< "la file d'attente doit être rejouée à la première ouverture";

	leg->End();
}

//Une coupure est une perte ANNONCÉE des deux côtés, et le texte de la coupure
//est perdu : c'est l'arbitrage du 2026-09-21. Un seul cycle de reprise, donc un
//seul test — il dure le temps du backoff (5 s).
TEST(WsClientEndpoint, LaCoupureAnnonceLaPerteEtLaJambeRevient)
{
	PeerFixture fixture;
	if (!fixture.Start(39500))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto leg = std::make_shared<WSEndpoint>(MediaFrame::Text);
	RtpSink sink;
	leg->AddListener(&sink);

	ASSERT_EQ(1, leg->Connect(fixture.Url()));
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().Opens() > 0; }));

	//Le pair raccroche
	ASSERT_TRUE(fixture.Peer().CloseCurrent());

	//1. Le pair RTP l'apprend par un U+FFFD (T.140 §5.3)
	ASSERT_TRUE(WaitFor([&]{ return sink.Count(FFFD) > 0; }))
		<< "une coupure non annoncée colle deux phrases sans le dire";

	//2. Le texte de la coupure est PERDU : il ne doit jamais être rejoué
	InjectText(*leg,"pendant la coupure",2);

	//3. La reprise repart d'elle-même (5 s de backoff)
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().Opens() > 1; }, 15000))
		<< "la jambe cliente doit se reconnecter indéfiniment";

	//4. Et la reconnexion s'annonce au pair WebSocket par un U+FFFD
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().CountReceived(FFFD) > 0; }))
		<< "le pair WebSocket doit apprendre qu'il manque du texte";

	EXPECT_EQ(0, fixture.Peer().CountReceived("pendant la coupure"))
		<< "rejouer après l'U+FFFD afficherait un texte déjà déclaré perdu";

	//Le dialogue reprend sur la nouvelle connexion
	InjectText(*leg,"apres la reprise",3);
	EXPECT_TRUE(WaitFor([&]{ return fixture.Peer().CountReceived("apres la reprise") > 0; }))
		<< "la jambe reconnectée doit porter le texte comme avant";

	leg->End();
}

//End() est le SEUL arrêt volontaire de la reprise. Sans lui, fermer la jambe
//la rouvrirait aussitôt.
TEST(WsClientEndpoint, EndArreteLaReprise)
{
	PeerFixture fixture;
	if (!fixture.Start(39550))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto leg = std::make_shared<WSEndpoint>(MediaFrame::Text);
	RtpSink sink;
	leg->AddListener(&sink);

	ASSERT_EQ(1, leg->Connect(fixture.Url()));
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().Opens() > 0; }));

	leg->End();
	ASSERT_TRUE(WaitFor([&]{ return fixture.Peer().Closes() > 0; }))
		<< "End() doit fermer la connexion en cours";

	//Largement de quoi laisser passer un backoff de 5 s
	usleep(7000 * 1000);
	EXPECT_EQ(1, fixture.Peer().Opens())
		<< "une jambe terminée ne doit plus jamais se reconnecter";
}

//Ce qui est PERMANENT se dit tout de suite : une URL illisible ou un schéma
//inconnu n'arment rien, sinon le serveur boucle pour toujours sur une faute de
//frappe du contrôleur.
TEST(WsClientEndpoint, UneUrlInutilisableNArmeRien)
{
	PeerFixture fixture;
	if (!fixture.Start(39600))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	auto leg = std::make_shared<WSEndpoint>(MediaFrame::Text);

	EXPECT_EQ(0, leg->Connect("http://127.0.0.1/t140")) << "seuls ws:// et wss:// sont des jambes";
	EXPECT_EQ(0, leg->Connect("ws://"))		    << "une URL sans hôte n'est pas une cible";
	EXPECT_EQ(0, leg->Connect("pas une url"));

	EXPECT_FALSE(leg->IsClientMode()) << "aucune de ces URL ne doit armer la jambe";

	//Rien n'a été tenté : le pair n'a rien vu
	EXPECT_EQ(0, fixture.Peer().Opens());
}

//§4.7 : en mode client il n'y a PAS d'URL locale. Publier l'adresse d'écoute
//serait publier une adresse où personne n'entrera pour cette jambe — la
//duplication d'adresse que NETWORK-CONFIGURATION.md interdit.
TEST(WsClientEndpoint, LaJambeClientePublieLaCibleEtPasLEcoute)
{
	PeerFixture fixture;
	if (!fixture.Start(39650))
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";

	//Ce que publierait une jambe SERVEUR : un port d'écoute bien réel
	WSEndpoint::SetLocalPort(4443);

	Endpoint endpoint(L"cx-ws-client",
			  /*audioSupported=*/false,
			  /*videoSupported=*/false,
			  /*textSupported=*/true);
	ASSERT_EQ(0, endpoint.Init());

	const std::string url = fixture.Url();
	ASSERT_EQ(1, endpoint.ConnectMediaConnection(MediaFrame::Text, MediaFrame::VIDEO_MAIN,
						     url.c_str()));

	char* candidate = endpoint.GetMediaCandidates(MediaFrame::WS, MediaFrame::Text);
	ASSERT_TRUE(candidate != NULL) << "une jambe cliente doit publier quelque chose";
	const std::string published = candidate;
	free(candidate);

	EXPECT_EQ(url, published) << "c'est la CIBLE qui est publiée, pas notre écoute";
	EXPECT_EQ(std::string::npos, published.find("4443"))
		<< "le port d'écoute du serveur n'a rien à faire dans cette URL";

	endpoint.End();
}

//§9.4 : un pair qui accepte le TCP puis se tait — un wss:// pointé sur un port
//en clair, un 101 jamais écrit, un trou noir réseau. Le réacteur n'attend que
//des événements, et il n'en viendra aucun.
TEST(WsClientEndpoint, UneOuvertureMuetteEstAbandonnee)
{
	//Un pair qui accepte et ne dit rien
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(listener, 0);

	int optval = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	socklen_t len = sizeof(addr);
	ASSERT_EQ(0, bind(listener, (sockaddr*)&addr, sizeof(addr)));
	ASSERT_EQ(0, listen(listener, 5));
	ASSERT_EQ(0, getsockname(listener, (sockaddr*)&addr, &len));
	const int mutePort = ntohs(addr.sin_port);

	PeerFixture fixture;
	if (!fixture.Start(39700))
	{
		close(listener);
		GTEST_SKIP() << "Aucun port loopback disponible (sandbox réseau ?)";
	}

	//Le délai réel est de 10 s : le test n'a pas à les attendre.
	const DWORD saved = WebSocketConnection::GetOpeningTimeout();
	WebSocketConnection::SetOpeningTimeout(300);

	auto bare = std::make_shared<BareListener>();
	ASSERT_TRUE(fixture.Server().Connect("ws://127.0.0.1:"+std::to_string(mutePort)+"/t140", bare));

	//Le TCP aboutit — c'est tout le problème : sans échéance, rien ne suivrait.
	EXPECT_TRUE(WaitFor([&]{ return bare->IsClosed(); }, 5000))
		<< "une ouverture muette doit être abandonnée, pas laissée ouverte";
	EXPECT_FALSE(bare->IsOpened());
	EXPECT_GT(bare->Errors(), 0) << "l'abandon se dit par onError, comme un refus TCP";

	WebSocketConnection::SetOpeningTimeout(saved);
	close(listener);
}
