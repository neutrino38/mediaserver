/**
 * test_bfcp_codec.cpp — le codec binaire BFCP (RFC 4582 §5, RFC 8855 §5) :
 * l'en-tête commun, les TLV d'attributs, et l'aller-retour de chaque primitive.
 *
 * Conception : docs/conception/BFCP-INTERNE/SPEC.md §4.1, lot 1.
 *
 * Trois messages sont comparés à des octets ÉCRITS À LA MAIN depuis la RFC,
 * pas produits par le code sous test : un FloorRequest, un FloorRequestStatus
 * complet, un HelloAck. C'est la seule façon de prouver qu'on parle bien BFCP
 * et pas un dialecte cohérent avec lui-même. Le reste est un aller-retour, qui
 * ne prouve que la symétrie.
 *
 * La robustesse du parseur face à des octets hostiles est ailleurs :
 * test_bfcp_codec_hardening.cpp.
 */
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "bfcp/BFCPMessage.h"
#include "bfcp/messages.h"
#include "bfcp/attributes.h"

namespace {

typedef std::vector<BYTE> Bytes;

Bytes Serialize(const BFCPMessage& msg)
{
	BYTE buffer[4096];
	const size_t n = msg.Serialize(buffer, sizeof(buffer));
	if (n == BFCPMessage::Failed)
		return Bytes();
	return Bytes(buffer, buffer + n);
}

// Rend la primitive attendue, ou nullptr. Le test possède ce qu'il reçoit.
template <class T>
T* ParseAs(const Bytes& bytes, BFCPMessage::Primitive expected)
{
	BFCPMessage* msg = BFCPMessage::Parse(bytes.data(), bytes.size());
	if (! msg)
		return nullptr;
	if (msg->GetPrimitive() != expected) {
		delete msg;
		return nullptr;
	}
	return static_cast<T*>(msg);
}

std::string Hex(const Bytes& b)
{
	static const char* digits = "0123456789abcdef";
	std::string out;
	for (size_t i=0; i<b.size(); i++) {
		if (i && i % 4 == 0)
			out += ' ';
		out += digits[b[i] >> 4];
		out += digits[b[i] & 0x0f];
	}
	return out;
}

const int CONF	= 0x12345678;
const int TRANS	= 0x1234;
const int USER	= 0x5678;

} // namespace


// ---- L'en-tête commun -------------------------------------------------------

TEST(BfcpHeader, LEnteteEstCeluiDeLaRfc)
{
	// Hello : aucun attribut, donc l'en-tête seul, et rien après lui.
	BFCPMsgHello hello(TRANS, CONF, USER);
	const Bytes wire = Serialize(hello);

	const Bytes expected = {
		0x20,			// Ver=1, R=0, F=0, Res=0
		0x0b,			// Primitive = Hello (11)
		0x00, 0x00,		// Payload Length = 0 mots de 4 octets
		0x12, 0x34, 0x56, 0x78,	// Conference ID
		0x12, 0x34,		// Transaction ID
		0x56, 0x78		// User ID
	};

	EXPECT_EQ(Hex(expected), Hex(wire));
	EXPECT_EQ(12u, wire.size()) << "l'en-tete commun fait 12 octets";
}

TEST(BfcpHeader, LaVersion2PoseLeDrapeauReponseur)
{
	// RFC 8855 : version 2 sur transport non fiable, et le bit R sur une
	// réponse. Le premier octet vaut alors 0x50, pas le 0x21 de libbfcp.
	BFCPMsgHello hello(TRANS, CONF, USER);
	hello.SetVersion(BFCPMessage::VersionUnreliable);
	hello.SetResponder(true);
	const Bytes wire = Serialize(hello);

	ASSERT_FALSE(wire.empty());
	EXPECT_EQ(0x50, wire[0]);

	std::unique_ptr<BFCPMessage> back(ParseAs<BFCPMessage>(wire, BFCPMessage::Hello));
	ASSERT_NE(nullptr, back.get());
	EXPECT_EQ(BFCPMessage::VersionUnreliable, back->GetVersion());
	EXPECT_TRUE(back->IsResponder());
}

