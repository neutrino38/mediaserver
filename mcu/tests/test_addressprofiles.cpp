/**
 * test_addressprofiles.cpp — table des profils d'adressage
 * (NETWORK-CONFIGURATION.md).
 *
 * Étape 4 du chantier IPv6. Tests ACTIFS : la table doit être juste dès
 * maintenant, elle décide de l'adresse annoncée dans chaque SDP.
 *
 * L'essentiel de ce qui est vérifié ici, ce sont les REFUS. Une table
 * d'adressage fausse ne se voit pas au démarrage : elle se voit appel par
 * appel, en production, sous forme de média qui ne remonte pas. Chaque contrôle
 * manquant est une panne différée.
 *
 * Les cas qui exigent une adresse réellement attachée à la machine sont sautés
 * (SKIP) là où l'hôte n'en a pas — jamais contournés par un faux positif.
 */
#include <gtest/gtest.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <string>

#include "addressprofiles.h"

namespace {

// Remet la table à zéro autour de chaque test : elle est statique, donc
// partagée par tout le processus de test.
class AddressProfilesTest : public ::testing::Test
{
protected:
	void SetUp()    override { AddressProfiles::Reset(); }
	void TearDown() override { AddressProfiles::Reset(); }
};

// Première adresse RÉELLEMENT attachée répondant au critère demandé. C'est la
// seule façon honnête de tester les contrôles : inventer une adresse ferait
// passer le test sans exercer `IsLocallyAttached`.
IPAddress FirstAttached(int family, bool wantPrivateV4)
{
	ifaddrs* list = NULL;
	if (getifaddrs(&list) != 0)
		return IPAddress();

	IPAddress found;

	for (ifaddrs* p = list; p && !found.IsSet(); p = p->ifa_next)
	{
		if (!p->ifa_addr || p->ifa_addr->sa_family != family)
			continue;

		const IPAddress addr = IPAddress::FromSockaddr(p->ifa_addr);

		if (!addr.IsAnnounceable())
			continue;
		if (wantPrivateV4 && !addr.IsPrivateV4())
			continue;

		found = addr;
	}

	freeifaddrs(list);
	return found;
}

} // namespace


/* =========================================================================
 * §1 — NOMS. Ce sont ceux que le contrôleur enverra dans StartReceiving.
 * ========================================================================= */

TEST_F(AddressProfilesTest, LesQuatreProfilsSeNommentEtSeRelisent)
{
	const AddressProfiles::Id ids[] = {
		AddressProfiles::PublicV4, AddressProfiles::PublicV6,
		AddressProfiles::InternalV4, AddressProfiles::InternalV6
	};
	const char* const names[] = { "publicv4", "publicv6", "internalv4", "internalv6" };

	for (int i = 0; i < 4; ++i)
	{
		EXPECT_STREQ(names[i], AddressProfiles::NameOf(ids[i]));

		AddressProfiles::Id parsed;
		ASSERT_TRUE(AddressProfiles::ParseId(names[i], parsed)) << names[i];
		EXPECT_EQ(ids[i], parsed);
	}
}

// ADVERSE — un nom inconnu est un REFUS, jamais un défaut silencieux : sinon la
// faute de frappe d'un contrôleur passerait pour un appel nominal, servi sur la
// mauvaise interface.
TEST_F(AddressProfilesTest, UnNomDeProfilInconnuEstRefuse)
{
	AddressProfiles::Id parsed = AddressProfiles::InternalV6;

	EXPECT_FALSE(AddressProfiles::ParseId("public4", parsed))  << "ancien nom : refus";
	EXPECT_FALSE(AddressProfiles::ParseId("PUBLICV4", parsed)) << "casse : refus";
	EXPECT_FALSE(AddressProfiles::ParseId("", parsed));
	EXPECT_FALSE(AddressProfiles::ParseId(NULL, parsed));
	EXPECT_EQ(AddressProfiles::InternalV6, parsed) << "la sortie n'est pas touchee en cas d'echec";
}


/* =========================================================================
 * §2 — REFUS AU DÉMARRAGE. Mieux vaut un serveur qui ne démarre pas qu'un
 *      serveur qui annonce une adresse fausse pendant six mois.
 * ========================================================================= */

