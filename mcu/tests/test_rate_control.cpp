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
 *
 * SUITE RateControlThreshold (2026-08-18) : deux défauts de la GARDE du
 * détecteur, rapportés par la séance de mesure du lot 3 — la re-montée
 * n'aboutit pas parce que des OverUsing interrompent la montée. LOT 1bis : les
 * deux corrigés, préfixes DISABLED_ levés ; la suite entière fait garde-fou.
 * La correction a demandé un TROISIÈME point que les mesures ont révélé : la
 * bascule exige que le délai continue de croître (offset >= prevOffset).
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <list>
#include <thread>
#include <unistd.h>
#include <vector>

#include "rtp.h"
#include "remoterateestimator.h"
#include "remoteratecontrol.h"
#include "rembthrottler.h"

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
// La taille d'image fixe le débit (défaut ~303 kb/s). Rend l'instant atteint.
QWORD FeedRegular(RemoteRateEstimator& estimator, DWORD ssrc, QWORD from, DWORD durationMs,
		  DWORD frameBytes = kFrameBytes)
{
	QWORD now = from;
	for (QWORD end = from + durationMs; now < end; now += kFrameMs)
		estimator.Update(ssrc, now, /*ts=*/now, frameBytes, /*mark=*/true);
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

// L'estimation doit exister TÔT. L'initialisation valait 500 ms + 60 s (un
// « TMMBR skipping delay »), et comme la réestimation périodique est gardée par
// lastChange + 1000 < now, aucune estimation n'arrivait avant 61,5 s de vidéo
// continue : tout appel plus court était inobservable — constaté en mesure le
// 2026-08-17, deux appels de 50 s sans une seule trace « BWE: estimation ».
// Contrat : une estimation sous les 10 s, et rien avant 2 s (les accumulateurs
// ont le temps de se remplir : on ne prononce pas un débit sur trois paquets).
TEST(RateControlEstimator, LEstimationArriveDansLesPremieresSecondes)
{
	RemoteRateEstimator estimator;
	BitrateCapture capture;
	const DWORD ssrc = 0x1234;
	const QWORD start = 100000;

	estimator.AddListener(&capture);

	FeedRegular(estimator, ssrc, start, /*durationMs=*/2000);
	EXPECT_TRUE(capture.targets.empty())
		<< "estimation prononcée après 2 s : les accumulateurs n'ont pas eu le temps de se remplir";

	FeedRegular(estimator, ssrc, start + 2000, /*durationMs=*/8000);
	ASSERT_FALSE(capture.targets.empty())
		<< "aucune estimation après 10 s de flux régulier — retard initial trop long";

	DWORD estimation = estimator.GetEstimatedBitrate();
	EXPECT_GE(estimation, kTargetBps / 2) << "estimation " << estimation;
	EXPECT_LE(estimation, kTargetBps * 3) << "estimation " << estimation;

	estimator.RemoveListener(&capture);
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

// ─── Plafond FENÊTRÉ (mesure alice_bob_1 du 2026-08-22) ─────────────────────
// Lien sain à 87 Mb/s, zéro OverUsing, et pourtant l'annonce s'effondrait de
// 826 à 165 kb/s en une seconde : le plafond glissant (1,5 × entrant) suivait
// chaque trou d'émission de la source — réouverture d'encodeur du pair — et le
// pair, qui obéit à la lettre à l'annonce, en faisait son nouveau régime ; la
// re-montée à +8 %/s donnait l'oscillation de 20 s. Contrat : sans signal de
// congestion, un trou plus court que la fenêtre ne fait pas chuter l'annonce.
TEST(RateControlEstimator, UnTrouDEmissionDeLaSourceNeFaitPasChuterLAnnonce)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;
	const DWORD kHighFrameBytes = 8250; // ~2 Mb/s à 30 im/s
	const DWORD kLowFrameBytes  = 250;  // ~60 kb/s : le trou

	QWORD now = FeedRegular(estimator, ssrc, 100000, 65000, kHighFrameBytes);
	DWORD before = estimator.GetEstimatedBitrate();
	ASSERT_GT(before, 1000000u) << "prérequis : estimation établie sur le flux à 2 Mb/s";

	now = FeedRegular(estimator, ssrc, now, 2000, kLowFrameBytes);
	DWORD during = estimator.GetEstimatedBitrate();

	EXPECT_GE(during, before * 9 / 10)
		<< "un trou d'émission de 2 s a fait chuter l'annonce de " << before
		<< " à " << during << " sans aucun signal de congestion";
}

// GARDE-FOU : l'anti-spirale reste — une baisse DURABLE de la source finit par
// être suivie, la fenêtre du plafond une fois écoulée. Sans lui, on annoncerait
// indéfiniment 1,5 × un débit qui ne circule plus.
TEST(RateControlEstimator, UneBaisseDurableDeLaSourceEstSuivie)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	QWORD now = FeedRegular(estimator, ssrc, 100000, 65000, 8250);
	ASSERT_GT(estimator.GetEstimatedBitrate(), 1000000u) << "prérequis : estimation établie";

	// 15 s à ~60 kb/s : la fenêtre (5 s) est largement écoulée.
	FeedRegular(estimator, ssrc, now, 15000, 250);

	EXPECT_LE(estimator.GetEstimatedBitrate(), 200000u)
		<< "l'anti-spirale est perdu : l'annonce ne suit plus une baisse durable";
}

// RÉGRESSION (mcu-gris.log, 2026-08-26) : une renégociation (ajout/retrait de
// piste audio) a interrompu toute réception vidéo plus de 5 s (la fenêtre du
// plafond glissant) ; à la reprise, au même débit qu'avant la coupure, le
// plafond a annoncé le plancher (16 kb/s) au lieu de suivre la reprise —
// l'image est restée grise le temps d'une remontée AIMD de plusieurs dizaines
// de secondes. Une coupure TOTALE n'est pas un signal de congestion, à la
// différence d'une baisse durable mais non nulle (test ci-dessus).
TEST(RateControlEstimator, UneCoupureDePlusDe5sNeFaitPasChuterLAnnonce)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;
	const DWORD kHighFrameBytes = 8250; // ~2 Mb/s à 30 im/s

	QWORD now = FeedRegular(estimator, ssrc, 100000, 65000, kHighFrameBytes);
	DWORD before = estimator.GetEstimatedBitrate();
	ASSERT_GT(before, 1000000u) << "prérequis : estimation établie sur le flux à 2 Mb/s";

	// Coupure totale (aucun paquet), plus longue que la fenêtre de 5 s.
	now += 5200;

	// Reprise au même débit qu'avant la coupure.
	estimator.Update(ssrc, now, /*ts=*/now, kHighFrameBytes, /*mark=*/true);
	DWORD after = estimator.GetEstimatedBitrate();

	EXPECT_GE(after, before / 2)
		<< "une coupure de 5,2 s sans trafic a fait chuter l'annonce de " << before
		<< " à " << after << " au lieu de suivre la reprise";
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

