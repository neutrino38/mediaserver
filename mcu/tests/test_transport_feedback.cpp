// Lot 6.1 (sender_bwe_plan.md) — le format de fil transport-cc (RTCP RTPFB
// fmt 15) et l'historique d'émission. Aller-retours nominaux, cas adverses de
// parsing, appariement rapport/historique avec déroulage des compteurs.

#include <gtest/gtest.h>
#include <vector>

#include "rtp.h"
#include "transportfeedback.h"
#include "sentpackethistory.h"

namespace {

// Construit l'enveloppe RTCP autour du champ, la sérialise, la reparse, et
// rend le champ reparsé. Le champ d'entrée est adopté par l'enveloppe.
bool RoundTrip(TransportWideFeedbackField* in, TransportWideFeedbackField& out)
{
	RTCPRTPFeedback* packet = RTCPRTPFeedback::Create(
		RTCPRTPFeedback::TransportWideFeedbackMessage, 0x1111, 0x2222);
	packet->AddField(in);
	BYTE buffer[2048];
	DWORD len = packet->Serialize(buffer, sizeof(buffer));
	delete packet;
	if (!len || len % 4)
		return false;
	RTCPRTPFeedback parsed;
	if (parsed.Parse(buffer, len) != len)
		return false;
	if (parsed.GetFieldCount() != 1)
		return false;
	out = *(TransportWideFeedbackField*)parsed.GetField(0);
	return true;
}

// ---------------------------------------------------------------------------
// Suite TransportFeedbackWire — le format fmt 15 lui-même.
// ---------------------------------------------------------------------------

TEST(TransportFeedbackWire, AllerRetourSimple)
{
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(100, 5000000);
	in->fbSeq = 7;
	ASSERT_TRUE(in->AddReceived(100, 5000000));
	ASSERT_TRUE(in->AddReceived(101, 5001000));	// +1 ms
	ASSERT_TRUE(in->AddReceived(102, 5001250));	// +250 us

	TransportWideFeedbackField out;
	ASSERT_TRUE(RoundTrip(in, out));
	EXPECT_EQ(100, out.baseSeq);
	EXPECT_EQ(7, out.fbSeq);
	EXPECT_TRUE(out.hasTimestamps);
	ASSERT_EQ(3u, out.packets.size());
	// Les instants se reconstruisent : référence + somme des deltas
	QWORD arrival = (QWORD)out.referenceTicks * TransportWideFeedbackField::BaseTickUs;
	QWORD expected[] = { 5000000, 5001000, 5001250 };
	for (size_t i = 0; i < out.packets.size(); ++i)
	{
		EXPECT_TRUE(out.packets[i].received);
		EXPECT_EQ((WORD)(100 + i), out.packets[i].seq);
		arrival += (QWORD)((long long)out.packets[i].deltaTicks * TransportWideFeedbackField::TickUs);
		EXPECT_EQ(expected[i], arrival) << "paquet " << i;
	}
}

TEST(TransportFeedbackWire, LesTrousDeviennentDesPertes)
{
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(10, 1000000);
	ASSERT_TRUE(in->AddReceived(10, 1000000));
	ASSERT_TRUE(in->AddReceived(14, 1002000));	// 11, 12, 13 perdus

	TransportWideFeedbackField out;
	ASSERT_TRUE(RoundTrip(in, out));
	ASSERT_EQ(5u, out.packets.size());
	EXPECT_TRUE(out.packets[0].received);
	EXPECT_FALSE(out.packets[1].received);
	EXPECT_FALSE(out.packets[2].received);
	EXPECT_FALSE(out.packets[3].received);
	EXPECT_TRUE(out.packets[4].received);
}

TEST(TransportFeedbackWire, GrandDeltaNegatif)
{
	// Une arrivée AVANT la précédente (réordonnancement) force le symbole
	// « grand delta » signé sur 16 bits.
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(1, 2000000);
	ASSERT_TRUE(in->AddReceived(1, 2000000));
	ASSERT_TRUE(in->AddReceived(2, 1995000));	// -5 ms

	TransportWideFeedbackField out;
	ASSERT_TRUE(RoundTrip(in, out));
	ASSERT_EQ(2u, out.packets.size());
	EXPECT_EQ(-5000 / TransportWideFeedbackField::TickUs, out.packets[1].deltaTicks);
}

TEST(TransportFeedbackWire, LeNumeroDeSequenceEnroule)
{
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(0xFFFE, 3000000);
	ASSERT_TRUE(in->AddReceived(0xFFFE, 3000000));
	ASSERT_TRUE(in->AddReceived(0xFFFF, 3000500));
	ASSERT_TRUE(in->AddReceived(0x0000, 3001000));
	ASSERT_TRUE(in->AddReceived(0x0001, 3001500));

	TransportWideFeedbackField out;
	ASSERT_TRUE(RoundTrip(in, out));
	ASSERT_EQ(4u, out.packets.size());
	EXPECT_EQ(0xFFFE, out.packets[0].seq);
	EXPECT_EQ(0x0001, out.packets[3].seq);
}

TEST(TransportFeedbackWire, UneLonguePlageDePertesTientDansUnRunLength)
{
	// 1000 paquets perdus entre deux reçus : l'encodage doit rester compact
	// (run-length) et l'aller-retour exact.
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(0, 4000000);
	ASSERT_TRUE(in->AddReceived(0, 4000000));
	ASSERT_TRUE(in->AddReceived(1001, 4100000));

	TransportWideFeedbackField out;
	ASSERT_TRUE(RoundTrip(in, out));
	ASSERT_EQ(1002u, out.packets.size());
	DWORD lost = 0;
	for (const TransportWideFeedbackField::PacketStatus& p : out.packets)
		if (!p.received)
			lost++;
	EXPECT_EQ(1000u, lost);
}

TEST(TransportFeedbackWire, UnRapportVideEstRefuse)
{
	// À la construction : rien à encoder.
	TransportWideFeedbackField empty;
	BYTE buffer[64];
	EXPECT_EQ(0u, empty.GetSize());
	EXPECT_EQ(0u, empty.Serialize(buffer, sizeof(buffer)));

	// Au parsing : un compte de zéro est interdit (témoin transport_feedback.cc).
	BYTE data[12] = { 0 };
	// baseSeq=5, count=0, refTime, fbSeq
	data[1] = 5;
	TransportWideFeedbackField field;
	EXPECT_EQ(0u, field.Parse(data, sizeof(data)));
}

TEST(TransportFeedbackWire, LeSymboleReserveInvalideLeRapport)
{
	// Un run-length de symbole 3 suivi d'assez d'octets : le témoin rejette.
	BYTE data[16] = { 0 };
	data[1] = 1;	// baseSeq = 1
	data[3] = 1;	// count = 1
	// chunk run-length : symbole 3, longueur 1 -> 0x6001
	data[8] = 0x60;
	data[9] = 0x01;
	// 3 octets de "delta" pour que la section timestamps paraisse présente
	TransportWideFeedbackField field;
	EXPECT_EQ(0u, field.Parse(data, sizeof(data)));
}

TEST(TransportFeedbackWire, SansDeltasLeRapportResteLisible)
{
	// Le format autorise un rapport sans la section des deltas : les symboles
	// disent seulement reçu/perdu. Trois petits deltas annoncés (3 octets)
	// mais 2 octets restants (le bourrage) : la section est absente.
	BYTE data[12] = { 0 };
	data[1] = 50;	// baseSeq = 50
	data[3] = 3;	// count = 3
	// chunk vecteur 2 bits : [petit, petit, petit, 0, 0, 0, 0] -> 0xD500
	data[8] = 0xD5;
	data[9] = 0x00;
	TransportWideFeedbackField out;
	ASSERT_EQ(sizeof(data), out.Parse(data, sizeof(data)));
	EXPECT_FALSE(out.hasTimestamps);
	ASSERT_EQ(3u, out.packets.size());
	EXPECT_TRUE(out.packets[0].received);
	EXPECT_TRUE(out.packets[1].received);
	EXPECT_TRUE(out.packets[2].received);
	EXPECT_EQ(50, out.packets[0].seq);
}

TEST(TransportFeedbackWire, AucuneTroncatureNeFaitDeborderLeParseur)
{
	// Un rapport valide, puis toutes ses troncatures, directement sur le
	// parseur du champ : il rend 0 ou consomme tout, sans lire au-delà.
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(200, 7000000);
	for (int i = 0; i < 30; i += 2)
		ASSERT_TRUE(in->AddReceived((WORD)(200 + i), 7000000 + i * 40000));

	RTCPRTPFeedback* packet = RTCPRTPFeedback::Create(
		RTCPRTPFeedback::TransportWideFeedbackMessage, 1, 2);
	packet->AddField(in);
	BYTE buffer[512];
	DWORD len = packet->Serialize(buffer, sizeof(buffer));
	delete packet;
	ASSERT_GT(len, 12u);

	// L'enveloppe RTCP (12 octets) puis le champ seul
	for (DWORD cut = 0; cut < len - 12; ++cut)
	{
		BYTE copy[512];
		memcpy(copy, buffer + 12, cut);
		TransportWideFeedbackField field;
		DWORD consumed = field.Parse(copy, cut);
		EXPECT_TRUE(consumed == 0 || consumed <= cut) << "troncature a " << cut;
	}
	// Et l'enveloppe elle-même tronquée ne doit pas crasher
	for (DWORD cut = 0; cut < len; cut += 3)
	{
		BYTE copy[512];
		memcpy(copy, buffer, cut);
		RTCPRTPFeedback parsed;
		DWORD consumed = parsed.Parse(copy, cut);
		EXPECT_TRUE(consumed == 0 || consumed <= cut);
	}
}

TEST(TransportFeedbackWire, LEnveloppeTraverseLeParseurCompose)
{
	// Le chemin d'arrivée réel : un compound RTCP portant le RTPFB fmt 15
	// doit traverser RTCPCompoundPacket::Parse — c'est lui qui alimente
	// ProcessRTCPPacket. Avant le lot 6.1, ce paquet produisait une ligne
	// d'erreur et le sous-paquet était jeté.
	TransportWideFeedbackField* in = new TransportWideFeedbackField();
	in->SetBase(300, 8000000);
	ASSERT_TRUE(in->AddReceived(300, 8000000));
	ASSERT_TRUE(in->AddReceived(302, 8001000));

	RTCPRTPFeedback* packet = RTCPRTPFeedback::Create(
		RTCPRTPFeedback::TransportWideFeedbackMessage, 0xAAAA, 0xBBBB);
	packet->AddField(in);
	BYTE buffer[256];
	DWORD len = packet->Serialize(buffer, sizeof(buffer));
	delete packet;
	ASSERT_GT(len, 0u);

	ASSERT_TRUE(RTCPCompoundPacket::IsRTCP(buffer, len));
	RTCPCompoundPacket* compound = RTCPCompoundPacket::Parse(buffer, len);
	ASSERT_TRUE(compound != NULL);
	ASSERT_EQ(1u, compound->GetPacketCount());
	RTCPPacket* sub = compound->GetPacket(0);
	ASSERT_EQ(RTCPPacket::RTPFeedback, sub->GetType());
	RTCPRTPFeedback* fb = (RTCPRTPFeedback*)sub;
	EXPECT_EQ(RTCPRTPFeedback::TransportWideFeedbackMessage, fb->GetFeedbackType());
	ASSERT_EQ(1u, fb->GetFieldCount());
	TransportWideFeedbackField* out = (TransportWideFeedbackField*)fb->GetField(0);
	EXPECT_EQ(300, out->baseSeq);
	EXPECT_EQ(3u, out->packets.size());
	delete compound;
}

// ---------------------------------------------------------------------------
// Suite SentPacketHistoryTest — l'historique d'émission et l'appariement.
// ---------------------------------------------------------------------------

TEST(SentPacketHistoryTest, AppariementSimple)
{
	SentPacketHistory history;
	for (WORD seq = 1; seq <= 5; ++seq)
		history.OnPacketSent(seq, 1000000 + seq * 20000, 1200);

	TransportWideFeedbackField feedback;
	feedback.SetBase(1, 9000000);
	for (WORD seq = 1; seq <= 5; ++seq)
		ASSERT_TRUE(feedback.AddReceived(seq, 9000000 + seq * 20000));

	DWORD lost = 0, unknown = 0;
	std::vector<SentPacketHistory::Result> results = history.ProcessFeedback(feedback, lost, unknown);
	EXPECT_EQ(0u, lost);
	EXPECT_EQ(0u, unknown);
	ASSERT_EQ(5u, results.size());
	for (size_t i = 0; i < results.size(); ++i)
	{
		EXPECT_EQ(1000000 + (i + 1) * 20000, results[i].sentTimeUs);
		EXPECT_EQ(1200u, results[i].size);
		if (i)
			EXPECT_EQ(20000u, results[i].recvTimeUs - results[i - 1].recvTimeUs);
	}
}

TEST(SentPacketHistoryTest, PertesEtInconnusComptesAPart)
{
	SentPacketHistory history;
	history.OnPacketSent(10, 1000000, 1000);
	// 11 jamais émis (trou local), 12 émis
	history.OnPacketSent(12, 1040000, 1000);

	TransportWideFeedbackField feedback;
	feedback.SetBase(10, 5000000);
	ASSERT_TRUE(feedback.AddReceived(10, 5000000));
	ASSERT_TRUE(feedback.AddReceived(11, 5001000));	// inconnu de l'historique
	// 12 perdu : absent du rapport... il faut le déclarer perdu par un trou
	ASSERT_TRUE(feedback.AddReceived(13, 5002000));	// inconnu aussi

	DWORD lost = 0, unknown = 0;
	std::vector<SentPacketHistory::Result> results = history.ProcessFeedback(feedback, lost, unknown);
	EXPECT_EQ(1u, lost);	// le 12, trou du rapport
	EXPECT_EQ(2u, unknown);	// 11 et 13, jamais émis
	ASSERT_EQ(1u, results.size());
	EXPECT_EQ(1000000u, results[0].sentTimeUs);
}

TEST(SentPacketHistoryTest, LeDoublonDAcquittementEstIgnore)
{
	// Une retransmission repart avec le même numéro (cf. ReSendPacket) : le
	// second acquittement du même seq ne produit pas de second résultat.
	SentPacketHistory history;
	history.OnPacketSent(20, 1000000, 800);

	TransportWideFeedbackField f1;
	f1.SetBase(20, 4000000);
	ASSERT_TRUE(f1.AddReceived(20, 4000000));
	DWORD lost, unknown;
	EXPECT_EQ(1u, history.ProcessFeedback(f1, lost, unknown).size());

	TransportWideFeedbackField f2;
	f2.SetBase(20, 4100000);
	ASSERT_TRUE(f2.AddReceived(20, 4100000));
	EXPECT_EQ(0u, history.ProcessFeedback(f2, lost, unknown).size());
}

TEST(SentPacketHistoryTest, LaPurgeEstUneDuree)
{
	SentPacketHistory history;
	history.OnPacketSent(1, 1000000, 100);
	history.OnPacketSent(2, 2000000, 100);
	EXPECT_EQ(2u, history.GetSize());
	// 61 s plus tard : le premier sort, le second reste
	history.OnPacketSent(3, 1000000 + 61 * 1000000ULL, 100);
	EXPECT_EQ(2u, history.GetSize());
}

TEST(SentPacketHistoryTest, LeCompteurSeDeroule)
{
	SentPacketHistory history;
	history.OnPacketSent(0xFFFE, 1000000, 500);
	history.OnPacketSent(0xFFFF, 1020000, 500);
	history.OnPacketSent(0x0000, 1040000, 500);	// enroulement
	history.OnPacketSent(0x0001, 1060000, 500);
	EXPECT_EQ(4u, history.GetSize());

	TransportWideFeedbackField feedback;
	feedback.SetBase(0xFFFE, 8000000);
	ASSERT_TRUE(feedback.AddReceived(0xFFFE, 8000000));
	ASSERT_TRUE(feedback.AddReceived(0xFFFF, 8020000));
	ASSERT_TRUE(feedback.AddReceived(0x0000, 8040000));
	ASSERT_TRUE(feedback.AddReceived(0x0001, 8060000));

	DWORD lost, unknown;
	std::vector<SentPacketHistory::Result> results = history.ProcessFeedback(feedback, lost, unknown);
	EXPECT_EQ(0u, unknown) << "l'enroulement a cassé l'appariement";
	ASSERT_EQ(4u, results.size());
	EXPECT_EQ(1000000u, results[0].sentTimeUs);
	EXPECT_EQ(1060000u, results[3].sentTimeUs);
}

TEST(SentPacketHistoryTest, LesArriveesRestentComparablesEntreRapports)
{
	// Deux rapports successifs : les instants d'arrivée déroulés doivent
	// rester sur la même échelle (c'est eux qui portent le délai unilatéral).
	SentPacketHistory history;
	for (WORD seq = 1; seq <= 4; ++seq)
		history.OnPacketSent(seq, seq * 100000, 1000);

	TransportWideFeedbackField f1;
	f1.SetBase(1, 10000000);
	ASSERT_TRUE(f1.AddReceived(1, 10000000));
	ASSERT_TRUE(f1.AddReceived(2, 10100000));
	DWORD lost, unknown;
	std::vector<SentPacketHistory::Result> r1 = history.ProcessFeedback(f1, lost, unknown);
	ASSERT_EQ(2u, r1.size());

	TransportWideFeedbackField f2;
	f2.SetBase(3, 10200000);
	ASSERT_TRUE(f2.AddReceived(3, 10200000));
	ASSERT_TRUE(f2.AddReceived(4, 10300000));
	std::vector<SentPacketHistory::Result> r2 = history.ProcessFeedback(f2, lost, unknown);
	ASSERT_EQ(2u, r2.size());

	EXPECT_EQ(100000u, r2[0].recvTimeUs - r1[1].recvTimeUs);
	EXPECT_EQ(100000u, r2[1].recvTimeUs - r2[0].recvTimeUs);
}

} // namespace