TEST_F(AddressProfilesTest, RefuseUneAdresseNonAnnoncable)
{
	std::string error;

	EXPECT_FALSE(AddressProfiles::AddPublic(IPAddress::Parse("127.0.0.1"), error));
	EXPECT_NE(std::string::npos, error.find("annoncable")) << error;

	EXPECT_FALSE(AddressProfiles::AddPublic(IPAddress::Parse("::1"), error));
	EXPECT_FALSE(AddressProfiles::AddPublic(IPAddress::Parse("224.0.0.1"), error));
	EXPECT_FALSE(AddressProfiles::AddPublic(IPAddress::Parse("0.0.0.0"), error));
	EXPECT_FALSE(AddressProfiles::AddPublic(IPAddress(), error)) << "adresse vide";
}

// ADVERSE — le contrôle qui attrape la faute de frappe sur une adresse INTERNE :
// elle sert à choisir l'interface de service, elle n'a aucun sens si aucune ne
// la porte. Le dire au démarrage plutôt qu'au premier appel.
TEST_F(AddressProfilesTest, RefuseUneAdresseInterneAttacheeANulleInterface)
{
	const IPAddress inventee = IPAddress::Parse("10.255.255.254");
	if (AddressProfiles::IsLocallyAttached(inventee))
		GTEST_SKIP() << "cet hote porte justement " << inventee.ToString();

	std::string error;
	EXPECT_FALSE(AddressProfiles::AddInternal(inventee, error));
	EXPECT_NE(std::string::npos, error.find("interface")) << error;
}

// COMPATIBILITÉ — `--public-ip` a toujours désigné « l'adresse que les pairs
// atteignent, qui n'est pas celle liée localement » : derrière NAT, elle n'est
// attachée à AUCUNE de nos interfaces. Elle doit donc rester acceptée, en mode
// annonce seule (bind « toutes interfaces », comme aujourd'hui). Exiger
// l'attachement casserait ces déploiements du jour au lendemain.
TEST_F(AddressProfilesTest, UneAdressePubliqueNonAttacheeResteAcceptee)
{
	const IPAddress publique = IPAddress::Parse("198.51.100.7");
	ASSERT_FALSE(AddressProfiles::IsLocallyAttached(publique));

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(publique, error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	EXPECT_TRUE(AddressProfiles::AnnouncedAddress(AddressProfiles::PublicV4) == publique);
	EXPECT_FALSE(AddressProfiles::BindAddress(AddressProfiles::PublicV4).IsSet())
		<< "bind vide = ecoute historique sur toutes interfaces";
	EXPECT_NE(std::string::npos, AddressProfiles::Describe().find("toutes interfaces"))
		<< AddressProfiles::Describe();
}

// La v4 interne doit être privée : une publique déclarée interne est presque à
// coup sûr une erreur de configuration.
TEST_F(AddressProfilesTest, RefuseUneAdresseInterneV4NonPrivee)
{
	std::string error;

	//Le contrôle de plage passe AVANT celui de l'attachement, précisément pour
	//que l'exploitant lise ce qu'il a fait de travers plutôt qu'un message qui
	//l'enverrait chercher ailleurs. Le test ne dépend donc d'aucune interface.
	EXPECT_FALSE(AddressProfiles::AddInternal(IPAddress::Parse("8.8.8.8"), error));
	EXPECT_NE(std::string::npos, error.find("privees")) << error;

	//Une adresse de documentation n'est pas privée non plus : « non routable »
	//et « privée » ne sont pas la même question.
	EXPECT_FALSE(AddressProfiles::AddInternal(IPAddress::Parse("192.0.2.1"), error));
	EXPECT_NE(std::string::npos, error.find("privees")) << error;
}

// ... alors que la v6 interne n'a AUCUNE contrainte de plage : un réseau
// interne v6 est souvent numéroté dans une plage globale déléguée.
TEST_F(AddressProfilesTest, AccepteUneAdresseInterneV6QuelleQueSoitSaPlage)
{
	const IPAddress v6 = FirstAttached(AF_INET6, false);
	if (!v6.IsSet())
		GTEST_SKIP() << "pas d'adresse v6 annoncable attachee sur cet hote";

	std::string error;
	EXPECT_TRUE(AddressProfiles::AddInternal(v6, error)) << error;
	EXPECT_TRUE(AddressProfiles::IsAvailable(AddressProfiles::InternalV6));
}

TEST_F(AddressProfilesTest, RefuseDeuxAdressesPourLeMemeProfil)
{
	const IPAddress addr = FirstAttached(AF_INET, false);
	if (!addr.IsSet())
		GTEST_SKIP() << "pas d'adresse v4 annoncable attachee sur cet hote";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(addr, error)) << error;

	EXPECT_FALSE(AddressProfiles::AddPublic(addr, error));
	EXPECT_NE(std::string::npos, error.find("deja renseigne")) << error;
}