// ─────────────────────────────────────────────────────────────────────────────
// LOT 2 — l'amortisseur du feedback sortant (RembThrottler) et le paquet REMB.
//
// L'amortisseur est pur : horloge donnée par l'appelant, aucune émission. Les
// tests énoncent la règle du témoin — une baisse part tout de suite, une hausse
// attend 200 ms, un plafond externe compose par min().
// ─────────────────────────────────────────────────────────────────────────────

// La toute première estimation part sans attendre : rien n'a encore été dit au
// pair, et le retenir 200 ms retarderait l'ouverture de la boucle.
TEST(RateControlThrottler, LaPremiereAnnoncePartTOutDeSuite)
{
	RembThrottler throttler;
	DWORD out = 0;

	EXPECT_TRUE(throttler.OnEstimateChanged(500000, 0, out));
	EXPECT_EQ(500000u, out);
}

// Une hausse est une bonne nouvelle : elle peut attendre la période. C'est tout
// l'objet de l'amortisseur — ne pas émettre un paquet RTCP par soubresaut de
// l'estimateur.
TEST(RateControlThrottler, UneHausseAttendLaPeriode)
{
	RembThrottler throttler;
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(500000, 1000, out));

	EXPECT_FALSE(throttler.OnEstimateChanged(600000, 1100, out))
		<< "hausse annoncée avant les 200 ms de la période";
	EXPECT_TRUE(throttler.OnEstimateChanged(600000, 1201, out));
	EXPECT_EQ(600000u, out);
}

// Une baisse franche est le message urgent : le lien sature, le pair doit
// ralentir maintenant, pas dans 200 ms.
TEST(RateControlThrottler, UneBaisseFranchePartImmediatement)
{
	RembThrottler throttler;
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(500000, 1000, out));

	EXPECT_TRUE(throttler.OnEstimateChanged(300000, 1010, out))
		<< "baisse de 40 % retenue par la période";
	EXPECT_EQ(300000u, out);
}

// Le seuil de 3 % sépare la baisse d'un bruit de mesure : sous le seuil, on
// attend la période comme pour une hausse.
TEST(RateControlThrottler, UneBaisseDansLeBruitAttendLaPeriode)
{
	RembThrottler throttler;
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(1000000, 1000, out));

	// -1 % : 990000 * 1,03 = 1 019 700 > 1 000 000, donc retenu.
	EXPECT_FALSE(throttler.OnEstimateChanged(990000, 1100, out));
	// -5 % : 950000 * 1,03 = 978 500 < 1 000 000, donc immédiat.
	EXPECT_TRUE(throttler.OnEstimateChanged(950000, 1110, out));
	EXPECT_EQ(950000u, out);
}

// Le plafond venu de l'autre patte (lot 5) compose par min() : on annonce le
// plus contraint des deux, jamais la mesure locale seule.
TEST(RateControlThrottler, LePlafondExterneComposeParMin)
{
	RembThrottler throttler;
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(2000000, 1000, out));
	ASSERT_EQ(2000000u, out);

	// Le plafond mord : il part tout de suite.
	EXPECT_TRUE(throttler.SetMaxBitrate(800000, 1010, out));
	EXPECT_EQ(800000u, out);

	// Et il continue de mordre sur les estimations suivantes.
	ASSERT_TRUE(throttler.OnEstimateChanged(2000000, 1300, out));
	EXPECT_EQ(800000u, out) << "l'estimation locale a ignoré le plafond externe";

	// La mesure locale reste la mesure locale : c'est elle qui est mémorisée,
	// pas la composition — sinon la levée du plafond ne rendrait rien.
	EXPECT_EQ(2000000u, throttler.GetLastSent());
}

// Un plafond qui ne mord pas et qui arrive dans la période n'a rien à dire au
// pair : pas de paquet.
TEST(RateControlThrottler, UnPlafondQuiNeMordPasNEmetRien)
{
	RembThrottler throttler;
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(500000, 1000, out));

	EXPECT_FALSE(throttler.SetMaxBitrate(900000, 1050, out));
}

// Le plafond ne s'oublie pas quand la mesure locale passe en dessous puis
// remonte : la levée doit être explicite.
TEST(RateControlThrottler, LePlafondSurvitAUneMesureBasse)
{
	RembThrottler throttler;
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(2000000, 1000, out));
	ASSERT_TRUE(throttler.SetMaxBitrate(800000, 1010, out));

	// Mesure locale sous le plafond : c'est elle qui gouverne.
	ASSERT_TRUE(throttler.OnEstimateChanged(400000, 1050, out));
	EXPECT_EQ(400000u, out);

	// Elle remonte : le plafond reprend la main.
	ASSERT_TRUE(throttler.OnEstimateChanged(2000000, 1400, out));
	EXPECT_EQ(800000u, out);

	// Levée explicite du plafond.
	ASSERT_TRUE(throttler.SetMaxBitrate(RembThrottler::NoLimit, 1700, out));
	EXPECT_EQ(2000000u, out);
}

// ─── Politique du dialecte TMMBR (mesures alice_bob_1 2026-08-22, §7.9 2026-08-30) ──
// Linphone détruit et recrée son encodeur VP8 à CHAQUE TMMBR de valeur différente
// (msvideoqualitycontroller.c → vp8.c enc_set_configuration) : une trame clé par
// annonce. Le TMMBR est collant (RFC 5104), donc aucune raison de redire une valeur
// voisine, jamais : seul un pas franc de hausse part, et une baisse n'est franche
// qu'à 10 % (un pas d'AIMD vaut 15 %, le bruit du plafond glissant 3 %).

