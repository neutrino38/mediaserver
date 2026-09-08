// Le générateur de rapports d'arrivée
// transport-cc. Ce que NOUS devons au pair : sans ces rapports, son estimateur
// émetteur lit notre silence comme un RTT infini et recule jusqu'à son plancher.
// Horloge passée en paramètre : aucune de ces suites ne dépend du temps réel.

#include <gtest/gtest.h>
#include <vector>

#include "rtp.h"
#include "transportfeedback.h"
#include "sentpackethistory.h"

namespace {

const QWORD T0 = 1000000;	// une seconde, pour que la cadence démarre armée

// Sérialise le champ dans son enveloppe RTCP, le reparse, et rend le champ
// reparsé : c'est le seul moyen de prouver que ce que le générateur fabrique
// est lisible sur le fil.
bool WireRoundTrip(const TransportWideFeedbackField& in, TransportWideFeedbackField& out)
{
	TransportWideFeedbackField* copy = new TransportWideFeedbackField(in);
	RTCPRTPFeedback* packet = RTCPRTPFeedback::Create(
		RTCPRTPFeedback::TransportWideFeedbackMessage, 0x1111, 0x2222);
	packet->AddField(copy);
	BYTE buffer[2048];
	DWORD len = packet->Serialize(buffer, sizeof(buffer));
	delete packet;
	if (!len || len % 4)
		return false;
	RTCPRTPFeedback parsed;
	if (parsed.Parse(buffer, len) != len || parsed.GetFieldCount() != 1)
		return false;
	out = *(TransportWideFeedbackField*)parsed.GetField(0);
	return true;
}

// Compte les statuts reçus d'un champ
DWORD CountReceived(const TransportWideFeedbackField& field)
{
	DWORD n = 0;
	for (size_t i = 0; i < field.packets.size(); ++i)
		if (field.packets[i].received)
			n++;
	return n;
}

// ---------------------------------------------------------------------------
// Suite TransportFeedbackGenerator — accumulation, cadence, découpe.
// ---------------------------------------------------------------------------

TEST(TransportFeedbackGenerator, SansPaquetIlNYARienARapporter)
{
	TransportWideFeedbackGenerator gen;
	TransportWideFeedbackField field;

	EXPECT_FALSE(gen.HasPending());
	EXPECT_FALSE(gen.ShouldSend(T0));
	EXPECT_FALSE(gen.BuildFeedback(field, T0));
}

TEST(TransportFeedbackGenerator, LePremierRapportCouvreLesArrivees)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(0xABCD, 100, T0);
	gen.OnPacketReceived(0xABCD, 101, T0 + 5000);
	gen.OnPacketReceived(0xABCD, 102, T0 + 12000);

	EXPECT_EQ(0xABCDu, gen.GetMediaSSRC());
	EXPECT_EQ(3u, gen.GetPendingCount());
	ASSERT_TRUE(gen.ShouldSend(T0 + 12000));

	TransportWideFeedbackField field;
	ASSERT_TRUE(gen.BuildFeedback(field, T0 + 12000));
	EXPECT_EQ(100, field.baseSeq);
	ASSERT_EQ(3u, field.packets.size());
	EXPECT_EQ(3u, CountReceived(field));

	// Les instants se reconstruisent par référence + somme des deltas
	TransportWideFeedbackField out;
	ASSERT_TRUE(WireRoundTrip(field, out));
	QWORD arrival = (QWORD)out.referenceTicks * TransportWideFeedbackField::BaseTickUs;
	QWORD expected[] = { T0, T0 + 5000, T0 + 12000 };
	for (size_t i = 0; i < out.packets.size(); ++i)
	{
		arrival += (QWORD)out.packets[i].deltaTicks * TransportWideFeedbackField::TickUs;
		EXPECT_EQ(expected[i], arrival) << "paquet " << i;
	}

	// Tout est rapporté : plus rien en attente
	EXPECT_FALSE(gen.HasPending());
	EXPECT_EQ(0u, gen.GetPendingCount());
}

TEST(TransportFeedbackGenerator, LaCadenceEspaceLesRapports)
{
	TransportWideFeedbackGenerator gen;
	TransportWideFeedbackField field;

	gen.OnPacketReceived(1, 10, T0);
	ASSERT_TRUE(gen.ShouldSend(T0));
	ASSERT_TRUE(gen.BuildFeedback(field, T0));

	// Un paquet de plus, mais l'intervalle par défaut n'est pas écoulé
	gen.OnPacketReceived(1, 11, T0 + 10000);
	EXPECT_TRUE(gen.HasPending());
	EXPECT_FALSE(gen.ShouldSend(T0 + 10000));
	EXPECT_FALSE(gen.ShouldSend(T0 + TransportWideFeedbackGenerator::DefaultIntervalUs - 1));
	EXPECT_TRUE(gen.ShouldSend(T0 + TransportWideFeedbackGenerator::DefaultIntervalUs));
}

