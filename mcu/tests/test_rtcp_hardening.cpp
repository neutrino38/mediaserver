/**
 * test_rtcp_hardening.cpp — suite ADVERSE du parseur RTCP (chantier 5, cf.
 * network_parsers_hardening_plan.md).
 *
 * RTCP arrive par UDP, avant toute authentification quand SRTP n'est pas armé :
 * n'importe qui capable d'émettre vers le port média fait exécuter ce code. Les
 * paquets ci-dessous sont donc construits pour MENTIR — un en-tête qui annonce
 * plus long qu'il n'est, un compteur de rapports plus grand que la place
 * disponible, une longueur qui, soustraite, passe sous zéro.
 *
 * Deux registres, complémentaires :
 *  - le parseur doit REFUSER (retour 0 / NULL / aucun champ inventé) ;
 *  - il ne doit pas lire UN SEUL octet au-delà du datagramme. C'est ce que
 *    prouve GuardedBuffer (page de garde, cf. tests/guardedbuffer.h) : le
 *    parsing tourne dans un fork (EXPECT_EXIT), et un débordement d'un octet
 *    tue ce fork au lieu de passer inaperçu.
 *
 * Toute la suite est nommée Rtcp* pour rester filtrable :
 *     ./tests/runtests --gtest_filter='Rtcp*'
 */
#include <gtest/gtest.h>

#include <vector>

#include "guardedbuffer.h"
#include "rtp.h"

namespace {

// --- Fabrique de paquets RTCP ------------------------------------------------
//
// En-tête commun RFC 3550 : V=2, P, count(5b) | PT | longueur en mots de 32
// bits MOINS UN. La longueur est celle qu'on ANNONCE ; la taille réellement
// remise au parseur est celle du vecteur — c'est tout l'objet de la suite que
// les deux ne coïncident pas.
void PushHeader(std::vector<BYTE>& out, BYTE count, BYTE pt, WORD lengthWords)
{
	out.push_back(0x80 | (count & 0x1F));
	out.push_back(pt);
	out.push_back(lengthWords >> 8);
	out.push_back(lengthWords & 0xFF);
}

void Push32(std::vector<BYTE>& out, DWORD value)
{
	out.push_back(value >> 24);
	out.push_back((value >> 16) & 0xFF);
	out.push_back((value >> 8) & 0xFF);
	out.push_back(value & 0xFF);
}

// Joue Parse() derrière une page de garde, dans un processus fils. Le test
// échoue si le fils meurt — donc si le parseur a lu hors du datagramme.
#define EXPECT_PARSE_NE_DEBORDE_PAS(paquet)                                       \
	do {                                                                      \
		GuardedBuffer guarded((paquet).data(), (paquet).size());           \
		ASSERT_TRUE(guarded.IsValid());                                    \
		EXPECT_EXIT({                                                      \
			RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(      \
				guarded.data(), (DWORD)guarded.Size());            \
			delete rtcp;                                               \
			_exit(0);                                                  \
		}, ::testing::ExitedWithCode(0), "");                              \
	} while (0)

// Un ReceiverReport minimal et VALIDE, pour préfixer un compound dont on veut
// éprouver le second sous-paquet (IsRTCP ne regarde que le premier).
void PushValidReceiverReport(std::vector<BYTE>& out)
{
	PushHeader(out, 0, 201, 1);
	Push32(out, 0x11223344);
}

} // namespace

// ---------------------------------------------------------------------------
// Le paquet composé lui-même
// ---------------------------------------------------------------------------

// Un compound bien formé reste parsé : le durcissement ne doit pas jeter le
// trafic honnête (garde-fou anti-régression).
TEST(RtcpCompound, UnPaquetValideResteParse)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);

	RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(pkt.data(), pkt.size());
	ASSERT_TRUE(rtcp != NULL);
	EXPECT_EQ(1u, rtcp->GetPacketCount());
	EXPECT_EQ(RTCPPacket::ReceiverReport, rtcp->GetPacket(0)->GetType());
	delete rtcp;
}

// IsRTCP ne valide QUE le premier en-tête. Si un sous-paquet suivant n'a pas
// même la place d'un en-tête (4 octets), il ne faut pas le lire quand même.
TEST(RtcpCompound, UnSecondSousPaquetTronqueNEstPasLu)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	//Trois octets : moins qu'un en-tête commun.
	pkt.push_back(0x81);
	pkt.push_back(200);
	pkt.push_back(0x00);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);

	//Et le parseur rend ce qu'il a pu lire honnêtement, sans inventer.
	RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(pkt.data(), pkt.size());
	if (rtcp)
	{
		EXPECT_EQ(1u, rtcp->GetPacketCount());
		delete rtcp;
	}
}

// ---------------------------------------------------------------------------
// Rapports d'émission / de réception
// ---------------------------------------------------------------------------