TEST(RateControlThrottler, EnDialecteTMMBRLeBruitNEmetJamais)
{
	RembThrottler throttler;
	throttler.SetPolicy(RembThrottler::TmmbrPolicy);
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(4500000, 1000, out));

	// 60 s d'estimations qui oscillent de ±5 % autour de 4,5 Mb/s, une par
	// seconde : c'est le régime mesuré le 2026-08-30 (pair auto-limité, plafond
	// à 1,5 x l'entrant). Avec la période de 5 s, une annonce partait toutes les
	// 5 s et Linphone produisait une trame clé toutes les 2,6 s.
	const DWORD values[] = { 4600000, 4400000, 4550000, 4290000, 4700000, 4450000 };
	for (int i = 0; i < 60; i++)
		EXPECT_FALSE(throttler.OnEstimateChanged(values[i % 6], 2000 + i * 1000, out))
			<< "annonce partie à t=" << 2000 + i * 1000 << " pour " << values[i % 6];
}

TEST(RateControlThrottler, EnDialecteTMMBRSeulUnPasFrancDeHaussePart)
{
	RembThrottler throttler;
	throttler.SetPolicy(RembThrottler::TmmbrPolicy);
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(1000000, 1000, out));

	// +19 % : sous le pas franc, ne part pas — même après 10 s.
	EXPECT_FALSE(throttler.OnEstimateChanged(1190000, 2000, out));
	EXPECT_FALSE(throttler.OnEstimateChanged(1190000, 12000, out))
		<< "le temps seul a fait partir une hausse en TMMBR";
	// +20 % : part sans attendre.
	EXPECT_TRUE(throttler.OnEstimateChanged(1200000, 12100, out));
	EXPECT_EQ(1200000u, out);
}

TEST(RateControlThrottler, EnDialecteTMMBRUneBaisseDansLeBruitNEmetPas)
{
	RembThrottler throttler;
	throttler.SetPolicy(RembThrottler::TmmbrPolicy);
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(1000000, 1000, out));

	// -5 % : bruit du plafond glissant, pas une congestion.
	EXPECT_FALSE(throttler.OnEstimateChanged(950000, 1200, out));
	EXPECT_FALSE(throttler.OnEstimateChanged(950000, 9000, out));
	// -10 % : franche, part tout de suite.
	EXPECT_TRUE(throttler.OnEstimateChanged(900000, 9100, out));
	EXPECT_EQ(900000u, out);
}

TEST(RateControlThrottler, EnDialecteTMMBRUneBaisseFranchePartToujoursImmediatement)
{
	RembThrottler throttler;
	throttler.SetPolicy(RembThrottler::TmmbrPolicy);
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(1000000, 1000, out));

	EXPECT_TRUE(throttler.OnEstimateChanged(850000, 1200, out))
		<< "une baisse de 15 % (un pas d'AIMD) retenue";
	EXPECT_EQ(850000u, out);
}

// Le dialecte REMB garde ses seuils : une baisse de 5 % part, une hausse attend
// 200 ms puis part. Chrome attend des annonces périodiques et n'a pas le défaut
// de Linphone.
TEST(RateControlThrottler, LeDialecteREMBGardeSesSeuils)
{
	RembThrottler throttler;
	throttler.SetPolicy(RembThrottler::RembPolicy);
	DWORD out = 0;

	ASSERT_TRUE(throttler.OnEstimateChanged(1000000, 1000, out));
	EXPECT_TRUE(throttler.OnEstimateChanged(950000, 1010, out));
	EXPECT_FALSE(throttler.OnEstimateChanged(960000, 1100, out));
	EXPECT_TRUE(throttler.OnEstimateChanged(960000, 1211, out));
}

// Le champ REMB annonce le NOMBRE de SSRC qu'il porte : la valeur était écrite
// en dur à 1 alors que la liste en sérialise autant qu'elle en contient — un
// REMB à deux flux se lisait amputé du second.
TEST(RateControlRemb, LeChampAnnonceTousSesSSRC)
{
	std::list<DWORD> ssrcs;
	ssrcs.push_back(0x11111111);
	ssrcs.push_back(0x22222222);

	RTCPPayloadFeedback::ApplicationLayerFeeedbackField* field =
		RTCPPayloadFeedback::ApplicationLayerFeeedbackField::CreateReceiverEstimatedMaxBitrate(ssrcs, 500000);

	BYTE* payload = field->GetPayload();
	ASSERT_EQ(8u + 4 * 2, field->GetLength());
	EXPECT_EQ('R', payload[0]);
	EXPECT_EQ('E', payload[1]);
	EXPECT_EQ('M', payload[2]);
	EXPECT_EQ('B', payload[3]);
	EXPECT_EQ(2, payload[4]) << "Num SSRC ne compte pas les SSRC réellement portés";
	EXPECT_EQ(0x11111111u, get4(payload, 8));
	EXPECT_EQ(0x22222222u, get4(payload, 12));

	delete field;
}

// L'exposant/mantisse du REMB : le débit relu doit retomber sur celui qu'on a
// demandé, à la précision des 18 bits de mantisse près (0,4 % au pire).
TEST(RateControlRemb, LeDebitSeRelitAvecSaPrecision)
{
	const DWORD rates[] = { 16000, 300000, 2500000, 30000000, 0xFFFFFFFF };

	for (DWORD rate : rates)
	{
		std::list<DWORD> ssrcs;
		ssrcs.push_back(0x33333333);

		RTCPPayloadFeedback::ApplicationLayerFeeedbackField* field =
			RTCPPayloadFeedback::ApplicationLayerFeeedbackField::CreateReceiverEstimatedMaxBitrate(ssrcs, rate);

		BYTE* payload  = field->GetPayload();
		BYTE  exp      = payload[5] >> 2;
		QWORD mantissa = ((QWORD)(payload[5] & 0x03) << 16) | ((QWORD)payload[6] << 8) | payload[7];
		QWORD decoded  = mantissa << exp;

		// La troncature de la mantisse ne peut que sous-estimer, jamais gonfler
		// le débit annoncé — annoncer plus que mesuré dirait « fonce » à un pair
		// qui sature.
		EXPECT_LE(decoded, (QWORD)rate) << "débit " << rate;
		EXPECT_GE(decoded * 1000, (QWORD)rate * 995) << "débit " << rate;

		delete field;
	}
}

// Le vrai listener n'est pas passif : RTPSession::onTargetBitrateRequested
// envoie le REMB, et pour nommer les flux qu'il couvre il rappelle
// GetSSRCs(). Le harnais, lui, ne rappelait jamais l'estimateur — c'est
// exactement ce que ce test ajoute.
class ReentrantCapture : public RemoteRateEstimator::Listener
{
public:
	ReentrantCapture(RemoteRateEstimator& estimator) : estimator(estimator) {}