TEST(BfcpHeader, LaVersion1NePoseAucunDrapeau)
{
	BFCPMsgHello hello(TRANS, CONF, USER);
	const Bytes wire = Serialize(hello);

	ASSERT_FALSE(wire.empty());
	EXPECT_EQ(0x20, wire[0]);

	std::unique_ptr<BFCPMessage> back(ParseAs<BFCPMessage>(wire, BFCPMessage::Hello));
	ASSERT_NE(nullptr, back.get());
	EXPECT_EQ(BFCPMessage::VersionReliable, back->GetVersion());
	EXPECT_FALSE(back->IsResponder());
}

TEST(BfcpHeader, LaLongueurEstEnMotsDeQuatreOctets)
{
	BFCPMsgFloorRequest req(TRANS, CONF, USER);
	req.AddFloorId(1);
	const Bytes wire = Serialize(req);

	ASSERT_EQ(16u, wire.size()) << "12 d'en-tete + un FLOOR-ID de 4";
	EXPECT_EQ(0x00, wire[2]);
	EXPECT_EQ(0x01, wire[3]) << "un seul mot de 4 octets";
}


// ---- Les octets d'un FloorRequest, écrits depuis la RFC ---------------------

TEST(BfcpCodec, UnFloorRequestEstCeluiDeLaRfc)
{
	BFCPMsgFloorRequest req(TRANS, CONF, USER);
	req.AddFloorId(1);
	req.SetBeneficiaryId(0x4321);

	const Bytes expected = {
		0x20, 0x01, 0x00, 0x02,		// Ver=1, FloorRequest, 2 mots
		0x12, 0x34, 0x56, 0x78,		// Conference ID
		0x12, 0x34, 0x56, 0x78,		// Transaction ID, User ID
		// FLOOR-ID : type 2 -> (2<<1)|M=1 = 0x05, Length 4, valeur 1
		0x05, 0x04, 0x00, 0x01,
		// BENEFICIARY-ID : type 1 -> (1<<1)|M=0 = 0x02, Length 4
		0x02, 0x04, 0x43, 0x21
	};

	EXPECT_EQ(Hex(expected), Hex(Serialize(req)));
}

TEST(BfcpCodec, UnAttributTexteEstBourreSansCompterLeBourrage)
{
	BFCPMsgFloorRequest req(TRANS, CONF, USER);
	req.AddFloorId(1);
	req.SetParticipantProvidedInfo("abc");

	const Bytes wire = Serialize(req);
	ASSERT_EQ(24u, wire.size());

	// PARTICIPANT-PROVIDED-INFO : type 8 -> (8<<1)|0 = 0x10.
	// Length = 2 d'en-tete + 3 de texte = 5, et un octet de bourrage suit,
	// qui n'est PAS compte par Length.
	EXPECT_EQ(0x10, wire[16]);
	EXPECT_EQ(0x05, wire[17]) << "Length exclut le bourrage";
	EXPECT_EQ('a', wire[18]);
	EXPECT_EQ('c', wire[20]);
	EXPECT_EQ(0x00, wire[21]) << "le bourrage est a zero";

	std::unique_ptr<BFCPMsgFloorRequest> back(ParseAs<BFCPMsgFloorRequest>(wire, BFCPMessage::FloorRequest));
	ASSERT_NE(nullptr, back.get());
	EXPECT_EQ("abc", back->GetParticipantProvidedInfo()) << "le bourrage n'entre pas dans le texte";
}