// Un SenderReport annonce 8 octets et en fait lire 28 : les six mots
// (ssrc, NTP, RTP ts, compteurs) sont lus sans que la place ait été vérifiée.
TEST(RtcpSenderReport, UnEnTeteTropCourtNeFaitPasLireLesCompteurs)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, 0, 200, 1); //annonce 8 octets
	Push32(pkt, 0xDEADBEEF);    //...et n'en fournit que 8

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// Le compteur de blocs de rapport est un champ du paquet : 31 blocs annoncés
// dans un datagramme qui n'en porte aucun ne doivent pas être lus.
TEST(RtcpSenderReport, LeCompteurDeRapportsNeDepassePasLeDatagramme)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, 31, 200, 6); //6+1 mots = 28 octets, cohérents...
	Push32(pkt, 0x11111111);     //ssrc
	for (int i = 0; i < 5; ++i)  //...mais AUCUN bloc de rapport derrière
		Push32(pkt, 0);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// Un ReceiverReport de 4 octets ne porte même pas son SSRC.
TEST(RtcpReceiverReport, UnPaquetSansSsrcNEstPasLu)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, 0, 201, 0); //annonce 4 octets, en fournit 4

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// ---------------------------------------------------------------------------
// BYE
// ---------------------------------------------------------------------------

// count=31 dans un BYE qui ne porte aucun SSRC : 124 octets seraient lus
// derrière l'en-tête.
TEST(RtcpBye, LeCompteurDeSsrcEstBorneParLaTailleRecue)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 31, 203, 0);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// La raison du BYE est préfixée d'une longueur sur un octet : 255 annoncés
// dans un paquet qui n'en porte qu'un.
TEST(RtcpBye, LaLongueurDeLaRaisonEstBorneeParLaTailleRecue)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 1, 203, 2); //12 octets annoncés
	Push32(pkt, 0x22222222);    //un ssrc
	pkt.push_back(255);         //« la raison fait 255 octets »
	pkt.push_back('x');         //...et il n'y en a qu'un
	pkt.push_back(0);
	pkt.push_back(0);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// ---------------------------------------------------------------------------
// Extended jitter report
// ---------------------------------------------------------------------------

TEST(RtcpExtendedJitter, LeCompteurDeGigueEstBorneParLaTailleRecue)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 31, 195, 0);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// ---------------------------------------------------------------------------
// APP : le cas où la soustraction passe sous zéro
// ---------------------------------------------------------------------------

// APP lit ssrc + nom (8 octets) puis calcule la taille des données par
// `packetSize - 12`. Sur un paquet de 4 octets, la soustraction (non signée)
// donne ~4 Go : c'est la taille du malloc, puis celle du memcpy.
TEST(RtcpApp, UnPaquetPlusCourtQueSonEnTeteNeProduitPasDeTailleNegative)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 0, 204, 0); //APP annoncé en 4 octets

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

TEST(RtcpApp, UnPaquetValideRendSesDonnees)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, 3, 204, 3); //16 octets
	Push32(pkt, 0x33333333);    //ssrc
	pkt.push_back('T');         //nom
	pkt.push_back('E');
	pkt.push_back('S');
	pkt.push_back('T');
	Push32(pkt, 0x0A0B0C0D);    //4 octets de données applicatives

	RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(pkt.data(), pkt.size());
	ASSERT_TRUE(rtcp != NULL);
	ASSERT_EQ(1u, rtcp->GetPacketCount());
	RTCPApp* app = (RTCPApp*)rtcp->GetPacket(0);
	EXPECT_EQ(0x33333333u, app->GetSSRC());
	EXPECT_EQ(4u, app->GetDataSize());
	delete rtcp;
}

// ---------------------------------------------------------------------------
// Feedback (RTPFB / PSFB)
// ---------------------------------------------------------------------------

// Les deux SSRC du feedback occupent 8 octets derrière l'en-tête : un paquet
// de 4 octets n'en porte aucun.
TEST(RtcpRtpFeedback, UnPaquetSansSsrcNEstPasLu)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 1, 205, 0); //RTPFB/NACK annoncé en 4 octets

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

TEST(RtcpPayloadFeedback, UnPaquetSansSsrcNEstPasLu)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 1, 206, 0); //PSFB/PLI annoncé en 4 octets

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// PLI reste le feedback le plus courant : il doit continuer de passer.
TEST(RtcpPayloadFeedback, UnePliValideResteParsee)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, RTCPPayloadFeedback::PictureLossIndication, 206, 2);
	Push32(pkt, 0x44444444);
	Push32(pkt, 0x55555555);

	RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(pkt.data(), pkt.size());
	ASSERT_TRUE(rtcp != NULL);
	ASSERT_EQ(1u, rtcp->GetPacketCount());
	RTCPPayloadFeedback* fb = (RTCPPayloadFeedback*)rtcp->GetPacket(0);
	EXPECT_EQ(RTCPPayloadFeedback::PictureLossIndication, fb->GetFeedbackType());
	EXPECT_EQ(0x44444444u, fb->GetSenderSSRC());
	EXPECT_EQ(0x55555555u, fb->GetMediaSSRC());
	delete rtcp;
}

