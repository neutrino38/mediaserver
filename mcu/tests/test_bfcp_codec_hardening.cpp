/**
 * test_bfcp_codec_hardening.cpp — suite ADVERSE du codec BFCP.
 *
 * Le parseur BFCP lit du réseau : un endpoint hostile, ou simplement cassé,
 * choisit chaque octet. Ces tests décrivent ce qu'il ne doit JAMAIS faire, et
 * ils le vérifient sur deux registres :
 *
 *   1. refuser proprement — Parse rend NULL, sans planter, sans boucler ;
 *   2. ne pas lire un octet de trop — le message est placé contre une page
 *      interdite (guardedbuffer.h), donc tout débordement devient un SIGSEGV,
 *      et le test tourne dans un EXPECT_EXIT pour que seul lui tombe.
 *
 * Conception : docs/conception/BFCP-INTERNE/SPEC.md §4.1, lot 1. Modèle :
 * test_rtcp_hardening.cpp, même technique, même doctrine (TEST.md).
 *
 * Deux garde-fous ferment la suite : un message honnête reste décodé. Sans eux
 * un parseur qui refuserait tout passerait cette suite entière.
 */
#include <gtest/gtest.h>

#include <vector>

#include "guardedbuffer.h"
#include "bfcp/BFCPMessage.h"
#include "bfcp/messages.h"
#include "bfcp/attributes.h"

namespace {

typedef std::vector<BYTE> Bytes;

// Un en-tête commun bien formé, dont l'appelant fixe la primitive et le nombre
// de mots de charge utile.
void PushHeader(Bytes& out, BFCPMessage::Primitive primitive, int words)
{
	out.push_back(0x20);			// Ver=1
	out.push_back((BYTE)primitive);
	out.push_back((BYTE)(words >> 8));
	out.push_back((BYTE)(words & 0xff));
	for (int i=0; i<4; i++)
		out.push_back(0x11);		// Conference ID
	out.push_back(0x00); out.push_back(0x01);	// Transaction ID
	out.push_back(0x00); out.push_back(0x02);	// User ID
}

void PushAttr(Bytes& out, int type, bool mandatory, int length, const Bytes& contents)
{
	out.push_back((BYTE)((type << 1) | (mandatory ? 1 : 0)));
	out.push_back((BYTE)length);
	for (size_t i=0; i<contents.size(); i++)
		out.push_back(contents[i]);
	while (out.size() % 4)
		out.push_back(0x00);
}

} // namespace


// Joue le parseur derrière la page de garde : un octet lu en trop tue le fils,
// et le test échoue avec le code de sortie.
#define EXPECT_PARSE_NE_DEBORDE_PAS(paquet)                                   \
	do {                                                                  \
		GuardedBuffer guarded((paquet).data(), (paquet).size());      \
		ASSERT_TRUE(guarded.IsValid());                               \
		EXPECT_EXIT({                                                 \
			BFCPMessage* msg = BFCPMessage::Parse(guarded.data(), guarded.Size()); \
			delete msg;                                           \
			_exit(0);                                             \
		}, ::testing::ExitedWithCode(0), "");                         \
	} while (0)

#define EXPECT_REFUSE(paquet) \
	EXPECT_EQ(nullptr, BFCPMessage::Parse((paquet).data(), (paquet).size()))


// ---- L'en-tête commun -------------------------------------------------------

TEST(BfcpHeaderHardening, UnEnteteTronqueEstRefuse)
{
	// Toutes les longueurs sous les 12 octets réglementaires.
	for (size_t len = 0; len < BFCPMessage::HeaderLen; len++) {
		Bytes packet;
		PushHeader(packet, BFCPMessage::Hello, 0);
		packet.resize(len);

		EXPECT_REFUSE(packet) << "en-tete de " << len << " octets";
		EXPECT_PARSE_NE_DEBORDE_PAS(packet);
	}
}

