/**
 * test_ipaddress.cpp — IPAddress / IPEndpoint (mcu/include/ipaddress.h).
 *
 * CES TESTS SONT ACTIFS : ils sont joués par `make check`, contrairement à
 * `test_ipv6.cpp` qui décrit la CIBLE du chantier (suites `IPv6*`, tests
 * `DISABLED_`). La brique d'adresse, elle, doit être verte dès maintenant —
 * c'est la fondation sur laquelle RTPSession sera migré.
 *
 * Ils sont ADVERSES : ce qui est vérifié en priorité, ce sont les formes que le
 * code historique acceptait en silence (`inet_addr` : 192.168.1, 0300.0250.0.1,
 * et surtout INADDR_NONE rendu comme une adresse valide), les pièges propres à
 * IPv6 (compression, casse, zone, v4-mapped) et les allers-retours avec l'API
 * socket — y compris un aller-retour RÉEL par le noyau (§7).
 */
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <set>
#include <string>

#include "ipaddress.h"

namespace {

// Le loopback IPv6 est-il utilisable ici ? (conteneur sans v6 -> SKIP)
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

} // namespace


/* =========================================================================
 * §1 — PARSE : ce qui est accepté.
 * ========================================================================= */

TEST(IPAddressParse, AccepteUneAdresseV4)
{
	int err = -1;
	const IPAddress a = IPAddress::Parse("192.0.2.1", err);

	EXPECT_EQ(0, err);
	ASSERT_TRUE(a.IsSet());
	EXPECT_TRUE(a.IsV4());
	EXPECT_EQ("192.0.2.1", a.ToString());
}

// Les trois écritures de la MÊME adresse : compressée, complète, casse haute.
// RFC 4291 §2.2 — la représentation hexadécimale est insensible à la casse.
TEST(IPAddressParse, LesTroisEcrituresDUneMemeAdresseSontEgales)
{
	const IPAddress court = IPAddress::Parse("2001:db8::1");
	const IPAddress longg = IPAddress::Parse("2001:0db8:0000:0000:0000:0000:0000:0001");
	const IPAddress haut  = IPAddress::Parse("2001:DB8::1");

	ASSERT_TRUE(court.IsSet());
	ASSERT_TRUE(longg.IsSet());
	ASSERT_TRUE(haut.IsSet());

	EXPECT_TRUE(court == longg);
	EXPECT_TRUE(court == haut);
}

// L'adresse non spécifiée est valide — c'est la convention « latche-moi ».
TEST(IPAddressParse, AccepteLAdresseNonSpecifiee)
{
	const IPAddress v6 = IPAddress::Parse("::");
	ASSERT_TRUE(v6.IsSet());
	EXPECT_TRUE(v6.IsUnspecified());
	EXPECT_FALSE(v6.IsUnicastDestination());

	const IPAddress v4 = IPAddress::Parse("0.0.0.0");
	ASSERT_TRUE(v4.IsSet());
	EXPECT_TRUE(v4.IsUnspecified());
}

// ADVERSE — « 2001:db8::1:5000 » est une adresse ENTIÈRE, pas « 2001:db8::1
// port 5000 ». Aucun découpage adresse:port n'est permis en v6.
TEST(IPAddressParse, UneAdresseQuiRessembleAUnCoupleAdressePortResteUneAdresse)
{
	const IPAddress a = IPAddress::Parse("2001:db8::1:5000");
	ASSERT_TRUE(a.IsSet());
	EXPECT_TRUE(a.IsV6());
	EXPECT_FALSE(a == IPAddress::Parse("2001:db8::1"));
}

TEST(IPAddressParse, AccepteUneZoneParNomEtParIndex)
{
	const IPAddress parNom = IPAddress::Parse("fe80::1%lo");
	ASSERT_TRUE(parNom.IsSet());
	EXPECT_NE(0u, parNom.Scope());
	EXPECT_FALSE(parNom.NeedsScope());

	const unsigned int lo = if_nametoindex("lo");
	if (lo)
	{
		char text[64];
		snprintf(text, sizeof(text), "fe80::1%%%u", lo);
		const IPAddress parIndex = IPAddress::Parse(text);
		ASSERT_TRUE(parIndex.IsSet());
		EXPECT_EQ(lo, parIndex.Scope());
	}
}

// La surcharge sans `err` : l'échec se lit sur IsSet().
TEST(IPAddressParse, LaSurchargeSansErreurDitLEchecParIsSet)
{
	EXPECT_TRUE(IPAddress::Parse("192.0.2.1").IsSet());
	EXPECT_FALSE(IPAddress::Parse("pas une adresse").IsSet());
}

TEST(IPAddressParse, AccepteUneStdString)
{
	const std::string text = "2001:db8::1";
	int err = -1;
	EXPECT_TRUE(IPAddress::Parse(text, err).IsSet());
	EXPECT_EQ(0, err);
	EXPECT_TRUE(IPAddress::Parse(text).IsSet());
}


/* =========================================================================
 * §2 — PARSE : ce qui est REFUSÉ. C'est la moitié qui compte : `inet_addr`
 *      rendait INADDR_NONE — soit 255.255.255.255, une adresse de BROADCAST —
 *      sur toutes ces entrées, et ce retour n'était pas testé.
 * ========================================================================= */

TEST(IPAddressParse, RefuseLeVideEtLePointeurNul)
{
	int err = 0;
	EXPECT_FALSE(IPAddress::Parse((const char*)NULL, err).IsSet());
	EXPECT_EQ(EINVAL, err);

	err = 0;
	EXPECT_FALSE(IPAddress::Parse("", err).IsSet());
	EXPECT_EQ(EINVAL, err);
}