	void onTargetBitrateRequested(DWORD bitrate) override
	{
		estimator.GetSSRCs(ssrcs);
		estimated = estimator.GetEstimatedBitrate();
		notifications++;
	}

	RemoteRateEstimator&	estimator;
	std::list<DWORD>	ssrcs;
	DWORD			estimated = 0;
	int			notifications = 0;
};

// GARDE-FOU : un listener a le droit d'interroger l'estimateur depuis la
// notification. La consigne était publiée SOUS le verrou écrivain, et Use n'est
// pas réentrant : le premier REMB pendait le thread RTP définitivement — la
// patte vidéo muette, puis toute destruction de session bloquée derrière
// (RTPSession::DeleteStreams attend un IncUse qui ne sera jamais rendu).
TEST(RateControlEstimator, UnListenerPeutInterrogerLEstimateurDepuisLaNotification)
{
	// Sur le tas et volontairement fui si le thread pend : un thread bloqué ne
	// se joint pas, et détruire l'estimateur sous ses pieds remplacerait le
	// diagnostic par un crash.
	RemoteRateEstimator* estimator = new RemoteRateEstimator();
	ReentrantCapture* listener = new ReentrantCapture(*estimator);

	estimator->AddListener(listener);
	estimator->AddStream(0x1234);

	std::atomic<bool> done(false);
	std::thread feeder([&] {
		FeedRegular(*estimator, 0x1234, 1000, 8000);
		done = true;
	});

	for (int i = 0; i < 500 && !done; i++)
		usleep(10000);

	if (!done)
	{
		feeder.detach();
		FAIL() << "notification sous verrou : le thread qui nourrit l'estimateur est pendu";
	}

	feeder.join();
	EXPECT_GT(listener->notifications, 0) << "aucune consigne publiée en 8 s de trafic";
	EXPECT_FALSE(listener->ssrcs.empty()) << "le listener n'a pas pu lire les SSRC couverts";

	estimator->RemoveListener(listener);
	delete listener;
	delete estimator;
}

// Listener lent : il tient la notification assez longtemps pour qu'un autre
// thread tente de le retirer PENDANT qu'il est appelé.
class SlowCapture : public RemoteRateEstimator::Listener
{
public:
	void onTargetBitrateRequested(DWORD bitrate) override
	{
		notifying = true;
		usleep(300000);
		calls++;
		notifying = false;
	}

	std::atomic<bool>	notifying{false};
	std::atomic<int>	calls{0};
};

// GARDE-FOU : RemoveListener doit ATTENDRE la fin d'une notification en vol.
// Sinon ~RTPSession libère la session alors que le thread RTP d'une autre jambe
// du même Endpoint est en train de l'appeler — écriture après libération, tas
// corrompu, et crash différé ailleurs (ici, dans le SSL_free du DTLS).
TEST(RateControlEstimator, RemoveListenerAttendLaNotificationEnVol)
{
	RemoteRateEstimator estimator;
	SlowCapture listener;

	estimator.AddListener(&listener);
	estimator.AddStream(0x1234);

	std::atomic<bool> done(false);
	std::thread feeder([&] {
		FeedRegular(estimator, 0x1234, 1000, 8000);
		done = true;
	});

	// Attendre d'être effectivement DANS la notification.
	for (int i = 0; i < 1000 && !listener.notifying && !done; i++)
		usleep(1000);
	ASSERT_TRUE(listener.notifying) << "aucune notification observée : le test ne prouve rien";

	estimator.RemoveListener(&listener);
	// C'est l'assertion utile : au retour, plus rien ne tient le listener.
	EXPECT_FALSE(listener.notifying)
		<< "RemoveListener a rendu la main pendant une notification : "
		   "le listener peut être détruit sous les pieds de l'appelant";

	const int atRemoval = listener.calls;
	usleep(500000);
	EXPECT_EQ(atRemoval, listener.calls) << "notifié après RemoveListener";

	feeder.join();
}

// ---------------------------------------------------------------------------
// Suite RateControlThreshold — la garde du détecteur de délai : ce qui décide
// qu'un dépassement de seuil est une congestion. Séance de mesure du
// 2026-08-18 (annexe D) : la re-montée n'aboutit jamais parce que la montée est
// INTERROMPUE par des OverUsing à mi-course, qui clouent l'estimation à son
// maximum connu — 1210 kb/s annoncés pour 1804 kb/s réellement reçus.
//
// Le verdict ne s'observe PAS sur l'état final : un épisode dure quelques
// images et l'hypothèse revient à Normal juste après. C'est le nombre
// d'épisodes qui compte, relevé après chaque image.
// ---------------------------------------------------------------------------

// Compte les entrées en OverUsing d'un détecteur nourri image par image.
class OveruseCounter
{
public:
	explicit OveruseCounter(RemoteRateControl& ctrl) : ctrl(ctrl) {}

	// Une image à la cadence nominale, retardée de extraMs : le retard est
	// permanent, les images suivantes repartent de là. Un à-coup, pas une file
	// qui se remplit.
	void Feed(int frames, DWORD extraMs = 0)
	{
		for (int i = 0; i < frames; ++i)
		{
			time += kFrameMs + extraMs;
			ts   += kFrameMs;
			ctrl.Update(time, ts, 1000, /*mark=*/true);
			Observe();
		}
	}

	// Releve l'hypothese apres une image nourrie par ailleurs : un episode dure
	// quelques images et l'etat final n'en garde aucune trace.
	void Observe()
	{
		const bool over = ctrl.GetUsage() == RemoteRateControl::OverUsing;
		if (over && !wasOver) ++episodes;
		wasOver = over;
	}

	int episodes = 0;

private:
	RemoteRateControl& ctrl;
	QWORD time = 100000;
	QWORD ts   = 100000;
	bool wasOver = false;
};

// GARDE-FOU : un à-coup unique — une file qui prend 40 ms de plus et les garde,
// le plus banal des événements réseau — ne caractérise pas une congestion. Vrai
// aujourd'hui, à ne pas perdre en durcissant la garde.
TEST(RateControlThreshold, UnAcoupIsoleNeDeclarePasDeCongestion)
{
	RemoteRateControl ctrl;
	OveruseCounter flux(ctrl);

	flux.Feed(300);
	ASSERT_EQ(0, flux.episodes) << "prérequis : un flux régulier ne déclare rien";

	flux.Feed(1, /*extraMs=*/40);
	flux.Feed(150);
	EXPECT_EQ(0, flux.episodes) << "congestion déclarée sur un à-coup isolé de 40 ms";
}