TEST(BfcpHeaderHardening, UneLongueurDeChargeMenteuseEstRefusee)
{
	// L'en-tête annonce 100 mots de 4 octets, le datagramme n'en porte aucun.
	// Un parseur qui croit l'en-tête lit 400 octets qui ne lui appartiennent pas.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 100);

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpHeaderHardening, UneLongueurDeChargeAUnMotPresEstRefusee)
{
	// Le cas limite : un mot de trop. C'est celui qu'un test grossier rate.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 2);
	PushAttr(packet, BFCPAttribute::FloorId, true, 4, {0x00, 0x01});

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpHeaderHardening, UneVersionInconnueEstRefusee)
{
	// 1 et 2 seulement. Le 0 et les versions futures sont jetés.
	for (int version = 0; version < 8; version++) {
		if (version == BFCPMessage::VersionReliable || version == BFCPMessage::VersionUnreliable)
			continue;

		Bytes packet;
		PushHeader(packet, BFCPMessage::Hello, 0);
		packet[0] = (BYTE)(version << 5);

		EXPECT_REFUSE(packet) << "version " << version;
	}
}

TEST(BfcpHeaderHardening, UnMessageFragmenteEstJete)
{
	// On ne réassemble pas : le drapeau F fait jeter le message plutôt que
	// lire un en-tête de 12 octets là où il y en a 20.
	Bytes packet;
	PushHeader(packet, BFCPMessage::Hello, 0);
	packet[0] |= 0x08;

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}


// ---- Les TLV d'attributs ----------------------------------------------------

TEST(BfcpAttrHardening, UneLongueurNulleNeFaitPasBouclerLeParseur)
{
	// Length = 0 ne couvre même pas l'en-tête du TLV : un curseur qui avance
	// de Length reste sur place, et la boucle ne finit jamais. Ce test se
	// termine, ou il ne se termine pas.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 1);
	packet.push_back((BYTE)(BFCPAttribute::FloorId << 1) | 1);
	packet.push_back(0x00);			// Length = 0
	packet.push_back(0x00);
	packet.push_back(0x01);

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UneLongueurDUnSeulOctetNeFaitPasBouclerLeParseur)
{
	// Length = 1 : sous les 2 octets de son propre en-tête. Même piège.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 1);
	packet.push_back((BYTE)(BFCPAttribute::FloorId << 1) | 1);
	packet.push_back(0x01);
	packet.push_back(0x00);
	packet.push_back(0x01);

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnAttributPlusLongQueLaChargeEstRefuse)
{
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 1);
	packet.push_back((BYTE)(BFCPAttribute::ParticipantProvidedInfo << 1));
	packet.push_back(0xff);			// 255 octets annoncés, 2 disponibles
	packet.push_back('a');
	packet.push_back('b');

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnEnteteDAttributACheValSurLaFinEstRefuse)
{
	// Un seul octet de reste : trop peu même pour un en-tête de TLV. Le
	// parseur ne doit pas lire l'octet de longueur qui n'existe pas.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 2);
	PushAttr(packet, BFCPAttribute::FloorId, true, 4, {0x00, 0x01});
	packet.push_back(0x05);
	packet.resize(packet.size());

	// La charge annoncée est de 2 mots, on en fournit 1 + 1 octet.
	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnAttributEntierTropCourtEstRefuse)
{
	// FLOOR-ID annonce Length 3 : un seul octet de contenu là où il en faut 2.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 1);
	packet.push_back((BYTE)(BFCPAttribute::FloorId << 1) | 1);
	packet.push_back(0x03);
	packet.push_back(0x00);
	packet.push_back(0x00);			// bourrage

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnGroupeTropCourtPourSonIdentifiantEstRefuse)
{
	// FLOOR-REQUEST-INFORMATION doit porter au moins ses 2 octets d'id.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequestStatus, 1);
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestInformation << 1) | 1);
	packet.push_back(0x03);
	packet.push_back(0x00);
	packet.push_back(0x00);

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnEnfantQuiDebordeSonGroupeEstRefuse)
{
	// Le groupe dit 12 octets, son enfant en annonce 200 : l'enfant ne doit
	// pas être lu au-delà de son parent, ni le parent au-delà du message.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequestStatus, 3);
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestInformation << 1) | 1);
	packet.push_back(0x0c);			// Length 12
	packet.push_back(0x00); packet.push_back(0x09);
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestStatus << 1) | 1);
	packet.push_back(0xc8);			// 200 octets dans un groupe de 12
	packet.push_back(0x00); packet.push_back(0x01);
	packet.push_back(0x0a); packet.push_back(0x04);
	packet.push_back(0x03); packet.push_back(0x00);

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnGroupeQuiSeContientLuiMemeNeFaitPasBoucler)
{
	// Un groupe dont l'enfant est du même type et de la même taille : un
	// parseur naïf récurse sans fin. La longueur décroît à chaque niveau, donc
	// la descente doit s'arrêter.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequestStatus, 2);
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestInformation << 1) | 1);
	packet.push_back(0x08);
	packet.push_back(0x00); packet.push_back(0x09);
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestInformation << 1) | 1);
	packet.push_back(0x04);
	packet.push_back(0x00); packet.push_back(0x09);

	// Ce qui compte est que Parse RENDE la main.
	BFCPMessage* msg = BFCPMessage::Parse(packet.data(), packet.size());
	delete msg;
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}