TEST(IPAddressParse, RefuseLesFormesLaxistesDeInetAddr)
{
	//Formes courtes et octales : acceptées par inet_addr, refusées par inet_pton.
	EXPECT_FALSE(IPAddress::Parse("192.168.1").IsSet());
	EXPECT_FALSE(IPAddress::Parse("0300.0250.0.1").IsSet());
	EXPECT_FALSE(IPAddress::Parse("1.2.3.4.5").IsSet());
	EXPECT_FALSE(IPAddress::Parse("256.0.0.1").IsSet());
}

TEST(IPAddressParse, RefuseLesNotationsV6Invalides)
{
	EXPECT_FALSE(IPAddress::Parse("2001::db8::1").IsSet())    << "double compression";
	EXPECT_FALSE(IPAddress::Parse("1:2:3:4:5:6:7:8:9").IsSet()) << "neuf groupes";
	EXPECT_FALSE(IPAddress::Parse("2001:db8::12345").IsSet())  << "groupe de 5 chiffres";
	EXPECT_FALSE(IPAddress::Parse("::ffff:1.2.3.4.5").IsSet()) << "v4-mapped malformée";
}

// ADVERSE — les crochets appartiennent à la syntaxe des URL (RFC 3986 §3.2.2).
TEST(IPAddressParse, RefuseUnLitteralEntreCrochets)
{
	EXPECT_FALSE(IPAddress::Parse("[2001:db8::1]").IsSet());
	EXPECT_FALSE(IPAddress::Parse("[2001:db8::1").IsSet());
}

TEST(IPAddressParse, RefuseLesEspacesDeTeteEtDeQueue)
{
	EXPECT_FALSE(IPAddress::Parse(" 2001:db8::1").IsSet());
	EXPECT_FALSE(IPAddress::Parse("2001:db8::1 ").IsSet());
	EXPECT_FALSE(IPAddress::Parse(" 192.0.2.1").IsSet());
	EXPECT_FALSE(IPAddress::Parse("192.0.2.1\n").IsSet());
}

// RFC 4291 §2.5.5.1 : la forme « IPv4-compatible » est dépréciée. On la refuse
// sciemment — mais `::` et `::1`, qui en relèvent syntaxiquement, restent valides.
TEST(IPAddressParse, RefuseLaFormeIPv4CompatibleDepreciee)
{
	EXPECT_FALSE(IPAddress::Parse("::192.0.2.1").IsSet());
	EXPECT_FALSE(IPAddress::Parse("::1.2.3.4").IsSet());

	EXPECT_TRUE(IPAddress::Parse("::").IsSet())  << "non spécifiée : à garder";
	EXPECT_TRUE(IPAddress::Parse("::1").IsSet()) << "loopback : à garder";
}

// Une zone inexploitable est un refus, pas un « on verra bien » : émettre par
// une interface arbitraire est pire que ne pas émettre.
TEST(IPAddressParse, RefuseUneZoneInexploitable)
{
	EXPECT_FALSE(IPAddress::Parse("fe80::1%").IsSet());
	EXPECT_FALSE(IPAddress::Parse("fe80::1%interface-qui-nexiste-pas").IsSet());
}


/* =========================================================================
 * §3 — V4-MAPPED (invariant 3). Le piège n°1 du dual-stack.
 * ========================================================================= */

// `::ffff:a.b.c.d` est dé-mappée DÈS L'ENTRÉE : le reste du programme ne voit
// jamais la forme mappée, donc aucune comparaison ne peut se tromper de forme.
TEST(IPAddressMapped, LeParseDemappeVersLaFormeV4)
{
	const IPAddress a = IPAddress::Parse("::ffff:192.168.255.254");

	ASSERT_TRUE(a.IsSet());
	EXPECT_TRUE(a.IsV4());
	EXPECT_FALSE(a.IsV4Mapped());
	EXPECT_EQ("192.168.255.254", a.ToString());
}

TEST(IPAddressMapped, UneAdresseMappeeEstEgaleASonEquivalentV4)
{
	EXPECT_TRUE(IPAddress::Parse("::ffff:192.168.255.254") == IPAddress::Parse("192.168.255.254"));
	EXPECT_TRUE(IPAddress::Parse("192.0.2.1").MappedToV6() == IPAddress::Parse("192.0.2.1"));
}

// ADVERSE — la classification traverse le mapping : sinon la même adresse
// change de politique de rattrapage NAT selon la famille de la socket.
TEST(IPAddressMapped, LaClassificationTraverseLeMapping)
{
	const IPAddress mappee = IPAddress::Parse("192.168.0.1").MappedToV6();

	ASSERT_TRUE(mappee.IsV4Mapped());
	EXPECT_TRUE(mappee.IsPrivate())   << "192.168/16 reste privée à travers ::ffff:";
	EXPECT_TRUE(IPAddress::Parse("127.0.0.1").MappedToV6().IsLoopback());
	EXPECT_TRUE(IPAddress::Parse("224.0.0.1").MappedToV6().IsMulticast());
}

TEST(IPAddressMapped, MappedToV6EstIdempotentSurUneV6)
{
	const IPAddress v6 = IPAddress::Parse("2001:db8::1");
	EXPECT_TRUE(v6.MappedToV6() == v6);
	EXPECT_TRUE(v6.Unmapped() == v6);
}


/* =========================================================================
 * §4 — CLASSIFICATION. `IsPrivate` porte la politique de rattrapage NAT : une
 *      erreur ici rouvre le latching sur des adresses publiques.
 * ========================================================================= */

