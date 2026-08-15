/**
 * test_stunclient.cpp — découverte d'adresse publique et verdict NAT 1:1.
 *
 * `--nat auto` interroge un serveur STUN pour découvrir l'adresse vue de
 * l'extérieur, et surtout pour vérifier que le NAT **conserve les ports** : le
 * mediaserver annonce des ports RTP, et un NAT qui les translate rend faux tout
 * ce qu'il publie.
 *
 * Ces tests montent un VRAI serveur STUN local, dans un thread. C'est la seule
 * façon honnête de tester un client réseau : une sonde qui n'aurait jamais vu
 * un paquet ne prouverait rien du décodage, ni du XOR, ni du verdict. Le faux
 * serveur sait aussi MENTIR sur le port — c'est ainsi qu'on vérifie que le
 * refus fonctionne, cas qu'aucun réseau de test ne produirait sur commande.
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "stunclient.h"
#include "medkit/stunmessage.h"

namespace {

// Serveur STUN minimal : répond à toute Binding Request par une Binding
// Response portant XOR-MAPPED-ADDRESS. `portOffset` décale le port annoncé —
// c'est ainsi qu'on simule un NAT qui translate les ports.
class FakeStunServer
{
public:
	explicit FakeStunServer(int portOffset = 0) : offset(portOffset) {}

	bool Start()
	{
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return false;

		sockaddr_in addr = {};
		addr.sin_family      = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0)
			return false;

		socklen_t len = sizeof(addr);
		if (getsockname(fd, (sockaddr*)&addr, &len) != 0)
			return false;

		port    = ntohs(addr.sin_port);
		running = true;
		worker  = std::thread(&FakeStunServer::Run, this);
		return true;
	}

	~FakeStunServer()
	{
		running = false;
		if (fd >= 0)
			shutdown(fd, SHUT_RDWR);
		if (worker.joinable())
			worker.join();
		if (fd >= 0)
			close(fd);
	}

	WORD Port() const { return port; }
	int  Served() const { return served.load(); }

private:
	void Run()
	{
		while (running)
		{
			pollfd pfd;
			pfd.fd      = fd;
			pfd.events  = POLLIN;
			pfd.revents = 0;

			if (poll(&pfd, 1, 50) <= 0)
				continue;

			BYTE        buffer[1500];
			sockaddr_in from = {};
			socklen_t   len  = sizeof(from);

			const ssize_t size = recvfrom(fd, buffer, sizeof(buffer), 0, (sockaddr*)&from, &len);
			if (size <= 0)
				continue;

			STUNMessage* req = STUNMessage::Parse(buffer, size);
			if (!req)
				continue;

			//La réponse porte l'adresse SOURCE vue par le serveur — décalée du
			//port si l'on simule une traduction.
			sockaddr_in mapped = from;
			mapped.sin_port = htons((WORD)(ntohs(from.sin_port) + offset));

			STUNMessage* resp = req->CreateResponse();
			resp->AddXorAddressAttribute(&mapped);

			BYTE  out[1500];
			DWORD outLen = resp->NonAuthenticatedFingerPrint(out, sizeof(out));

			//Compté AVANT l'envoi : sinon le client peut recevoir sa réponse,
			//terminer, et lire le compteur avant que ce thread ne l'incrémente.
			//La course était dans le test, pas dans le client — et elle ne se
			//voyait qu'en suite complète, jamais en test isolé.
			++served;
			sendto(fd, out, outLen, 0, (sockaddr*)&from, len);
			delete resp;
			delete req;
		}
	}

	int              fd      = -1;
	WORD             port    = 0;
	int              offset  = 0;
	std::atomic<bool> running{false};
	std::atomic<int>  served{0};
	std::thread      worker;
};

const IPAddress kLoopback = IPAddress::Parse("127.0.0.1");

} // namespace


/* =========================================================================
 * §1 — ANALYSE DE L'ADRESSE DU SERVEUR.
 * ========================================================================= */

TEST(StunClientServer, AccepteHoteEtPortEtPoseLeDefaut3478)
{
	IPEndpoint  server;
	std::string error;

	ASSERT_TRUE(StunClient::ParseServer("127.0.0.1:19302", server, error)) << error;
	EXPECT_TRUE(server.Address() == kLoopback);
	EXPECT_EQ(19302, server.Port());

	ASSERT_TRUE(StunClient::ParseServer("127.0.0.1", server, error)) << error;
	EXPECT_EQ(3478, server.Port()) << "RFC 5389 §9 : port STUN par defaut";
}

