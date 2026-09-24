/**
 * test_bfcp_udp.cpp — le transport UDP de la pile BFCP interne, et la
 * fiabilité que UDP ne donne pas (RFC 8855 §6.2).
 *
 * Conception : docs/conception/BFCP-INTERNE/SPEC.md §4.3, lot 3.
 *
 * Chaque test ouvre un VRAI point d'accès et lui parle depuis une vraie socket
 * UDP en bouclage. Ce qui est éprouvé ici ne se voit pas autrement : ce que le
 * serveur réémet et quand, ce qu'il rejoue à l'identique, ce qu'il jette.
 *
 * Les délais sont ceux de la RFC : 500 ms puis doublement. Les tests qui les
 * observent sont donc lents de quelques secondes — c'est le prix d'éprouver
 * une horloge plutôt que de la simuler.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "bfcp/BFCPUdpTransport.h"
#include "bfcp/BFCPFloorControlServer.h"
#include "rtpsessionset.h"

namespace {

typedef std::chrono::steady_clock Clock;

long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

template <class Pred>
bool WaitFor(Pred pred, long deadlineMs = 3000)
{
	const Clock::time_point t0 = Clock::now();
	while (! pred() && ElapsedMs(t0) < deadlineMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	return pred();
}

const int CONF	= 77;
const int FLOOR	= 1;
const int ALICE	= 10;

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

// Un pair BFCP en UDP : il envoie des datagrammes bruts et lit ce qui revient.
class Peer
{
public:
	Peer() : fd(-1) {}

	~Peer()
	{
		if (fd >= 0)
			close(fd);
	}

	bool Open()
	{
		fd = socket(AF_INET6, SOCK_DGRAM, 0);
		if (fd < 0)
			return false;

		sockaddr_in6 addr = {};
		addr.sin6_family = AF_INET6;
		addr.sin6_addr	 = in6addr_loopback;
		if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
			return false;

		socklen_t len = sizeof(addr);
		if (getsockname(fd, (sockaddr*)&addr, &len) != 0)
			return false;
		port = ntohs(addr.sin6_port);
		return true;
	}

	WORD Port() const { return port; }

	bool SendTo(WORD serverPort, const std::vector<BYTE>& bytes)
	{
		sockaddr_in6 to = {};
		to.sin6_family	= AF_INET6;
		to.sin6_addr	= in6addr_loopback;
		to.sin6_port	= htons(serverPort);
		return sendto(fd, bytes.data(), bytes.size(), 0, (sockaddr*)&to, sizeof(to)) == (ssize_t)bytes.size();
	}

	bool SendTo(WORD serverPort, const BFCPMessage& msg, int version = BFCPMessage::VersionUnreliable)
	{
		BYTE buffer[4096];
		const size_t n = msg.Serialize(buffer, sizeof(buffer), version);
		if (n == BFCPMessage::Failed)
			return false;
		return SendTo(serverPort, std::vector<BYTE>(buffer, buffer + n));
	}

	// Rend le datagramme brut suivant, vide au bout du délai.
	std::vector<BYTE> ReceiveRaw(long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();

		while (ElapsedMs(t0) < deadlineMs) {
			pollfd pfd = { fd, POLLIN, 0 };
			const long left = deadlineMs - ElapsedMs(t0);
			if (poll(&pfd, 1, (int)(left > 50 ? 50 : left)) <= 0)
				continue;

			BYTE buffer[4096];
			const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
			if (n > 0)
				return std::vector<BYTE>(buffer, buffer + n);
		}
		return std::vector<BYTE>();
	}

	BFCPMessage* Receive(long deadlineMs = 3000)
	{
		const std::vector<BYTE> raw = ReceiveRaw(deadlineMs);
		if (raw.empty())
			return nullptr;
		return BFCPMessage::Parse(raw.data(), raw.size());
	}

	// Lit jusqu'à tomber sur cette primitive, ou rend nullptr.
	BFCPMessage* ReceiveOfType(BFCPMessage::Primitive wanted, long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();
		while (ElapsedMs(t0) < deadlineMs) {
			BFCPMessage* msg = Receive(deadlineMs - ElapsedMs(t0));
			if (! msg)
				return nullptr;
			if (msg->GetPrimitive() == wanted)
				return msg;
			delete msg;
		}
		return nullptr;
	}

	// Attend un FloorRequestStatus portant ce statut-la.
	BFCPMessage* ReceiveStatus(BFCPAttrRequestStatus::Status wanted, long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();
		while (ElapsedMs(t0) < deadlineMs) {
			BFCPMessage* msg = Receive(deadlineMs - ElapsedMs(t0));
			if (! msg)
				return nullptr;
			if (msg->GetPrimitive() == BFCPMessage::FloorRequestStatus) {
				const BFCPAttrFloorRequestInformation* info = ((BFCPMsgFloorRequestStatus*)msg)->GetFloorRequestInformation();
				if (info && info->GetOverallRequestStatus() && info->GetOverallRequestStatus()->GetRequestStatus()
				    && info->GetOverallRequestStatus()->GetRequestStatus()->GetStatus() == wanted)
					return msg;
			}
			delete msg;
		}
		return nullptr;
	}

	// Attend un message portant ce transaction id : c'est ainsi qu'on
	// reconnait la reemission d'un message deja recu.
	BFCPMessage* ReceiveTransaction(int transactionId, long deadlineMs = 3000)
	{
		const Clock::time_point t0 = Clock::now();
		while (ElapsedMs(t0) < deadlineMs) {
			BFCPMessage* msg = Receive(deadlineMs - ElapsedMs(t0));
			if (! msg)
				return nullptr;
			if (msg->GetTransactionId() == transactionId)
				return msg;
			delete msg;
		}
		return nullptr;
	}

	void Drain(long ms = 200)
	{
		const Clock::time_point t0 = Clock::now();
		while (ElapsedMs(t0) < ms) {
			pollfd pfd = { fd, POLLIN, 0 };
			if (poll(&pfd, 1, 10) <= 0)
				continue;
			BYTE buffer[4096];
			(void)recv(fd, buffer, sizeof(buffer), 0);
		}
	}

private:
	int	fd;
	WORD	port = 0;
};

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

class BfcpUdp : public ::testing::Test
{
protected:
	CountingListener			listener;
	std::unique_ptr<BFCPFloorControlServer>	server;
	std::unique_ptr<BFCPUdpEndpoint>	endpoint;
	Peer					peer;
	WORD					port = 0;

	void SetUp() override
	{
		if (! HasIPv6Loopback())
			GTEST_SKIP() << "pas de boucle locale IPv6 sur cette machine";

		server.reset(new BFCPFloorControlServer(CONF, &listener));
		ASSERT_TRUE(server->AddFloor(FLOOR));
		ASSERT_TRUE(server->AddUser(ALICE));

		endpoint.reset(new BFCPUdpEndpoint(server.get(), ALICE));
		port = endpoint->Open();
		ASSERT_NE(0, port);

		ASSERT_TRUE(peer.Open());
		ASSERT_TRUE(server->UserConnected(ALICE, endpoint.get()));
	}

	void TearDown() override
	{
		if (endpoint)
			endpoint->Close();
		if (server)
			server->End();
		endpoint.reset();
		server.reset();
	}

	// Un Hello du pair, et le HelloAck qui revient : c'est ce qui accroche
	// l'adresse source et fixe la version.
	bool Handshake(int version = BFCPMessage::VersionUnreliable)
	{
		if (! peer.SendTo(port, BFCPMsgHello(1, CONF, ALICE), version))
			return false;

		std::unique_ptr<BFCPMessage> ack(peer.ReceiveOfType(BFCPMessage::HelloAck));
		if (! ack)
			return false;

		// Le serveur salue en retour sur UDP, et c'est une transaction A LUI :
		// tant qu'elle n'est pas acquittee, elle est reemise. Un vrai pair
		// acquitte ; ne pas le faire ici ferait lire ces reemissions a tous
		// les tests qui guettent autre chose.
		std::unique_ptr<BFCPMessage> hello(peer.ReceiveOfType(BFCPMessage::Hello));
		if (! hello)
			return false;

		BFCPMessage helloAck(BFCPMessage::HelloAck, hello->GetTransactionId(), CONF, ALICE);
		helloAck.SetResponder(true);
		if (! peer.SendTo(port, helloAck, version))
			return false;

		return WaitFor([&]{ return endpoint->GetPendingCount() == 0; });
	}
};

} // namespace


// ---- L'accrochage de l'adresse ---------------------------------------------

TEST_F(BfcpUdp, LePremierDatagrammeAccrocheLaSource)
{
	EXPECT_FALSE(endpoint->HasRemote()) << "rien recu, nulle part ou repondre";

	ASSERT_TRUE(peer.SendTo(port, BFCPMsgHello(1, CONF, ALICE)));

	std::unique_ptr<BFCPMessage> ack(peer.ReceiveOfType(BFCPMessage::HelloAck));
	ASSERT_NE(nullptr, ack.get()) << "la reponse doit revenir a la source entendue";
	EXPECT_TRUE(endpoint->HasRemote());
	EXPECT_TRUE(WaitFor([&]{ return listener.connected.load() == 1; }));
}

TEST_F(BfcpUdp, UnDatagrammeDUneAutreSourceEstJete)
{
	ASSERT_TRUE(Handshake());
	peer.Drain();

	// Un tiers écrit sur notre port : il n'est pas notre pair.
	Peer intruder;
	ASSERT_TRUE(intruder.Open());
	BFCPMsgFloorRequest request(50, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(intruder.SendTo(port, request));

	std::unique_ptr<BFCPMessage> nothing(intruder.Receive(500));
	EXPECT_EQ(nullptr, nothing.get()) << "l'intrus ne doit rien recevoir";
	EXPECT_EQ(0, listener.requests.load()) << "et le chair ne doit rien apprendre";
}

TEST_F(BfcpUdp, LAdresseDuSdpEstRemplaceeParCelleEntendue)
{
	// Le cas du NAT : le SDP annonce une adresse, les datagrammes viennent
	// d'une autre. C'est celle qu'on a entendue qui est joignable.
	Peer elsewhere;
	ASSERT_TRUE(elsewhere.Open());
	endpoint->SetRemote(IPAddress::Parse("::1").To(elsewhere.Port()));
	ASSERT_TRUE(endpoint->HasRemote());

	ASSERT_TRUE(Handshake());

	// Le HelloAck est parti vers `peer`, pas vers l'adresse du SDP.
	std::unique_ptr<BFCPMessage> stray(elsewhere.Receive(300));
	EXPECT_EQ(nullptr, stray.get());
}


// ---- La version reflétée ----------------------------------------------------

TEST_F(BfcpUdp, UnPairEnVersion1RecoitDuVersion1)
{
	// Les endpoints servis par libbfcp parlent la version 1 sur UDP. Leur
	// répondre en version 2 serait leur parler une langue qu'ils jettent.
	ASSERT_TRUE(peer.SendTo(port, BFCPMsgHello(1, CONF, ALICE), BFCPMessage::VersionReliable));

	const std::vector<BYTE> raw = peer.ReceiveRaw();
	ASSERT_FALSE(raw.empty());
	EXPECT_EQ(BFCPMessage::VersionReliable, raw[0] >> 5) << "la version du pair, pas la notre";
}

TEST_F(BfcpUdp, UnPairEnVersion2RecoitDuVersion2AvecLeDrapeauReponse)
{
	ASSERT_TRUE(peer.SendTo(port, BFCPMsgHello(1, CONF, ALICE), BFCPMessage::VersionUnreliable));

	const std::vector<BYTE> raw = peer.ReceiveRaw();
	ASSERT_FALSE(raw.empty());
	EXPECT_EQ(BFCPMessage::VersionUnreliable, raw[0] >> 5);
	EXPECT_NE(0, raw[0] & 0x10) << "un HelloAck repond : le drapeau R est pose";
}

TEST_F(BfcpUdp, UnMessageFragmenteEstJete)
{
	ASSERT_TRUE(Handshake());
	peer.Drain();

	// On ne réassemble pas. Le drapeau F fait jeter, sans réponse.
	BYTE buffer[4096];
	BFCPMsgHello hello(60, CONF, ALICE);
	const size_t n = hello.Serialize(buffer, sizeof(buffer));
	ASSERT_NE(BFCPMessage::Failed, n);
	buffer[0] |= 0x08;

	ASSERT_TRUE(peer.SendTo(port, std::vector<BYTE>(buffer, buffer + n)));

	std::unique_ptr<BFCPMessage> nothing(peer.Receive(500));
	EXPECT_EQ(nullptr, nothing.get());
}

TEST_F(BfcpUdp, UnDatagrammeIllisibleNeTuePasLePointDAcces)
{
	// Contrairement à TCP, un datagramme cassé ne dit rien du suivant : il n'y
	// a pas de flux à perdre. Le point d'accès doit continuer à servir.
	ASSERT_TRUE(Handshake());
	peer.Drain();

	ASSERT_TRUE(peer.SendTo(port, std::vector<BYTE>{0xff, 0x01, 0x02}));

	ASSERT_TRUE(peer.SendTo(port, BFCPMsgHello(61, CONF, ALICE)));
	std::unique_ptr<BFCPMessage> ack(peer.ReceiveOfType(BFCPMessage::HelloAck));
	EXPECT_NE(nullptr, ack.get()) << "le point d'acces doit survivre a un datagramme cassé";
}


// ---- Le Hello du serveur ----------------------------------------------------

TEST_F(BfcpUdp, LeServeurSalueALaSuiteDUnHello)
{
	// libbfcp salue en retour après son HelloAck, et les endpoints en service
	// l'attendent. C'est une transaction DU SERVEUR : elle attend un
	// acquittement, et sans lui elle est réémise.
	ASSERT_TRUE(peer.SendTo(port, BFCPMsgHello(1, CONF, ALICE)));

	std::unique_ptr<BFCPMessage> ack(peer.ReceiveOfType(BFCPMessage::HelloAck));
	ASSERT_NE(nullptr, ack.get());

	std::unique_ptr<BFCPMessage> hello(peer.ReceiveOfType(BFCPMessage::Hello));
	ASSERT_NE(nullptr, hello.get());
	EXPECT_NE(0, hello->GetTransactionId()) << "sans id, le pair n'a rien a acquitter";
	EXPECT_EQ(1, endpoint->GetPendingCount()) << "elle attend son acquittement";

	BFCPMessage helloAck(BFCPMessage::HelloAck, hello->GetTransactionId(), CONF, ALICE);
	helloAck.SetResponder(true);
	ASSERT_TRUE(peer.SendTo(port, helloAck));
	EXPECT_TRUE(WaitFor([&]{ return endpoint->GetPendingCount() == 0; }));
}

TEST_F(BfcpUdp, LeServeurPeutSaluerLePremier)
{
	// StartSending : le SDP donne l'adresse du pair, et le serveur salue sans
	// avoir rien entendu. C'est ce que faisait SendHello dans libbfcp.
	endpoint->SetRemote(IPAddress::Parse("::1").To(peer.Port()));
	ASSERT_TRUE(endpoint->HasRemote());

	ASSERT_TRUE(endpoint->SendServerHello());

	std::unique_ptr<BFCPMessage> hello(peer.ReceiveOfType(BFCPMessage::Hello));
	ASSERT_NE(nullptr, hello.get());
	EXPECT_NE(0, hello->GetTransactionId());
	EXPECT_EQ(1, endpoint->GetPendingCount());
}


// ---- La fiabilité : réémission, acquittement, abandon ----------------------

TEST_F(BfcpUdp, UnGrantedSansAcquittementEstReemisA500msPuis1s)
{
	// LE test de la RFC 8855 §6.2 : le premier délai est 500 ms, le suivant
	// le double.
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, request));
	ASSERT_TRUE(WaitFor([&]{ return listener.requests.load() == 1; }));

	const Clock::time_point t0 = Clock::now();
	ASSERT_TRUE(server->GrantFloorRequest(listener.lastFloorRequestId.load()));

	// L'octroi envoie Accepted PUIS Granted : deux transactions distinctes,
	// pas un envoi et sa reemission. On suit celle du Granted.
	std::unique_ptr<BFCPMessage> granted(peer.ReceiveStatus(BFCPAttrRequestStatus::Granted));
	ASSERT_NE(nullptr, granted.get());
	const int transactionId = granted->GetTransactionId();
	const long firstMs = ElapsedMs(t0);
	EXPECT_LT(firstMs, 300) << "le premier envoi est immediat";
	EXPECT_NE(0, transactionId) << "une notification UDP porte un id, pour etre acquittee";

	// Première réémission du MEME message, autour de 500 ms après son envoi.
	std::unique_ptr<BFCPMessage> resent(peer.ReceiveTransaction(transactionId, 2000));
	ASSERT_NE(nullptr, resent.get()) << "sans acquittement, il doit revenir";
	const long secondMs = ElapsedMs(t0);
	EXPECT_GE(secondMs - firstMs, 400) << "pas avant 500 ms";
	EXPECT_LT(secondMs - firstMs, 1200);

	// Deuxième réémission, au double : autour de 1 s après la première.
	std::unique_ptr<BFCPMessage> again(peer.ReceiveTransaction(transactionId, 3000));
	ASSERT_NE(nullptr, again.get());
	const long thirdMs = ElapsedMs(t0);
	EXPECT_GE(thirdMs - secondMs, 800) << "l'intervalle double";
}

TEST_F(BfcpUdp, UnGrantedAcquitteNEstPlusReemis)
{
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, request));
	ASSERT_TRUE(WaitFor([&]{ return listener.requests.load() == 1; }));

	ASSERT_TRUE(server->GrantFloorRequest(listener.lastFloorRequestId.load()));

	// Accepted et Granted sont deux transactions : le pair acquitte les deux.
	std::unique_ptr<BFCPMessage> accepted(peer.ReceiveStatus(BFCPAttrRequestStatus::Accepted));
	ASSERT_NE(nullptr, accepted.get());
	std::unique_ptr<BFCPMessage> granted(peer.ReceiveStatus(BFCPAttrRequestStatus::Granted));
	ASSERT_NE(nullptr, granted.get());
	EXPECT_NE(accepted->GetTransactionId(), granted->GetTransactionId())
		<< "deux transactions distinctes, deux identifiants";

	for (BFCPMessage* msg : { accepted.get(), granted.get() }) {
		BFCPMessage ack(BFCPMessage::FloorRequestStatusAck, msg->GetTransactionId(), CONF, ALICE);
		ack.SetResponder(true);
		ASSERT_TRUE(peer.SendTo(port, ack));
	}

	EXPECT_TRUE(WaitFor([&]{ return endpoint->GetPendingCount() == 0; }))
		<< "un acquittement doit fermer la transaction";

	std::unique_ptr<BFCPMessage> nothing(peer.ReceiveOfType(BFCPMessage::FloorRequestStatus, 1200));
	EXPECT_EQ(nullptr, nothing.get()) << "plus rien ne doit etre reemis";
}

TEST_F(BfcpUdp, PendingEtAcceptedNeSontPasReemis)
{
	// libbfcp ne les réémet pas, et un Pending en retard dirait moins que le
	// Granted qui le suit.
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, request));

	std::unique_ptr<BFCPMessage> pending(peer.ReceiveOfType(BFCPMessage::FloorRequestStatus));
	ASSERT_NE(nullptr, pending.get());
	EXPECT_EQ(0, endpoint->GetPendingCount()) << "une reponse ne se reemet pas";
}

TEST_F(BfcpUdp, UnPairQuiNAcquitteJamaisPerdSaParole)
{
	// L'abandon au-delà de 16 s. Le test l'attend vraiment : c'est le seul
	// moyen de prouver que le floor revient plutot que de rester bloque.
	ASSERT_TRUE(Handshake());
	peer.Drain();
	listener.grantFrom = server.get();

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, request));
	ASSERT_TRUE(WaitFor([&]{ return listener.granted.load() == 1; }));
	ASSERT_NE(0, server->GetGrantedFloorRequestId(FLOOR));

	// Le pair se tait. Passé 16 s, le serveur doit reprendre la parole.
	EXPECT_TRUE(WaitFor([&]{ return listener.released.load() >= 1; }, 25000))
		<< "un pair qui n'acquitte plus doit rendre son floor";
	EXPECT_EQ(0, server->GetGrantedFloorRequestId(FLOOR));
}


// ---- Le cache des réponses --------------------------------------------------

TEST_F(BfcpUdp, UneRequeteRepeteeRecoitLaMemeReponseOctetPourOctet)
{
	// Un datagramme perdu fait répéter la requête. La servir deux fois
	// creerait une seconde demande de parole : c'est la reponse gardee qu'il
	// faut rejouer.
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMsgFloorRequest request(99, CONF, ALICE);
	request.AddFloorId(FLOOR);

	ASSERT_TRUE(peer.SendTo(port, request));
	const std::vector<BYTE> first = peer.ReceiveRaw();
	ASSERT_FALSE(first.empty());
	ASSERT_TRUE(WaitFor([&]{ return listener.requests.load() == 1; }));

	// La même requête, même transaction id.
	ASSERT_TRUE(peer.SendTo(port, request));
	const std::vector<BYTE> second = peer.ReceiveRaw();
	ASSERT_FALSE(second.empty());

	EXPECT_EQ(first, second) << "la meme reponse, octet pour octet";
	EXPECT_EQ(1, listener.requests.load()) << "et une seule demande vue par le chair";
}

TEST_F(BfcpUdp, UneRequeteAvecUnAutreTransactionIdEstServieANouveau)
{
	// Le garde-fou du test precedent : le cache ne doit pas avaler une vraie
	// seconde requete.
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMsgFloorRequest first(101, CONF, ALICE);
	first.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, first));
	ASSERT_TRUE(WaitFor([&]{ return listener.requests.load() == 1; }));

	BFCPMsgFloorRequest second(102, CONF, ALICE);
	second.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, second));
	EXPECT_TRUE(WaitFor([&]{ return listener.requests.load() == 2; }))
		<< "un transaction id different est une requete differente";
}


// ---- Le cycle complet -------------------------------------------------------

TEST_F(BfcpUdp, UneParoleVaEtRevientSurUdp)
{
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMsgFloorRequest request(20, CONF, ALICE);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, request));

	std::unique_ptr<BFCPMessage> pending(peer.ReceiveOfType(BFCPMessage::FloorRequestStatus));
	ASSERT_NE(nullptr, pending.get());
	EXPECT_EQ(20, pending->GetTransactionId());
	EXPECT_TRUE(pending->IsResponder()) << "une reponse porte le drapeau R";
	EXPECT_EQ(BFCPAttrRequestStatus::Pending,
		((BFCPMsgFloorRequestStatus*)pending.get())->GetFloorRequestInformation()->GetOverallRequestStatus()->GetRequestStatus()->GetStatus());

	ASSERT_TRUE(WaitFor([&]{ return listener.requests.load() == 1; }));
	ASSERT_TRUE(server->GrantFloorRequest(listener.lastFloorRequestId.load()));

	// Accepted puis Granted. Ce sont des transactions du serveur : le drapeau
	// R n'y est pas, et un transaction id non nul les identifie.
	std::unique_ptr<BFCPMessage> accepted(peer.ReceiveOfType(BFCPMessage::FloorRequestStatus));
	ASSERT_NE(nullptr, accepted.get());
	EXPECT_FALSE(accepted->IsResponder());
	EXPECT_NE(0, accepted->GetTransactionId()) << "sur UDP une notification porte un id, pour etre acquittee";

	EXPECT_EQ(listener.lastFloorRequestId.load(), server->GetGrantedFloorRequestId(FLOOR));
}

TEST_F(BfcpUdp, UnGoodbyeDuPairEstAcquitte)
{
	ASSERT_TRUE(Handshake());
	peer.Drain();

	BFCPMessage bye(BFCPMessage::Goodbye, 30, CONF, ALICE);
	ASSERT_TRUE(peer.SendTo(port, bye));

	std::unique_ptr<BFCPMessage> ack(peer.ReceiveOfType(BFCPMessage::GoodbyeAck));
	ASSERT_NE(nullptr, ack.get());
	EXPECT_EQ(30, ack->GetTransactionId());
	EXPECT_FALSE(server->IsUserConnected(ALICE));
}

TEST_F(BfcpUdp, UnMessageDUnAutreUtilisateurEstJete)
{
	// Une socket par participant : un message qui nomme quelqu'un d'autre
	// n'est pas arrivé au bon endroit.
	ASSERT_TRUE(Handshake());
	peer.Drain();

	ASSERT_TRUE(server->AddUser(20));
	BFCPMsgFloorRequest request(40, CONF, 20);
	request.AddFloorId(FLOOR);
	ASSERT_TRUE(peer.SendTo(port, request));

	std::unique_ptr<BFCPMessage> nothing(peer.Receive(500));
	EXPECT_EQ(nullptr, nothing.get());
	EXPECT_EQ(0, listener.requests.load());
}