// IsPrivateV4 porte la POLITIQUE de rattrapage NAT : les plages v4 privées au
// sens propre, ni plus ni moins. C'est la règle historique IsRFC1918, dont tout
// élargissement rouvrirait le latching sur des adresses qui n'en relèvent pas.
TEST(IPAddressRange, PrivateV4ReprendExactementLesPlagesHistoriques)
{
	EXPECT_TRUE(IPAddress::Parse("10.0.0.1").IsPrivateV4());
	EXPECT_TRUE(IPAddress::Parse("172.16.0.1").IsPrivateV4());
	EXPECT_TRUE(IPAddress::Parse("172.31.255.254").IsPrivateV4());
	EXPECT_TRUE(IPAddress::Parse("192.168.1.1").IsPrivateV4());
	EXPECT_TRUE(IPAddress::Parse("100.64.0.1").IsPrivateV4())  << "RFC 6598, NAT opérateur";
	EXPECT_TRUE(IPAddress::Parse("169.254.1.1").IsPrivateV4()) << "RFC 3927, link-local";
	EXPECT_TRUE(IPAddress::Parse("::ffff:192.168.1.1").IsPrivateV4()) << "à travers le mapping";

	//Les bornes, dans le bon sens : 172.15 et 172.32 sont PUBLIQUES.
	EXPECT_FALSE(IPAddress::Parse("172.15.255.254").IsPrivateV4());
	EXPECT_FALSE(IPAddress::Parse("172.32.0.1").IsPrivateV4());
	EXPECT_FALSE(IPAddress::Parse("100.63.255.254").IsPrivateV4());
	EXPECT_FALSE(IPAddress::Parse("8.8.8.8").IsPrivateV4());

	//Jamais vrai pour une v6, ULA comprise : il n'y a pas de NAT en v6.
	EXPECT_FALSE(IPAddress::Parse("fd00:1234::1").IsPrivateV4());
	EXPECT_FALSE(IPAddress::Parse("fe80::1%lo").IsPrivateV4());
	EXPECT_FALSE(IPAddress().IsPrivateV4());
}

// ADVERSE, LE PIÈGE DU VOCABULAIRE — IsPrivate répond « non routable sur
// l'Internet public », ce qui est PLUS LARGE que « privée ». 192.0.2.1 est une
// adresse de documentation : non routable, et pourtant nullement NATée. Les
// confondre ferait ouvrir un rattrapage NAT sur une adresse qui n'en relève pas.
TEST(IPAddressRange, PrivateEstUnSurEnsembleStrictDePrivateV4)
{
	const char* const nonRoutablesMaisPasPrivees[] = {
		"0.0.0.0",            //0.0.0.0/8      « this network »
		"127.0.0.1",          //127/8          loopback
		"192.0.0.1",          //192.0.0.0/24   affectations de protocole IETF
		"192.0.2.1",          //192.0.2.0/24   documentation (RFC 5737)
		"198.18.0.1",         //198.18.0.0/15  benchmarking (RFC 2544)
		"198.51.100.1",       //198.51.100/24  documentation
		"203.0.113.1",        //203.0.113/24   documentation
		"240.0.0.1",          //240/4          réservé (classe E)
		"255.255.255.255",    //broadcast
	};

	for (size_t i = 0; i < sizeof(nonRoutablesMaisPasPrivees) / sizeof(*nonRoutablesMaisPasPrivees); ++i)
	{
		const IPAddress a = IPAddress::Parse(nonRoutablesMaisPasPrivees[i]);
		ASSERT_TRUE(a.IsSet()) << nonRoutablesMaisPasPrivees[i];
		EXPECT_TRUE(a.IsPrivate())    << nonRoutablesMaisPasPrivees[i] << " : non routable";
		EXPECT_FALSE(a.IsPrivateV4()) << nonRoutablesMaisPasPrivees[i]
			<< " : non routable, mais PAS privée — aucun rattrapage NAT ici";
	}

	//Et le sur-ensemble contient bien le sous-ensemble.
	EXPECT_TRUE(IPAddress::Parse("192.168.1.1").IsPrivate());
	EXPECT_TRUE(IPAddress::Parse("10.0.0.1").IsPrivate());

	//Une adresse réellement routable n'est ni l'un ni l'autre.
	EXPECT_FALSE(IPAddress::Parse("8.8.8.8").IsPrivate());
	EXPECT_FALSE(IPAddress::Parse("2001:4860:4860::8888").IsPrivate());
}

// Le multicast est EXCLU d'IsPrivate : il est routable, simplement pas unicast.
TEST(IPAddressRange, LeMulticastNEstPasClasseNonRoutable)
{
	EXPECT_FALSE(IPAddress::Parse("224.0.0.1").IsPrivate());
	EXPECT_FALSE(IPAddress::Parse("ff02::1").IsPrivate());
	EXPECT_TRUE(IPAddress::Parse("224.0.0.1").IsMulticast());
	EXPECT_TRUE(IPAddress::Parse("ff02::1").IsMulticast());
}