// Les crochets appartiennent à la syntaxe des URL, et c'est ICI qu'ils sont
// légitimes : sans eux, « adresse:port » serait indécoupable en v6.
TEST(StunClientServer, UnLitteralV6ExigeDesCrochets)
{
	IPEndpoint  server;
	std::string error;

	//Sans crochets, « ::1 » ne peut pas être distingué d'un couple adresse:port.
	EXPECT_FALSE(StunClient::ParseServer("::1", server, error));

	EXPECT_FALSE(StunClient::ParseServer("[2001:db8::1", server, error))
		<< "crochet fermant manquant";
	EXPECT_NE(std::string::npos, error.find("crochet")) << error;
}

TEST(StunClientServer, RefuseCeQuiNeSeResoutPas)
{
	IPEndpoint  server;
	std::string error;

	EXPECT_FALSE(StunClient::ParseServer("", server, error));
	EXPECT_FALSE(StunClient::ParseServer(NULL, server, error));
	EXPECT_FALSE(StunClient::ParseServer("nexiste-pas.invalid", server, error));
	EXPECT_FALSE(StunClient::ParseServer("127.0.0.1:0", server, error)) << "port nul";
}


/* =========================================================================
 * §2 — SONDE ET VERDICT, contre un vrai serveur.
 * ========================================================================= */

TEST(StunClientProbe, LaSondeRendLAdresseEtLePortVusDuServeur)
{
	FakeStunServer server;
	if (!server.Start())
		GTEST_SKIP() << "impossible de monter le serveur STUN de test";

	StunClient::Mapping mapping;
	std::string         error;

	ASSERT_TRUE(StunClient::Probe(kLoopback, kLoopback.To(server.Port()), mapping, error)) << error;

	EXPECT_TRUE(mapping.address == kLoopback) << mapping.address.ToString();
	EXPECT_NE(0, mapping.localPort);
	EXPECT_EQ(mapping.localPort, mapping.port)
		<< "en loopback il n'y a pas de traduction : le port revient tel quel";
}

// Le cas nominal de --nat auto : adresse stable, ports conservés => 1:1.
TEST(StunClientDiscover, DeuxSondesConcordantesDonnentUnVerdictUnAUn)
{
	FakeStunServer server;
	if (!server.Start())
		GTEST_SKIP() << "impossible de monter le serveur STUN de test";

	IPAddress   discovered;
	bool        oneToOne = false;
	std::string error;

	ASSERT_TRUE(StunClient::Discover(kLoopback, kLoopback.To(server.Port()),
	                                 discovered, oneToOne, error)) << error;

	EXPECT_TRUE(discovered == kLoopback);
	EXPECT_TRUE(oneToOne) << error;
	EXPECT_EQ(2, server.Served()) << "deux sondes, depuis deux ports locaux differents";
}

// ADVERSE, LE TEST QUI JUSTIFIE LA FONCTION — un NAT qui translate les ports.
// L'adresse est parfaitement découvrable, et pourtant la configuration est
// inutilisable : les ports RTP annoncés ne seraient pas ceux que le pair doit
// joindre. Le verdict doit être NON, et l'appelant doit refuser de démarrer.
TEST(StunClientDiscover, UnNatQuiTranslateLesPortsEstDetecte)
{
	FakeStunServer server(/*portOffset=*/1000);
	if (!server.Start())
		GTEST_SKIP() << "impossible de monter le serveur STUN de test";

	IPAddress   discovered;
	bool        oneToOne = true;
	std::string error;

	//Discover REUSSIT : l'adresse est connue. C'est le VERDICT qui est negatif —
	//les deux sorties sont distinctes, et les confondre ferait passer un echec
	//reseau pour un NAT symetrique, ou l'inverse.
	ASSERT_TRUE(StunClient::Discover(kLoopback, kLoopback.To(server.Port()),
	                                 discovered, oneToOne, error)) << error;

	EXPECT_TRUE(discovered == kLoopback);
	EXPECT_FALSE(oneToOne);
	EXPECT_NE(std::string::npos, error.find("translate les ports")) << error;
}

TEST(StunClientProbe, UnServeurMuetEstUnEchecEtPasUnVerdict)
{
	//Port sur lequel personne n'ecoute : la sonde retransmet puis abandonne.
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_GE(fd, 0);
	sockaddr_in addr = {};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ASSERT_EQ(0, bind(fd, (sockaddr*)&addr, sizeof(addr)));
	socklen_t len = sizeof(addr);
	ASSERT_EQ(0, getsockname(fd, (sockaddr*)&addr, &len));
	const WORD deadPort = ntohs(addr.sin_port);
	close(fd);

	StunClient::Mapping mapping;
	std::string         error;

	EXPECT_FALSE(StunClient::Probe(kLoopback, kLoopback.To(deadPort), mapping, error));
	EXPECT_FALSE(error.empty());
}