TEST(BfcpAttrHardening, UnTexteNEstPasLuAuDelaDeSaLongueur)
{
	// STATUS-INFO sans NUL final : un parseur qui ferait strlen() sortirait
	// du tampon. C'est le test que la page de garde rend concluant.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequestStatus, 5);
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestInformation << 1) | 1);
	packet.push_back(0x14);			// Length 20
	packet.push_back(0x00); packet.push_back(0x09);
	packet.push_back((BYTE)(BFCPAttribute::OverallRequestStatus << 1));
	packet.push_back(0x0c);			// 2 d'en-tete + 2 d'id + 8 de STATUS-INFO
	packet.push_back(0x00); packet.push_back(0x09);
	packet.push_back((BYTE)(BFCPAttribute::StatusInfo << 1));
	packet.push_back(0x06);			// 2 + 4 caracteres, bourrage exclu
	packet.push_back('a'); packet.push_back('b');
	packet.push_back('c'); packet.push_back('d');
	packet.push_back(0x00); packet.push_back(0x00);	// le bourrage, lui, est sur le fil
	packet.push_back((BYTE)(BFCPAttribute::FloorRequestStatus << 1) | 1);
	packet.push_back(0x04);
	packet.push_back(0x00); packet.push_back(0x01);

	EXPECT_PARSE_NE_DEBORDE_PAS(packet);

	BFCPMessage* msg = BFCPMessage::Parse(packet.data(), packet.size());
	ASSERT_NE(nullptr, msg);
	BFCPMsgFloorRequestStatus* status = (BFCPMsgFloorRequestStatus*)msg;
	ASSERT_NE(nullptr, status->GetFloorRequestInformation());
	ASSERT_NE(nullptr, status->GetFloorRequestInformation()->GetOverallRequestStatus());
	ASSERT_NE(nullptr, status->GetFloorRequestInformation()->GetOverallRequestStatus()->GetStatusInfo());
	EXPECT_EQ("abcd", status->GetFloorRequestInformation()->GetOverallRequestStatus()->GetStatusInfo()->GetValue());
	delete msg;
}

TEST(BfcpAttrHardening, UnBourrageManquantEnFinDeMessageEstRefuse)
{
	// Length 5 occupe 8 octets avec son bourrage. Un message qui s'arrête à 6
	// ment sur sa propre longueur de charge : il est refusé, pas complété.
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequestStatus, 1);
	packet.push_back((BYTE)(BFCPAttribute::ParticipantProvidedInfo << 1));
	packet.push_back(0x05);
	packet.push_back('a'); packet.push_back('b');

	EXPECT_REFUSE(packet);
	EXPECT_PARSE_NE_DEBORDE_PAS(packet);
}