TEST(BfcpCodec, UnCodeDErreurTientSurTroisOctetsPlusUnDeBourrage)
{
	BFCPMsgError error(TRANS, CONF, USER);
	error.SetErrorCode(BFCPAttrErrorCode::UnknownPrimitive);

	const Bytes expected = {
		0x20, 0x0d, 0x00, 0x01,		// Ver=1, Error (13), 1 mot
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		// ERROR-CODE : type 6 -> (6<<1)|M=1 = 0x0d, Length 3, code 3, bourrage
		0x0d, 0x03, 0x03, 0x00
	};

	EXPECT_EQ(Hex(expected), Hex(Serialize(error)));
}


// ---- Les octets d'un FloorRequestStatus complet ----------------------------

TEST(BfcpCodec, UnFloorRequestStatusCompletEstCeluiDeLaRfc)
{
	// Ce que le serveur envoie réellement à l'octroi : une
	// FLOOR-REQUEST-INFORMATION qui groupe OVERALL-REQUEST-STATUS, une
	// FLOOR-REQUEST-STATUS par floor, BENEFICIARY et REQUESTED-BY.
	BFCPMsgFloorRequestStatus status(TRANS, CONF, USER);

	BFCPAttrFloorRequestInformation* info = new BFCPAttrFloorRequestInformation(9);

	BFCPAttrOverallRequestStatus* overall = new BFCPAttrOverallRequestStatus(9);
	overall->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Granted, 0));
	info->SetOverallRequestStatus(overall);

	BFCPAttrFloorRequestStatus* perFloor = new BFCPAttrFloorRequestStatus(1);
	perFloor->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Granted, 0));
	info->AddFloorRequestStatus(perFloor);

	info->SetBeneficiaryInformation(new BFCPAttrBeneficiaryInformation(0x5678));
	info->SetRequestedByInformation(new BFCPAttrRequestedByInformation(0x5678));

	status.SetFloorRequestInformation(info);

	const Bytes expected = {
		0x20, 0x04, 0x00, 0x07,		// Ver=1, FloorRequestStatus (4), 7 mots
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		// FLOOR-REQUEST-INFORMATION : type 15 -> (15<<1)|M=1 = 0x1f
		// Length = 4 (en-tete + id) + 8 (overall) + 8 (per floor) + 4 + 4 = 28
		0x1f, 0x1c, 0x00, 0x09,
		//   OVERALL-REQUEST-STATUS : type 18 -> (18<<1)|0 = 0x24, Length 8
		0x24, 0x08, 0x00, 0x09,
		//     REQUEST-STATUS : type 5 -> (5<<1)|0 = 0x0a, Length 4, Granted (3), file 0
		0x0a, 0x04, 0x03, 0x00,
		//   FLOOR-REQUEST-STATUS : type 17 -> (17<<1)|M=1 = 0x23, Length 8, floor 1
		0x23, 0x08, 0x00, 0x01,
		//     REQUEST-STATUS
		0x0a, 0x04, 0x03, 0x00,
		//   BENEFICIARY-INFORMATION : type 14 -> (14<<1)|0 = 0x1c, Length 4
		0x1c, 0x04, 0x56, 0x78,
		//   REQUESTED-BY-INFORMATION : type 16 -> (16<<1)|0 = 0x20, Length 4
		0x20, 0x04, 0x56, 0x78
	};

	EXPECT_EQ(Hex(expected), Hex(Serialize(status)));
}

