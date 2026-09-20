/**
 * test_ws_client_handshake.cpp — ouverture d'une connexion WebSocket SORTANTE
 * (lot 2 de docs/conception/WS-CLIENT/SPEC.md).
 *
 * Le mediaserver joue ici le navigateur : il ouvre la socket, envoie la requête
 * `GET … Upgrade`, vérifie le 101 et parle ensuite en trames masquées. Ce que
 * ces tests tiennent, et que rien d'autre ne tient :
 *
 *   - l'aller-retour complet contre le VRAI serveur WebSocket du mcu
 *     (`TextEchoWebsocketHandler`) : poignée de main, message masqué, écho ;
 *   - le piège 1 du SPEC : la première trame COLLÉE au 101 dans le même segment
 *     TCP. Elle est perdue si le client ignore ce que le parseur HTTP a
 *     consommé — et c'est exactement la première phrase du correspondant ;
 *   - le piège 4 : un échec AVANT le 101 (404, clé d'acceptation fausse, port
 *     fermé) doit émettre `onError` puis `onClose`. Sans cela le WSEndpoint
 *     attend indéfiniment un pair qui ne viendra jamais, sans une ligne pour le
 *     dire ;
 *   - le durcissement RFC 6455 §5.1 laissé ouvert par le lot 1 : un client qui
 *     REÇOIT une trame masquée ferme.
 *
 * La connexion est une machine à état passive : le test la pilote par un poll()
 * unique (`ClientDriver`), dans le même ordre que le réacteur de
 * `WebSocketServer`. Le lot 3 déplacera ce pilotage dans le réacteur du serveur.
 *
 *     ./tests/runtests --gtest_filter='WsClientHandshake*'
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "websocketconnection.h"
#include "websocketserver.h"

namespace {

//---- Le pair vu par la connexion cliente ------------------------------------

//Enregistre tout ce que la connexion notifie. Les callbacks arrivent tous sur
//le thread du test (celui qui pompe) : pas de verrou nécessaire.
class RecordingListener : public WebSocket::Listener
{
public:
	virtual void onOpen(WebSocket*)						{ opened = true;		}
	virtual void onMessageStart(WebSocket*,WebSocket::MessageType type,const DWORD)
	{
		lastType = type;
		message.clear();
	}
	virtual void onMessageData(WebSocket*,const BYTE* data,const DWORD size)	{ message.append((const char*)data,size); }
	virtual void onMessageEnd(WebSocket*)					{ ++messages;			}
	virtual void onError(WebSocket*)					{ ++errors; errorBeforeClose = errorBeforeClose || !closed; }
	virtual void onClose(WebSocket*)					{ closed = true; ++closes;	}

public:
	bool		opened		= false;
	bool		closed		= false;
	bool		errorBeforeClose= false;
	int		errors		= 0;
	int		closes		= 0;
	int		messages	= 0;
	std::string	message;
	WebSocket::MessageType lastType = WebSocket::Text;
};

//Le serveur est le seul Listener de connexion : en mode client il n'a rien à
//faire (personne ne demande d'upgrade, et le test pompe en boucle courte).
class SilentConnListener : public WebSocketConnection::Listener
{
public:
	virtual void onUpgradeRequest(WebSocketConnection*)	{ ADD_FAILURE() << "onUpgradeRequest en mode client"; }
	virtual void onWakeupNeeded()				{ }
};

//---- Pilotage de la connexion cliente ---------------------------------------

//Socket TCP loopback en cours de connexion (connect() NON bloquant), tel que le
//réacteur le fournira au lot 3.
int ConnectNonBlocking(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS)
	{
		close(fd);
		return -1;
	}
	return fd;
}

//WebSocketServer::Init() rend la main AVANT que son thread n'ait appelé
//listen() : une connexion immédiate serait refusée. On attend l'écoute par des
//sondes bloquantes, comme le fait test_websocket_echo.cpp.
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

class ClientDriver
{
public:
	ClientDriver(int fd, const std::string& host, const std::string& path,
		     std::shared_ptr<RecordingListener> listener)
	{
		this->listener = listener;
		conn = std::make_shared<WebSocketConnection>(&connListener, 1);
		conn->InitClient(fd, std::make_unique<WebSocketPlainTransport>(), host, path, listener);
	}

	~ClientDriver()
	{
		Finish();
	}

	WebSocketConnection& Conn()	{ return *conn;		}
	bool IsFinished() const		{ return finished;	}

	//Pompe jusqu'à ce que `until` soit vrai, ou expiration. Même ordre de
	//traitement que le réacteur de WebSocketServer.
	bool Pump(const std::function<bool()>& until, int timeoutMs = 3000)
	{
		for (int waited = 0; waited < timeoutMs; waited += 10)
		{
			if (until())
				return true;
			if (finished)
				return until();

			pollfd pfd;
			pfd.fd	    = conn->GetFd();
			pfd.events  = conn->GetPollEvents();
			pfd.revents = 0;

			if (poll(&pfd, 1, 10) > 0)
			{
				if (pfd.revents & (POLLNVAL|POLLERR|POLLHUP))
					Finish();
				else
				{
					if (pfd.revents & POLLOUT)
						conn->OnWritable();
					if (pfd.revents & POLLIN)
						conn->OnReadable();
				}
			}
			if (!finished && conn->IsFinished())
				Finish();
		}
		return until();
	}

	//Ce que fait WebSocketServer::CloseConnection : notifier puis fermer.
	void Finish()
	{
		if (finished)
			return;
		finished = true;
		conn->NotifyClose();
		conn->End();
	}

private:
	SilentConnListener			connListener;
	std::shared_ptr<RecordingListener>	listener;
	std::shared_ptr<WebSocketConnection>	conn;
	bool					finished = false;
};

//---- Un pair scripté, pour ce qu'aucun vrai serveur n'émettrait -------------

//Accepte UNE connexion, lit la requête d'upgrade, calcule la clé d'acceptation
//attendue, puis laisse le script écrire ce qu'il veut. Le socket reste ouvert
//jusqu'à Stop() : une fermeture prématurée donnerait un POLLHUP qui masquerait
//ce que le test veut voir.
class ScriptedServer
{
public:
	typedef std::function<void(int fd, const std::string& acceptKey)> Script;

	bool Start(Script script)
	{
		listenFd = socket(AF_INET, SOCK_STREAM, 0);
		if (listenFd < 0)
			return false;

		int optval = 1;
		setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = 0;			//port éphémère
		addr.sin_addr.s_addr = inet_addr("127.0.0.1");
		if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd, 1) < 0)
		{
			close(listenFd);
			listenFd = -1;
			return false;
		}

		socklen_t len = sizeof(addr);
		if (getsockname(listenFd, (sockaddr*)&addr, &len) < 0)
		{
			close(listenFd);
			listenFd = -1;
			return false;
		}
		port = ntohs(addr.sin_port);

		running = true;
		thread = std::thread(&ScriptedServer::Serve, this, script);
		return true;
	}

	void Stop()
	{
		running = false;
		if (thread.joinable())
			thread.join();
		if (clientFd >= 0) { close(clientFd); clientFd = -1; }
		if (listenFd >= 0) { close(listenFd); listenFd = -1; }
	}

	~ScriptedServer()	{ Stop(); }

	int GetPort() const	{ return port; }

	//Ce que le client a écrit après la poignée de main
	std::string Received()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return received;
	}

	//À appeler DEPUIS le script : lit ce que le client envoie tant que le
	//serveur tourne.
	void ReadClientFrames(int fd)
	{
		BYTE buf[1024];
		while (running)
		{
			pollfd pfd; pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
			if (poll(&pfd, 1, 20) <= 0)
				continue;
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n <= 0)
				return;
			std::lock_guard<std::mutex> lock(mutex);
			received.append((const char*)buf, n);
		}
	}

private:
	void Serve(Script script)
	{
		//accept() non bloquant : Stop() doit pouvoir nous sortir d'ici
		int flags = fcntl(listenFd, F_GETFL, 0);
		fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);

		while (running && clientFd < 0)
		{
			pollfd pfd; pfd.fd = listenFd; pfd.events = POLLIN; pfd.revents = 0;
			if (poll(&pfd, 1, 20) > 0)
				clientFd = accept(listenFd, NULL, 0);
		}
		if (clientFd < 0)
			return;

		//Lire la requête d'upgrade jusqu'à la ligne vide
		std::string request;
		BYTE buf[1024];
		while (running && request.find("\r\n\r\n") == std::string::npos)
		{
			pollfd pfd; pfd.fd = clientFd; pfd.events = POLLIN; pfd.revents = 0;
			if (poll(&pfd, 1, 20) <= 0)
				continue;
			ssize_t n = read(clientFd, buf, sizeof(buf));
			if (n <= 0)
				return;
			request.append((const char*)buf, n);
		}
		if (!running)
			return;

		{
			std::lock_guard<std::mutex> lock(mutex);
			upgradeRequest = request;
		}

		script(clientFd, ComputeAccept(request));
	}

	//La clé d'acceptation que le client attend pour CETTE requête
	static std::string ComputeAccept(const std::string& request)
	{
		const std::string field = "Sec-WebSocket-Key: ";
		size_t pos = request.find(field);
		if (pos == std::string::npos)
			return std::string();
		pos += field.size();
		size_t end = request.find("\r\n", pos);
		return WebSocketConnection::ComputeAcceptKey(request.substr(pos, end-pos));
	}

private:
	int			listenFd = -1;
	int			clientFd = -1;
	int			port = 0;
	std::atomic<bool>	running{false};
	std::thread		thread;
	std::mutex		mutex;
	std::string		received;
	std::string		upgradeRequest;
};

bool WriteAll(int fd, const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		ssize_t n = write(fd, data.data()+sent, data.size()-sent);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

std::string Response101(const std::string& acceptKey)
{
	return std::string("HTTP/1.1 101 Switching Protocols\r\n")
		+ "Upgrade: websocket\r\n"
		+ "Connection: Upgrade\r\n"
		+ "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
		+ "\r\n";
}

//Trame texte non masquée (ce qu'un serveur émet)
std::string ServerTextFrame(const std::string& payload)
{
	WebSocketFrameHeader header(true, WebSocketFrameHeader::TextFrame, payload.size(), 0);
	std::string frame((char*)header.GetData(), header.GetSize());
	frame += payload;
	return frame;
}

//Trame texte MASQUÉE — interdite dans le sens serveur → client (RFC 6455 §5.1)
std::string MaskedTextFrame(const std::string& payload, DWORD mask)
{
	WebSocketFrameHeader header(true, WebSocketFrameHeader::TextFrame, payload.size(), mask);
	std::string frame((char*)header.GetData(), header.GetSize());

	BYTE maskBytes[4];
	set4(maskBytes, 0, mask);
	for (size_t i = 0; i < payload.size(); ++i)
		frame.push_back((char)((BYTE)payload[i] ^ maskBytes[i & 3]));
	return frame;
}

} // namespace

//=============================================================================
// Contre le VRAI serveur WebSocket du mcu
//=============================================================================

TEST(WsClientHandshake, LOuvertureClienteAboutitEtLEchoRevient)
{
	WebSocketServer server;
	auto echo = std::make_shared<TextEchoWebsocketHandler>();
	server.AddHandler("/echo", echo.get());

	int port = 0;
	for (int p = 39100; p < 39150; ++p)
		if (server.Init(p)) { port = p; break; }
	if (port == 0)
		GTEST_SKIP() << "Aucun port loopback disponible pour le serveur WebSocket";

	if (!WaitForListener(port))
	{
		server.End();
		GTEST_SKIP() << "Connexion loopback impossible (sandbox réseau ?)";
	}

	int fd = ConnectNonBlocking(port);
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1:"+std::to_string(port), "/echo", listener);

	ASSERT_TRUE(driver.Pump([&]{ return listener->opened || listener->closed; }))
		<< "la poignée de main cliente n'a pas abouti";
	ASSERT_TRUE(listener->opened);
	EXPECT_EQ(0, listener->errors);
	EXPECT_EQ(WebSocketConnection::Opened, driver.Conn().GetClientState());

	//Le serveur ne renverrait rien d'exploitable si la trame n'était pas masquée
	//comme il l'attend (RFC 6455 §5.3).
	const std::string payload = "bonjour le serveur";
	driver.Conn().SendMessage(payload);

	ASSERT_TRUE(driver.Pump([&]{ return listener->messages > 0; }))
		<< "pas d'écho reçu";
	EXPECT_EQ(payload, listener->message);
	EXPECT_EQ(WebSocket::Text, listener->lastType);

	driver.Conn().Close();
	EXPECT_TRUE(driver.Pump([&]{ return listener->closed; }));
	EXPECT_EQ(1, listener->closes);

	server.End();
}

TEST(WsClientHandshake, UnCheminSansHandlerEchoueEtSeDit)
{
	WebSocketServer server;
	auto echo = std::make_shared<TextEchoWebsocketHandler>();
	server.AddHandler("/echo", echo.get());

	int port = 0;
	for (int p = 39150; p < 39200; ++p)
		if (server.Init(p)) { port = p; break; }
	if (port == 0)
		GTEST_SKIP() << "Aucun port loopback disponible pour le serveur WebSocket";

	if (!WaitForListener(port))
	{
		server.End();
		GTEST_SKIP() << "Connexion loopback impossible (sandbox réseau ?)";
	}

	int fd = ConnectNonBlocking(port);
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1:"+std::to_string(port), "/personne", listener);

	//Le serveur répond 404 : l'échec doit se voir, et dans le bon ordre
	ASSERT_TRUE(driver.Pump([&]{ return listener->closed; }))
		<< "un refus avant le 101 n'a notifié personne";
	EXPECT_FALSE(listener->opened);
	EXPECT_EQ(1, listener->errors);
	EXPECT_TRUE(listener->errorBeforeClose) << "onError doit précéder onClose";
	EXPECT_EQ(WebSocketConnection::Failed, driver.Conn().GetClientState());

	server.End();
}

//=============================================================================
// Contre un pair scripté
//=============================================================================

//Piège 1 du SPEC : le serveur colle sa première trame au 101, dans le MÊME
//segment TCP. Le parseur HTTP s'arrête à la fin des en-têtes ; ce qui suit
//appartient au parseur de trames. L'ignorer, c'est perdre la première phrase.
TEST(WsClientHandshake, LaTrameColleeAu101NEstPasPerdue)
{
	ScriptedServer server;
	ASSERT_TRUE(server.Start([](int fd, const std::string& acceptKey) {
		//UN SEUL write : 101 + première trame texte
		WriteAll(fd, Response101(acceptKey) + ServerTextFrame("premiere phrase"));
	})) << "pair scripté impossible à démarrer";

	int fd = ConnectNonBlocking(server.GetPort());
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1", "/t140", listener);

	ASSERT_TRUE(driver.Pump([&]{ return listener->messages > 0 || listener->closed; }));
	EXPECT_TRUE(listener->opened);
	ASSERT_EQ(1, listener->messages) << "la trame collée au 101 a été perdue";
	EXPECT_EQ("premiere phrase", listener->message);

	server.Stop();
}

//RFC 6455 §5.1 : un serveur ne masque jamais. Un client qui reçoit une trame
//masquée DOIT fermer. Versant laissé ouvert par le lot 1, faute de connexion
//cliente ouverte.
TEST(WsClientHandshake, UneTrameMasqueeDuServeurFermeLaConnexion)
{
	ScriptedServer server;
	ASSERT_TRUE(server.Start([](int fd, const std::string& acceptKey) {
		WriteAll(fd, Response101(acceptKey));
		WriteAll(fd, MaskedTextFrame("interdit", 0x37fa213d));
	}));

	int fd = ConnectNonBlocking(server.GetPort());
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1", "/t140", listener);

	ASSERT_TRUE(driver.Pump([&]{ return listener->closed; }))
		<< "la connexion n'a pas été fermée sur trame masquée";
	EXPECT_TRUE(listener->opened) << "le 101 avait bien été accepté";
	EXPECT_EQ(0, listener->messages) << "une trame masquée ne doit rien livrer";

	server.Stop();
}

//La clé d'acceptation est la seule preuve que le pair parle WebSocket et qu'il
//répond à NOTRE requête.
TEST(WsClientHandshake, UnMauvaisSecWebSocketAcceptEstRefuse)
{
	ScriptedServer server;
	ASSERT_TRUE(server.Start([](int fd, const std::string&) {
		//Clé calculée sur une autre requête : syntaxiquement valide, fausse ici
		WriteAll(fd, Response101("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	}));

	int fd = ConnectNonBlocking(server.GetPort());
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1", "/t140", listener);

	ASSERT_TRUE(driver.Pump([&]{ return listener->closed; }));
	EXPECT_FALSE(listener->opened) << "un 101 non prouvé ne doit pas ouvrir la jambe";
	EXPECT_EQ(1, listener->errors);
	EXPECT_TRUE(listener->errorBeforeClose);
	EXPECT_EQ(WebSocketConnection::Failed, driver.Conn().GetClientState());

	server.Stop();
}

//Ce que le pair voit sur le fil : toute trame cliente est masquée (§5.3). Le
//lot 1 le tient dans la classe Frame ; ici c'est le fil réel.
TEST(WsClientHandshake, LaTrameSortanteEstMasqueeSurLeFil)
{
	ScriptedServer server;
	ScriptedServer* self = &server;
	ASSERT_TRUE(server.Start([self](int fd, const std::string& acceptKey) {
		WriteAll(fd, Response101(acceptKey));
		self->ReadClientFrames(fd);
	}));

	int fd = ConnectNonBlocking(server.GetPort());
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1", "/t140", listener);

	ASSERT_TRUE(driver.Pump([&]{ return listener->opened || listener->closed; }));
	ASSERT_TRUE(listener->opened);

	driver.Conn().SendMessage(std::string("masque moi"));
	ASSERT_TRUE(driver.Pump([&]{ return server.Received().size() >= 2; }))
		<< "rien n'est arrivé au pair";

	const std::string wire = server.Received();
	EXPECT_EQ(WebSocketFrameHeader::TextFrame, (BYTE)(wire[0] & 0x0F));
	EXPECT_TRUE((BYTE)wire[1] & 0x80) << "une trame cliente non masquée fait fermer le pair";
	//Le payload masqué ne doit pas se lire en clair sur le fil
	EXPECT_EQ(std::string::npos, wire.find("masque moi"));

	server.Stop();
}

//Un connect() non bloquant ne dit son échec que par SO_ERROR : POLLOUT arrive
//dans les deux cas. Sans cette lecture, la requête d'upgrade partirait dans le
//vide.
TEST(WsClientHandshake, UnPortFermeSeSignaleParOnError)
{
	//Un port éphémère réservé puis relâché : personne n'y écoute
	int probe = socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(probe, 0);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	ASSERT_EQ(0, bind(probe, (sockaddr*)&addr, sizeof(addr)));
	socklen_t len = sizeof(addr);
	ASSERT_EQ(0, getsockname(probe, (sockaddr*)&addr, &len));
	const int deadPort = ntohs(addr.sin_port);
	close(probe);

	int fd = ConnectNonBlocking(deadPort);
	ASSERT_GE(fd, 0);

	auto listener = std::make_shared<RecordingListener>();
	ClientDriver driver(fd, "127.0.0.1:"+std::to_string(deadPort), "/t140", listener);

	//Attendre la fin du connect() sans passer par le chemin POLLERR du réacteur :
	//c'est SO_ERROR qu'on veut exercer.
	pollfd pfd; pfd.fd = driver.Conn().GetFd(); pfd.events = POLLOUT; pfd.revents = 0;
	ASSERT_GT(poll(&pfd, 1, 2000), 0) << "le connect() n'a jamais rendu la main";
	driver.Conn().OnWritable();

	EXPECT_EQ(1, listener->errors);
	EXPECT_FALSE(listener->opened);
	EXPECT_EQ(WebSocketConnection::Failed, driver.Conn().GetClientState());
	EXPECT_TRUE(driver.Conn().IsFinished());

	//Le réacteur fermera : onClose suit onError
	driver.Finish();
	EXPECT_TRUE(listener->closed);
	EXPECT_EQ(1, listener->errors) << "onError ne doit pas être émis deux fois";
}