TEST(TransportFeedbackGenerator, LIntervalleSuitLeDebitDEmission)
{
	TransportWideFeedbackGenerator gen;

	// Débit inconnu : on s'en tient au rapport le plus espacé
	gen.SetSendBitrate(0);
	EXPECT_EQ(TransportWideFeedbackGenerator::MaxIntervalUs, gen.GetIntervalUs());

	// Lien confortable : les rapports occupent 5 % au plus, donc le plancher
	gen.SetSendBitrate(2000000);
	EXPECT_EQ(TransportWideFeedbackGenerator::MinIntervalUs, gen.GetIntervalUs());

	// Lien étranglé : on s'espace, sans jamais dépasser le plafond
	gen.SetSendBitrate(10000);
	EXPECT_EQ(TransportWideFeedbackGenerator::MaxIntervalUs, gen.GetIntervalUs());
	EXPECT_GE(gen.GetIntervalUs(), TransportWideFeedbackGenerator::MinIntervalUs);
}

TEST(TransportFeedbackGenerator, LesTrousDeviennentDesPertes)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(1, 10, T0);
	gen.OnPacketReceived(1, 13, T0 + 3000);

	TransportWideFeedbackField field;
	ASSERT_TRUE(gen.BuildFeedback(field, T0 + 3000));
	// 10 reçu, 11 et 12 perdus, 13 reçu
	ASSERT_EQ(4u, field.packets.size());
	EXPECT_TRUE(field.packets[0].received);
	EXPECT_FALSE(field.packets[1].received);
	EXPECT_FALSE(field.packets[2].received);
	EXPECT_TRUE(field.packets[3].received);

	TransportWideFeedbackField out;
	ASSERT_TRUE(WireRoundTrip(field, out));
	ASSERT_EQ(4u, out.packets.size());
	EXPECT_EQ(2u, CountReceived(out));
}

TEST(TransportFeedbackGenerator, LeReordonnancementRouvreLaFenetre)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(1, 10, T0);
	gen.OnPacketReceived(1, 12, T0 + 2000);

	TransportWideFeedbackField first;
	ASSERT_TRUE(gen.BuildFeedback(first, T0 + 2000));
	EXPECT_EQ(10, first.baseSeq);

	// Le retardataire arrive après son propre rapport : il doit être redit,
	// sinon le pair le compte perdu à jamais.
	gen.OnPacketReceived(1, 11, T0 + 3000);
	EXPECT_TRUE(gen.HasPending());

	TransportWideFeedbackField second;
	ASSERT_TRUE(gen.BuildFeedback(second, T0 + TransportWideFeedbackGenerator::DefaultIntervalUs + T0));
	EXPECT_EQ(11, second.baseSeq);
	ASSERT_EQ(2u, second.packets.size());	// 11 reçu, puis 12 redit
	EXPECT_TRUE(second.packets[0].received);
	EXPECT_TRUE(second.packets[1].received);
}

TEST(TransportFeedbackGenerator, LeDoublonNeRedatePasLePaquet)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(1, 20, T0);
	gen.OnPacketReceived(1, 20, T0 + 50000);	// même seq, plus tard

	EXPECT_EQ(1u, gen.GetPendingCount());

	TransportWideFeedbackField field;
	ASSERT_TRUE(gen.BuildFeedback(field, T0 + 50000));
	TransportWideFeedbackField out;
	ASSERT_TRUE(WireRoundTrip(field, out));
	// La première arrivée fait foi : le delta reste dans le pas de référence
	QWORD arrival = (QWORD)out.referenceTicks * TransportWideFeedbackField::BaseTickUs
			+ (QWORD)out.packets[0].deltaTicks * TransportWideFeedbackField::TickUs;
	EXPECT_EQ(T0, arrival);
}

TEST(TransportFeedbackGenerator, LesArriveesTropVieillesSontOubliees)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(1, 30, T0);
	// Au-delà de la fenêtre de retour, la première arrivée ne sert plus
	gen.OnPacketReceived(1, 31, T0 + TransportWideFeedbackGenerator::BackWindowUs + 1);

	EXPECT_EQ(1u, gen.GetPendingCount());

	TransportWideFeedbackField field;
	ASSERT_TRUE(gen.BuildFeedback(field, T0 + TransportWideFeedbackGenerator::BackWindowUs + 1));
	EXPECT_EQ(31, field.baseSeq);
	EXPECT_EQ(1u, field.packets.size());
}