TEST(BfcpCodec, UnFloorRequestStatusRevientEntier)
{
	BFCPMsgFloorRequestStatus status(TRANS, CONF, USER);
	BFCPAttrFloorRequestInformation* info = new BFCPAttrFloorRequestInformation(9);

	BFCPAttrOverallRequestStatus* overall = new BFCPAttrOverallRequestStatus(9);
	overall->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Denied, 2));
	overall->SetStatusInfo(new BFCPAttrStatusInfo("nope"));
	info->SetOverallRequestStatus(overall);

	BFCPAttrFloorRequestStatus* perFloor = new BFCPAttrFloorRequestStatus(1);
	perFloor->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Denied, 2));
	info->AddFloorRequestStatus(perFloor);

	info->SetBeneficiaryInformation(new BFCPAttrBeneficiaryInformation(0x1111));
	info->SetRequestedByInformation(new BFCPAttrRequestedByInformation(0x2222));
	status.SetFloorRequestInformation(info);

	const Bytes wire = Serialize(status);
	ASSERT_FALSE(wire.empty());

	std::unique_ptr<BFCPMsgFloorRequestStatus> back(ParseAs<BFCPMsgFloorRequestStatus>(wire, BFCPMessage::FloorRequestStatus));
	ASSERT_NE(nullptr, back.get());
	EXPECT_EQ(TRANS, back->GetTransactionId());
	EXPECT_EQ(CONF, back->GetConferenceId());
	EXPECT_EQ(USER, back->GetUserId());

	const BFCPAttrFloorRequestInformation* got = back->GetFloorRequestInformation();
	ASSERT_NE(nullptr, got);
	EXPECT_EQ(9, got->GetFloorRequestId());

	ASSERT_NE(nullptr, got->GetOverallRequestStatus());
	ASSERT_NE(nullptr, got->GetOverallRequestStatus()->GetRequestStatus());
	EXPECT_EQ(BFCPAttrRequestStatus::Denied, got->GetOverallRequestStatus()->GetRequestStatus()->GetStatus());
	EXPECT_EQ(2, got->GetOverallRequestStatus()->GetRequestStatus()->GetQueuePosition());
	ASSERT_NE(nullptr, got->GetOverallRequestStatus()->GetStatusInfo());
	EXPECT_EQ("nope", got->GetOverallRequestStatus()->GetStatusInfo()->GetValue());

	ASSERT_EQ(1, got->CountFloorRequestStatuses());
	EXPECT_EQ(1, got->GetFloorRequestStatus(0)->GetFloorId());
	ASSERT_NE(nullptr, got->GetFloorRequestStatus(0)->GetRequestStatus());
	EXPECT_EQ(BFCPAttrRequestStatus::Denied, got->GetFloorRequestStatus(0)->GetRequestStatus()->GetStatus());

	ASSERT_NE(nullptr, got->GetBeneficiaryInformation());
	EXPECT_EQ(0x1111, got->GetBeneficiaryInformation()->GetBeneficiaryId());
	ASSERT_NE(nullptr, got->GetRequestedByInformation());
	EXPECT_EQ(0x2222, got->GetRequestedByInformation()->GetRequestedById());
}


// ---- Les octets d'un HelloAck ----------------------------------------------

TEST(BfcpCodec, UnHelloAckEstCeluiDeLaRfc)
{
	BFCPMsgHelloAck ack(TRANS, CONF, USER);
	ack.AddSupportedPrimitive(BFCPMessage::FloorRequest);
	ack.AddSupportedPrimitive(BFCPMessage::FloorRelease);
	ack.AddSupportedAttribute(BFCPAttribute::BeneficiaryId);
	ack.AddSupportedAttribute(BFCPAttribute::FloorId);

	const Bytes expected = {
		0x20, 0x0c, 0x00, 0x02,		// Ver=1, HelloAck (12), 2 mots
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		// SUPPORTED-PRIMITIVES : type 11 -> (11<<1)|M=1 = 0x17, Length 4.
		// Une primitive est un octet PLEIN : 1 et 2.
		0x17, 0x04, 0x01, 0x02,
		// SUPPORTED-ATTRIBUTES : type 10 -> (10<<1)|M=1 = 0x15, Length 4.
		// Un type d'attribut tient sur 7 bits : il est DECALE. 1<<1, 2<<1.
		0x15, 0x04, 0x02, 0x04
	};

	EXPECT_EQ(Hex(expected), Hex(Serialize(ack)));
}

