/**
 * test_rate_control.cpp — caractérisation du contrôle de débit (lot 0).
 *
 * Harnais du chantier rate-control (diagnostic : rate-control.md, plan :
 * rate_control_plan.md). Les classes sous test — RemoteRateEstimator et
 * RemoteRateControl — sont pures : on les nourrit de paquets synthétiques à
 * horloge SIMULÉE via l'overload Update(ssrc, now, ts, size, mark), et on
 * observe l'estimation par un Listener de capture.
 *
 * CONVENTION (la même que le tag :ipv6) : chaque test affirme le comportement
 * CORRECT. Ceux qui échouent sur le code actuel — ils caractérisent les défauts
 * §3.1/§3.2/§3.3/§3.4 du diagnostic — portent le préfixe DISABLED_ et sont donc
 * exclus de `make check` ; le tableau de bord du chantier est :
 *
 *     make check-ratecontrol
 *
 * Le lot 1 corrige les défauts et LÈVE les préfixes DISABLED_ un à un : chaque
 * test levé devient un garde-fou. Les tests SANS préfixe passent avant comme
 * après — ce sont les garde-fous anti-régression du comportement déjà correct.
 *
 * HISTORIQUE. Classement initial (lot 0, 2026-08-15) : 7 DISABLED_ (rouges),
 * 4 gardes-fous verts ; deux mesures du classement ont corrigé le diagnostic
 * (dérive linéaire invisible §3.4 e ; throttle qui se réarme §3.1 bis).
 * LOT 1 (2026-08-15) : les 7 défauts corrigés (alignement sur le témoin
 * ../webrtc), tous les préfixes DISABLED_ levés — la suite entière est jouée
 * par `make check` et fait garde-fou.
 */
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "rtp.h"
#include "remoterateestimator.h"
#include "remoteratecontrol.h"

namespace {

// Cadence vidéo simulée : 30 images/s de 1250 octets = 300 kb/s.
const DWORD kFrameMs    = 33;
const DWORD kFrameBytes = 1250;
const DWORD kTargetBps  = kFrameBytes * 8 * 1000 / kFrameMs; // ~303 kb/s

// Capture du seul point de sortie de l'estimateur.
class BitrateCapture : public RemoteRateEstimator::Listener
{
public:
	void onTargetBitrateRequested(DWORD bitrate) override { targets.push_back(bitrate); }
	std::vector<DWORD> targets;
};

// Nourrit l'estimateur (overload 5 args, horloge simulée) d'un flux régulier :
// une image par tick de kFrameMs, horloge média alignée sur l'horloge murale.
// Rend l'instant atteint.
QWORD FeedRegular(RemoteRateEstimator& estimator, DWORD ssrc, QWORD from, DWORD durationMs)
{
	QWORD now = from;
	for (QWORD end = from + durationMs; now < end; now += kFrameMs)
		estimator.Update(ssrc, now, /*ts=*/now, kFrameBytes, /*mark=*/true);
	return now;
}

// Fait basculer le détecteur d'un flux en OverUsing par le critère RTT
// (rtt > 1,5 × précédent) : le moyen le plus court d'armer le retour de perte.
void ForceOveruseViaRtt(RemoteRateEstimator& estimator, DWORD ssrc, QWORD now)
{
	estimator.UpdateRTT(ssrc, 50, now);   // mémorise 50 ms
	estimator.UpdateRTT(ssrc, 100, now);  // 100 > 1,5×50 → OverUsing
}

// ---------------------------------------------------------------------------
// Suite RateControlEstimator — la machine AIMD, au niveau RemoteRateEstimator.
// ---------------------------------------------------------------------------

// GARDE-FOU : pas de trafic, pas d'estimation — le contrat de GetEstimatedBitrate.
TEST(RateControlEstimator, LEstimateurRendZeroSansTrafic)
{
	RemoteRateEstimator estimator;
	EXPECT_EQ(0u, estimator.GetEstimatedBitrate());
}

// GARDE-FOU : nourri correctement (overload 5 args), l'estimateur suit un flux
// régulier. C'est la référence : le chemin de production doit faire pareil.
TEST(RateControlEstimator, LEstimationSuitUnFluxRegulier)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	// 75 s simulées : le premier tick AIMD n'arrive qu'après le retard initial
	// de 500 + 60 000 ms (rate-control.md, annexe B), puis un tick par seconde.
	FeedRegular(estimator, ssrc, /*from=*/100000, /*durationMs=*/75000);

