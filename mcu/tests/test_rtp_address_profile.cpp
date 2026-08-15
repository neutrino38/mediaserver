/**
 * test_rtp_address_profile.cpp — profil d'adressage d'une session RTP.
 *
 * Étape 6 du chantier IPv6 (ipv6.md §14.3) : le contrôleur demande un profil
 * dans StartSending/StartReceiving, le serveur en tire l'adresse à LIER (donc
 * l'interface) et l'adresse à ANNONCER (donc la ligne c= du SDP).
 *
 * Ce qui est vérifié ici, ce sont surtout les REFUS et la stabilité : un profil
 * qui retomberait en silence sur le défaut ferait émettre par la mauvaise
 * interface, et personne ne le verrait avant l'absence de média chez le pair.
 */
#include <gtest/gtest.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <string>

#include "addressprofiles.h"
#include "rtpsession.h"

namespace {

class StubListener : public RTPSession::Listener
{
public:
	void onFPURequested(RTPSession*) override {}
	void onReceiverEstimatedMaxBitrate(RTPSession*, DWORD) override {}
	void onTempMaxMediaStreamBitrateRequest(RTPSession*, DWORD, DWORD) override {}
};

class Session
{
public:
	Session() : session(MediaFrame::Audio, &listener)
	{
		ok = (session.Init() == 1);
	}
	~Session() { if (ok) session.End(); }

	bool         ok = false;
	StubListener listener;
	RTPSession   session;
};

// Première adresse réellement attachée : les tests de bind n'ont de sens que
// sur une adresse que le noyau accepte de lier.
IPAddress FirstAttached(int family)
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
		if (addr.IsAnnounceable())
			found = addr;
	}
	freeifaddrs(list);
	return found;
}

class RtpAddressProfileTest : public ::testing::Test
{
protected:
	void SetUp()    override { AddressProfiles::Reset(); }
	void TearDown() override { AddressProfiles::Reset(); }
};

} // namespace