// L'ULA est l'analogue v6 du RFC 1918, mais elle N'EST PAS un critère de
// configuration : `--internal-ip` accepte aussi l'unicast global en v6, parce
// qu'un réseau interne v6 est le plus souvent numéroté dans une plage déléguée
// (NETWORK-CONFIGURATION.md). Ce prédicat sert au diagnostic.
TEST(IPAddressRange, UniqueLocalEstLAnalogueV6DuRFC1918)
{
	EXPECT_TRUE(IPAddress::Parse("fd00:1234::1").IsUniqueLocalV6());
	EXPECT_TRUE(IPAddress::Parse("fc00::1").IsUniqueLocalV6());
	EXPECT_TRUE(IPAddress::Parse("fdff:ffff::1").IsUniqueLocalV6());

	//fe00:: est hors de fc00::/7 (le 7e bit fait la limite), fe80:: est
	//link-local — aucune des deux n'est une ULA.
	EXPECT_FALSE(IPAddress::Parse("fe00::1").IsUniqueLocalV6());
	EXPECT_FALSE(IPAddress::Parse("fe80::1%lo").IsUniqueLocalV6());
	EXPECT_FALSE(IPAddress::Parse("2001:db8::1").IsUniqueLocalV6());
	EXPECT_FALSE(IPAddress::Parse("192.168.1.1").IsUniqueLocalV6()) << "jamais vrai pour une v4";

	//Non routable, mais sans rapport avec le NAT.
	EXPECT_TRUE(IPAddress::Parse("fd00:1234::1").IsPrivate());
	EXPECT_FALSE(IPAddress::Parse("fd00:1234::1").IsPrivateV4());
}

// Les plages v6 non routables qui ne sont ni ULA ni link-local.
TEST(IPAddressRange, LesPlagesV6NonRoutablesSontClassees)
{
	EXPECT_TRUE(IPAddress::Parse("::1").IsPrivate())            << "loopback";
	EXPECT_TRUE(IPAddress::Parse("::").IsPrivate())             << "non spécifiée";
	EXPECT_TRUE(IPAddress::Parse("2001:db8::1").IsPrivate())    << "documentation (RFC 3849)";
	EXPECT_TRUE(IPAddress::Parse("3fff::1").IsPrivate())        << "documentation (RFC 9637)";
	EXPECT_TRUE(IPAddress::Parse("100::1").IsPrivate())         << "discard-only (RFC 6666)";
	EXPECT_TRUE(IPAddress::Parse("fec0::1").IsPrivate())        << "site-local déprécié (RFC 3879)";
	EXPECT_TRUE(IPAddress::Parse("fe80::1%lo").IsPrivate())     << "link-local";

	EXPECT_FALSE(IPAddress::Parse("2000::1").IsPrivate())       << "unicast global";
	EXPECT_FALSE(IPAddress::Parse("3ffe::1").IsPrivate())       << "hors 3fff::/20";
	EXPECT_FALSE(IPAddress::Parse("2001:db9::1").IsPrivate())   << "voisin de la doc, mais global";
}

// Point de revue tranché dans ipaddress.h : Teredo et 6to4 sont classés NON
// privés (globales au sens du routage), mais restent des destinations valides.
TEST(IPAddressRange, TeredoEt6to4SontClassesNonPrivesMaisRestentDesDestinations)
{
	const IPAddress teredo = IPAddress::Parse("2001:0:4136:e378:8000:63bf:3fff:fdd2");
	const IPAddress sixto4 = IPAddress::Parse("2002:c000:204::1");

	ASSERT_TRUE(teredo.IsSet());
	ASSERT_TRUE(sixto4.IsSet());

	EXPECT_TRUE(teredo.IsTeredo());
	EXPECT_TRUE(sixto4.Is6to4());
	EXPECT_FALSE(teredo.IsPrivate());
	EXPECT_FALSE(sixto4.IsPrivate());
	EXPECT_TRUE(teredo.IsUnicastDestination());
	EXPECT_TRUE(sixto4.IsUnicastDestination());

	//2001:db8:: n'est PAS du Teredo (le préfixe court sur 32 bits, pas 16).
	EXPECT_FALSE(IPAddress::Parse("2001:db8::1").IsTeredo());
}

TEST(IPAddressRange, LoopbackMulticastEtNonSpecifiee)
{
	EXPECT_TRUE(IPAddress::Parse("127.0.0.1").IsLoopback());
	EXPECT_TRUE(IPAddress::Parse("127.255.255.254").IsLoopback());
	EXPECT_TRUE(IPAddress::Parse("::1").IsLoopback());

	EXPECT_TRUE(IPAddress::Parse("224.0.0.1").IsMulticast());
	EXPECT_TRUE(IPAddress::Parse("239.255.255.250").IsMulticast());
	EXPECT_TRUE(IPAddress::Parse("ff02::1").IsMulticast());
	EXPECT_FALSE(IPAddress::Parse("223.255.255.254").IsMulticast()) << "borne basse de 224/4";

	EXPECT_TRUE(IPAddress::Parse("fe80::1%lo").IsLinkLocal());
	EXPECT_TRUE(IPAddress::Parse("169.254.0.1").IsLinkLocal());
}

// Une adresse VIDE n'est pas « l'adresse non spécifiée » : c'est l'absence
// d'adresse (invariant 1). La distinction remplace la sentinelle INADDR_ANY.
TEST(IPAddressRange, UneAdresseVideNEstPasLAdresseNonSpecifiee)
{
	const IPAddress vide;

	EXPECT_FALSE(vide.IsSet());
	EXPECT_FALSE(vide.IsUnspecified());
	EXPECT_FALSE(vide.IsUnicastDestination());
	EXPECT_FALSE(vide.IsAnnounceable());
	EXPECT_EQ("", vide.ToString());

	const IPAddress nonSpecifiee = IPAddress::Parse("::");
	EXPECT_TRUE(nonSpecifiee.IsSet());
	EXPECT_TRUE(nonSpecifiee.IsUnspecified());
}