// Un champ SLI fait 4 octets ; le décodage allait chercher son pictureId dans
// le cinquième.
TEST(RtcpPayloadFeedback, UnChampSliNeLitPasUnCinquiemeOctet)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, RTCPPayloadFeedback::SliceLossIndication, 206, 3);
	Push32(pkt, 0x66666666);
	Push32(pkt, 0x77777777);
	Push32(pkt, 0x89ABCDEF); //le champ SLI, seul et dernier

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// REMB est décodé dans le Dump du feedback applicatif : le nombre de SSRC
// annoncé y sert de borne de boucle sans confrontation à la taille du champ.
TEST(RtcpPayloadFeedback, UnRembMenteurNeFaitPasLireHorsDuChamp)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, RTCPPayloadFeedback::ApplicationLayerFeeedbackMessage, 206, 4);
	Push32(pkt, 0x88888888);
	Push32(pkt, 0x99999999);
	pkt.push_back('R'); pkt.push_back('E'); pkt.push_back('M'); pkt.push_back('B');
	pkt.push_back(255);  //« 255 SSRC suivent »
	pkt.push_back(0x00); //exposant/mantisse
	pkt.push_back(0x01);
	pkt.push_back(0x00); //...et rien derrière

	GuardedBuffer guarded(pkt.data(), pkt.size());
	ASSERT_TRUE(guarded.IsValid());
	EXPECT_EXIT({
		RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(guarded.data(), (DWORD)guarded.Size());
		if (rtcp)
		{
			//Le Dump est le chemin qui décode REMB.
			rtcp->Dump();
			delete rtcp;
		}
		_exit(0);
	}, ::testing::ExitedWithCode(0), "");
}

// ---------------------------------------------------------------------------
// FIR / NACK (types 192 et 193 : jamais premiers, mais atteignables en second)
// ---------------------------------------------------------------------------

TEST(RtcpFullIntraRequest, UnPaquetSansSsrcNEstPasLu)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 0, 192, 0);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

TEST(RtcpNack, UnPaquetTropCourtNEstPasLu)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 0, 193, 0);

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

// L'aller-retour prouve que les champs sont aux bons décalages : NACK plaçait
// son BLP à l'offset 2 en lecture et à l'offset 6 en écriture.
TEST(RtcpNack, LesChampsSeRelisentLaOuIlsSontEcrits)
{
	RTCPNACK nack;
	nack.SetSSRC(0x0A0B0C0D);
	nack.SetFSN(0x1234);
	nack.SetBLP(0x5678);

	BYTE buffer[64];
	memset(buffer, 0, sizeof(buffer));
	DWORD len = nack.Serialize(buffer, sizeof(buffer));
	ASSERT_GT(len, 0u);

	RTCPNACK relu;
	ASSERT_GT(relu.Parse(buffer, len), 0u);
	EXPECT_EQ(0x0A0B0C0Du, relu.GetSSRC());
	EXPECT_EQ(0x1234, relu.GetFSN());
	EXPECT_EQ(0x5678, relu.GetBLP());
}

// ---------------------------------------------------------------------------
// SDES
// ---------------------------------------------------------------------------

TEST(RtcpSdes, UneDescriptionTronqueeNEstPasLue)
{
	std::vector<BYTE> pkt;
	PushValidReceiverReport(pkt);
	PushHeader(pkt, 1, 202, 2); //12 octets annoncés
	Push32(pkt, 0xABCDEF01);    //ssrc de la description
	pkt.push_back(1);           //item CNAME
	pkt.push_back(200);         //« 200 octets »
	pkt.push_back('a');         //...et il y en a deux
	pkt.push_back('b');

	EXPECT_PARSE_NE_DEBORDE_PAS(pkt);
}

TEST(RtcpSdes, UnCnameValideEstLu)
{
	std::vector<BYTE> pkt;
	PushHeader(pkt, 1, 202, 3); //16 octets
	Push32(pkt, 0xABCDEF01);
	pkt.push_back(1);           //CNAME
	pkt.push_back(4);
	pkt.push_back('i'); pkt.push_back('v'); pkt.push_back('e'); pkt.push_back('s');
	pkt.push_back(0);           //fin de liste
	pkt.push_back(0);           //padding jusqu'au mot de 32 bits

	RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(pkt.data(), pkt.size());
	ASSERT_TRUE(rtcp != NULL);
	ASSERT_EQ(1u, rtcp->GetPacketCount());
	RTCPSDES* sdes = (RTCPSDES*)rtcp->GetPacket(0);
	ASSERT_EQ(1u, sdes->GetDescriptionCount());
	RTCPSDES::Description* desc = sdes->GetDescription(0);
	ASSERT_EQ(1u, desc->GetItemCount());
	EXPECT_EQ(0xABCDEF01u, desc->GetSSRC());
	EXPECT_EQ(4, desc->GetItem(0)->GetSize());
	delete rtcp;
}
