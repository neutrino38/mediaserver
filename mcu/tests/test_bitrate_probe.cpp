/**
 * test_bitrate_probe.cpp — BitrateProbe (src/jsr309/BitrateProbe.h) : la sonde
 * au-dessus de la limite TMMBR du pair, dimensionnée sur les constantes de
 * Linphone. Tout est en temps simulé, aucun test ne dort.
 *
 * Scénario de référence (séance netem du 2026-09-02, journal de Bob) : lien
 * restauré, Bob demande 55 kb/s et son VBE est aveugle (images d'un paquet).
 * Notre estimateur dit 15 Mb/s, la consigne 2500.
 */
#include <gtest/gtest.h>

#include "../src/jsr309/BitrateProbe.h"

namespace {

const QWORD S = 1000000;	// une seconde en µs

// Fait avancer la sonde image par image (15 im/s) pendant `seconds`, en
// rappelant Apply avec les mêmes entrées. Rend la dernière cible.
static int Tick(BitrateProbe& p, int& target, int peer, int bwe, int ceiling,
               QWORD& now, double seconds)
{
	int out = target;
	const int frames = (int)(seconds * 15);
	for (int i = 0; i < frames; ++i)
	{
		now += S / 15;
		out = p.Apply(target, peer, bwe, ceiling, now);
	}
	return out;
}

TEST(BitrateProbe, NeSondePasQuandLePairNeBornePas)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	// La cible (800) est sous la limite du pair (2000) : c'est nous qui bornons.
	int target = 800;
	EXPECT_EQ(800, Tick(p, target, 2000, 15000, 2500, now, 20));
	EXPECT_FALSE(p.IsProbing());
}

TEST(BitrateProbe, NeSondePasSansEstimateur)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	int target = 55;
	EXPECT_EQ(55, Tick(p, target, 55, /*bwe=*/0, 2500, now, 20));
	EXPECT_FALSE(p.IsProbing());
}

TEST(BitrateProbe, NeSondePasQuandLePairEstDejaALaConsigne)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	int target = 2500;
	EXPECT_EQ(2500, Tick(p, target, 2500, 15000, 2500, now, 20));
	EXPECT_FALSE(p.IsProbing());
}

// Le cœur : 55 kb/s, lien propre. Après 5 s d'armement la sonde part au
// plancher de 500 (3 paquets/image), dure 6 s, puis revient à 55.
TEST(BitrateProbe, SondeAuPlancherPuisRevientSiLePairNeSuitPas)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	int target = 55;

	EXPECT_EQ(55, Tick(p, target, 55, 15000, 2500, now, 4.9))
		<< "pas encore arme : 5 s de lien propre exiges";

	int during = Tick(p, target, 55, 15000, 2500, now, 0.5);
	ASSERT_TRUE(p.IsProbing());
	EXPECT_EQ(BitrateProbe::FloorKbps, during) << "55 x 1,4 = 77 < plancher 500";
	EXPECT_EQ(500, p.GetProbeKbps());

	EXPECT_EQ(500, Tick(p, target, 55, 15000, 2500, now, 5.0)) << "6 s de sonde";
	EXPECT_EQ(55,  Tick(p, target, 55, 15000, 2500, now, 1.0)) << "fin de sonde : retour a la limite";
	EXPECT_FALSE(p.IsProbing());

	// Le pair n'a pas suivi : l'intervalle a double, rien avant.
	EXPECT_EQ(2 * BitrateProbe::IntervalMinUs, p.GetIntervalUs());
	EXPECT_EQ(55, Tick(p, target, 55, 15000, 2500, now, 19.0));
	EXPECT_FALSE(p.IsProbing());
}

// Le pas de x1,4 au-dessus du plancher, borne par la consigne.
TEST(BitrateProbe, LePasEstDeUnVirguleQuatreBorneParLaConsigne)
{
	EXPECT_EQ(500,  BitrateProbe::Wanted(55,   2500));
	EXPECT_EQ(1400, BitrateProbe::Wanted(1000, 2500));
	EXPECT_EQ(2500, BitrateProbe::Wanted(2000, 2500)) << "jamais au-dessus de la consigne";
}

// Abandon a la premiere perte : notre estimateur redescend sous la valeur
// sondee, la cible revient a la limite du pair tout de suite, intervalle double.
TEST(BitrateProbe, LaPerteInterromptLaSondeEtEspaceLaSuivante)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	int target = 55;
	Tick(p, target, 55, 15000, 2500, now, 5.5);
	ASSERT_TRUE(p.IsProbing());

	// L'etage de perte a reagi : l'estimateur tombe a 300, sous les 500 sondes.
	int after = Tick(p, target, 55, 300, 2500, now, 0.1);
	EXPECT_EQ(55, after);
	EXPECT_FALSE(p.IsProbing());
	EXPECT_EQ(BitrateProbe::Abort, p.LastEvent());
	EXPECT_EQ(2 * BitrateProbe::IntervalMinUs, p.GetIntervalUs());
}

// Le pair a suivi : son TMMBR remonte pendant la sonde. La sonde s'arrete (ce
// n'est plus la meme limite), l'intervalle revient au minimum, et la sonde
// suivante part du nouveau palier — l'echelle monte.
TEST(BitrateProbe, QuandLePairSuitLEchelleMonte)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	int target = 55;
	Tick(p, target, 55, 15000, 2500, now, 5.5);
	ASSERT_TRUE(p.IsProbing());

	// Bob a remesure : TMMBR 520. L'appelant recalcule target = min(...) = 520.
	target = 520;
	EXPECT_EQ(520, Tick(p, target, 520, 15000, 2500, now, 0.1));
	EXPECT_EQ(BitrateProbe::IntervalMinUs, p.GetIntervalUs()) << "le pair a suivi : pas de back-off";

	// Prochaine sonde : 520 x 1,4 = 728, apres l'intervalle minimal + armement.
	Tick(p, target, 520, 15000, 2500, now, 10.0);
	int next = Tick(p, target, 520, 15000, 2500, now, 5.5);
	ASSERT_TRUE(p.IsProbing());
	EXPECT_EQ(728, next);
}

// La sonde ne depasse jamais notre propre estimateur.
TEST(BitrateProbe, LaSondeEstBorneeParNotreEstimateur)
{
	BitrateProbe p;
	QWORD now = 10 * S;
	int target = 55;
	// L'estimateur ne porte que 600 : la sonde voudrait 500 (ok) — mais avec un
	// pair a 1000 elle voudrait 1400, et l'estimateur a 1200 la coupe a 1200.
	Tick(p, target, 55, 600, 2500, now, 5.5);
	EXPECT_EQ(500, p.GetProbeKbps());

	BitrateProbe q;
	QWORD t2 = 10 * S;
	int tg = 1000;
	int v = Tick(q, tg, 1000, 1200, 2500, t2, 5.5);
	ASSERT_FALSE(q.IsProbing()) << "1200 < 1400 voulu : le lien ne porte pas la sonde, on n ose pas";
	EXPECT_EQ(1000, v);
}

} // namespace
