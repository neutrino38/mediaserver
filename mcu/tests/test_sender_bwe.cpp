// Lot 6.2 (sender_bwe_plan.md) — le cœur de l'estimateur émetteur, sous
// horloge simulée : détecteur trendline, AIMD sur débit acquitté, étage de
// perte. Tout est nourri par des rapports synthétiques, sans réseau.

#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

#include "senderbwe.h"
#include "trendlinedetector.h"

namespace {

const DWORD kPacketBytes = 1200;

// Lien simulé : un débit, une file. Rend l'instant d'arrivée d'un paquet.
struct LinkSim
{
	double capacityBps;
	QWORD  freeAtUs = 0;

	explicit LinkSim(double bps) : capacityBps(bps) {}

	QWORD Deliver(QWORD sendUs, DWORD bytes)
	{
		QWORD txUs = (QWORD)(bytes * 8e6 / capacityBps);
		QWORD start = std::max(sendUs, freeAtUs);
		freeAtUs = start + txUs;
		return freeAtUs;
	}
};

// Émet à débit constant pendant durationMs, livre par le lien, nourrit
// l'estimateur par rapports groupés toutes les 100 ms. Rend l'instant final.
QWORD Drive(SenderBWE& bwe, LinkSim& link, double sendBps, QWORD fromUs, DWORD durationMs)
{
	const QWORD intervalUs = (QWORD)(kPacketBytes * 8e6 / sendBps);
	std::vector<SentPacketHistory::Result> batch;
	QWORD now = fromUs;
	QWORD nextReport = fromUs + 100000;
	for (QWORD end = fromUs + (QWORD)durationMs * 1000; now < end; now += intervalUs)
	{
		QWORD arrival = link.Deliver(now, kPacketBytes);
		batch.push_back({ now, arrival, kPacketBytes });
		if (now >= nextReport)
		{
			bwe.ProcessFeedback(batch, 0, now);
			batch.clear();
			nextReport += 100000;
		}
	}
	if (!batch.empty())
		bwe.ProcessFeedback(batch, 0, now);
	return now;
}

// ---------------------------------------------------------------------------
// Suite TrendlineDetectorTest — le détecteur seul.
// ---------------------------------------------------------------------------

TEST(TrendlineDetectorTest, UnFluxRegulierResteNormal)
{
	TrendlineDetector detector;
	QWORD send = 1000000, recv = 1050000;
	for (int i = 0; i < 200; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;	// 20 ms d'intervalle : chaque paquet est un groupe
		recv += 20000;
	}
	EXPECT_EQ(TrendlineDetector::Normal, detector.GetUsage());
}

TEST(TrendlineDetectorTest, UneFileQuiSeRemplitDeclencheLaSurutilisation)
{
	TrendlineDetector detector;
	QWORD send = 1000000, recv = 1050000;
	// Le récepteur prend 2 ms de retard par paquet : dérive LINÉAIRE — le cas
	// que le Kalman local ne voyait pas (§3.4 e) et que la pente voit.
	for (int i = 0; i < 100 && detector.GetUsage() != TrendlineDetector::OverUsing; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 22000;
	}
	EXPECT_EQ(TrendlineDetector::OverUsing, detector.GetUsage());
}

TEST(TrendlineDetectorTest, LeCalmeRevientApresLaCongestion)
{
	TrendlineDetector detector;
	QWORD send = 1000000, recv = 1050000;
	for (int i = 0; i < 100; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 22000;
	}
	ASSERT_EQ(TrendlineDetector::OverUsing, detector.GetUsage()) << "prérequis : congestion vue";
	// La file cesse de croître (elle ne se vide même pas : délai constant)
	for (int i = 0; i < 100; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 20000;
	}
	EXPECT_NE(TrendlineDetector::OverUsing, detector.GetUsage());
}

TEST(TrendlineDetectorTest, LeSeuilRedescendApresUneChuteDeCapacite)
{
	// Une chute de capacite fait grimper la pente, et le seuil la poursuit en
	// partie (bande d'adaptation +15 ms). Ce qui est garanti : la detection a
	// bien eu lieu, le seuil reste borne, il REDESCEND vite au calme (k_down
	// est 4,5 x k_up) et le detecteur voit encore la congestion suivante.
	TrendlineDetector detector;
	QWORD send = 1000000, recv = 1050000;
	for (int i = 0; i < 50; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 20000;
	}
	// Effondrement : chaque paquet prend 30 ms de retard de plus
	bool detected = false;
	for (int i = 0; i < 30; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 50000;
		detected |= detector.GetUsage() == TrendlineDetector::OverUsing;
	}
	EXPECT_TRUE(detected) << "la chute de capacite n'a pas ete detectee";
	EXPECT_LE(detector.GetThreshold(), 600.0);
	// Retour au calme prolonge : le seuil doit retomber pres de son plancher
	for (int i = 0; i < 500; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 20000;
	}
	EXPECT_LE(detector.GetThreshold(), 10.0) << "le seuil ne redescend pas au calme";
	// Et une nouvelle congestion se voit toujours
	bool again = false;
	for (int i = 0; i < 100 && !again; ++i)
	{
		detector.OnPacket(send, recv, kPacketBytes);
		send += 20000;
		recv += 22000;
		again = detector.GetUsage() == TrendlineDetector::OverUsing;
	}
	EXPECT_TRUE(again) << "le detecteur est reste aveugle apres l'episode";
}

TEST(TrendlineDetectorTest, UneRafaleResteUnSeulGroupe)
{
	// Des paquets envoyés ensemble (même instant d'envoi) ne fabriquent pas
	// de faux deltas : ils appartiennent au même groupe d'envoi.
	TrendlineDetector detector;
	QWORD send = 1000000, recv = 1050000;
	for (int i = 0; i < 300; ++i)
	{
		// une image = 4 paquets au même instant, arrivées espacées de 1 ms
		for (int p = 0; p < 4; ++p)
			detector.OnPacket(send, recv + p * 1000, kPacketBytes);
		send += 33000;
		recv += 33000;
	}
	EXPECT_EQ(TrendlineDetector::Normal, detector.GetUsage());
}

// ---------------------------------------------------------------------------
// Suite SenderBWETest — l'estimateur complet sous horloge simulée.
// ---------------------------------------------------------------------------

TEST(SenderBWETest, PasDeRapportPasDEstimation)
{
	SenderBWE bwe;
	EXPECT_FALSE(bwe.HasEstimate());
	EXPECT_EQ(0u, bwe.GetEstimatedBitrate());
}

TEST(SenderBWETest, LaCibleMonteSurUnLienLarge)
{
	SenderBWE bwe;
	bwe.SetStartBitrate(300000, 1000000);
	LinkSim link(2000000);
	// 10 s à 300 kb/s sur un lien de 2000 : découverte multiplicative,
	// plafonnée à 1,5 x l'acquitté + 10 kb/s.
	Drive(bwe, link, 300000, 1000000, 10000);
	ASSERT_TRUE(bwe.HasEstimate());
	DWORD target = bwe.GetEstimatedBitrate();
	EXPECT_GT(target, 330000u) << "la découverte n'a pas monté";
	EXPECT_LE(target, (DWORD)(1.5 * 320000) + 20000) << "plafond glissant dépassé";
}

TEST(SenderBWETest, LaSurchargeDescendVersLeDebitAcquitte)
{
	SenderBWE bwe;
	bwe.SetStartBitrate(2000000, 1000000);
	LinkSim link(1000000);
	// 2000 kb/s poussés dans 1000 : la file gonfle, le détecteur doit
	// déclencher et la descente porter sur le débit ACQUITTÉ (~1000), pas sur
	// notre propre estimation (2000).
	Drive(bwe, link, 2000000, 1000000, 8000);
	DWORD target = bwe.GetEstimatedBitrate();
	EXPECT_LT(target, 1100000u) << "pas descendu vers l'acquitté";
	EXPECT_GT(target, 400000u) << "effondrement sous beta x acquitté";
}

TEST(SenderBWETest, UnSeulRetourAuCalmeRelanceLaMontee)
{
	SenderBWE bwe;
	bwe.SetStartBitrate(2000000, 1000000);
	LinkSim link(1000000);
	// Surcharge : descente
	QWORD now = Drive(bwe, link, 2000000, 1000000, 6000);
	DWORD low = bwe.GetEstimatedBitrate();
	ASSERT_LT(low, 1200000u) << "prérequis : la surcharge a fait descendre";
	// Retour au calme : la source repasse sous le lien. La cible doit
	// remonter — pas rester verrouillée en Decrease (le défaut É2 du chemin
	// REMB, qu'on ne veut pas réintroduire ici).
	LinkSim calm(1000000);
	calm.freeAtUs = now;
	Drive(bwe, calm, 700000, now, 6000);
	EXPECT_GT(bwe.GetEstimatedBitrate(), low) << "la montée ne repart pas";
}

TEST(SenderBWETest, LesPertesFortesFontDescendreUneFoisParFenetre)
{
	SenderBWE bwe;
	bwe.SetStartBitrate(1000000, 1000000);
	bwe.UpdateRTT(50);
	LinkSim link(5000000);
	QWORD now = Drive(bwe, link, 1000000, 1000000, 3000);
	DWORD before = bwe.GetEstimatedBitrate();
	ASSERT_GT(before, 0u);

	// 20 % de pertes (51/256) : descente x(512-51)/512 ~ -10 %
	bwe.UpdateFractionLost(51, now);
	DWORD after = bwe.GetEstimatedBitrate();
	EXPECT_LT(after, before);
	EXPECT_GE(after, (DWORD)(before * 0.85));

	// Un second rapport immédiat ne descend PAS une seconde fois : la fenêtre
	// est de 300 ms + RTT
	bwe.UpdateFractionLost(51, now + 100000);
	EXPECT_EQ(after, bwe.GetEstimatedBitrate());
}

TEST(SenderBWETest, LesPertesFaiblesNEmpechentPasLaMontee)
{
	SenderBWE bwe;
	bwe.SetStartBitrate(500000, 1000000);
	bwe.UpdateRTT(50);
	LinkSim link(5000000);
	QWORD now = 1000000;
	// 6 s de trafic sain avec un rapport de perte < 2 % chaque seconde
	for (int i = 0; i < 6; ++i)
	{
		now = Drive(bwe, link, 500000, now, 1000);
		bwe.UpdateFractionLost(2, now);	// 0,8 %
	}
	EXPECT_GT(bwe.GetEstimatedBitrate(), 500000u) << "la montée est bloquée par des pertes anodines";
}

TEST(SenderBWETest, LesPertesMoyennesNeFontRien)
{
	SenderBWE bwe;
	bwe.SetStartBitrate(1000000, 1000000);
	bwe.UpdateRTT(50);
	LinkSim link(5000000);
	QWORD now = Drive(bwe, link, 1000000, 1000000, 3000);
	DWORD before = bwe.GetEstimatedBitrate();
	// 5 % de pertes (13/256) : zone morte 2-10 %, la cible ne bouge pas
	bwe.UpdateFractionLost(13, now + 1000);
	EXPECT_EQ(before, bwe.GetEstimatedBitrate());
}

TEST(SenderBWETest, LEstimationResteBornee)
{
	SenderBWE bwe;
	bwe.SetMinMaxBitrate(64000, 1500000);
	bwe.SetStartBitrate(300000, 1000000);
	LinkSim link(50000000);
	Drive(bwe, link, 3000000, 1000000, 20000);
	EXPECT_LE(bwe.GetEstimatedBitrate(), 1500000u);
	EXPECT_GE(bwe.GetEstimatedBitrate(), 64000u);
}

TEST(SenderBWETest, SansDepartLInitialisationPrendCinqSecondes)
{
	// Sans SetStartBitrate : 5 s de débit acquitté avant la première
	// estimation (témoin kInitializationTime), pas d'estimation avant.
	SenderBWE bwe;
	LinkSim link(2000000);
	Drive(bwe, link, 400000, 1000000, 3000);
	EXPECT_FALSE(bwe.HasEstimate()) << "estimation prononcée avant 5 s de débit";
	Drive(bwe, link, 400000, 4000000, 4000);
	EXPECT_TRUE(bwe.HasEstimate());
	// Elle part du débit acquitté, pas d'une constante
	EXPECT_GT(bwe.GetEstimatedBitrate(), 200000u);
	EXPECT_LT(bwe.GetEstimatedBitrate(), 800000u);
}

} // namespace
