/**
 * test_bfcp_dualstack.cpp — BFCP (sous-module libbfcp) : adresse d'écoute.
 *
 * Étape 1 du chantier IPv6 (ipv6.md §5.4, §6) : libbfcp est le seul composant
 * déjà écrit en `sockaddr_storage`, seul son DÉFAUT était IPv4. C'est donc le
 * premier endroit où le motif dual-stack se vérifie sur du code de production.
 *
 * Ces tests sont ACTIFS (joués par `make check`) : contrairement à
 * `test_ipv6.cpp`, ils décrivent un comportement qui doit être vrai maintenant.
 *
 * Ils vérifient le NOYAU, pas nos intentions : chaque cas crée une vraie socket
 * par l'API publique de la bibliothèque, puis lit ce à quoi elle est liée avec
 * `getsockname`. C'est la seule façon de distinguer « la bibliothèque dit
 * qu'elle écoute là » de « elle y écoute vraiment » — la distinction qui a fait
 * tomber le bug de conversion d'adresse décrit plus bas.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "BFCPconnection.h"
#include "ipaddress.h"

namespace {

// BFCPConnection est abstraite (les deux notifications de connexion sont
// virtuelles pures) et `AddClient` est protégée : une sous-classe minimale est
// la façon normale de l'exercer. Aucun test ici n'ouvre de connexion BFCP, donc
// les notifications ne sont jamais appelées.
class OpenConnection : public BFCPConnection
{
public:
	using BFCPConnection::AddClient;

	int  ProcessBFCPmessage(bfcp_received_message*, BFCP_SOCKET) override { return 0; }
	bool OnBFCPConnected(BFCP_SOCKET, const char*, int) override         { return true; }
	bool OnBFCPDisconnected(BFCP_SOCKET) override                        { return true; }
};

bool HasIPv6Loopback()
{
	const int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;

	sockaddr_in6 addr = {};
	addr.sin6_family = AF_INET6;
	addr.sin6_addr   = in6addr_loopback;
	const bool ok = (bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
	close(fd);
	return ok;
}

// À quoi la socket est-elle RÉELLEMENT liée ?
IPEndpoint BoundTo(int fd)
{
	IPEndpoint out;
	if (getsockname(fd, out.Data(), out.LenPtr()) != 0)
		return IPEndpoint();
	return out;
}

} // namespace


// Le défaut de `Client2ServerInfo::Init` : « :: », toutes interfaces, AF_INET6.
// C'était `0.0.0.0`/AF_INET, et comme c'est cette famille qui décide de celle de
// la socket (`CreateSocket`), le serveur BFCP était injoignable en IPv6 quoi
// qu'on configure par ailleurs.
TEST(BfcpDualStack, LAdresseLocaleParDefautEstLaNonSpecifieeV6)
{
	OpenConnection conn;
	EXPECT_STREQ("::", conn.getLocalAdress());
}

// ADVERSE — la conversion texte -> sockaddr écrivait les octets de l'adresse à
// l'OFFSET 0 de la sockaddr, donc par-dessus `sa_family` et `sin_port`, que les
// deux lignes suivantes réécrivaient aussitôt : l'adresse était perdue et
// `sin_addr` restait à zéro. Toute adresse donnée sous forme de chaîne valait
// donc 0.0.0.0 — sans effet visible sur un bind local (INADDR_ANY fonctionne),
// mais fatal pour une destination UDP.
//
// Aucun accesseur ne rendait ce défaut visible (`getLocalAdress` renvoie la
// chaîne d'entrée, pas l'état) : il faut aller lire la socket.
TEST(BfcpDualStack, UneAdresseV4DonneeEnTexteEstReellementLiee)
{
	OpenConnection conn;

	char             local[] = "127.0.0.1";
	const BFCP_SOCKET fd = conn.AddClient(BFCP_OVER_UDP, BFCPConnectionRole::PASSIVE, local, 0);
	ASSERT_NE(INVALID_SOCKET, fd);

	const IPEndpoint bound = BoundTo(fd);
	ASSERT_TRUE(bound.IsSet());
	EXPECT_TRUE(bound.Address() == IPAddress::Parse("127.0.0.1"))
		<< "socket liee a " << bound.ToString()
		<< " : l'adresse demandee a ete perdue a la conversion";
}

TEST(BfcpDualStack, UneAdresseV6DonneeEnTexteEstReellementLiee)
{
	if (!HasIPv6Loopback())
		GTEST_SKIP() << "pas de loopback IPv6 dans cet environnement";

	OpenConnection conn;

	char             local[] = "::1";
	const BFCP_SOCKET fd = conn.AddClient(BFCP_OVER_UDP, BFCPConnectionRole::PASSIVE, local, 0);
	ASSERT_NE(INVALID_SOCKET, fd);

	const IPEndpoint bound = BoundTo(fd);
	ASSERT_TRUE(bound.IsSet());
	EXPECT_EQ(AF_INET6, bound.Sockaddr()->sa_family);
	EXPECT_TRUE(bound.Address() == IPAddress::Parse("::1")) << bound.ToString();
}

// Sans adresse explicite, la socket doit écouter les DEUX familles : c'est tout
// l'intérêt d'un défaut v6 (IPV6_V6ONLY=0 posé avant le bind). Sans cela, la
// bascule du défaut aurait fait perdre les clients IPv4.
TEST(BfcpDualStack, SansAdresseExpliciteLaSocketEntendLesDeuxFamilles)
{
	if (!HasIPv6Loopback())
		GTEST_SKIP() << "pas de loopback IPv6 dans cet environnement";

	OpenConnection conn;

	const BFCP_SOCKET fd = conn.AddClient(BFCP_OVER_UDP, BFCPConnectionRole::PASSIVE, NULL, 0);
	ASSERT_NE(INVALID_SOCKET, fd);

	const IPEndpoint bound = BoundTo(fd);
	ASSERT_TRUE(bound.IsSet());
	EXPECT_EQ(AF_INET6, bound.Sockaddr()->sa_family);
	EXPECT_TRUE(bound.Address().IsUnspecified()) << bound.ToString();

	int       v6only = -1;
	socklen_t len    = sizeof(v6only);
	ASSERT_EQ(0, getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &len));
	EXPECT_EQ(0, v6only) << "V6ONLY=1 : les clients BFCP en IPv4 seraient perdus";
}

// La destination, elle, n'est pas liée à une socket : c'est le chemin où le bug
// de conversion était fatal (sendto vers 0.0.0.0). `getRemoteAdress` renvoyant
// la chaîne d'entrée, on vérifie ce qui est vérifiable ici — que l'API accepte
// les deux familles — et le reste l'est par la socket ci-dessus.
TEST(BfcpDualStack, LEndpointDistantAccepteLesDeuxFamilles)
{
	OpenConnection conn;

	ASSERT_TRUE(conn.setRemoteEndpoint("192.0.2.1", 3238));
	EXPECT_STREQ("192.0.2.1", conn.getRemoteAdress());

	ASSERT_TRUE(conn.setRemoteEndpoint("2001:db8::1", 3238));
	EXPECT_STREQ("2001:db8::1", conn.getRemoteAdress());

	EXPECT_FALSE(conn.setRemoteEndpoint("pas une adresse", 3238));
	EXPECT_FALSE(conn.setRemoteEndpoint("192.0.2.1", 0)) << "port nul : refus";
}