TEST(IPAddressRange, DestinationUnicastRefuseMulticastNonSpecifieeEtLinkLocalSansZone)
{
	EXPECT_FALSE(IPAddress::Parse("ff02::1").IsUnicastDestination());
	EXPECT_FALSE(IPAddress::Parse("224.0.0.1").IsUnicastDestination());
	EXPECT_FALSE(IPAddress::Parse("::").IsUnicastDestination());
	EXPECT_FALSE(IPAddress::Parse("fe80::1").IsUnicastDestination()) << "sans zone : inatteignable";

	EXPECT_TRUE(IPAddress::Parse("fe80::1%lo").IsUnicastDestination()) << "avec zone : exploitable";
	EXPECT_TRUE(IPAddress::Parse("fd00:1234::1").IsUnicastDestination());
	EXPECT_TRUE(IPAddress::Parse("192.0.2.1").IsUnicastDestination());
	EXPECT_TRUE(IPAddress::Parse("::1").IsUnicastDestination()) << "loopback : destination valide (tests)";
}

// Annonçable est PLUS STRICT que destination : publier ::1 ou une link-local
// dans un SDP donne une adresse que le pair ne peut pas joindre.
TEST(IPAddressRange, AnnoncableEstPlusStrictQueDestination)
{
	EXPECT_TRUE(IPAddress::Parse("192.0.2.1").IsAnnounceable());
	EXPECT_TRUE(IPAddress::Parse("2001:db8::1").IsAnnounceable());
	EXPECT_TRUE(IPAddress::Parse("fd00:1234::1").IsAnnounceable()) << "ULA : annonçable en interne";

	EXPECT_FALSE(IPAddress::Parse("::1").IsAnnounceable());
	EXPECT_FALSE(IPAddress::Parse("127.0.0.1").IsAnnounceable());
	EXPECT_FALSE(IPAddress::Parse("ff02::1").IsAnnounceable());
	EXPECT_FALSE(IPAddress::Parse("::").IsAnnounceable());
	EXPECT_FALSE(IPAddress::Parse("fe80::1%lo").IsAnnounceable()) << "la zone est un index LOCAL";
}


/* =========================================================================
 * §5 — SORTIE TEXTE. C'est la chaîne que VOIT le pair dans le SDP.
 * ========================================================================= */

TEST(IPAddressText, NormaliseVersLaFormeCanoniqueRFC5952)
{
	EXPECT_EQ("2001:db8::1", IPAddress::Parse("2001:0db8:0000:0000:0000:0000:0000:0001").ToString());
	EXPECT_EQ("2001:db8::1", IPAddress::Parse("2001:DB8::1").ToString())
		<< "RFC 5952 §4.3 : la sortie est en minuscules";
	EXPECT_EQ("2001:db8:85a3:8d3:1319:8a2e:370:7344",
	          IPAddress::Parse("2001:0db8:85a3:08d3:1319:8a2e:0370:7344").ToString())
		<< "zéros de tête supprimés";
	EXPECT_EQ("::", IPAddress::Parse("::").ToString());
}

// ADVERSE, RFC 5952 §4.2.2 : une suite d'UN SEUL groupe nul ne se compresse
// PAS. « 2001:db8:0:1:1:1:1:1 » ne doit pas devenir « 2001:db8::1:1:1:1:1 ».
TEST(IPAddressText, UnGroupeNulUniqueNeSeCompressePas)
{
	EXPECT_EQ("2001:db8:0:1:1:1:1:1", IPAddress::Parse("2001:db8:0:1:1:1:1:1").ToString());
}

TEST(IPAddressText, LaZoneEstSuffixee)
{
	const IPAddress a = IPAddress::Parse("fe80::1%lo");
	ASSERT_TRUE(a.IsSet());
	EXPECT_EQ("fe80::1%lo", a.ToString());
}

TEST(IPAddressText, LesCrochetsSontReservesAuxUrl)
{
	EXPECT_EQ("[2001:db8::1]", IPAddress::Parse("2001:db8::1").ToUrlString());
	EXPECT_EQ("192.0.2.1",     IPAddress::Parse("192.0.2.1").ToUrlString())
		<< "une adresse v4 ne prend JAMAIS de crochets";
	EXPECT_EQ("2001:db8::1",   IPAddress::Parse("2001:db8::1").ToString())
		<< "ToString reste nu : c'est lui qui alimente c= et a=candidate:";
	EXPECT_EQ("", IPAddress().ToUrlString());
}

// RFC 6874 : dans une URL, le '%' de la zone s'écrit "%25".
TEST(IPAddressText, LaZoneEstEchappeeDansUneUrl)
{
	const IPAddress a = IPAddress::Parse("fe80::1%lo");
	ASSERT_TRUE(a.IsSet());
	EXPECT_EQ("[fe80::1%25lo]", a.ToUrlString());
}


/* =========================================================================
 * §6 — SOCKADDR. Aller-retour avec l'API noyau, sans cast chez l'appelant.
 * ========================================================================= */

TEST(IPAddressSockaddr, AllerRetourV4)
{
	const IPAddress  a  = IPAddress::Parse("192.0.2.1");
	const IPEndpoint ep = a.To(5004);

	ASSERT_TRUE(ep.IsSet());
	EXPECT_EQ((socklen_t)sizeof(sockaddr_in), ep.Len());
	EXPECT_EQ(AF_INET, ep.Sockaddr()->sa_family);
	EXPECT_EQ(5004, ep.Port());
	EXPECT_TRUE(ep.Address() == a);
	EXPECT_EQ("192.0.2.1:5004", ep.ToString());
}