TEST(TransportFeedbackGenerator, UnRapportTropLongEstScinde)
{
	TransportWideFeedbackGenerator gen;
	const DWORD count = TransportWideFeedbackGenerator::MaxStatusPerReport + 100;
	for (DWORD i = 0; i < count; ++i)
		gen.OnPacketReceived(1, (WORD)(1000 + i), T0 + i * 1000);

	QWORD now = T0 + count * 1000;
	TransportWideFeedbackField first;
	ASSERT_TRUE(gen.BuildFeedback(first, now));
	EXPECT_EQ(TransportWideFeedbackGenerator::MaxStatusPerReport, (DWORD)first.packets.size());
	EXPECT_EQ(1000, first.baseSeq);
	// Le reste part au rapport suivant, rien n'est perdu en route
	EXPECT_EQ(100u, gen.GetPendingCount());

	TransportWideFeedbackField second;
	ASSERT_TRUE(gen.BuildFeedback(second, now + TransportWideFeedbackGenerator::MaxIntervalUs));
	EXPECT_EQ((WORD)(1000 + TransportWideFeedbackGenerator::MaxStatusPerReport), second.baseSeq);
	EXPECT_EQ(100u, (DWORD)second.packets.size());
	EXPECT_FALSE(gen.HasPending());

	// Et le plus gros des deux tient dans un datagramme
	TransportWideFeedbackField out;
	ASSERT_TRUE(WireRoundTrip(first, out));
	EXPECT_LT(first.GetSize(), 1200u);
}

TEST(TransportFeedbackGenerator, LeCompteurDeRapportSIncremente)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(1, 40, T0);
	TransportWideFeedbackField first;
	ASSERT_TRUE(gen.BuildFeedback(first, T0));

	gen.OnPacketReceived(1, 41, T0 + 200000);
	TransportWideFeedbackField second;
	ASSERT_TRUE(gen.BuildFeedback(second, T0 + 200000));

	EXPECT_EQ((BYTE)(first.fbSeq + 1), second.fbSeq);
}

TEST(TransportFeedbackGenerator, LEnroulementDuCompteurNeCassePasLaFenetre)
{
	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(1, 65534, T0);
	gen.OnPacketReceived(1, 65535, T0 + 1000);
	gen.OnPacketReceived(1, 0,     T0 + 2000);
	gen.OnPacketReceived(1, 1,     T0 + 3000);

	EXPECT_EQ(4u, gen.GetPendingCount());

	TransportWideFeedbackField field;
	ASSERT_TRUE(gen.BuildFeedback(field, T0 + 3000));
	EXPECT_EQ(65534, field.baseSeq);
	ASSERT_EQ(4u, field.packets.size());
	EXPECT_EQ(4u, CountReceived(field));
	EXPECT_EQ(1, field.packets[3].seq);
}

// ---------------------------------------------------------------------------
// La boucle complète : ce que nous rapportons est ce que le pair apparie.
// ---------------------------------------------------------------------------

TEST(TransportFeedbackGenerator, LeRapportSAppariieALHistoriqueDEmissionDuPair)
{
	// Le « pair », c'est notre propre historique d'émission : s'il retrouve ses
	// paquets dans notre rapport, un estimateur émetteur le peut aussi.
	SentPacketHistory peer;
	peer.OnPacketSent(200, T0 - 30000, 1200);
	peer.OnPacketSent(201, T0 - 20000, 1200);
	peer.OnPacketSent(202, T0 - 10000, 1200);

	TransportWideFeedbackGenerator gen;
	gen.OnPacketReceived(0x1234, 200, T0);
	gen.OnPacketReceived(0x1234, 201, T0 + 10000);
	gen.OnPacketReceived(0x1234, 202, T0 + 20000);

	TransportWideFeedbackField field;
	ASSERT_TRUE(gen.BuildFeedback(field, T0 + 20000));

	TransportWideFeedbackField onTheWire;
	ASSERT_TRUE(WireRoundTrip(field, onTheWire));

	DWORD lost = 0, unknown = 0;
	std::vector<SentPacketHistory::Result> results = peer.ProcessFeedback(onTheWire, lost, unknown);
	ASSERT_EQ(3u, results.size());
	EXPECT_EQ(0u, lost);
	EXPECT_EQ(0u, unknown);
	// Les écarts d'arrivée sont ceux que nous avons mesurés
	EXPECT_EQ(10000u, results[1].recvTimeUs - results[0].recvTimeUs);
	EXPECT_EQ(10000u, results[2].recvTimeUs - results[1].recvTimeUs);
	// ... et les instants d'émission viennent de l'historique
	EXPECT_EQ(10000u, results[1].sentTimeUs - results[0].sentTimeUs);
}

}	// namespace