TEST(BfcpCodec, LesDeuxListesNeSeCodentPasPareil)
{
	// Le piège du HelloAck : une primitive est un octet plein, un type
	// d'attribut est décalé d'un bit. Confondre les deux passe inaperçu tant
	// qu'on ne compare pas les octets.
	BFCPMsgHelloAck ack(TRANS, CONF, USER);
	ack.AddSupportedPrimitive(BFCPMessage::Goodbye);		// 16
	ack.AddSupportedAttribute(BFCPAttribute::OverallRequestStatus);	// 18

	const Bytes wire = Serialize(ack);
	ASSERT_EQ(20u, wire.size());
	EXPECT_EQ(0x10, wire[14]) << "Goodbye = 16, tel quel";
	EXPECT_EQ(0x24, wire[18]) << "OVERALL-REQUEST-STATUS = 18, decale d'un bit";

	std::unique_ptr<BFCPMsgHelloAck> back(ParseAs<BFCPMsgHelloAck>(wire, BFCPMessage::HelloAck));
	ASSERT_NE(nullptr, back.get());
	EXPECT_TRUE(back->GetSupportedPrimitives().Has(BFCPMessage::Goodbye));
	EXPECT_TRUE(back->GetSupportedAttributes().Has(BFCPAttribute::OverallRequestStatus));
}


// ---- Aller-retour des autres primitives ------------------------------------

TEST(BfcpCodec, UnFloorRequestRevientEntier)
{
	BFCPMsgFloorRequest req(TRANS, CONF, USER);
	req.AddFloorId(1);
	req.AddFloorId(2);
	req.SetBeneficiaryId(0x4321);
	req.SetParticipantProvidedInfo("partage d'ecran");

	std::unique_ptr<BFCPMsgFloorRequest> back(ParseAs<BFCPMsgFloorRequest>(Serialize(req), BFCPMessage::FloorRequest));
	ASSERT_NE(nullptr, back.get());
	ASSERT_EQ(2, back->CountFloorIds());
	EXPECT_EQ(1, back->GetFloorId(0));
	EXPECT_EQ(2, back->GetFloorId(1));
	ASSERT_TRUE(back->HasBeneficiaryId());
	EXPECT_EQ(0x4321, back->GetBeneficiaryId());
	ASSERT_TRUE(back->HasParticipantProvidedInfo());
	EXPECT_EQ("partage d'ecran", back->GetParticipantProvidedInfo());
}

TEST(BfcpCodec, UnFloorRequestSansFloorIdEstRefuseAuParsing)
{
	// La grammaire impose 1*(FLOOR-ID) : un message qui n'en porte aucun n'est
	// pas un FloorRequest, et le parseur ne doit pas en rendre un.
	const Bytes wire = {
		0x20, 0x01, 0x00, 0x01,
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		0x02, 0x04, 0x43, 0x21		// BENEFICIARY-ID seul
	};

	EXPECT_EQ(nullptr, BFCPMessage::Parse(wire.data(), wire.size()));
}

TEST(BfcpCodec, UnFloorReleaseRevientEntier)
{
	BFCPMsgFloorRelease rel(TRANS, CONF, USER);
	rel.SetFloorRequestId(42);

	std::unique_ptr<BFCPMsgFloorRelease> back(ParseAs<BFCPMsgFloorRelease>(Serialize(rel), BFCPMessage::FloorRelease));
	ASSERT_NE(nullptr, back.get());
	EXPECT_EQ(42, back->GetFloorRequestId());
}

TEST(BfcpCodec, UnFloorReleaseSansFloorRequestIdEstRefuse)
{
	const Bytes wire = {
		0x20, 0x02, 0x00, 0x00,
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78
	};

	EXPECT_EQ(nullptr, BFCPMessage::Parse(wire.data(), wire.size()));
}