// Pas de NAT en IPv6, par choix — et le refus est explicite, pas un silence.
TEST_F(AddressProfilesTest, RefuseUnNatIPv6)
{
	std::string error;

	EXPECT_FALSE(AddressProfiles::SetNat(IPAddress::Parse("2001:db8::1"), error));
	EXPECT_NE(std::string::npos, error.find("IPv4")) << error;
}

TEST_F(AddressProfilesTest, RefuseUnNatSansAdressePubliqueV4)
{
	std::string error;

	//L'adresse NAT, elle, n'a pas à être attachée localement : elle vit sur le
	//routeur. Elle est donc acceptée ici, et c'est Freeze qui tranche.
	ASSERT_TRUE(AddressProfiles::SetNat(IPAddress::Parse("198.51.100.7"), error)) << error;

	EXPECT_FALSE(AddressProfiles::Freeze(error));
	EXPECT_NE(std::string::npos, error.find("--public-ip")) << error;
}

TEST_F(AddressProfilesTest, RefuseDeDemarrerSansAucunProfil)
{
	std::string error;

	EXPECT_FALSE(AddressProfiles::Freeze(error));
	EXPECT_NE(std::string::npos, error.find("aucun profil")) << error;
}

// ADVERSE — un profil par défaut indisponible échouerait à CHAQUE appel : c'est
// le pire des deux mondes, un serveur qui démarre et ne sert rien.
TEST_F(AddressProfilesTest, RefuseUnProfilParDefautIndisponible)
{
	const IPAddress addr = FirstAttached(AF_INET, false);
	if (!addr.IsSet())
		GTEST_SKIP() << "pas d'adresse v4 annoncable attachee sur cet hote";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(addr, error)) << error;
	ASSERT_TRUE(AddressProfiles::SetDefault(AddressProfiles::InternalV6, error)) << error;

	EXPECT_FALSE(AddressProfiles::Freeze(error));
	EXPECT_NE(std::string::npos, error.find("internalv6")) << error;
}


/* =========================================================================
 * §3 — NAT : le seul endroit du produit où bind et annoncée divergent.
 * ========================================================================= */