// Le compteur de candidats n'est remis à zéro, dans la branche « sous le seuil »,
// que si l'hypothèse CHANGEAIT — donc jamais sur un flux déjà Normal
// (remoteratecontrol.cpp:241-252). Il compte « 3 dépassements depuis toujours »
// et non « 3 consécutifs » : les candidats survivent à des secondes de trafic
// sain et s'additionnent. Mesuré : un à-coup de 32 ms ne déclare rien, trois
// à-coups de 32 ms séparés de 2 s de calme chacun déclarent une congestion.
// Contrat : ce qui caractérise une congestion est une DURÉE de surutilisation
// continue, remise à zéro dès le retour sous le seuil — ce que mesure le témoin.
TEST(RateControlThreshold, LesCandidatsNeSurviventPasAuRetourAuCalme)
{
	RemoteRateControl ctrl;
	OveruseCounter flux(ctrl);

	flux.Feed(300);
	flux.Feed(1, /*extraMs=*/32);
	flux.Feed(60);
	ASSERT_EQ(0, flux.episodes) << "prérequis : un à-coup de 32 ms seul ne suffit pas";

	flux.Feed(1, /*extraMs=*/32);
	flux.Feed(60);
	flux.Feed(1, /*extraMs=*/32);
	flux.Feed(60);
	EXPECT_EQ(0, flux.episodes)
		<< "congestion déclarée par cumul de trois à-coups que 2 s de trafic sain séparent";
}

// Le seuil est fixe par région : 35 ms en BelowMax, 25 en MaxUnknown, 12 en
// NearMax et AboveMax (remoteratecontrol.cpp:327). Dès que l'estimation
// approche son maximum connu, le seuil tombe à 12 ms — or le T comparé vaut
// min(fps,30)·offset, donc un offset instantané de 1,3 ms y devient 39 ms. Le
// cercle est fermé : OverUsing -> Decrease -> NearMax -> seuil 12 -> OverUsing
// plus facile encore, et l'estimation ne franchit jamais son propre maximum.
// L'à-coup de 20 ms est choisi entre les deux seuils : il isole l'effet de la
// région, à signal d'arrivée rigoureusement identique.
// Contrat : la région ne décide pas seule du verdict.
TEST(RateControlThreshold, LeVerdictNeDependPasDeLaRegion)
{
	RemoteRateControl below, nearMax;
	below.SetRateControlRegion(RemoteRateControl::BelowMax);
	nearMax.SetRateControlRegion(RemoteRateControl::NearMax);
	OveruseCounter fluxBelow(below), fluxNear(nearMax);

	for (OveruseCounter* flux : { &fluxBelow, &fluxNear })
	{
		flux->Feed(300);
		for (int acoup = 0; acoup < 3; ++acoup)
		{
			flux->Feed(1, /*extraMs=*/20);
			flux->Feed(60);
		}
	}

	EXPECT_EQ(fluxBelow.episodes, fluxNear.episodes)
		<< "même signal, verdicts différents : " << fluxBelow.episodes
		<< " episode(s) en BelowMax (seuil 35 ms) contre " << fluxNear.episodes
		<< " en NearMax (seuil 12 ms)";
}

// GARDE-FOU de la correction à venir : durcir la garde ou relever le seuil ne
// doit pas rendre le détecteur sourd. Une file qui se remplit vraiment reste
// vue, y compris dans la région où le seuil est le plus bas.
TEST(RateControlThreshold, UneVraieCongestionResteVueEnRegionNearMax)
{
	RemoteRateControl ctrl;
	ctrl.SetRateControlRegion(RemoteRateControl::NearMax);
	OveruseCounter flux(ctrl);
	flux.Feed(400, /*extraMs=*/2);	// la file se remplit de 2 ms par image
	EXPECT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage());
	EXPECT_GT(flux.episodes, 0);
}



// ---------------------------------------------------------------------------
// Suite RateControlJitter — la gigue ne doit pas mentir. Le critere de la seance
// est « faux positifs <= 10 % des echantillons » ; un test le prononce plus
// durement : ZERO bascule sur une gigue sans derive. Conception : lot 3bis de
// rate_control_plan.md.
// ---------------------------------------------------------------------------

// Gigue deterministe. Pas de rand() : un test doit se rejouer a l'identique, et
// un defaut qui n'apparait qu'une fois sur trois ne se corrige pas. Le
// generateur est un LCG minimal, dont on ne veut que la reproductibilite.
class Jitter
{
public:
	explicit Jitter(DWORD seed) : state(seed) {}

	// Ecart signe, borne a +/- amplitudeMs.
	int Next(int amplitudeMs)
	{
		state = state * 1103515245u + 12345u;
		const int span = 2 * amplitudeMs + 1;
		return (int)((state >> 16) % span) - amplitudeMs;
	}

private:
	DWORD state;
};

// Nourrit un detecteur d'un flux dont l'horloge MEDIA est reguliere et dont
// l'ARRIVEE porte une gigue. L'instant d'arrivee reste strictement croissant :
// un paquet qui doublerait le precedent serait ignore par la garde de
// desordonnancement, et le test ne mesurerait plus rien.
// driftMs ajoute une derive permanente par image : c'est la file qui se remplit.
void FeedJittered(OveruseCounter& flux, RemoteRateControl& ctrl,
                  QWORD& time, QWORD& ts, int frames,
                  Jitter& jitter, int amplitudeMs, int driftMs = 0)
{
	QWORD nominal = time;
	for (int i = 0; i < frames; ++i)
	{
		nominal += kFrameMs + driftMs;
		const int ecart = jitter.Next(amplitudeMs);
		QWORD arrivee = (QWORD)std::max<int64_t>((int64_t)nominal + ecart, (int64_t)time + 1);
		time = arrivee;
		ts  += kFrameMs;
		ctrl.Update(time, ts, 1000, /*mark=*/true);
		flux.Observe();
	}
}