TEST(BfcpCodec, UnFloorQueryRevientEntierEtPeutEtreVide)
{
	BFCPMsgFloorQuery query(TRANS, CONF, USER);
	query.AddFloorId(1);
	std::unique_ptr<BFCPMsgFloorQuery> back(ParseAs<BFCPMsgFloorQuery>(Serialize(query), BFCPMessage::FloorQuery));
	ASSERT_NE(nullptr, back.get());
	ASSERT_EQ(1, back->CountFloorIds());
	EXPECT_EQ(1, back->GetFloorId(0));

	// Le désabonnement : un FloorQuery sans aucun floor est valide.
	BFCPMsgFloorQuery empty(TRANS, CONF, USER);
	const Bytes wire = Serialize(empty);
	EXPECT_EQ(12u, wire.size());
	std::unique_ptr<BFCPMsgFloorQuery> none(ParseAs<BFCPMsgFloorQuery>(wire, BFCPMessage::FloorQuery));
	ASSERT_NE(nullptr, none.get());
	EXPECT_EQ(0, none->CountFloorIds());
}

TEST(BfcpCodec, UnFloorStatusRevientEntier)
{
	BFCPMsgFloorStatus status(TRANS, CONF, USER);
	status.SetFloorId(1);

	BFCPAttrFloorRequestInformation* info = new BFCPAttrFloorRequestInformation(7);
	BFCPAttrFloorRequestStatus* perFloor = new BFCPAttrFloorRequestStatus(1);
	perFloor->SetRequestStatus(new BFCPAttrRequestStatus(BFCPAttrRequestStatus::Granted, 0));
	info->AddFloorRequestStatus(perFloor);
	status.AddFloorRequestInformation(info);

	std::unique_ptr<BFCPMsgFloorStatus> back(ParseAs<BFCPMsgFloorStatus>(Serialize(status), BFCPMessage::FloorStatus));
	ASSERT_NE(nullptr, back.get());
	ASSERT_TRUE(back->HasFloorId());
	EXPECT_EQ(1, back->GetFloorId());
	ASSERT_EQ(1, back->CountFloorRequestInformations());
	EXPECT_EQ(7, back->GetFloorRequestInformation(0)->GetFloorRequestId());
}

TEST(BfcpCodec, UnFloorStatusVideRevientVide)
{
	BFCPMsgFloorStatus status(TRANS, CONF, USER);
	const Bytes wire = Serialize(status);
	EXPECT_EQ(12u, wire.size());

	std::unique_ptr<BFCPMsgFloorStatus> back(ParseAs<BFCPMsgFloorStatus>(wire, BFCPMessage::FloorStatus));
	ASSERT_NE(nullptr, back.get());
	EXPECT_FALSE(back->HasFloorId());
	EXPECT_EQ(0, back->CountFloorRequestInformations());
}

TEST(BfcpCodec, UneErreurRevientAvecSonTexte)
{
	BFCPMsgError error(TRANS, CONF, USER);
	error.SetErrorCode(BFCPAttrErrorCode::UnauthorizedOperation);
	error.SetErrorInfo("you cannot release that FloorRequest");

	std::unique_ptr<BFCPMsgError> back(ParseAs<BFCPMsgError>(Serialize(error), BFCPMessage::Error));
	ASSERT_NE(nullptr, back.get());
	EXPECT_EQ(BFCPAttrErrorCode::UnauthorizedOperation, back->GetErrorCode());
	ASSERT_TRUE(back->HasErrorInfo());
	EXPECT_EQ("you cannot release that FloorRequest", back->GetErrorInfo());
}

TEST(BfcpCodec, UneErreurSansCodeEstRefusee)
{
	const Bytes wire = {
		0x20, 0x0d, 0x00, 0x00,
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78
	};

	EXPECT_EQ(nullptr, BFCPMessage::Parse(wire.data(), wire.size()));
}