TEST_F(AddressProfilesTest, LeNatFaitDivergerLAdresseAnnonceeDeLAdresseLiee)
{
	const IPAddress locale = FirstAttached(AF_INET, false);
	if (!locale.IsSet())
		GTEST_SKIP() << "pas d'adresse v4 annoncable attachee sur cet hote";

	const IPAddress publique = IPAddress::Parse("198.51.100.7");

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(locale, error)) << error;
	ASSERT_TRUE(AddressProfiles::SetNat(publique, error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	EXPECT_TRUE(AddressProfiles::BindAddress(AddressProfiles::PublicV4) == locale)
		<< "la socket lie toujours l'adresse locale";
	EXPECT_TRUE(AddressProfiles::AnnouncedAddress(AddressProfiles::PublicV4) == publique)
		<< "le SDP porte l'adresse vue de l'exterieur";

	EXPECT_NE(std::string::npos, AddressProfiles::Describe().find("(NAT)"))
		<< AddressProfiles::Describe();
}

// Sans --nat, les deux adresses d'un profil sont la même : le cas nominal.
TEST_F(AddressProfilesTest, SansNatLesDeuxAdressesSontIdentiques)
{
	const IPAddress locale = FirstAttached(AF_INET, false);
	if (!locale.IsSet())
		GTEST_SKIP() << "pas d'adresse v4 annoncable attachee sur cet hote";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(locale, error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	EXPECT_TRUE(AddressProfiles::BindAddress(AddressProfiles::PublicV4) ==
	            AddressProfiles::AnnouncedAddress(AddressProfiles::PublicV4));
	EXPECT_EQ(std::string::npos, AddressProfiles::Describe().find("(NAT)"));
}


/* =========================================================================
 * §4 — ORDRE DES OPTIONS, GEL, LECTURE.
 * ========================================================================= */

// ADVERSE — `--nat` AVANT `--public-ip` doit donner exactement le même résultat
// que l'inverse : le sens d'un /etc/sysconfig ne peut pas dépendre de l'ordre
// des lignes. C'est la raison pour laquelle les contrôles croisés sont dans
// Freeze et non au fil de la lecture.
TEST_F(AddressProfilesTest, LOrdreDesOptionsNEstPasSignificatif)
{
	const IPAddress locale   = FirstAttached(AF_INET, false);
	const IPAddress publique = IPAddress::Parse("198.51.100.7");
	if (!locale.IsSet())
		GTEST_SKIP() << "pas d'adresse v4 annoncable attachee sur cet hote";

	std::string error;
	ASSERT_TRUE(AddressProfiles::SetNat(publique, error)) << error;
	ASSERT_TRUE(AddressProfiles::AddPublic(locale, error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	EXPECT_TRUE(AddressProfiles::AnnouncedAddress(AddressProfiles::PublicV4) == publique);
}

TEST_F(AddressProfilesTest, LeDefautEstPublicV4EtSeDeplace)
{
	EXPECT_EQ(AddressProfiles::PublicV4, AddressProfiles::Default())
		<< "comportement historique : un appel sans profil demande publicv4";

	const IPAddress v6 = FirstAttached(AF_INET6, false);
	if (!v6.IsSet())
		GTEST_SKIP() << "pas d'adresse v6 annoncable attachee sur cet hote";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(v6, error)) << error;
	ASSERT_TRUE(AddressProfiles::SetDefault(AddressProfiles::PublicV6, error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	EXPECT_EQ(AddressProfiles::PublicV6, AddressProfiles::Default())
		<< "--default-profile debloque un hote v6-only sans toucher au contrat";
}

// Une table qui change en cours de route donnerait des appels annoncés
// différemment selon l'heure.
TEST_F(AddressProfilesTest, LaTableEstFigeeApresFreeze)
{
	const IPAddress locale = FirstAttached(AF_INET, false);
	if (!locale.IsSet())
		GTEST_SKIP() << "pas d'adresse v4 annoncable attachee sur cet hote";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(locale, error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	EXPECT_FALSE(AddressProfiles::AddInternal(IPAddress::Parse("192.168.0.1"), error));
	EXPECT_FALSE(AddressProfiles::SetNat(IPAddress::Parse("198.51.100.7"), error));
	EXPECT_FALSE(AddressProfiles::SetDefault(AddressProfiles::PublicV4, error));
	EXPECT_FALSE(AddressProfiles::Freeze(error)) << "gel idempotent : non, refus explicite";
}

TEST_F(AddressProfilesTest, UnProfilIndisponibleNeRendAucuneAdresse)
{
	EXPECT_FALSE(AddressProfiles::IsAvailable(AddressProfiles::InternalV4));
	EXPECT_FALSE(AddressProfiles::BindAddress(AddressProfiles::InternalV4).IsSet());
	EXPECT_FALSE(AddressProfiles::AnnouncedAddress(AddressProfiles::InternalV4).IsSet());
	EXPECT_EQ(0, AddressProfiles::AvailableCount());
}

// Describe() est la matière de l'API d'introspection : elle doit citer
// les quatre profils, disponibles ou non, et désigner le défaut.
TEST_F(AddressProfilesTest, DescribeCiteLesQuatreProfilsEtLeDefaut)
{
	const std::string vide = AddressProfiles::Describe();

	EXPECT_NE(std::string::npos, vide.find("publicv4"));
	EXPECT_NE(std::string::npos, vide.find("publicv6"));
	EXPECT_NE(std::string::npos, vide.find("internalv4"));
	EXPECT_NE(std::string::npos, vide.find("internalv6"));
	EXPECT_NE(std::string::npos, vide.find("indisponible"));
	EXPECT_NE(std::string::npos, vide.find("[defaut]"));
}


/* =========================================================================
 * §5 — IsLocallyAttached, le contrôle sur lequel repose tout le §2.
 * ========================================================================= */

TEST_F(AddressProfilesTest, IsLocallyAttachedReconnaitLaLoopbackEtIgnoreLInvente)
{
	EXPECT_TRUE(AddressProfiles::IsLocallyAttached(IPAddress::Parse("127.0.0.1")))
		<< "la loopback est attachee (elle est refusee ailleurs, pour non-annoncabilite)";

	EXPECT_FALSE(AddressProfiles::IsLocallyAttached(IPAddress::Parse("192.0.2.1")));
	EXPECT_FALSE(AddressProfiles::IsLocallyAttached(IPAddress::Parse("2001:db8::1")));
	EXPECT_FALSE(AddressProfiles::IsLocallyAttached(IPAddress()));
}