// Une gigue symetrique ne remplit aucune file : le delai moyen ne bouge pas. La
// declarer congestion, c'est demander a la source de ralentir pour un lien qui
// n'est pas charge.
//
// MESURE (2026-08-18) : le filtre absorbe une gigue non biaisee jusqu'a
// +/-100 ms, et aussi par bouffees correlees de 5 images — zero candidat, pas
// seulement zero bascule. Ce test forme une PAIRE avec
// UneGigueQuiCacheUneDeriveLaLaissePasser : meme generateur, meme amplitude,
// seule la derive change, et lui produit 15 candidats et 2 episodes. C'est cette
// paire qui donne sa valeur au verdict — un test de faux positif qui ne
// s'accompagne pas de son pendant ne prouve pas que le harnais mord.
TEST(RateControlJitter, UneGigueSymetriqueNEstPasUneCongestion)
{
	RemoteRateControl ctrl;
	OveruseCounter flux(ctrl);
	Jitter jitter(1);
	QWORD time = 100000, ts = 100000;

	FeedJittered(flux, ctrl, time, ts, 300, jitter, /*amplitudeMs=*/2);
	ASSERT_EQ(0, flux.episodes) << "prérequis : la gigue de base ne déclare rien";

	FeedJittered(flux, ctrl, time, ts, 600, jitter, /*amplitudeMs=*/30);
	EXPECT_EQ(0, flux.episodes)
		<< "congestion déclarée sur une gigue de +/-30 ms sans dérive";
}

// Le meme signal, au niveau de l'estimateur : ce que la gigue coute vraiment
// n'est pas une bascule d'hypothese, c'est le debit qu'on annonce au pair.
TEST(RateControlJitter, UneGigueNAffaissePasLEstimation)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;
	Jitter jitter(2);

	QWORD nominal = 100000, time = 100000, ts = 100000;
	for (int i = 0; i < 2400; ++i)	// ~80 s : le premier tick AIMD passe
	{
		nominal += kFrameMs;
		const int ecart = i < 300 ? jitter.Next(2) : jitter.Next(30);
		time = (QWORD)std::max<int64_t>((int64_t)nominal + ecart, (int64_t)time + 1);
		ts  += kFrameMs;
		estimator.Update(ssrc, time, ts, kFrameBytes, /*mark=*/true);
	}

	const DWORD estimation = estimator.GetEstimatedBitrate();
	EXPECT_GE(estimation, (DWORD)(kTargetBps * 0.75))
		<< "estimation " << estimation << " effondrée par la gigue, pour "
		<< kTargetBps << " b/s réellement reçus";
}

// GARDE-FOU, et pendant de UneGigueSymetriqueNEstPasUneCongestion : le prix a
// ne pas payer pour les deux tests precedents. On corrige un faux positif en
// rendant le detecteur plus tolerant, et la tolerance a une limite — une file
// qui se remplit VRAIMENT doit rester vue, meme noyee dans du bruit d'arrivee de
// meme amplitude. Mesure : 15 candidats, 2 episodes.
TEST(RateControlJitter, UneGigueQuiCacheUneDeriveLaLaissePasser)
{
	RemoteRateControl ctrl;
	OveruseCounter flux(ctrl);
	Jitter jitter(3);
	QWORD time = 100000, ts = 100000;

	FeedJittered(flux, ctrl, time, ts, 300, jitter, /*amplitudeMs=*/2);
	FeedJittered(flux, ctrl, time, ts, 600, jitter, /*amplitudeMs=*/30, /*driftMs=*/2);
	EXPECT_GT(flux.episodes, 0)
		<< "dérive de 2 ms par image manquée parce que noyée dans +/-30 ms de gigue";
}

// Une image I est plus grosse, donc plus longue a serialiser : elle arrive en
// retard et les images P qui suivent rattrapent. La cadence moyenne est tenue,
// aucune file ne se remplit. C'est le motif le plus banal d'un flux video, et
// c'est celui qui a produit un entrant a 47 kb/s puis un burst dans la seance du
// 2026-08-18.
TEST(RateControlJitter, UneRafaleDeTramesNEstPasUneCongestion)
{
	RemoteRateControl ctrl;
	OveruseCounter flux(ctrl);
	QWORD time = 100000, ts = 100000;

	for (int i = 0; i < 900; ++i)	// 30 s, un GOP par seconde
	{
		const bool cle = (i % 30) == 0;
		ts += kFrameMs;
		// L'image cle prend 20 ms de plus a passer le lien ; la suivante arrive
		// d'autant plus tot, la cadence moyenne est tenue.
		const int retard = cle ? 20 : ((i % 30) == 1 ? -20 : 0);
		time = (QWORD)std::max<int64_t>((int64_t)time + kFrameMs + retard, (int64_t)time + 1);
		ctrl.Update(time, ts, cle ? 20000 : 1000, /*mark=*/true);
		flux.Observe();
	}

	EXPECT_EQ(0, flux.episodes)
		<< "congestion déclarée sur un GOP régulier, sans file qui se remplisse";
}

// Nourrit un flux video sain d'un rapport de perte par seconde, comme le fait
// RTCP — y compris quand il n'y a rien a perdre : c'est ce rapport a zero qui
// ramene le chemin au calme.
void FeedLossPhase(RemoteRateControl& ctrl, QWORD& time, QWORD& ts,
                   int seconds, DWORD lostPerReport)
{
	for (int s = 0; s < seconds; ++s)
	{
		for (int i = 0; i < 30; ++i)
		{
			time += kFrameMs;
			ts   += kFrameMs;
			for (int p = 0; p < 10; ++p)
				ctrl.Update(time, ts, 100, /*mark=*/p == 9);
		}
		ctrl.UpdateLost(lostPerReport, time);
	}
}

// ---------------------------------------------------------------------------
// Suite RateControlLoss — le chemin de perte (UpdateLost), que ni l'escalier ni
// la gigue n'exercent. Conception : lot 3bis de rate_control_plan.md.
// ---------------------------------------------------------------------------

// Un seul test couvrait ce chemin, et il vérifiait une ABSENCE de détection : le
// sens utile du seuil de 2,5 % n'était pas couvert.
TEST(RateControlLoss, UnePerteMassiveEstUneCongestion)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	for (int i = 0; i < 305; ++i)
	{
		time += kFrameMs;
		ts   += kFrameMs;
		for (int p = 0; p < 10; ++p)
			ctrl.Update(time, ts, 100, /*mark=*/p == 9);
		// 20 % de pertes, rapportées une fois par seconde comme le fait RTCP.
		// La boucle s'achève sur cinq images SANS rapport : sans elles le test
		// passerait par coïncidence de calendrier, en relevant l'hypothèse à
		// l'instant précis du dernier rapport.
		if (i < 300 && i % 30 == 29)
			ctrl.UpdateLost(60, time);
	}
	EXPECT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage())
		<< "20 % de pertes soutenues pendant 10 s ne sont pas vues";
}

