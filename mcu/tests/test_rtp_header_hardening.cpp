/**
 * test_rtp_header_hardening.cpp — suite ADVERSE de l'en-tête RTP (chantier 5,
 * cf. network_parsers_hardening_plan.md).
 *
 * Un en-tête RTP décrit sa PROPRE longueur : douze octets fixes, plus quatre
 * par CSRC (compteur `cc`, 4 bits), plus une extension dont la longueur est
 * annoncée en mots de 32 bits (jusqu'à 262 140 octets). Rien de tout cela n'est
 * garanti par la taille du datagramme reçu — et `RTPSession` ne vérifiait que
 * `size >= 12`.
 *
 * Les conséquences se lisent dans les invariants testés ici : la longueur du
 * média est calculée par `taille - longueur d'en-tête`, donc elle passe sous
 * zéro (et devient ~4 Go en DWORD) dès que l'en-tête annoncé dépasse le
 * datagramme ; `GetMediaData()` pointe alors hors du tampon interne du paquet,
 * et le décodage des extensions parcourt jusqu'à 256 Ko après lui.
 *
 * On teste donc deux choses :
 *  - le paquet menteur est REJETÉ (`IsValid()` faux) et ne rend pas de taille
 *    absurde ;
 *  - un paquet honnête, extension comprise, continue d'être décodé (garde-fou
 *    anti-régression : ce durcissement ne doit pas jeter le trafic réel).
 *
 *     ./tests/runtests --gtest_filter='RtpHeader*'
 */
#include <gtest/gtest.h>

#include <new>
#include <vector>

#include "guardedbuffer.h"
#include "rtp.h"

namespace {

// En-tête RTP minimal : V=2, pas de padding, X/cc paramétrables.
void PushRtpHeader(std::vector<BYTE>& out, bool extension, BYTE cc, BYTE pt = 96)
{
	out.push_back(0x80 | (extension ? 0x10 : 0x00) | (cc & 0x0F));
	out.push_back(pt);
	out.push_back(0x00); out.push_back(0x01); //seq
	out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x64); //ts
	out.push_back(0xDE); out.push_back(0xAD); out.push_back(0xBE); out.push_back(0xEF); //ssrc
}

} // namespace

// ---------------------------------------------------------------------------
// Ce que le paquet annonce doit tenir dans ce qui a été reçu
// ---------------------------------------------------------------------------

TEST(RtpHeader, UnPaquetSansCsrcNiExtensionEstLuNormalement)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, false, 0);
	for (int i = 0; i < 10; ++i)
		pkt.push_back(0xAA);

	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	EXPECT_TRUE(packet.IsValid());
	EXPECT_EQ(12u, packet.GetRTPHeaderLen());
	EXPECT_EQ(10u, packet.GetMediaLength());
	EXPECT_EQ(0xDEADBEEFu, packet.GetSSRC());
}

// cc=15 réclame 60 octets de CSRC derrière l'en-tête fixe. Le datagramme n'en
// porte aucun : la longueur du média deviendrait 12 - 72, soit ~4 Go.
TEST(RtpHeader, UnCompteurDeCsrcMenteurNeProduitPasDeTailleNegative)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, false, 15);

	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	EXPECT_FALSE(packet.IsValid());
	EXPECT_EQ(0u, packet.GetMediaLength());
}

// X=1 sans la place du moindre en-tête d'extension.
TEST(RtpHeader, UneExtensionAnnonceeSansEnTeteEstRejetee)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, true, 0);

	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	EXPECT_FALSE(packet.IsValid());
	EXPECT_EQ(0u, packet.GetMediaLength());
}

// L'extension annonce 1024 mots (4 Ko) dans un datagramme de 20 octets.
TEST(RtpHeader, UneLongueurDExtensionMensongereEstRejetee)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, true, 0);
	pkt.push_back(0xBE); pkt.push_back(0xDE); //profil « one byte header »
	pkt.push_back(0x04); pkt.push_back(0x00); //1024 mots annoncés
	pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00); pkt.push_back(0x00);

	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	EXPECT_FALSE(packet.IsValid());
	EXPECT_EQ(0u, packet.GetMediaLength());
}