TEST(IPAddressSockaddr, AllerRetourV6AvecZone)
{
	const IPAddress a = IPAddress::Parse("fe80::1%lo");
	ASSERT_TRUE(a.IsSet());

	const IPEndpoint ep = a.To(5004);
	ASSERT_TRUE(ep.IsSet());
	EXPECT_EQ((socklen_t)sizeof(sockaddr_in6), ep.Len());
	EXPECT_EQ(AF_INET6, ep.Sockaddr()->sa_family);
	EXPECT_EQ(a.Scope(), ((const sockaddr_in6*)ep.Sockaddr())->sin6_scope_id)
		<< "la zone doit traverser la sockaddr, sinon le noyau ne sait pas par où sortir";
	EXPECT_TRUE(ep.Address() == a);
	EXPECT_EQ("[fe80::1%25lo]:5004", ep.ToString());
}

// La forme qu'exige la socket unique dual-stack : AF_INET6 quelle que soit la
// famille de départ, avec la v4 mappée — et l'adresse revient dé-mappée.
TEST(IPAddressSockaddr, DualStackMappeLaV4EtLaRendDemappee)
{
	const IPAddress  a  = IPAddress::Parse("192.0.2.1");
	const IPEndpoint ep = a.ToDualStack(5004);

	ASSERT_TRUE(ep.IsSet());
	EXPECT_EQ((socklen_t)sizeof(sockaddr_in6), ep.Len());
	EXPECT_EQ(AF_INET6, ep.Sockaddr()->sa_family);
	EXPECT_TRUE(IN6_IS_ADDR_V4MAPPED(&((const sockaddr_in6*)ep.Sockaddr())->sin6_addr));

	EXPECT_TRUE(ep.Address() == a);
	EXPECT_TRUE(ep.Address().IsV4()) << "le programme ne doit jamais voir la forme mappée";
	EXPECT_EQ("192.0.2.1:5004", ep.ToString());
}

TEST(IPAddressSockaddr, DualStackNeTouchePasAUneV6)
{
	const IPAddress  a  = IPAddress::Parse("2001:db8::1");
	const IPEndpoint ep = a.ToDualStack(5004);

	EXPECT_EQ((socklen_t)sizeof(sockaddr_in6), ep.Len());
	EXPECT_TRUE(ep.Address() == a);
}

TEST(IPAddressSockaddr, FromSockaddrDemappe)
{
	sockaddr_in6 mapped = {};
	mapped.sin6_family = AF_INET6;
	mapped.sin6_port   = htons(5004);
	ASSERT_EQ(1, inet_pton(AF_INET6, "::ffff:192.0.2.1", &mapped.sin6_addr));

	const IPAddress a = IPAddress::FromSockaddr((const sockaddr*)&mapped);
	EXPECT_TRUE(a.IsV4());
	EXPECT_EQ("192.0.2.1", a.ToString());

	EXPECT_EQ(5004, IPAddress::PortOf((const sockaddr*)&mapped));
}

TEST(IPAddressSockaddr, RefusePolimentCeQuiNEstPasUneAdresse)
{
	EXPECT_FALSE(IPAddress::FromSockaddr(NULL).IsSet());
	EXPECT_EQ(0, IPAddress::PortOf(NULL));

	sockaddr_storage inconnu = {};
	inconnu.ss_family = AF_UNIX;
	EXPECT_FALSE(IPAddress::FromSockaddr((const sockaddr*)&inconnu).IsSet());
	EXPECT_EQ(0, IPAddress::PortOf((const sockaddr*)&inconnu));

	//Une adresse vide ne produit pas de sockaddr : 0 octet, rien à envoyer.
	sockaddr_storage out = {};
	EXPECT_EQ(0, (int)IPAddress().FillSockaddr(out, 5004));
	EXPECT_FALSE(IPAddress().To(5004).IsSet());
}

// ADVERSE — LenPtr() doit REPOSER la capacité : sans cela, un recvfrom sur un
// IPEndpoint déjà rempli d'une sockaddr_in tronquerait une source v6.
TEST(IPAddressSockaddr, LenPtrReposeLaCapaciteAvantChaqueLecture)
{
	IPEndpoint ep = IPAddress::Parse("192.0.2.1").To(5004);
	ASSERT_EQ((socklen_t)sizeof(sockaddr_in), ep.Len());

	EXPECT_EQ((socklen_t)sizeof(sockaddr_storage), *ep.LenPtr());
}

TEST(IPAddressSockaddr, LaConversionImpliciteCompileEtPointeSurLaSockaddr)
{
	const IPEndpoint ep = IPAddress::Parse("192.0.2.1").To(5004);

	//C'est exactement l'écriture attendue aux points d'usage.
	const sockaddr* sa = ep;
	ASSERT_NE((const sockaddr*)NULL, sa);
	EXPECT_EQ(AF_INET, sa->sa_family);
	EXPECT_EQ(ep.Sockaddr(), sa);
}

TEST(IPAddressSockaddr, EgaliteDEndpointsAdresseEtPort)
{
	const IPAddress a = IPAddress::Parse("192.0.2.1");

	EXPECT_TRUE(a.To(5004) == a.To(5004));
	EXPECT_TRUE(a.To(5004) == a.ToDualStack(5004))
		<< "même pair, même port : la famille de la sockaddr ne doit pas départager";
	EXPECT_FALSE(a.To(5004) == a.To(5006));
	EXPECT_FALSE(a.To(5004) == IPAddress::Parse("192.0.2.2").To(5004));
	EXPECT_TRUE(IPEndpoint() == IPEndpoint());
	EXPECT_FALSE(IPEndpoint() == a.To(5004));
}


/* =========================================================================
 * §7 — ALLER-RETOUR RÉEL PAR LE NOYAU. Les §1-6 vérifient notre code contre
 *      lui-même ; celui-ci vérifie que le NOYAU accepte ce qu'on lui donne —
 *      la seule preuve qui vaille pour une socket dual-stack.
 * ========================================================================= */

