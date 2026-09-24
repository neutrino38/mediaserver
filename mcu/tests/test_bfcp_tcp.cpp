/**
 * test_bfcp_tcp.cpp — le transport TCP de la pile BFCP interne.
 *
 * Conception : docs/conception/BFCP-INTERNE/SPEC.md §4.2, lot 2.
 *
 * Chaque test ouvre une VRAIE écoute et s'y connecte en bouclage, puis parle
 * BFCP sur le fil. Rien n'est simulé : c'est le noyau qui découpe, et c'est
 * précisément ce qu'on veut éprouver — un message écrit octet par octet, deux
 * messages dans une seule écriture, une connexion qui tombe.
 *
 * Deux défauts de libbfcp ont leur test ici, pour qu'ils ne reviennent pas :
 * le surplus d'une lecture jeté, et une écoute qui n'entend que l'IPv4.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "bfcp/BFCPTcpTransport.h"
#include "bfcp/BFCPFloorControlServer.h"
#include "rtpsessionset.h"

namespace {

typedef std::chrono::steady_clock Clock;

template <class Pred>
bool WaitFor(Pred pred, long deadlineMs = 3000)
{
	const Clock::time_point t0 = Clock::now();
	while (! pred() && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < deadlineMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	return pred();
}

const int CONF	= 42;
const int FLOOR	= 1;
const int ALICE	= 10;

// Le chair : il compte ce que le serveur lui remonte, et peut octroyer tout seul.
struct CountingListener : public BFCPFloorControlServer::Listener
{
	std::atomic<int>	requests{0};
	std::atomic<int>	granted{0};
	std::atomic<int>	released{0};
	std::atomic<int>	connected{0};
	std::atomic<int>	lastFloorRequestId{0};
	BFCPFloorControlServer*	grantFrom = nullptr;

	void onFloorRequest(int floorRequestId, int, int, std::set<int>) override
	{
		lastFloorRequestId = floorRequestId;
		requests++;
		if (grantFrom)
			grantFrom->GrantFloorRequest(floorRequestId);
	}
	void onFloorGranted(int, int, std::set<int>) override	{ granted++; }
	void onFloorReleased(int, int, std::set<int>) override	{ released++; }
	void onUserConnected(int) override			{ connected++; }
};

// Un client BFCP minimal, en bouclage, qui écrit et lit des octets bruts.
class Client
{
public:
	explicit Client(int family = AF_INET6) : fd(-1), family(family) {}

	~Client()
	{
		Disconnect();
	}

	bool Connect(WORD port)
	{
		fd = socket(family, SOCK_STREAM, 0);
		if (fd < 0)
			return false;

		if (family == AF_INET6) {
			sockaddr_in6 addr = {};
			addr.sin6_family = AF_INET6;
			addr.sin6_addr	 = in6addr_loopback;
			addr.sin6_port	 = htons(port);
			if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
				Disconnect();
				return false;
			}
		} else {
			sockaddr_in addr = {};
			addr.sin_family		= AF_INET;
			addr.sin_addr.s_addr	= htonl(INADDR_LOOPBACK);
			addr.sin_port		= htons(port);
			if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
				Disconnect();
				return false;
			}
		}
		return true;
	}

	void Disconnect()
	{
		if (fd >= 0)
			close(fd);
		fd = -1;
	}

	bool Send(const std::vector<BYTE>& bytes)
	{
		size_t sent = 0;
		while (sent < bytes.size()) {
			const ssize_t n = ::send(fd, &bytes[sent], bytes.size() - sent, MSG_NOSIGNAL);
			if (n <= 0)
				return false;
			sent += n;
		}
		return true;
	}

	// Un octet à la fois, avec une pause : le serveur DOIT recoller.
	bool SendByteByByte(const std::vector<BYTE>& bytes)
	{
		for (size_t i=0; i<bytes.size(); i++) {
			const BYTE one = bytes[i];
			if (::send(fd, &one, 1, MSG_NOSIGNAL) != 1)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	bool Send(const BFCPMessage& msg)
	{
		BYTE buffer[4096];
		const size_t n = msg.Serialize(buffer, sizeof(buffer));
		if (n == BFCPMessage::Failed)
			return false;
		return Send(std::vector<BYTE>(buffer, buffer + n));
	}

	// Lit un message entier, ou rend nullptr au bout du délai.
	BFCPMessage* Receive(long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();

		for (;;) {
			if (in.size() >= BFCPMessage::HeaderLen) {
				const size_t total = BFCPMessage::HeaderLen + get2(&in[0], 2) * 4;
				if (in.size() >= total) {
					BFCPMessage* msg = BFCPMessage::Parse(&in[0], total);
					in.erase(in.begin(), in.begin() + total);
					return msg;
				}
			}

			if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() >= deadlineMs)
				return nullptr;

			pollfd pfd = { fd, POLLIN, 0 };
			if (poll(&pfd, 1, 50) <= 0)
				continue;

			BYTE chunk[4096];
			const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
			if (n <= 0)
				return nullptr;
			in.insert(in.end(), chunk, chunk + n);
		}
	}

	// Vrai quand le serveur a fermé de son côté.
	bool WaitClosed(long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();
		while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < deadlineMs) {
			pollfd pfd = { fd, POLLIN, 0 };
			if (poll(&pfd, 1, 50) > 0) {
				BYTE chunk[4096];
				const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
				if (n == 0)
					return true;
				if (n < 0)
					return true;
				in.insert(in.end(), chunk, chunk + n);
			}
		}
		return false;
	}

	int Fd() const { return fd; }

private:
	int			fd;
	int			family;
	std::vector<BYTE>	in;
};

std::vector<BYTE> Bytes(const BFCPMessage& msg)
{
	BYTE buffer[4096];
	const size_t n = msg.Serialize(buffer, sizeof(buffer));
	if (n == BFCPMessage::Failed)
		return std::vector<BYTE>();
	return std::vector<BYTE>(buffer, buffer + n);
}

bool HasIPv6Loopback()
{
	const int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;
	sockaddr_in6 addr = {};
	addr.sin6_family = AF_INET6;
	addr.sin6_addr	 = in6addr_loopback;
	const bool ok = (bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
	close(fd);
	return ok;
}

class BfcpTcp : public ::testing::Test
{
protected:
	CountingListener			listener;
	std::unique_ptr<BFCPFloorControlServer>	server;
	std::unique_ptr<BFCPTcpListener>	tcp;
	WORD					port = 0;

	void SetUp() override
	{
		server.reset(new BFCPFloorControlServer(CONF, &listener));
		ASSERT_TRUE(server->AddFloor(FLOOR));
		ASSERT_TRUE(server->AddUser(ALICE));

		tcp.reset(new BFCPTcpListener(server.get()));
		port = tcp->Open();
		ASSERT_NE(0, port) << "l'ecoute doit s'ouvrir sur un port libre";
	}

	void TearDown() override
	{
		if (tcp)
			tcp->Close();
		if (server)
			server->End();
		tcp.reset();
		server.reset();
	}

	// Un Hello depuis ce client, et le HelloAck qui doit revenir.
	bool Handshake(Client& client)
	{
		if (! client.Send(BFCPMsgHello(1, CONF, ALICE)))
			return false;
		std::unique_ptr<BFCPMessage> ack(client.Receive());
		return ack && ack->GetPrimitive() == BFCPMessage::HelloAck;
	}
};

} // namespace


// ---- L'écoute ---------------------------------------------------------------

TEST_F(BfcpTcp, LEcouteRendLePortReellementLie)
{
	// Le port annoncé dans le SDP est celui que le noyau a donné, pas celui
	// qu'une socket sonde avait vu libre un instant plus tôt.
	EXPECT_NE(0, tcp->GetPort());
	EXPECT_EQ(port, tcp->GetPort());

	Client client;
	EXPECT_TRUE(client.Connect(port));
}

TEST_F(BfcpTcp, LEcouteEntendLesDeuxFamilles)
{
	// Le défaut de libbfcp : "0.0.0.0" en dur, donc IPv4 seulement. Une seule
	// écoute doit prendre les deux.
	Client v4(AF_INET);
	EXPECT_TRUE(v4.Connect(port)) << "un client IPv4 doit joindre l'ecoute";
	EXPECT_TRUE(Handshake(v4));

	if (! HasIPv6Loopback())
		GTEST_SKIP() << "pas de boucle locale IPv6 sur cette machine";

	Client v6(AF_INET6);
	EXPECT_TRUE(v6.Connect(port)) << "un client IPv6 doit joindre la MEME ecoute";
	EXPECT_TRUE(Handshake(v6));
}

TEST_F(BfcpTcp, PlusieursConnexionsCohabitent)
{
	Client first;
	Client second;
	ASSERT_TRUE(first.Connect(port));
	ASSERT_TRUE(second.Connect(port));

	EXPECT_TRUE(WaitFor([&]{ return tcp->GetConnectionCount() == 2; }));
}


// ---- Le découpage du flux ---------------------------------------------------

TEST_F(BfcpTcp, UnHelloRecoitUnHelloAck)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(client.Send(BFCPMsgHello(7, CONF, ALICE)));

	std::unique_ptr<BFCPMessage> ack(client.Receive());
	ASSERT_NE(nullptr, ack.get());
	EXPECT_EQ(BFCPMessage::HelloAck, ack->GetPrimitive());
	EXPECT_EQ(7, ack->GetTransactionId());
	EXPECT_EQ(CONF, ack->GetConferenceId());
	EXPECT_EQ(ALICE, ack->GetUserId());
	EXPECT_TRUE(WaitFor([&]{ return listener.connected.load() == 1; }));
}

TEST_F(BfcpTcp, UnMessageEcritOctetParOctetEstRecolle)
{
	// Une lecture partielle ne doit rien perdre : c'est le cas normal d'un
	// TCP qui fragmente.
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(client.SendByteByByte(Bytes(BFCPMsgHello(9, CONF, ALICE))));

	std::unique_ptr<BFCPMessage> ack(client.Receive());
	ASSERT_NE(nullptr, ack.get());
	EXPECT_EQ(BFCPMessage::HelloAck, ack->GetPrimitive());
	EXPECT_EQ(9, ack->GetTransactionId());
}

TEST_F(BfcpTcp, DeuxMessagesDansUneSeuleEcritureSontTousLesDeuxTraites)
{
	// LE défaut de libbfcp : elle jette ce qui suit le premier message d'une
	// lecture. Ici les deux doivent être servis.
	Client client;
	ASSERT_TRUE(client.Connect(port));

	std::vector<BYTE> both = Bytes(BFCPMsgHello(1, CONF, ALICE));
	const std::vector<BYTE> second = Bytes(BFCPMsgHello(2, CONF, ALICE));
	both.insert(both.end(), second.begin(), second.end());
	ASSERT_TRUE(client.Send(both));

	std::unique_ptr<BFCPMessage> first(client.Receive());
	ASSERT_NE(nullptr, first.get());
	EXPECT_EQ(1, first->GetTransactionId());

	std::unique_ptr<BFCPMessage> next(client.Receive());
	ASSERT_NE(nullptr, next.get()) << "le second message de la meme lecture est perdu";
	EXPECT_EQ(2, next->GetTransactionId());
}

TEST_F(BfcpTcp, TroisMessagesColesSontTousTraites)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));

	std::vector<BYTE> all;
	for (int i=1; i<=3; i++) {
		const std::vector<BYTE> one = Bytes(BFCPMsgHello(i, CONF, ALICE));
		all.insert(all.end(), one.begin(), one.end());
	}
	ASSERT_TRUE(client.Send(all));

	for (int i=1; i<=3; i++) {
		std::unique_ptr<BFCPMessage> ack(client.Receive());
		ASSERT_NE(nullptr, ack.get()) << "message " << i;
		EXPECT_EQ(i, ack->GetTransactionId());
	}
}

TEST_F(BfcpTcp, UnMessageQuiSeDeclarePlusGrandQueLaLimiteFermeLaConnexion)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));

	// En-tête annonçant 0xFFFF mots, soit 256 Ko : au-dessus de ce qu'on sert.
	std::vector<BYTE> header = Bytes(BFCPMsgHello(1, CONF, ALICE));
	header[2] = 0xff;
	header[3] = 0xff;
	ASSERT_TRUE(client.Send(header));

	EXPECT_TRUE(client.WaitClosed()) << "le serveur doit fermer plutot que grossir sans fin";
}

TEST_F(BfcpTcp, UnMessageIllisibleFermeLaConnexion)
{
	// Sur un flux, une longueur à laquelle on ne croit plus rend la suite
	// indéchiffrable : on ne sait plus où commence le message suivant.
	Client client;
	ASSERT_TRUE(client.Connect(port));

	std::vector<BYTE> junk = Bytes(BFCPMsgHello(1, CONF, ALICE));
	junk[0] = 0xe0;		// version 7, inconnue
	ASSERT_TRUE(client.Send(junk));

	EXPECT_TRUE(client.WaitClosed());
}


// ---- L'identité portée par la connexion ------------------------------------

TEST_F(BfcpTcp, UnUtilisateurInconnuRecoitUneErreur)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(client.Send(BFCPMsgHello(1, CONF, 999)));

	std::unique_ptr<BFCPMessage> reply(client.Receive());
	ASSERT_NE(nullptr, reply.get());
	ASSERT_EQ(BFCPMessage::Error, reply->GetPrimitive());
	EXPECT_EQ(BFCPAttrErrorCode::UserDoesNotExist, ((BFCPMsgError*)reply.get())->GetErrorCode());
}

TEST_F(BfcpTcp, UneAutreConferenceRecoitUneErreur)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(client.Send(BFCPMsgHello(1, CONF + 1, ALICE)));

	std::unique_ptr<BFCPMessage> reply(client.Receive());
	ASSERT_NE(nullptr, reply.get());
	ASSERT_EQ(BFCPMessage::Error, reply->GetPrimitive());
	EXPECT_EQ(BFCPAttrErrorCode::ConferenceDoesNotExist, ((BFCPMsgError*)reply.get())->GetErrorCode());
}


// ---- Le cycle complet d'une parole -----------------------------------------

TEST_F(BfcpTcp, UneDemandeDeParoleVaJusquAuChairEtRevientOctroyee)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(client.Send(request));

	// La réponse : Pending, avec le transaction id de la requête.
	std::unique_ptr<BFCPMessage> pending(client.Receive());
	ASSERT_NE(nullptr, pending.get());
	ASSERT_EQ(BFCPMessage::FloorRequestStatus, pending->GetPrimitive());
	EXPECT_EQ(20, pending->GetTransactionId());
	const BFCPAttrFloorRequestInformation* info = ((BFCPMsgFloorRequestStatus*)pending.get())->GetFloorRequestInformation();
	ASSERT_NE(nullptr, info);
	ASSERT_NE(nullptr, info->GetOverallRequestStatus());
	EXPECT_EQ(BFCPAttrRequestStatus::Pending, info->GetOverallRequestStatus()->GetRequestStatus()->GetStatus());

	ASSERT_TRUE(WaitFor([&]{ return listener.requests.load() == 1; }));

	// Le chair accepte, depuis son propre thread comme le fait XML-RPC.
	ASSERT_TRUE(server->GrantFloorRequest(listener.lastFloorRequestId.load()));

	std::unique_ptr<BFCPMessage> accepted(client.Receive());
	ASSERT_NE(nullptr, accepted.get());
	ASSERT_EQ(BFCPMessage::FloorRequestStatus, accepted->GetPrimitive());
	EXPECT_EQ(BFCPAttrRequestStatus::Accepted,
		((BFCPMsgFloorRequestStatus*)accepted.get())->GetFloorRequestInformation()->GetOverallRequestStatus()->GetRequestStatus()->GetStatus());

	std::unique_ptr<BFCPMessage> granted(client.Receive());
	ASSERT_NE(nullptr, granted.get());
	ASSERT_EQ(BFCPMessage::FloorRequestStatus, granted->GetPrimitive());
	EXPECT_EQ(BFCPAttrRequestStatus::Granted,
		((BFCPMsgFloorRequestStatus*)granted.get())->GetFloorRequestInformation()->GetOverallRequestStatus()->GetRequestStatus()->GetStatus());
	EXPECT_EQ(0, granted->GetTransactionId()) << "notification serveur sur transport fiable";
}

TEST_F(BfcpTcp, LeDetenteurQuiLibereEstEntendu)
{
	listener.grantFrom = server.get();

	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(client.Send(request));
	ASSERT_TRUE(WaitFor([&]{ return listener.granted.load() == 1; }));

	const int floorRequestId = listener.lastFloorRequestId.load();
	EXPECT_EQ(floorRequestId, server->GetGrantedFloorRequestId(FLOOR));

	BFCPMsgFloorRelease release(21, CONF, ALICE);
	release.SetFloorRequestId(floorRequestId);
	ASSERT_TRUE(client.Send(release));

	EXPECT_TRUE(WaitFor([&]{ return listener.released.load() == 1; }));
	EXPECT_EQ(0, server->GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpTcp, LaFermetureParLeClientRendLaParole)
{
	// Le cas réel : l'endpoint raccroche sans rien libérer. Sa parole doit
	// tomber, sinon le partage reste bloqué sur un participant parti.
	listener.grantFrom = server.get();

	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(client.Send(request));
	ASSERT_TRUE(WaitFor([&]{ return listener.granted.load() == 1; }));
	ASSERT_NE(0, server->GetGrantedFloorRequestId(FLOOR));

	client.Disconnect();

	EXPECT_TRUE(WaitFor([&]{ return listener.released.load() == 1; })) << "le chair doit apprendre la chute";
	EXPECT_EQ(0, server->GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpTcp, UneConnexionFermeeEstRecoltee)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));
	ASSERT_TRUE(WaitFor([&]{ return tcp->GetConnectionCount() == 1; }));

	ASSERT_TRUE(server->IsUserConnected(ALICE));

	client.Disconnect();

	// Sans récolte, chaque appel laisserait une connexion et son descripteur.
	EXPECT_TRUE(WaitFor([&]{ return tcp->GetConnectionCount() == 0; }));
	EXPECT_FALSE(server->IsUserConnected(ALICE)) << "le transport recolte doit etre detache";
}

TEST_F(BfcpTcp, LeRetraitDUnUtilisateurFermeSaConnexion)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));

	ASSERT_TRUE(server->RemoveUser(ALICE));

	// Le Goodbye part, puis le serveur ferme.
	std::unique_ptr<BFCPMessage> bye(client.Receive());
	ASSERT_NE(nullptr, bye.get());
	EXPECT_EQ(BFCPMessage::Goodbye, bye->GetPrimitive());
	EXPECT_TRUE(client.WaitClosed());
	EXPECT_TRUE(WaitFor([&]{ return tcp->GetConnectionCount() == 0; }));
}

TEST_F(BfcpTcp, UnGoodbyeDuClientEstAcquitte)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));

	ASSERT_TRUE(client.Send(BFCPMessage(BFCPMessage::Goodbye, 30, CONF, ALICE)));

	std::unique_ptr<BFCPMessage> ack(client.Receive());
	ASSERT_NE(nullptr, ack.get());
	EXPECT_EQ(BFCPMessage::GoodbyeAck, ack->GetPrimitive());
	EXPECT_EQ(30, ack->GetTransactionId());
}


// ---- L'abonnement au floor --------------------------------------------------

TEST_F(BfcpTcp, UnAbonneEstNotifieDesChangements)
{
	ASSERT_TRUE(server->AddUser(20));
	listener.grantFrom = server.get();

	Client watcher;
	ASSERT_TRUE(watcher.Connect(port));
	ASSERT_TRUE(watcher.Send(BFCPMsgHello(1, CONF, 20)));
	std::unique_ptr<BFCPMessage> helloAck(watcher.Receive());
	ASSERT_NE(nullptr, helloAck.get());

	BFCPMsgFloorQuery query(2, CONF, 20);
	query.AddFloorId(FLOOR);
	ASSERT_TRUE(watcher.Send(query));

	std::unique_ptr<BFCPMessage> status(watcher.Receive());
	ASSERT_NE(nullptr, status.get());
	ASSERT_EQ(BFCPMessage::FloorStatus, status->GetPrimitive());
	EXPECT_EQ(0, ((BFCPMsgFloorStatus*)status.get())->CountFloorRequestInformations());

	// Un autre demande et obtient la parole : l'abonné doit le voir.
	Client speaker;
	ASSERT_TRUE(speaker.Connect(port));
	ASSERT_TRUE(Handshake(speaker));
	BFCPMsgFloorRequest request(3, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(speaker.Send(request));

	std::unique_ptr<BFCPMessage> notified(watcher.Receive());
	ASSERT_NE(nullptr, notified.get());
	ASSERT_EQ(BFCPMessage::FloorStatus, notified->GetPrimitive());
	const BFCPMsgFloorStatus* floorStatus = (BFCPMsgFloorStatus*)notified.get();
	ASSERT_EQ(1, floorStatus->CountFloorRequestInformations());
	EXPECT_EQ(BFCPAttrRequestStatus::Granted,
		floorStatus->GetFloorRequestInformation(0)->GetOverallRequestStatus()->GetRequestStatus()->GetStatus());
}


// ---- Fermeture de l'écoute --------------------------------------------------

TEST_F(BfcpTcp, FermerLEcouteFermeLesConnexionsEnCours)
{
	Client client;
	ASSERT_TRUE(client.Connect(port));
	ASSERT_TRUE(Handshake(client));
	ASSERT_TRUE(WaitFor([&]{ return tcp->GetConnectionCount() == 1; }));

	ASSERT_TRUE(server->IsUserConnected(ALICE));

	tcp->Close();

	EXPECT_EQ(0, tcp->GetConnectionCount());
	// Le serveur doit avoir LACHE le transport. Sans ca il garde un pointeur
	// vers un objet detruit, et le Goodbye suivant ecrit dans du vide : une
	// corruption de tas qui ne se voit que bien plus loin.
	EXPECT_FALSE(server->IsUserConnected(ALICE)) << "l'ecoute fermee doit detacher le transport";
	EXPECT_TRUE(client.WaitClosed());

	// Le port est libre : plus personne n'écoute.
	Client late;
	EXPECT_FALSE(late.Connect(port));
}
