/**
 * test_red_fec_hardening.cpp — suite ADVERSE des charges utiles RED (RFC 2198)
 * et ULPFEC (RFC 5109), chantier 5 (cf. network_parsers_hardening_plan.md).
 *
 * RED est utilisé pour le texte temps réel (T140RED) et pour transporter la
 * FEC vidéo : son en-tête est une suite de blocs de quatre octets, chacun
 * disant « un autre bloc suit » par son bit de poids fort, et la longueur du
 * bloc de données correspondant sur dix bits. Rien n'oblige un émetteur à
 * poser le bloc final, ni à annoncer des longueurs qui tiennent dans le
 * paquet.
 *
 * ULPFEC, lui, annonce une « longueur de protection » sur seize bits, qui sert
 * de taille de copie vers un tampon de MTU octets.
 *
 *     ./tests/runtests --gtest_filter='Red*:Fec*'
 */
#include <gtest/gtest.h>

#include <new>
#include <vector>

#include "fecdecoder.h"
#include "guardedbuffer.h"
#include "rtp.h"

namespace {

void PushRtpHeader(std::vector<BYTE>& out, BYTE pt)
{
	out.push_back(0x80);
	out.push_back(pt);
	out.push_back(0x00); out.push_back(0x01); //seq
	out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x64); //ts
	out.push_back(0xDE); out.push_back(0xAD); out.push_back(0xBE); out.push_back(0xEF); //ssrc
}

// Bloc d'en-tête RED redondant : F=1, type, offset (14 bits), longueur (10 bits).
void PushRedundantHeader(std::vector<BYTE>& out, BYTE type, WORD offset, WORD length)
{
	out.push_back(0x80 | (type & 0x7F));
	out.push_back(offset >> 6);
	out.push_back(((offset & 0x3F) << 2) | ((length >> 8) & 0x03));
	out.push_back(length & 0xFF);
}

} // namespace

// ---------------------------------------------------------------------------
// RED
// ---------------------------------------------------------------------------

// Un paquet RED honnête : un bloc redondant de 3 octets, puis le primaire.
TEST(RedPayload, UnPaquetHonneteEstDecode)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, 100);
	PushRedundantHeader(pkt, 98, 200, 3);
	pkt.push_back(98);            //en-tête du bloc primaire, F=0
	pkt.push_back('a'); pkt.push_back('b'); pkt.push_back('c'); //bloc redondant
	pkt.push_back('X'); pkt.push_back('Y');                     //bloc primaire

	RTPRedundantPacket red(MediaFrame::Text, pkt.data(), pkt.size());
	ASSERT_TRUE(red.IsValid());
	EXPECT_EQ(1, red.GetRedundantCount());
	EXPECT_EQ(98, red.GetPrimaryType());
	EXPECT_EQ(3u, red.GetRedundantPayloadSize(0));
	EXPECT_EQ(2u, red.GetPrimaryPayloadSize());
	ASSERT_TRUE(red.GetPrimaryPayloadData() != NULL);
	EXPECT_EQ('X', red.GetPrimaryPayloadData()[0]);
}

// Tous les octets ont le bit « un autre bloc suit » : la boucle de lecture des
// en-têtes n'avait aucune borne et sortait du paquet.
TEST(RedPayload, UneSuiteDeBlocsSansFinNeSortPasDuPaquet)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, 100);
	for (int i = 0; i < 12; ++i)
		pkt.push_back(0xFF); //F=1 partout, jusqu'au dernier octet

	const size_t objectSize = (sizeof(RTPRedundantPacket) + 7) & ~(size_t)7;
	GuardedBuffer guarded(NULL, objectSize);
	ASSERT_TRUE(guarded.IsValid());

	EXPECT_EXIT({
		RTPRedundantPacket* red = new (guarded.data())
			RTPRedundantPacket(MediaFrame::Text, pkt.data(), pkt.size());
		red->~RTPRedundantPacket();
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}

// Les longueurs de blocs annoncées dépassent le paquet : la taille du bloc
// primaire (`taille - en-têtes - blocs`) passait sous zéro.
TEST(RedPayload, DesLongueursDeBlocsMensongeresNeProduisentPasDeTailleNegative)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, 100);
	PushRedundantHeader(pkt, 98, 200, 1000); //« 1000 octets de redondance »
	pkt.push_back(98);                       //bloc primaire
	pkt.push_back('X');                      //...et il reste un octet

	RTPRedundantPacket red(MediaFrame::Text, pkt.data(), pkt.size());
	//Le bloc redondant annoncé ne tient pas : rien ne doit être exposé au-delà
	//de ce qui a été reçu.
	EXPECT_GE(6u, red.GetPrimaryPayloadSize());
	EXPECT_GE(6u, red.GetRedundantPayloadSize(0));
}

// Un paquet RED vide (en-tête RTP seul) ne doit rien décoder.
TEST(RedPayload, UnPaquetSansCharge)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, 100);

	RTPRedundantPacket red(MediaFrame::Text, pkt.data(), pkt.size());
	EXPECT_EQ(0, red.GetRedundantCount());
	EXPECT_EQ(0u, red.GetPrimaryPayloadSize());
}

// Le bloc primaire est annoncé mais tronqué : son en-tête d'un octet est le
// dernier du paquet.
TEST(RedPayload, UnBlocPrimaireTronqueNeSortPasDuPaquet)
{
	std::vector<BYTE> pkt;
	PushRtpHeader(pkt, 100);
	PushRedundantHeader(pkt, 98, 200, 4);
	pkt.push_back(98); //en-tête primaire, et fin du paquet

	RTPRedundantPacket red(MediaFrame::Text, pkt.data(), pkt.size());
	EXPECT_EQ(0u, red.GetPrimaryPayloadSize());
}

// ---------------------------------------------------------------------------
// ULPFEC
// ---------------------------------------------------------------------------

// Le tampon interne d'une FECData fait MTU octets ; la charge utile d'un
// paquet RTP peut en faire davantage (le tampon RTP fait 1700 octets).
TEST(FecData, UneChargeUtilePlusGrandeQueLeTamponNEstPasCopiee)
{
	std::vector<BYTE> payload(1600, 0x5A);

	FECData fec(payload.data(), payload.size());
	EXPECT_GE((DWORD)MTU, fec.GetSize());
}

// La « longueur de protection » est annoncée sur seize bits et sert de taille
// de copie vers un tampon de MTU octets, sur la pile du récupérateur.
TEST(FecData, LaLongueurDeProtectionEstBorneeParLaTailleRecue)
{
	BYTE payload[32];
	memset(payload, 0, sizeof(payload));
	//En-tête FEC : L=0, puis la longueur de protection en octets 10-11.
	payload[10] = 0xFF;
	payload[11] = 0xFF; //65535 annoncés dans un paquet de 32 octets

	FECData fec(payload, sizeof(payload));
	EXPECT_GE(sizeof(payload), fec.GetLevel0Size());
}

// Un décodeur vide reçoit un paquet média : le nettoyage des paquets anciens
// déréférençait l'itérateur AVANT de le comparer à la fin de la table, donc
// sur une table vide il lisait un nœud qui n'existe pas.
TEST(FecDecoder, UnPremierPaquetSurUnDecodeurVideNeDereferenceRien)
{
	EXPECT_EXIT({
		FECDecoder decoder;
		BYTE data[32];
		memset(data, 0, sizeof(data));
		data[0] = 0x80;
		data[1] = 100;
		RTPTimedPacket packet(MediaFrame::Video, data, sizeof(data));
		decoder.AddPacket(&packet);
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}