	DWORD estimation = estimator.GetEstimatedBitrate();
	EXPECT_GE(estimation, kTargetBps / 2)     << "estimation " << estimation << " pour " << kTargetBps << " b/s entrants";
	EXPECT_LE(estimation, kTargetBps * 3)     << "estimation " << estimation << " pour " << kTargetBps << " b/s entrants";
}

// §3.1 — le chemin de PRODUCTION : rtpsession.cpp:3792 passait getTimeMS()
// comme TAILLE (chaque paquet déclarait ~2,6 milliards d'octets, estimation
// écrêtée au maximum en permanence). Depuis le lot 1 la taille sort du paquet
// (GetSize()) et l'argument piège n'existe plus. Ce test reproduit l'appel de
// production et exige le même résultat que le flux régulier ci-dessus.
TEST(RateControlEstimator, LEstimationSuitLeCheminDeProduction)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	RTPTimedPacket packet(MediaFrame::Video, /*codec=*/96, /*type=*/96);
	packet.SetSSRC(ssrc);
	packet.SetClockRate(90000);
	static BYTE payload[kFrameBytes];
	memset(payload, 0x42, sizeof(payload));
	packet.SetPayload(payload, kFrameBytes);
	packet.SetMark(true);

	// Base d'horloge réaliste (ms d'époque, 2026) : c'est sa troncature DWORD,
	// ~2,6 milliards, que le défaut §3.1 transforme en taille de paquet.
	QWORD now = 1755000000000ULL;
	DWORD ts  = 0;
	for (QWORD end = now + 75000; now < end; now += kFrameMs)
	{
		packet.SetTime(now);
		packet.SetTimestamp(ts += kFrameMs * 90); // 90 kHz
		// Depuis le lot 1, la taille sort du paquet : le 3e argument — qui
		// recevait getTimeMS() en production — n'existe plus.
		estimator.Update(ssrc, &packet);
	}

	DWORD estimation = estimator.GetEstimatedBitrate();
	EXPECT_GE(estimation, kTargetBps / 2) << "estimation " << estimation;
	EXPECT_LE(estimation, kTargetBps * 3) << "estimation " << estimation
		<< " — écrêtée au max configuré : la taille recue est un horodatage (§3.1)";
}

// §3.1 bis — rtpsession.cpp:3823 passe une TAILLE comme instant à UpdateLost.
// GARDE-FOU, et une MESURE du lot 0 qui corrige le diagnostic : le throttle
// n'est PAS « définitivement désarmé » — il se réarme seul au tick suivant
// (lastChange = now). Les dégâts réels de l'échange d'arguments sont un tick
// supplémentaire immédiat, avgChangePeriod empoisonné (~100 ticks à décroître
// de 0,9^n) et la conversion double→DWORD hors plage de responseTime (UB).
// Ce test fige le contrat qui, lui, tient déjà : un instant incohérent ne
// démultiplie pas les notifications.
TEST(RateControlEstimator, UnRapportDePerteNeDesarmePasLeThrottle)
{
	RemoteRateEstimator estimator;
	BitrateCapture capture;
	estimator.AddListener(&capture);
	const DWORD ssrc = 0x1234;

	QWORD now = FeedRegular(estimator, ssrc, 100000, 65000);
	ForceOveruseViaRtt(estimator, ssrc, now);

	// L'appel de production : le 3e argument est en réalité la taille du
	// paquet de perte (~1100), soit un « instant » très antérieur au présent.
	estimator.UpdateLost(ssrc, 3, 1100);

	// 2 s de flux régulier après le rapport : le throttle limite à ~1 tick/s.
	size_t before = capture.targets.size();
	FeedRegular(estimator, ssrc, now, 2000);
	size_t ticks = capture.targets.size() - before;

	EXPECT_LE(ticks, 6u) << ticks << " notifications en 2 s : le throttle "
		"une-par-seconde est désarmé (§3.1 bis)";
}

