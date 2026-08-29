/**
 * test_frame_decimator.cpp — le pas de décimation d'un transcodeur inline.
 *
 * Appel du 2026-08-29 : un encodeur VP8 720p trop lent pour la source a fait
 * jeter 10 697 paquets par le RTPBuffer et réclamer 57 trames clés au pair en
 * 64 s. La politique testée ici transforme ce retard en images sautées.
 *
 * Tout est en temps simulé : `Observe` reçoit l'horloge, aucun test ne dort.
 */
#include <gtest/gtest.h>

#include "../src/jsr309/FrameDecimator.h"

namespace {

const QWORD Budget20Fps = 50000;	// 50 ms entre deux images
const QWORD Usable = Budget20Fps*FrameDecimator::UsableShareNum/FrameDecimator::UsableShareDen;	// 40 ms

// Nourrit `n` encodages du même coût, à `budget` d'intervalle, et rend le
// nombre de changements de pas.
int Feed(FrameDecimator& d, int n, QWORD costUs, QWORD budget, QWORD& now)
{
	int changes = 0;
	for (int i = 0; i < n; ++i)
	{
		now += budget;
		if (d.Observe(costUs, budget, now))
			changes++;
	}
	return changes;
}

// Un encodeur qui tient dans le budget : toutes les images sont encodées,
// quel que soit le nombre d'échantillons.
TEST(FrameDecimator, DansLeBudgetToutEstEncode)
{
	FrameDecimator d;
	QWORD now = 1000000;
	EXPECT_EQ(0, Feed(d, 100, Usable/2, Budget20Fps, now));
	EXPECT_EQ(1, d.GetStep());
}

// Aucune décision avant MinSamples : la première image contient l'ouverture
// de l'encodeur et coûte plusieurs fois une image de régime.
TEST(FrameDecimator, PasDeDecisionAvantHuitEchantillons)
{
	FrameDecimator d;
	QWORD now = 1000000;
	EXPECT_EQ(0, Feed(d, FrameDecimator::MinSamples - 1, Usable*3, Budget20Fps, now));
	EXPECT_EQ(1, d.GetStep());
}

// Un coût de 2,2 fois la part utilisable : il faut sauter 2 images sur 3.
// Le pas monte à la première décision possible, sans attendre.
TEST(FrameDecimator, TropLentLePasMonteTouteDeSuite)
{
	FrameDecimator d;
	QWORD now = 1000000;
	const QWORD cost = Usable*22/10;	// 88 ms pour 40 ms utilisables → k = 3

	EXPECT_EQ(1, Feed(d, FrameDecimator::MinSamples, cost, Budget20Fps, now))
		<< "un seul changement, a la premiere decision";
	EXPECT_EQ(3, d.GetStep());

	// Il ne bat pas : le même coût laisse le même pas.
	EXPECT_EQ(0, Feed(d, 50, cost, Budget20Fps, now));
	EXPECT_EQ(3, d.GetStep());
}

// Le pas redescend seulement après RecoveryUs de coût sous le seuil : chaque
// changement de cadence coûte une trame clé, on ne le paie pas pour un creux.
TEST(FrameDecimator, LaRedescenteAttendTroisSecondesDeCalme)
{
	FrameDecimator d;
	QWORD now = 1000000;
	Feed(d, FrameDecimator::MinSamples, Usable*22/10, Budget20Fps, now);
	ASSERT_EQ(3, d.GetStep());

	// Retour à un coût qui tient dans le budget. La moyenne glissante met
	// quelques images à descendre ; puis 3 s doivent s'écouler.
	const QWORD cheap = Usable/4;
	const QWORD start = now;
	int changes = 0;
	while (now - start < FrameDecimator::RecoveryUs - Budget20Fps)
		changes += Feed(d, 1, cheap, Budget20Fps, now);
	EXPECT_EQ(0, changes) << "pas de redescente avant 3 s";
	EXPECT_EQ(3, d.GetStep());

	// Un peu plus de 3 s après le début du calme (la moyenne a mis ~10 images
	// à passer sous le seuil) : le pas redescend, en une fois.
	changes = Feed(d, 30, cheap, Budget20Fps, now);
	EXPECT_EQ(1, changes);
	EXPECT_EQ(1, d.GetStep());
}

// Un creux plus court que RecoveryUs ne fait pas redescendre le pas, et le
// compteur repart de zéro quand la charge revient.
TEST(FrameDecimator, UnCreuxCourtNeChangeRien)
{
	FrameDecimator d;
	QWORD now = 1000000;
	const QWORD cost = Usable*22/10;
	Feed(d, FrameDecimator::MinSamples, cost, Budget20Fps, now);
	ASSERT_EQ(3, d.GetStep());

	// 2 s de calme puis la charge revient : rien ne doit bouger.
	Feed(d, 40, Usable/4, Budget20Fps, now);		// 2 s
	Feed(d, 40, cost, Budget20Fps, now);		// la charge revient
	EXPECT_EQ(3, d.GetStep());

	// Et un nouveau calme repart de zéro : 2 s ne suffisent toujours pas.
	EXPECT_EQ(0, Feed(d, 40, Usable/4, Budget20Fps, now));
	EXPECT_EQ(3, d.GetStep());
}

// La trame clé qui suit une réouverture coûte plusieurs fois une image inter :
// un échantillon isolé, même énorme, ne doit pas faire monter le pas.
TEST(FrameDecimator, UneTrameCleIsoleeNeFaitPasMonterLePas)
{
	FrameDecimator d;
	QWORD now = 1000000;
	Feed(d, 20, Usable/2, Budget20Fps, now);
	ASSERT_EQ(1, d.GetStep());

	// Une image à 20 fois le coût courant.
	EXPECT_FALSE(d.Observe(Usable*10, Budget20Fps, now += Budget20Fps));
	EXPECT_EQ(1, d.GetStep());
	// Et la moyenne n'a pas été polluée au point de basculer à l'image suivante.
	EXPECT_EQ(0, Feed(d, 5, Usable/2, Budget20Fps, now));
	EXPECT_EQ(1, d.GetStep());
}

// Une charge DURABLE, elle, passe malgré l'écrêtage des échantillons : le pas
// monte par paliers jusqu'au pas nécessaire.
TEST(FrameDecimator, UneChargeDurablePasseMalgreLEcretage)
{
	FrameDecimator d;
	QWORD now = 1000000;
	Feed(d, 20, Usable/2, Budget20Fps, now);
	ASSERT_EQ(1, d.GetStep());

	Feed(d, 60, Usable*5, Budget20Fps, now);	// 200 ms par image
	EXPECT_EQ(5, d.GetStep());
}

// Au-delà de MaxStep, ce n'est plus la décimation qui peut sauver l'appel : le
// pas plafonne et le dit.
TEST(FrameDecimator, LePasPlafonneEtLeSignale)
{
	FrameDecimator d;
	QWORD now = 1000000;
	Feed(d, 100, Usable*100, Budget20Fps, now);
	EXPECT_EQ(FrameDecimator::MaxStep, d.GetStep());
	EXPECT_TRUE(d.IsSaturated());
}

// Le budget suit la source : la même charge est acceptable à 10 im/s et ne
// l'est pas à 30.
TEST(FrameDecimator, LeBudgetSuitLaCadenceDeLaSource)
{
	const QWORD cost = 60000;	// 60 ms par image

	FrameDecimator slow;
	QWORD now = 1000000;
	Feed(slow, 30, cost, 100000, now);	// source 10 im/s : 80 ms utilisables
	EXPECT_EQ(1, slow.GetStep());

	FrameDecimator fast;
	now = 1000000;
	Feed(fast, 30, cost, 33333, now);	// source 30 im/s : 26 ms utilisables
	EXPECT_EQ(3, fast.GetStep());
}

// L'appel du 2026-08-29 : x264 720p à 31-34 ms par image pour 32 ms utilisables.
// Le coût oscille de part et d'autre du seuil de montée ; avec un seul seuil le
// pas battait 1↔2 toutes les 16 s, une trame clé à chaque fois. Il doit monter à
// 2 une fois, puis ne plus bouger tant que le coût ne tient pas dans 7/10.
TEST(FrameDecimator, UnCoutSurLeSeuilNeFaitPasBattreLePas)
{
	FrameDecimator d;
	QWORD now = 1000000;
	const QWORD budget24Fps = 41000;	// 32,8 ms utilisables, comme dans l'appel

	// Le coût réel oscille lentement autour du seuil : ~6 s au-dessus, ~6 s en
	// dessous — plus long que les 3 s de calme, qui ne protègent donc pas.
	// Vingt minutes de ce régime.
	int changes = 0;
	for (int cycle = 0; cycle < 100; ++cycle)
	{
		changes += Feed(d, 146, 34000, budget24Fps, now);
		changes += Feed(d, 146, 30000, budget24Fps, now);
	}
	EXPECT_EQ(1, changes) << "une seule montee, aucune redescente";
	EXPECT_EQ(2, d.GetStep());

	// Le coût tombe franchement sous 7/10 de la part utilisable (23 ms) : là,
	// et seulement là, le pas redescend — après les 3 s d'usage.
	changes = Feed(d, 100, 15000, budget24Fps, now);
	EXPECT_EQ(1, changes);
	EXPECT_EQ(1, d.GetStep());
}

// Reset : nouveau codec, nouveau coût.
TEST(FrameDecimator, ResetRepartDeUn)
{
	FrameDecimator d;
	QWORD now = 1000000;
	Feed(d, 20, Usable*3, Budget20Fps, now);
	ASSERT_GT(d.GetStep(), 1);
	d.Reset();
	EXPECT_EQ(1, d.GetStep());
	EXPECT_EQ((QWORD)0, d.GetCostUs());
}

}	// namespace