// ---- Toutes les primitives, sur des octets aléatoires ----------------------

TEST(BfcpFuzzHardening, ChaquePrimitiveSurviteAUneChargeQuelconque)
{
	// Un balayage grossier : pour chaque primitive, une charge faite d'octets
	// qui n'ont aucun sens. Rien ne doit planter ni boucler.
	for (int primitive = 1; primitive <= 17; primitive++) {
		for (int seed = 0; seed < 16; seed++) {
			Bytes packet;
			PushHeader(packet, (BFCPMessage::Primitive)primitive, 2);
			for (int i=0; i<8; i++)
				packet.push_back((BYTE)((seed * 37 + i * 61 + primitive * 13) & 0xff));

			BFCPMessage* msg = BFCPMessage::Parse(packet.data(), packet.size());
			delete msg;
		}
	}
}

TEST(BfcpFuzzHardening, UnMessageTronqueAToutesLesLongueursNeDeborde)
{
	// Un FloorRequestStatus complet, coupé à chaque longueur possible. C'est
	// le cas d'un TCP qui livre une lecture partielle prise pour un message.
	BFCPMsgFloorRequestStatus status(0x1234, 0x11111111, 0x0002);
	BFCPAttrFloorRequestInformation* info = new BFCPAttrFloorRequestInformation(9);
	BFCPAttrOverallRequestStatus* overall = new BFCPAttrOverallRequestStatus(9);
	overall->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Granted, 0));
	overall->SetStatusInfo(new BFCPAttrStatusInfo("granted"));
	info->SetOverallRequestStatus(overall);
	BFCPAttrFloorRequestStatus* perFloor = new BFCPAttrFloorRequestStatus(1);
	perFloor->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Granted, 0));
	info->AddFloorRequestStatus(perFloor);
	info->SetBeneficiaryInformation(new BFCPAttrBeneficiaryInformation(2));
	info->SetRequestedByInformation(new BFCPAttrRequestedByInformation(2));
	status.SetFloorRequestInformation(info);

	BYTE buffer[512];
	const size_t full = status.Serialize(buffer, sizeof(buffer));
	ASSERT_NE(BFCPMessage::Failed, full);

	for (size_t len = 0; len < full; len++) {
		BFCPMessage* msg = BFCPMessage::Parse(buffer, len);
		// Tronqué : il n'y a rien de bon à en tirer.
		EXPECT_EQ(nullptr, msg) << "tronque a " << len << " octets sur " << full;
		delete msg;
	}

	// Entier, il passe : c'est le garde-fou de ce test.
	BFCPMessage* whole = BFCPMessage::Parse(buffer, full);
	EXPECT_NE(nullptr, whole);
	delete whole;
}


// ---- Garde-fous : un message honnête reste décodé --------------------------

TEST(BfcpHardeningGuard, UnFloorRequestValideResteParse)
{
	Bytes packet;
	PushHeader(packet, BFCPMessage::FloorRequest, 1);
	PushAttr(packet, BFCPAttribute::FloorId, true, 4, {0x00, 0x01});

	BFCPMessage* msg = BFCPMessage::Parse(packet.data(), packet.size());
	ASSERT_NE(nullptr, msg);
	EXPECT_EQ(BFCPMessage::FloorRequest, msg->GetPrimitive());
	EXPECT_EQ(1, ((BFCPMsgFloorRequest*)msg)->CountFloorIds());
	delete msg;
}

TEST(BfcpHardeningGuard, UnHelloValideResteParse)
{
	Bytes packet;
	PushHeader(packet, BFCPMessage::Hello, 0);

	BFCPMessage* msg = BFCPMessage::Parse(packet.data(), packet.size());
	ASSERT_NE(nullptr, msg);
	EXPECT_EQ(BFCPMessage::Hello, msg->GetPrimitive());
	EXPECT_EQ(0x11111111, msg->GetConferenceId());
	EXPECT_EQ(1, msg->GetTransactionId());
	EXPECT_EQ(2, msg->GetUserId());
	delete msg;
}