// Les deux détecteurs partageaient `hypothesis`, et le chemin du délai la
// réécrivait à CHAQUE image : une seule image au délai sain remettait Normal et
// effaçait la congestion que les pertes venaient de déclarer (mesuré :
// `OverUsing` à 0 image saine, `Normal` dès la 1re). Sur un lien à pertes sans
// bufferbloat — Wi-Fi, radio — le détecteur de pertes ne pouvait rien déclarer de
// durable. Chaque chemin porte désormais son hypothèse, composées par GetUsage().
// Contrat : une congestion vue par les pertes survit à un délai sain.
TEST(RateControlLoss, UnDelaiSainNEffacePasLAccumulationDesPertes)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	for (int i = 0; i < 200; ++i)
	{
		time += kFrameMs;	// arrivée exactement à l'heure : rien à détecter
		ts   += kFrameMs;
		for (int p = 0; p < 10; ++p)
			ctrl.Update(time, ts, 100, /*mark=*/p == 9);
		if (i % 30 == 29)
			ctrl.UpdateLost(60, time);
	}
	EXPECT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage())
		<< "le détecteur par délai a pillé le compteur du détecteur par perte";
}



// Le seuil de 2,5 % doit se franchir dans les DEUX sens : un détecteur qui monte
// sans redescendre condamne la source à ramper jusqu'à la fin de l'appel.
// Les rapports tombent chaque seconde, comme le fait RTCP même sans perte.
TEST(RateControlLoss, LeSeuilDePerteEstFranchiDansLesDeuxSens)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	// 1 % de pertes : sous le seuil, rien à signaler.
	FeedLossPhase(ctrl, time, ts, /*seconds=*/8, /*lostPerReport=*/3);
	EXPECT_NE(RemoteRateControl::OverUsing, ctrl.GetUsage()) << "1 % de pertes pris pour une congestion";

	// 10 % : franchement au-dessus.
	FeedLossPhase(ctrl, time, ts, /*seconds=*/8, /*lostPerReport=*/30);
	EXPECT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage()) << "10 % de pertes non vues";

	// Retour à 1 % : le calme doit revenir.
	FeedLossPhase(ctrl, time, ts, /*seconds=*/8, /*lostPerReport=*/3);
	EXPECT_NE(RemoteRateControl::OverUsing, ctrl.GetUsage())
		<< "congestion maintenue alors que les pertes sont retombées à 1 %";
}

// Symétrique de LesCandidatsNeSurviventPasAuRetourAuCalme, pour l'autre chemin :
// trois rafales de pertes que dix secondes de trafic propre séparent ne sont pas
// une congestion. Une rafale seule ne franchit pas la garde des trois rapports ;
// leur cumul ne doit pas la franchir non plus.
TEST(RateControlLoss, UnRapportDePerteNeSurvitPasAuRetourAuCalme)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;
	FeedLossPhase(ctrl, time, ts, /*seconds=*/5, /*lostPerReport=*/0);

	for (int rafale = 0; rafale < 3; ++rafale)
	{
		FeedLossPhase(ctrl, time, ts, /*seconds=*/1, /*lostPerReport=*/60);
		FeedLossPhase(ctrl, time, ts, /*seconds=*/10, /*lostPerReport=*/0);
		EXPECT_NE(RemoteRateControl::OverUsing, ctrl.GetUsage())
			<< "congestion déclarée après " << (rafale + 1)
			<< " rafale(s) de pertes, séparées par 10 s de trafic propre";
	}
}

// LA regression de la seance du 2026-08-18 (patte cx-31) : un unique rapport de
// perte a t+27 s a fait basculer le chemin perte, UpdateLost n'a plus jamais ete
// appelee — une fois en 4,9 minutes — et l'hypothese est restee OverUsing a vie.
// L'estimateur, maintenu en Decrease, s'est effondre de 2216 kb/s au plancher de
// 16 kb/s, ou il est reste 201 s. Avant la separation des hypotheses, la sortie
// etait fortuite : le detecteur de delai reecrivait l'hypothese a chaque image.
// Contrat : une congestion que plus rien ne confirme EXPIRE. Le detecteur de
// delai ne la reecrit pas — il porte l'horloge qui la fait expirer.
TEST(RateControlLoss, UneCongestionQuePlusRienNeConfirmeExpire)
{
	RemoteRateControl ctrl;
	QWORD time = 100000, ts = 100000;

	// Assez de rapports pour franchir la garde des trois, puis plus AUCUN.
	FeedLossPhase(ctrl, time, ts, /*seconds=*/8, /*lostPerReport=*/60);
	ASSERT_EQ(RemoteRateControl::OverUsing, ctrl.GetUsage()) << "prérequis : congestion vue";

	// 10 s de trafic parfaitement sain, sans le moindre rapport de perte.
	for (int i = 0; i < 300; ++i)
	{
		time += kFrameMs;
		ts   += kFrameMs;
		for (int p = 0; p < 10; ++p)
			ctrl.Update(time, ts, 100, /*mark=*/p == 9);
	}

	EXPECT_NE(RemoteRateControl::OverUsing, ctrl.GetUsage())
		<< "hypothèse de perte gelée : sans nouveau rapport RTCP elle ne retombe jamais, "
		   "et l'estimation s'effondre au plancher";
}

// ---------------------------------------------------------------------------
// Suite RateControlBrake — lot A de docs/reference/kalman-vs-webrtc.md : ce qui
// separe encore notre AIMD de celui du temoin, cote reaction.
// ---------------------------------------------------------------------------