// Le tampon interne du paquet fait 1700 octets : un datagramme plus grand ne
// doit pas y être copié.
TEST(RtpHeader, UnDatagrammePlusGrandQueLeTamponEstRejete)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, false, 0);
	pkt.resize(4096, 0x5A);

	RTPTimedPacket packet(MediaFrame::Video, pkt.data(), pkt.size());
	EXPECT_FALSE(packet.IsValid());
	EXPECT_EQ(0u, packet.GetMediaLength());
}

// Un paquet trop court pour l'en-tête fixe lui-même.
TEST(RtpHeader, UnDatagrammePlusCourtQueLEnTeteEstRejete)
{
	BYTE data[8] = { 0x80, 96, 0, 1, 0, 0, 0, 0 };

	RTPTimedPacket packet(MediaFrame::Video, data, sizeof(data));
	EXPECT_FALSE(packet.IsValid());
	EXPECT_EQ(0u, packet.GetMediaLength());
}

// ---------------------------------------------------------------------------
// Le décodage des extensions
// ---------------------------------------------------------------------------

// Une extension honnête (audio level, RFC 6464) reste décodée.
TEST(RtpHeader, UneExtensionHonneteEstToujoursDecodee)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, true, 0);
	pkt.push_back(0xBE); pkt.push_back(0xDE); //profil
	pkt.push_back(0x00); pkt.push_back(0x01); //1 mot d'extension
	pkt.push_back(0x10);                      //id=1, len=0 (1 octet)
	pkt.push_back(0x80 | 42);                 //vad=1, niveau=42
	pkt.push_back(0x00); pkt.push_back(0x00); //padding
	for (int i = 0; i < 4; ++i)
		pkt.push_back(0xAA);                  //média

	RTPTimedPacket packet(MediaFrame::Audio, pkt.data(), pkt.size());
	ASSERT_TRUE(packet.IsValid());
	EXPECT_EQ(4u, packet.GetMediaLength());

	RTPMap extMap;
	extMap[1] = RTPPacket::HeaderExtension::SSRCAudioLevel;
	packet.ProcessExtensions(extMap);

	EXPECT_TRUE(packet.HasAudioLevel());
	EXPECT_TRUE(packet.GetVAD());
	EXPECT_EQ(42, packet.GetLevel());
}

// Le paquet est construit DANS un tampon dont la fin touche une page interdite :
// tout parcours d'extension qui dépasse le tampon interne du paquet tue le fork.
TEST(RtpHeader, LeDecodageDExtensionNeSortPasDuPaquet)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, true, 0);
	pkt.push_back(0xBE); pkt.push_back(0xDE);
	pkt.push_back(0xFF); pkt.push_back(0xFF); //65535 mots annoncés (256 Ko)
	pkt.push_back(0x10); pkt.push_back(0x80);
	pkt.push_back(0x00); pkt.push_back(0x00);

	//Arrondi à 8 pour que l'objet, place au ras de la page de garde, reste aligne.
	const size_t objectSize = (sizeof(RTPTimedPacket) + 7) & ~(size_t)7;
	GuardedBuffer guarded(NULL, objectSize);
	ASSERT_TRUE(guarded.IsValid());

	EXPECT_EXIT({
		RTPTimedPacket* packet = new (guarded.data())
			RTPTimedPacket(MediaFrame::Audio, pkt.data(), pkt.size());
		RTPMap extMap;
		extMap[1] = RTPPacket::HeaderExtension::SSRCAudioLevel;
		//Sur un paquet rejeté, ceci ne doit rien parcourir du tout.
		packet->ProcessExtensions(extMap);
		packet->~RTPTimedPacket();
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}