TEST(IPAddressKernel, UneSocketDualStackRecoitUnPairV4EnFormeDemappee)
{
	if (!HasIPv6Loopback())
		GTEST_SKIP() << "pas de loopback IPv6 dans cet environnement";

	//Socket unique AF_INET6, V6ONLY=0 : le motif arbitré pour le média.
	const int server = socket(AF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(server, 0);

	int off = 0;
	ASSERT_EQ(0, setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)));

	const IPEndpoint bindTo = IPAddress::Any(AF_INET6).To(0);
	ASSERT_EQ(0, bind(server, bindTo, bindTo.Len()));

	//getsockname par le couple d'écriture Data()/LenPtr().
	IPEndpoint bound;
	ASSERT_EQ(0, getsockname(server, bound.Data(), bound.LenPtr()));
	const WORD port = bound.Port();
	ASSERT_NE(0, port);

	//Un pair IPv4 émet vers ce port, en loopback.
	const int client = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_GE(client, 0);

	const IPEndpoint to = IPAddress::Parse("127.0.0.1").To(port);
	const char       payload[] = "ipv6";
	ASSERT_EQ((ssize_t)sizeof(payload), sendto(client, payload, sizeof(payload), 0, to, to.Len()));

	//Réception : la source arrive en ::ffff:127.0.0.1 sur le fil, et doit
	//ressortir DÉ-MAPPÉE — c'est tout l'enjeu de l'invariant 3.
	char       buf[64];
	IPEndpoint from;
	const ssize_t n = recvfrom(server, buf, sizeof(buf), 0, from.Data(), from.LenPtr());

	close(client);
	close(server);

	ASSERT_EQ((ssize_t)sizeof(payload), n);
	EXPECT_EQ(AF_INET6, from.Sockaddr()->sa_family) << "la sockaddr brute reste v6";
	EXPECT_TRUE(from.Address().IsV4())              << "l'adresse rendue au programme est v4";
	EXPECT_TRUE(from.Address() == IPAddress::Parse("127.0.0.1"));
	EXPECT_TRUE(from.Address().IsLoopback());
}

TEST(IPAddressKernel, UneSocketDualStackRecoitUnPairV6)
{
	if (!HasIPv6Loopback())
		GTEST_SKIP() << "pas de loopback IPv6 dans cet environnement";

	const int server = socket(AF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(server, 0);

	int off = 0;
	ASSERT_EQ(0, setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)));

	const IPEndpoint bindTo = IPAddress::Parse("::1").To(0);
	ASSERT_EQ(0, bind(server, bindTo, bindTo.Len()));

	IPEndpoint bound;
	ASSERT_EQ(0, getsockname(server, bound.Data(), bound.LenPtr()));
	ASSERT_NE(0, bound.Port());

	const int client = socket(AF_INET6, SOCK_DGRAM, 0);
	ASSERT_GE(client, 0);

	const IPEndpoint to = IPAddress::Parse("::1").To(bound.Port());
	const char       payload[] = "ipv6";
	ASSERT_EQ((ssize_t)sizeof(payload), sendto(client, payload, sizeof(payload), 0, to, to.Len()));

	char       buf[64];
	IPEndpoint from;
	const ssize_t n = recvfrom(server, buf, sizeof(buf), 0, from.Data(), from.LenPtr());

	close(client);
	close(server);

	ASSERT_EQ((ssize_t)sizeof(payload), n);
	EXPECT_TRUE(from.Address().IsV6());
	EXPECT_TRUE(from.Address() == IPAddress::Parse("::1"));
}


/* =========================================================================
 * §8 — COMPARAISON.
 * ========================================================================= */

TEST(IPAddressCompare, UneAdresseVideNEstEgaleQuAUneAutreAdresseVide)
{
	EXPECT_TRUE(IPAddress() == IPAddress());
	EXPECT_FALSE(IPAddress() == IPAddress::Parse("192.0.2.1"));
	EXPECT_TRUE(IPAddress() != IPAddress::Parse("192.0.2.1"));
}

TEST(IPAddressCompare, LesFamillesNeSeConfondentPas)
{
	//0.0.0.0 et :: sont toutes deux « non spécifiées », mais pas la même adresse.
	EXPECT_FALSE(IPAddress::Parse("0.0.0.0") == IPAddress::Parse("::"));
	EXPECT_FALSE(IPAddress::Parse("192.0.2.1") == IPAddress::Parse("2001:db8::1"));
}

// La zone ne départage QUE si les deux adresses en portent une.
TEST(IPAddressCompare, LaZoneEstUnJokerQuandElleManque)
{
	const IPAddress sansZone = IPAddress::Parse("fe80::1");
	const IPAddress avecZone = IPAddress::Parse("fe80::1%lo");
	ASSERT_TRUE(sansZone.IsSet());
	ASSERT_TRUE(avecZone.IsSet());

	EXPECT_TRUE(sansZone == avecZone);

	IPAddress autreZone = avecZone;
	autreZone.SetScope(avecZone.Scope() + 1);
	EXPECT_FALSE(avecZone == autreZone) << "deux zones RENSEIGNÉES et différentes départagent";
}

TEST(IPAddressCompare, LOrdreTotalPermetDeClefUnConteneur)
{
	std::set<IPAddress> set;
	set.insert(IPAddress::Parse("192.0.2.1"));
	set.insert(IPAddress::Parse("192.0.2.2"));
	set.insert(IPAddress::Parse("2001:db8::1"));
	set.insert(IPAddress::Parse("::ffff:192.0.2.1"));   //doublon de la première

	EXPECT_EQ(3u, set.size());
	EXPECT_EQ(1u, set.count(IPAddress::Parse("192.0.2.1")));
	EXPECT_EQ(1u, set.count(IPAddress::Parse("2001:db8::1")));
}