// E1. UpdateLost et UpdateRTT rendent un NIVEAU, pas un front, et sont appeles a
// chaque trou de numero de sequence — pendant une congestion, le trou est la
// regle. Sans frein, chacun relançait toute la machine AIMD : mesure du
// 2026-08-18, 102 reestimations en 5,1 s, une toutes les 12 ms, et l'estimation
// qui degringole de 664 a 233 kb/s pendant que l'entrant tient 720.
// Contrat : le temoin ne descend qu'au front, ou apres clamp(rtt, 10, 200) ms
// (aimd_rate_control.cc, TimeToReduceFurther).
//
// Ce test forme une PAIRE avec LeFreinLaissePasserLaReactionSuivante : meme
// preparation, meme rapport de perte, seul l'instant change. Sans son pendant, un
// vert ici pourrait tout aussi bien signifier que rien ne descend jamais.
//
// Le front, lui, ne descend pas : la premiere surutilisation passe en Hold
// (ecart E8 du rapport, mesure du 2026-08-18). Le test ne le suppose donc pas.
TEST(RateControlBrake, DesRapportsDePerteRapprochesNeDescendentQuUneFois)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	QWORD now = FeedRegular(estimator, ssrc, /*from=*/100000, /*durationMs=*/70000);
	ForceOveruseViaRtt(estimator, ssrc, now);
	const DWORD apresFront = estimator.GetEstimatedBitrate();
	ASSERT_GT(apresFront, 0u) << "prérequis : une estimation existe";

	// Dix rapports dans les 100 ms qui suivent, le flux continuant d'arriver : le
	// frein les écarte. Sans les paquets, le débit mesuré s'effondrerait et la
	// clause « chute de moitié » s'ouvrirait — à raison, mais ce n'est pas ce
	// qu'on teste, et en production les paquets ne s'arrêtent pas.
	int descentes = 0;
	DWORD precedente = apresFront;
	for (int i = 1; i <= 10; ++i)
	{
		estimator.Update(ssrc, now + i * 10, /*ts=*/now + i * 10, kFrameBytes, /*mark=*/true);
		estimator.UpdateLost(ssrc, 50, now + i * 10);
		const DWORD courante = estimator.GetEstimatedBitrate();
		if (courante < precedente) ++descentes;
		precedente = courante;
	}

	EXPECT_LE(descentes, 1)
		<< "dix rapports en 100 ms ont produit " << descentes
		<< " descentes, de " << apresFront << " à " << precedente << " b/s";
}

// Le frein ne doit pas rendre sourd : passé le délai d'une réaction, la descente
// suivante doit avoir lieu. Sinon on aurait échangé une sur-réaction contre une
// absence de réaction.
TEST(RateControlBrake, LeFreinLaissePasserLaReactionSuivante)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	QWORD now = FeedRegular(estimator, ssrc, /*from=*/100000, /*durationMs=*/70000);
	ForceOveruseViaRtt(estimator, ssrc, now);
	const DWORD apresFront = estimator.GetEstimatedBitrate();
	ASSERT_GT(apresFront, 0u) << "prérequis : le front a bien fait descendre";

	// 500 ms plus tard : bien au-delà de clamp(rtt, 10, 200).
	estimator.UpdateLost(ssrc, 50, now + 500);
	EXPECT_LT(estimator.GetEstimatedBitrate(), apresFront)
		<< "le frein bloque une descente séparée d'une demi-seconde";
}

// E4. Au-dela de 1,5 x l'entrant, l'ancien code posait current = currentBitRate :
// un gel dont on ne sortait plus, ni vers le haut ni vers le bas. Mesure du
// 2026-08-18 : estimation immobile a 3841 kb/s pendant 90 s pour 2465 kb/s
// recus. C'est ce gel qui rendait la re-montee inobservable en boucle fermee.
// Contrat du temoin : un plafond GLISSANT, que l'estimation suit.
TEST(RateControlBrake, LEstimationSuitLePlafondAuLieuDeGeler)
{
	RemoteRateEstimator estimator;
	const DWORD ssrc = 0x1234;

	// Une source genereuse, le temps que l'estimation monte et bute au plafond.
	QWORD now = FeedRegular(estimator, ssrc, /*from=*/100000, /*durationMs=*/90000);
	const DWORD hautPlafond = estimator.GetEstimatedBitrate();
	ASSERT_GT(hautPlafond, 0u) << "prérequis : une estimation existe";

	// La source tombe de moitié : le plafond descend avec elle, donc
	// l'estimation aussi. Sous l'ancien gel, elle ne bougeait plus du tout.
	for (QWORD end = now + 60000; now < end; now += kFrameMs * 2)
		estimator.Update(ssrc, now, /*ts=*/now, kFrameBytes, /*mark=*/true);

	EXPECT_LT(estimator.GetEstimatedBitrate(), hautPlafond)
		<< "estimation figée à " << hautPlafond
		<< " kb/s alors que la source a été divisée par deux";
}


// E2. Sous surutilisation continue, l'etat restait Decrease : aucune transition
// ne l'en sortait. Le retour au calme le faisait passer a Hold, et il fallait un
// SECOND tick Normal pour relancer la montee — sur un lien qui alterne, deux
// ticks Normal consecutifs n'arrivent jamais. Mesure du 2026-08-19, boucle
// ouverte : estimation figee 17 s a 871 kb/s pendant que 1500 a 1860 kb/s
// passaient reellement. Contrat du temoin (aimd_rate_control.cc:310-311,
// « Stay on hold until the pipes are cleared ») : la descente laisse l'etat en
// Hold, donc UN SEUL retour au calme suffit a relancer la montee.
TEST(RateControlBrake, UnSeulRetourAuCalmeRelanceLaMontee)
{
	RemoteRateEstimator estimator;
	BitrateCapture capture;
	estimator.AddListener(&capture);
	const DWORD ssrc = 0x1234;

	QWORD now = FeedRegular(estimator, ssrc, /*from=*/100000, /*durationMs=*/70000);
	ForceOveruseViaRtt(estimator, ssrc, now);
	const size_t debut = capture.targets.size();

	// L'hypothese RTT vit EpisodicTtlMs puis expire faute de confirmation : un
	// tick AIMD descend pendant qu'elle vit, les suivants voient le calme.
	FeedRegular(estimator, ssrc, now, RemoteRateControl::EpisodicTtlMs + 3000);

	// Le creux est la premiere consigne minimale publiee apres la
	// surutilisation. Le tick SUIVANT est le premier au calme : il doit deja
	// remonter. Sans la correction, la machine passe ce tick a sortir de
	// Decrease (Decrease -> Hold) et republie le creux a l'identique — c'est le
	// second tick Normal qui montait, et un lien qui alterne n'en offre jamais
	// deux de suite.
	ASSERT_GT(capture.targets.size(), debut) << "prérequis : des ticks ont eu lieu";
	auto premier = capture.targets.begin() + debut;
	auto creux = std::min_element(premier, capture.targets.end());
	ASSERT_LT(*creux, *std::max_element(premier, capture.targets.end()))
		<< "prérequis : la surutilisation a fait descendre";
	ASSERT_NE(creux + 1, capture.targets.end())
		<< "prérequis : au moins un tick apres le creux";
	EXPECT_GT(*(creux + 1), *creux)
		<< "consigne restée à " << *creux
		<< " b/s au tick suivant le creux : la montée attend un second tick";
}


} // namespace