// Plancher 128 000 — SetTemporalMaxLimit rejette silencieusement tout maximum
// ≤ 128 kb/s (remoterateestimator.cpp:520), ce qui interdit d'annoncer un
// réseau lent. Contrat : une limite basse est acceptée et appliquée.
TEST(RateControlEstimator, UneLimiteTemporelleBasseEstRespectee)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	QWORD now = FeedRegular(estimator, ssrc, 100000, 65000);
	ASSERT_GT(estimator.GetEstimatedBitrate(), 64000u) << "prérequis : estimation établie";

	estimator.SetTemporalMaxLimit(64000);
	FeedRegular(estimator, ssrc, now, 3000); // au moins un tick applique la borne

	EXPECT_LE(estimator.GetEstimatedBitrate(), 64000u)
		<< "la limite 64 kb/s a été ignorée (plancher 128 000)";
}

// ---------------------------------------------------------------------------
// Suite RateControlDetector — le détecteur par flux, au niveau RemoteRateControl.
// ---------------------------------------------------------------------------

// §3.4 e — une file d'attente qui se remplit (chaque image arrive 2 ms plus
// tard que sa cadence média) doit être détectée comme surutilisation.
// HISTORIQUE (lot 0) : le code de 2013 ne la voyait JAMAIS — il passait au
// filtre curDelta−prevDelta, la dérivée SECONDE du délai, nulle pour une
// dérive linéaire dès la 3e image ; seule une accélération était visible.
// Le lot 1 passe curDelta lui-même (la première différence, comme le témoin).
TEST(RateControlDetector, UneDeriveDeDelaiConstanteEstDetectee)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	for (int i = 0; i < 400; ++i)
	{
		time += kFrameMs + 2;	// arrivée : 35 ms
		ts   += kFrameMs;	// média  : 33 ms → la file se remplit
		ctrl.Update(time, ts, 1000, /*mark=*/true);
	}
	EXPECT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage());
}

// Quand la dérive cesse, le détecteur doit revenir au calme — un détecteur
// figé en OverUsing (ou en Normal, §3.2) est un détecteur mort.
TEST(RateControlDetector, LeCalmeRevientApresLaCongestion)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	for (int i = 0; i < 400; ++i)
	{
		time += kFrameMs + 2;
		ts   += kFrameMs;
		ctrl.Update(time, ts, 1000, true);
	}
	ASSERT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage()) << "prérequis : congestion vue";

	// La file cesse de croître : cadence redevenue nominale.
	for (int i = 0; i < 400; ++i)
	{
		time += kFrameMs;
		ts   += kFrameMs;
		ctrl.Update(time, ts, 1000, true);
	}
	EXPECT_NE(RemoteRateControl::OverUsing, ctrl.GetUsage());
}

// §3.2 + §3.4 b — le facteur d'oubli est exponentié par une DIFFÉRENCE DE
// TAILLES signée (pow(1-alpha, deltaSize·30/1000)) : une image P après une
// grosse I rend varNoise négative, puis sqrt(varNoise) NaN, et le détecteur
// par délai meurt. Contrat : varNoise reste une variance — finie et positive.
TEST(RateControlDetector, LeBruitResteUneVariance)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;

	// Une image I de 100 Ko puis des P de 1 Ko : le scénario le plus banal du
	// monde vidéo, et le déclencheur du §3.2.
	ctrl.Update(time, ts, 100000, true);
	for (int i = 0; i < 60; ++i)
	{
		time += kFrameMs;
		ts   += kFrameMs;
		ctrl.Update(time, ts, 1000, true);
		double noise = ctrl.GetNoise();
		ASSERT_TRUE(std::isfinite(noise)) << "varNoise NaN/inf a l'image " << i;
		ASSERT_GE(noise, 0.0)             << "varNoise negative (" << noise << ") a l'image " << i;
	}
}