TEST(BfcpCodec, LesPrimitivesSansAttributRevivent)
{
	// Goodbye, GoodbyeAck et les deux acquittements ne portent rien : ils
	// doivent quand meme traverser le codec sans perdre leur en-tete.
	const BFCPMessage::Primitive bare[] = {
		BFCPMessage::Goodbye,
		BFCPMessage::GoodbyeAck,
		BFCPMessage::FloorRequestStatusAck,
		BFCPMessage::FloorStatusAck
	};

	for (size_t i=0; i<sizeof(bare)/sizeof(bare[0]); i++) {
		BFCPMessage msg(bare[i], TRANS, CONF, USER);
		const Bytes wire = Serialize(msg);
		ASSERT_EQ(12u, wire.size()) << BFCPMessage::PrimitiveName(bare[i]);

		std::unique_ptr<BFCPMessage> back(BFCPMessage::Parse(wire.data(), wire.size()));
		ASSERT_NE(nullptr, back.get()) << BFCPMessage::PrimitiveName(bare[i]);
		EXPECT_EQ(bare[i], back->GetPrimitive());
		EXPECT_EQ(TRANS, back->GetTransactionId());
		EXPECT_EQ(CONF, back->GetConferenceId());
		EXPECT_EQ(USER, back->GetUserId());
	}
}

TEST(BfcpCodec, UnePrimitiveInconnueEstRendueTelleQuelle)
{
	// Le serveur doit pouvoir répondre UnknownPrimitive : pour ça, le parseur
	// doit lui remonter le message, pas l'avaler.
	const Bytes wire = {
		0x20, 0x63, 0x00, 0x00,		// primitive 99
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78
	};

	std::unique_ptr<BFCPMessage> msg(BFCPMessage::Parse(wire.data(), wire.size()));
	ASSERT_NE(nullptr, msg.get());
	EXPECT_EQ(99, (int)msg->GetPrimitive());
	EXPECT_EQ(TRANS, msg->GetTransactionId());
}

TEST(BfcpCodec, UnAttributInconnuFacultatifEstSaute)
{
	// M a 0 : on ignore et on continue (RFC 4582 §5.2).
	const Bytes wire = {
		0x20, 0x01, 0x00, 0x02,
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		0x7e, 0x04, 0xde, 0xad,		// type 63, M=0 : inconnu, facultatif
		0x05, 0x04, 0x00, 0x01		// FLOOR-ID, M=1
	};

	std::unique_ptr<BFCPMsgFloorRequest> msg(ParseAs<BFCPMsgFloorRequest>(wire, BFCPMessage::FloorRequest));
	ASSERT_NE(nullptr, msg.get());
	ASSERT_EQ(1, msg->CountFloorIds());
	EXPECT_EQ(1, msg->GetFloorId(0));
}

TEST(BfcpCodec, UnAttributInconnuObligatoireFaitRefuserLeMessage)
{
	// M a 1 : on ne peut pas prétendre avoir compris.
	const Bytes wire = {
		0x20, 0x01, 0x00, 0x02,
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		0x05, 0x04, 0x00, 0x01,		// FLOOR-ID
		0x7f, 0x04, 0xde, 0xad		// type 63, M=1 : inconnu, obligatoire
	};

	EXPECT_EQ(nullptr, BFCPMessage::Parse(wire.data(), wire.size()));
}

TEST(BfcpCodec, UnStatutDeRequeteHorsEnumEstRefuse)
{
	// REQUEST-STATUS ne connaît que 7 valeurs : au-delà, l'attribut n'a pas de
	// sens et le message qui le porte non plus.
	const Bytes wire = {
		0x20, 0x04, 0x00, 0x03,
		0x12, 0x34, 0x56, 0x78,
		0x12, 0x34, 0x56, 0x78,
		0x1f, 0x0c, 0x00, 0x09,		// FLOOR-REQUEST-INFORMATION, Length 12
		0x23, 0x08, 0x00, 0x01,		//   FLOOR-REQUEST-STATUS, Length 8
		0x0a, 0x04, 0x63, 0x00		//     REQUEST-STATUS = 99
	};

	EXPECT_EQ(nullptr, BFCPMessage::Parse(wire.data(), wire.size()));
}