/* =========================================================================
 * §9 — RESOLVE. Le seul point où le DNS entre dans le produit.
 * ========================================================================= */

TEST(IPAddressResolve, UnLitteralNePassePasParLeResolveur)
{
	int err = -1;
	const std::list<IPAddress> out = IPAddress::Resolve("2001:db8::1", err);

	EXPECT_EQ(0, err);
	ASSERT_EQ(1u, out.size());
	EXPECT_TRUE(out.front() == IPAddress::Parse("2001:db8::1"));
}

// Un littéral non annonçable reste rendu tel quel : Resolve ne fait pas de
// politique, elle résout. Le filtre « annonçable » ne s'applique qu'aux NOMS,
// où il faut bien choisir une adresse parmi celles du résolveur.
TEST(IPAddressResolve, UnLitteralLoopbackEstRenduTelQuel)
{
	int err = -1;
	const std::list<IPAddress> out = IPAddress::Resolve("127.0.0.1", err);

	EXPECT_EQ(0, err);
	ASSERT_EQ(1u, out.size());
	EXPECT_TRUE(out.front().IsLoopback());
}

TEST(IPAddressResolve, LEntreeVideEstUnEINVAL)
{
	int err = 0;
	EXPECT_TRUE(IPAddress::Resolve((const char*)NULL, err).empty());
	EXPECT_EQ(EINVAL, err);

	err = 0;
	EXPECT_TRUE(IPAddress::Resolve("", err).empty());
	EXPECT_EQ(EINVAL, err);
}

// Codes errno, JAMAIS des EAI_* (négatifs en glibc, donc jamais reconnus).
TEST(IPAddressResolve, UnNomInconnuRendENOENTPasUnCodeEAI)
{
	int err = 0;
	const std::list<IPAddress> out =
		IPAddress::Resolve("nexiste-pas.invalid", err);   //.invalid : RFC 2606

	EXPECT_TRUE(out.empty());
	EXPECT_TRUE(err == ENOENT || err == EAGAIN) << "err = " << err;
	EXPECT_GT(err, 0) << "un EAI_* aurait fuité (ils sont négatifs en glibc)";
}

// CARACTÉRISATION — « localhost » ne résout QUE vers de la loopback, qui n'est
// pas annonçable : Resolve rend donc une liste vide, avec ENOENT.
//
// >>> C'est le point de contact avec le test (encore désactivé)
// `IPv6Dns.PublicIpAccepteUnNomDHote`, qui exige que `SetAnnouncedIp("localhost")`
// réussisse. Les deux ne peuvent pas être vrais ensemble : c'est la POLITIQUE
// D'ANNONCE (phase 5 du chantier) qui devra trancher, pas cette brique-ci. Ce
// test fige le comportement actuel pour que la bascule soit visible. <<<
TEST(IPAddressResolve, LocalhostNeDonneRienDAnnoncable)
{
	int err = 0;
	const std::list<IPAddress> out = IPAddress::Resolve("localhost", err);

	if (!out.empty())
		GTEST_SKIP() << "cet hôte fait pointer localhost ailleurs que sur la loopback";

	EXPECT_EQ(ENOENT, err);
}

// L'ordre est le NÔTRE : la famille préférée d'abord, quel que soit l'ordre du
// résolveur — sinon l'adresse publiée dans le SDP dépendrait de /etc/gai.conf.
TEST(IPAddressResolve, LaFamillePrefereeVientEnTete)
{
	char hostname[256];
	ASSERT_EQ(0, gethostname(hostname, sizeof(hostname)));

	int errV4 = 0, errV6 = 0;
	const std::list<IPAddress> v4First = IPAddress::Resolve(hostname, errV4, AF_INET);
	const std::list<IPAddress> v6First = IPAddress::Resolve(hostname, errV6, AF_INET6);

	bool hasV4 = false, hasV6 = false;
	for (std::list<IPAddress>::const_iterator it = v4First.begin(); it != v4First.end(); ++it)
	{
		hasV4 = hasV4 || it->IsV4();
		hasV6 = hasV6 || it->IsV6();
	}

	if (!(hasV4 && hasV6))
		GTEST_SKIP() << "cet hôte n'est pas double pile (annonçable dans les deux familles)";

	EXPECT_TRUE(v4First.front().IsV4());
	EXPECT_TRUE(v6First.front().IsV6());
	EXPECT_EQ(v4First.size(), v6First.size()) << "mêmes adresses, ordre différent";
}

TEST(IPAddressResolve, DeuxResolutionsSuccessivesDonnentLeMemeResultat)
{
	char hostname[256];
	ASSERT_EQ(0, gethostname(hostname, sizeof(hostname)));

	int err1 = 0, err2 = 0;
	const std::list<IPAddress> first  = IPAddress::Resolve(hostname, err1);
	const std::list<IPAddress> second = IPAddress::Resolve(hostname, err2);

	if (first.empty())
		GTEST_SKIP() << "le nom de cet hôte ne donne rien d'annonçable";

	ASSERT_EQ(first.size(), second.size());

	std::list<IPAddress>::const_iterator a = first.begin();
	std::list<IPAddress>::const_iterator b = second.begin();
	for (; a != first.end(); ++a, ++b)
		EXPECT_TRUE(*a == *b) << "l'adresse annoncée doit être stable : " << a->ToString();
}