// §3.4 d — l'amont ne mesure le bruit qu'en état stable (Normal) : mesurer le
// bruit pendant une congestion revient à prendre la congestion pour du bruit
// (overuse_estimator.cc:62-68). La condition fut en commentaire de 2013 au
// lot 1. Contrat : varNoise est gelée en OverUsing.
TEST(RateControlDetector, LeBruitEstGeleEnSurutilisation)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	for (int i = 0; i < 400; ++i)
	{
		time += kFrameMs + 2;
		ts   += kFrameMs;
		ctrl.Update(time, ts, 1000, true);
	}
	ASSERT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage()) << "prérequis : congestion vue";

	// La congestion continue, avec des tailles d'image qui varient.
	double frozen = ctrl.GetNoise();
	for (int i = 0; i < 60; ++i)
	{
		time += kFrameMs + 2;
		ts   += kFrameMs;
		ctrl.Update(time, ts, (i % 2) ? 1100 : 900, true);
		ASSERT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage());
	}
	EXPECT_DOUBLE_EQ(frozen, ctrl.GetNoise())
		<< "varNoise mesurée pendant la surutilisation (§3.4 d)";
}

// §3.4 c — la mise à jour de la covariance s'écrase elle-même : E[1][0] et
// E[1][1] lisent E[0][0]/E[0][1] DÉJÀ réécrits (il manque les temporaires
// e00/e01 de l'amont). GARDE-FOU : l'invariant semi-défini positif — celui que
// l'amont vérifie par RTC_DCHECK (overuse_estimator.cc:90-93) — TIENT sur
// cette entrée malgré l'écrasement (mesure lot 0) ; le correctif (c) reste
// dicté par la comparaison au témoin, et ce test garde l'invariant pendant.
TEST(RateControlDetector, LaCovarianceResteSemiDefiniePositive)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	const DWORD sizes[] = { 600, 1400, 1000, 800 };
	const int   jitter[] = { 0, 4, -2, 1 };
	for (int i = 0; i < 500; ++i)
	{
		time += kFrameMs + jitter[i % 4];
		ts   += kFrameMs;
		ctrl.Update(time, ts, sizes[i % 4], true);
		ASSERT_TRUE(ctrl.CovarianceIsPositiveSemiDefinite())
			<< "covariance non semi-définie positive à l'image " << i << " (§3.4 c)";
	}
}

// §3.3 — le critère de perte comparait deux fenêtres dans des unités
// différentes (lostCalc en µs réelles via getTime(), packetCalc en ms de
// flux) : le ratio était gonflé au point que quelques pertes isolées valaient
// congestion (mesure lot 0 : bascule au 5e rapport d'UNE perte). Depuis le
// lot 1, UpdateLost reçoit l'horloge de l'appelant et les fenêtres sont
// alignées sur 1 s. Contrat : un flux réaliste (10 paquets/image, 300 pkt/s)
// perdant 1 paquet par seconde (0,3 %) n'est pas une congestion (seuil 2,5 %).
TEST(RateControlDetector, QuelquesPertesRaresNeSontPasUneCongestion)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	for (int i = 0; i < 300; ++i)
	{
		time += kFrameMs;
		ts   += kFrameMs;
		// Une image = 10 paquets (le cas vidéo réel), bit marqueur sur le dernier
		for (int p = 0; p < 10; ++p)
			ctrl.Update(time, ts, 100, /*mark=*/p == 9);
		// Un rapport d'une perte toutes les ~1,65 s (tous les 50 images)
		if (i % 50 == 49)
			ctrl.UpdateLost(1, time);
	}
	EXPECT_NE(RemoteRateControl::OverUsing, ctrl.GetUsage())
		<< "6 pertes isolées sur ~3000 paquets déclarées congestion (§3.3)";
}

} // namespace