// Aucun profil demandé : le comportement d'un contrôleur qui ignore cette
// notion. Rien n'est verrouillé — sans quoi un appel muet interdirait à un
// appel ultérieur d'en demander un.
TEST_F(RtpAddressProfileTest, UnProfilVideNeVerrouilleRien)
{
	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	std::string error;
	EXPECT_TRUE(sess.session.SetAddressProfile(NULL,error)) << error;
	EXPECT_TRUE(sess.session.SetAddressProfile("",error))   << error;

	//... et un profil explicite reste possible ensuite.
	const IPAddress addr = FirstAttached(AF_INET);
	if (!addr.IsSet()) GTEST_SKIP() << "pas d'adresse v4 annoncable attachee";

	ASSERT_TRUE(AddressProfiles::AddPublic(addr,error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;
	EXPECT_TRUE(sess.session.SetAddressProfile("publicv4",error)) << error;
}

TEST_F(RtpAddressProfileTest, RefuseUnNomDeProfilInconnu)
{
	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	std::string error;
	EXPECT_FALSE(sess.session.SetAddressProfile("public4",error)) << "ancien nom";
	EXPECT_NE(std::string::npos, error.find("inconnu")) << error;

	EXPECT_FALSE(sess.session.SetAddressProfile("interne",error));
}

// ADVERSE — un profil indisponible est un ÉCHEC, jamais un repli silencieux sur
// le défaut : le contrôleur doit pouvoir retomber sur un autre profil, ce qu'il
// ne peut pas faire si le serveur lui a répondu « d'accord » en servant autre
// chose.
TEST_F(RtpAddressProfileTest, RefuseUnProfilIndisponible)
{
	const IPAddress addr = FirstAttached(AF_INET);
	if (!addr.IsSet()) GTEST_SKIP() << "pas d'adresse v4 annoncable attachee";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(addr,error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	EXPECT_FALSE(sess.session.SetAddressProfile("internalv4",error));
	EXPECT_NE(std::string::npos, error.find("indisponible")) << error;

	EXPECT_TRUE(sess.session.SetAddressProfile("publicv4",error)) << error;
}

// Le profil se fixe UNE FOIS. En RTP symétrique la socket est la même dans les
// deux sens : basculer en cours d'appel voudrait dire la relier sous le média,
// et le port publié dans le SDP changerait sans que le pair en sache rien.
TEST_F(RtpAddressProfileTest, LeProfilNeSeFixeQuUneFois)
{
	const IPAddress v4 = FirstAttached(AF_INET);
	if (!v4.IsSet()) GTEST_SKIP() << "pas d'adresse v4 annoncable attachee";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(v4,error)) << error;
	//Un second profil disponible, pour avoir vers quoi basculer.
	const bool hasInternal = AddressProfiles::AddInternal(v4,error);
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	ASSERT_TRUE(sess.session.SetAddressProfile("publicv4",error)) << error;

	//Le même : accepté, c'est le second appel (StartSending après
	//StartReceiving) du même contrôleur cohérent avec lui-même.
	EXPECT_TRUE(sess.session.SetAddressProfile("publicv4",error)) << error;

	if (!hasInternal)
		GTEST_SKIP() << "pas de second profil disponible pour tester la bascule";

	//Un AUTRE : refus explicite.
	EXPECT_FALSE(sess.session.SetAddressProfile("internalv4",error));
	EXPECT_NE(std::string::npos, error.find("deja fixe")) << error;
}

// Le profil décide de l'adresse liée : c'est ce qui choisit l'interface.
TEST_F(RtpAddressProfileTest, LeProfilLieLesSocketsSurSonAdresse)
{
	const IPAddress v4 = FirstAttached(AF_INET);
	if (!v4.IsSet()) GTEST_SKIP() << "pas d'adresse v4 annoncable attachee";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(v4,error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	EXPECT_FALSE(sess.session.GetBindAddress().IsSet())
		<< "avant toute demande : ecoute dual-stack sur toutes les interfaces";

	ASSERT_TRUE(sess.session.SetAddressProfile("publicv4",error)) << error;

	EXPECT_TRUE(sess.session.GetBindAddress() == v4);
	EXPECT_NE(0, sess.session.GetLocalPort()) << "la session reste utilisable apres re-bind";
}

// ADVERSE, LE CAS QUI JUSTIFIE TOUT LE MODÈLE — derrière NAT, l'adresse liée et
// l'adresse annoncée DIVERGENT. Confondre les deux, c'est soit annoncer une
// adresse qu'on ne peut pas lier, soit publier dans le SDP une adresse privée
// que le pair ne peut pas joindre.
TEST_F(RtpAddressProfileTest, DerriereNatLAdresseAnnonceeNEstPasLAdresseLiee)
{
	const IPAddress locale   = FirstAttached(AF_INET);
	const IPAddress publique = IPAddress::Parse("198.51.100.7");
	if (!locale.IsSet()) GTEST_SKIP() << "pas d'adresse v4 annoncable attachee";

	std::string error;
	ASSERT_TRUE(AddressProfiles::AddPublic(locale,error)) << error;
	ASSERT_TRUE(AddressProfiles::SetNat(publique,error)) << error;
	ASSERT_TRUE(AddressProfiles::Freeze(error)) << error;

	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	ASSERT_TRUE(sess.session.SetAddressProfile("publicv4",error)) << error;

	EXPECT_TRUE(sess.session.GetBindAddress() == locale)
		<< "la socket lie l'adresse locale, la seule qui existe sur cette machine";
	EXPECT_TRUE(sess.session.GetAnnouncedAddress() == publique)
		<< "le SDP porte l'adresse vue de l'exterieur";
}

// Sans table configurée (tests, point d'entrée qui aurait sauté la
// configuration), l'adresse annoncée retombe sur la valeur globale : aucune
// session ne se retrouve sans rien à publier.
TEST_F(RtpAddressProfileTest, SansTableLAdresseAnnonceeRetombeSurLaGlobale)
{
	Session sess;
	if (!sess.ok) GTEST_SKIP() << "pas de paire de ports RTP disponible";

	const char* global = RTPSession::GetAnnouncedIp();
	if (!global || !*global)
		GTEST_SKIP() << "cet hote n'a pas d'adresse annoncable";

	EXPECT_TRUE(sess.session.GetAnnouncedAddress() == IPAddress::Parse(global));
}
