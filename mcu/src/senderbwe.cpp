#include <algorithm>
#include <cmath>
#include "senderbwe.h"
#include "log.h"
#include "eventstreaminghandler.h"

//Constantes du temoin, fichier:valeur verifies sur ../webrtc e12c39e03c
static const double Beta                  = 0.85;	//aimd_rate_control.cc:35
static const QWORD  InitializationUs      = 5000000;	//aimd_rate_control.cc, kInitializationTime
static const QWORD  StreamTimeoutUs       = 2000000;	//delay_based_bwe.cc:42
static const QWORD  AckedInitialWindowMs  = 500;	//bitrate_estimator.cc:29
static const QWORD  AckedWindowMs         = 150;	//bitrate_estimator.cc:30
static const double AckedUncertaintyScale = 10.0;	//bitrate_estimator.cc:48
//Bornes du regime auto-limite (temoin alr_detector.cc) : on y entre en emettant
//moins de 65 % de la cible, on n'en sort qu'au-dela de 80 %.
static const double SelfLimitedEnter      = 0.65;	//alr_detector.cc
static const double SelfLimitedExit       = 0.80;	//alr_detector.cc
static const float  LowLossThreshold      = 0.02f;	//send_side_bandwidth_estimation.cc:50
static const float  HighLossThreshold     = 0.1f;	//send_side_bandwidth_estimation.cc:51
static const QWORD  BweIncreaseIntervalUs = 1000000;	//send_side_bandwidth_estimation.cc:39
static const QWORD  BweDecreaseIntervalUs = 300000;	//send_side_bandwidth_estimation.cc:40
static const QWORD  StartPhaseUs          = 2000000;	//send_side_bandwidth_estimation.cc:41
static const QWORD  LossReportTimeoutUs   = 6000000;	//1,2 x kMaxRtcpFeedbackInterval (5 s)

void SenderBWE::LinkCapacity::OnOveruse(double ackedKbps)
{
	const double alpha = 0.05;	//link_capacity_estimator.cc:39
	if (!hasEstimate)
		estimateKbps = ackedKbps;
	else
		estimateKbps = (1 - alpha) * estimateKbps + alpha * ackedKbps;
	const double norm = std::max(estimateKbps, 1.0);
	double error = estimateKbps - ackedKbps;
	deviationKbps = (1 - alpha) * deviationKbps + alpha * error * error / norm;
	deviationKbps = std::clamp(deviationKbps, 0.4, 2.5);
	hasEstimate = true;
}

double SenderBWE::LinkCapacity::UpperBoundKbps() const
{
	if (!hasEstimate)
		return 1e12;
	return estimateKbps + 3 * sqrt(deviationKbps * estimateKbps);
}

double SenderBWE::LinkCapacity::LowerBoundKbps() const
{
	if (!hasEstimate)
		return 0;
	return std::max(0.0, estimateKbps - 3 * sqrt(deviationKbps * estimateKbps));
}

SenderBWE::SenderBWE()
{
	state = Hold;
	delayCurrentBitrate = 0;
	delayInitialized = false;
	firstThroughputUs = 0;
	lastChangeUs = 0;
	linkCapacity.Reset();
	lastFeedbackUs = 0;

	ackedBitrate = -1;
	sentBitrate = -1;
	sentVar = 0;
	selfLimited = false;
	sentPrevMs = 0;
	hasSentPrev = false;
	sentSum = 0;
	sentWindowMs = 0;
	ackedVar = 50;
	ackedWindowMs = 0;
	ackedPrevMs = 0;
	hasAckedPrev = false;
	ackedSum = 0;

	lossBasedTarget = 0;
	lastFractionLost = 0;
	lastLossReportUs = 0;
	lastLossDecreaseUs = 0;
	hasDecreasedSinceLastLoss = false;
	firstReportUs = 0;

	lastTraceUs = 0;
	eventSource = NULL;

	rtt = 200;
	//Bornes du lot 1 (arbitrage A1) : en dessous de 16 kb/s mieux vaut geler
	minConfiguredBitrate = 16000;
	maxConfiguredBitrate = 30000000;
	atCeiling = false;
}

void SenderBWE::TraceEstimate(QWORD nowUs, bool changed)
{
	if (!changed && lastTraceUs && nowUs - lastTraceUs < 1000000)
		return;
	lastTraceUs = nowUs;
	Debug("BWE-TX: estimation stream=%s state=%s usage=%s target=%u delay=%u acked=%u lost=%u trend=%.3f threshold=%.1f sent=%u\n",
	      eventSource ? eventSource->GetName() : "",
	      GetStateName(), TrendlineDetector::GetName(detector.GetUsage()),
	      GetEstimatedBitrate() / 1000, delayCurrentBitrate / 1000, GetAckedBitrate() / 1000,
	      lastFractionLost, detector.GetTrend(), detector.GetThreshold(),
	      GetSentBitrate() / 1000);
}

void SenderBWE::SetMinMaxBitrate(DWORD min, DWORD max)
{
	if (min)
		minConfiguredBitrate = min;
	if (max)
		maxConfiguredBitrate = max;
}

void SenderBWE::SetStartBitrate(DWORD bitrate, QWORD nowUs)
{
	delayCurrentBitrate = bitrate;
	delayInitialized = true;
	lossBasedTarget = bitrate;
	lastChangeUs = nowUs;
	if (!firstReportUs)
		firstReportUs = nowUs;
}

void SenderBWE::UpdateRTT(DWORD rttMs)
{
	rtt = rttMs;
}

const char* SenderBWE::GetStateName() const
{
	switch (state)
	{
		case Hold:     return "Hold";
		case Increase: return "Increase";
		case Decrease: return "Decrease";
	}
	return "Unknown";
}

DWORD SenderBWE::GetEstimatedBitrate() const
{
	if (!lossBasedTarget)
		return 0;
	DWORD target = lossBasedTarget;
	//Le controleur de perte propose, le controleur de delai borne
	if (delayInitialized && delayCurrentBitrate && delayCurrentBitrate < target)
		target = delayCurrentBitrate;
	return std::clamp(target, minConfiguredBitrate, maxConfiguredBitrate);
}

//--- Debit acquitte -----------------------------------------------------------

void SenderBWE::UpdateSentBitrate(QWORD sentUs, DWORD bytes)
{
	QWORD nowMs = sentUs / 1000;
	QWORD window = sentBitrate < 0 ? AckedInitialWindowMs : AckedWindowMs;
	if (hasSentPrev && nowMs < sentPrevMs)
	{
		hasSentPrev = false;
		sentSum = 0;
		sentWindowMs = 0;
	}
	if (hasSentPrev)
	{
		sentWindowMs += nowMs - sentPrevMs;
		if (nowMs - sentPrevMs > window)
		{
			sentSum = 0;
			sentWindowMs %= window;
		}
	}
	sentPrevMs = nowMs;
	hasSentPrev = true;
	double sample = -1;
	if (sentWindowMs >= window)
	{
		sample = 8.0 * sentSum / (double)window;	//kb/s
		sentWindowMs -= window;
		sentSum = 0;
	}
	sentSum += bytes;
	if (sample < 0)
		return;
	if (sentBitrate < 0)
	{
		sentBitrate = sample;
		return;
	}
	FilterSample(sample, sentBitrate, sentVar);
}

bool SenderBWE::IsSelfLimited(DWORD currentBps)
{
	//Sans echantillon ni cible, le regime ne change pas : mieux vaut le
	//dernier connu qu'une bascule sur une absence de mesure.
	if (sentBitrate < 0 || !currentBps)
		return selfLimited;

	const double ratio = sentBitrate * 1000 / (double)currentBps;

	if (selfLimited)
	{
		if (ratio > SelfLimitedExit)
			selfLimited = false;
	}
	else if (ratio < SelfLimitedEnter)
	{
		selfLimited = true;
	}

	return selfLimited;
}

void SenderBWE::UpdateAckedBitrate(QWORD nowUs, DWORD bytes)
{
	QWORD nowMs = nowUs / 1000;
	QWORD window = ackedBitrate < 0 ? AckedInitialWindowMs : AckedWindowMs;
	if (hasAckedPrev && nowMs < ackedPrevMs)
	{
		hasAckedPrev = false;
		ackedSum = 0;
		ackedWindowMs = 0;
	}
	if (hasAckedPrev)
	{
		ackedWindowMs += nowMs - ackedPrevMs;
		if (nowMs - ackedPrevMs > window)
		{
			ackedSum = 0;
			ackedWindowMs %= window;
		}
	}
	ackedPrevMs = nowMs;
	hasAckedPrev = true;
	double sample = -1;
	if (ackedWindowMs >= window)
	{
		sample = 8.0 * ackedSum / (double)window;	//kb/s
		ackedWindowMs -= window;
		ackedSum = 0;
	}
	ackedSum += bytes;
	if (sample < 0)
		return;
	if (ackedBitrate < 0)
	{
		ackedBitrate = sample;
		return;
	}
	FilterSample(sample, ackedBitrate, ackedVar);
}

void SenderBWE::FilterSample(double sample, double& value, double& var)
{
	//L'incertitude croit avec l'ecart relatif (temoin bitrate_estimator.cc)
	double uncertainty = AckedUncertaintyScale * fabs(value - sample) / (value + sample);
	double sampleVar = uncertainty * uncertainty;
	double predVar = var + 5;
	value = (sampleVar * value + predVar * sample) / (sampleVar + predVar);
	var = sampleVar * predVar / (sampleVar + predVar);
}

//--- Controleur de delai ------------------------------------------------------

void SenderBWE::ChangeState(TrendlineDetector::BandwidthUsage usage, QWORD nowUs)
{
	switch (usage)
	{
		case TrendlineDetector::Normal:
			if (state == Hold)
			{
				lastChangeUs = nowUs;
				state = Increase;
			}
			break;
		case TrendlineDetector::OverUsing:
			if (state != Decrease)
				state = Decrease;
			break;
		case TrendlineDetector::UnderUsing:
			state = Hold;
			break;
	}
}

bool SenderBWE::TimeToReduceFurther(QWORD nowUs) const
{
	const QWORD interval = std::clamp((QWORD)rtt, (QWORD)10, (QWORD)200) * 1000;
	if (nowUs - lastChangeUs >= interval)
		return true;
	if (delayInitialized && ackedBitrate >= 0)
		return ackedBitrate * 1000 < 0.5 * delayCurrentBitrate;
	return false;
}

double SenderBWE::NearMaxIncreaseRateBpsPerSecond() const
{
	//Un paquet moyen par temps de reponse (temoin aimd_rate_control.cc:319-340)
	const double frameIntervalS = 1.0 / 30;
	double frameSizeBytes = delayCurrentBitrate * frameIntervalS / 8;
	double packetsPerFrame = ceil(frameSizeBytes / 1200.0);
	double avgPacketBytes = packetsPerFrame > 0 ? frameSizeBytes / packetsPerFrame : frameSizeBytes;
	double responseTimeS = 2 * (rtt + 100) / 1000.0;
	return std::max(4000.0, avgPacketBytes * 8 / responseTimeS);
}

DWORD SenderBWE::ClampBitrate(DWORD bitrate) const
{
	return std::clamp(bitrate, minConfiguredBitrate, maxConfiguredBitrate);
}

void SenderBWE::UpdateDelayEstimate(QWORD nowUs)
{
	//Initialisation : 5 s de debit acquitte avant la premiere estimation,
	//sauf surutilisation (temoin aimd_rate_control.cc, Update)
	TrendlineDetector::BandwidthUsage usage = detector.GetUsage();
	if (!delayInitialized)
	{
		if (ackedBitrate >= 0)
		{
			if (!firstThroughputUs)
				firstThroughputUs = nowUs;
			else if (nowUs - firstThroughputUs > InitializationUs)
			{
				delayCurrentBitrate = (DWORD)(ackedBitrate * 1000);
				delayInitialized = true;
			}
		}
		if (!delayInitialized && usage != TrendlineDetector::OverUsing)
			return;
		if (!delayInitialized)
		{
			//Surutilisation avant initialisation : on part du debit acquitte
			if (ackedBitrate < 0)
				return;
			delayCurrentBitrate = (DWORD)(ackedBitrate * 1000);
			delayInitialized = true;
		}
	}

	ChangeState(usage, nowUs);

	double ackedBps = ackedBitrate >= 0 ? ackedBitrate * 1000 : delayCurrentBitrate;
	DWORD current = delayCurrentBitrate;

	switch (state)
	{
		case Hold:
			break;
		case Increase:
		{
			if (ackedBps / 1000 > linkCapacity.UpperBoundKbps())
				linkCapacity.Reset();
			//Plafond glissant : pas plus de 1,5 x le debit acquitte + 10 kb/s.
			//SAUF en regime auto-limite (emis << cible) : l'acquitte ne peut
			//par construction pas depasser ce que nous emettons, et la cible
			//plafonne notre propre encodeur — applique la, le plafond se
			//refermait sur la cible elle-meme (seance du 2026-08-20, cible
			//gelee a 318 kb/s sur un lien revenu a 2000). Le temoin s'evade
			//par l'ALR et le sondage ; sans sondes, le debit emis tranche.
			//Le detecteur de delai garde la porte dans les deux regimes.
			bool selfLimited = IsSelfLimited(current);
			DWORD increaseLimit = (DWORD)(1.5 * ackedBps) + 10000;
			if (selfLimited || current < increaseLimit)
			{
				DWORD increased;
				//En regime auto-limite la montee est multiplicative meme si
				//une capacite est memorisee : cette memoire date du dernier
				//episode de congestion, et la cible ne s'applique a aucun
				//trafic reel — la prudence additive n'y protege rien et
				//coutait 2 minutes de re-montee (13 kb/s par seconde).
				if (linkCapacity.hasEstimate && !selfLimited)
				{
					//Capacite connue : montee ADDITIVE, un paquet par temps
					//de reponse
					double additive = NearMaxIncreaseRateBpsPerSecond()
						* std::min((nowUs - lastChangeUs) / 1e6, 1.0);
					increased = current + (DWORD)(additive + 0.5);
				} else {
					//Capacite inconnue : x1,08/s pour aller la trouver
					double alpha = pow(1.08, std::min((nowUs - lastChangeUs) / 1e6, 1.0));
					DWORD multiplicative = (DWORD)std::max(current * (alpha - 1.0), 1000.0);
					increased = current + multiplicative;
				}
				current = selfLimited ? increased : std::min(increased, increaseLimit);
			}
			Debug("BWE-TX: Increase rate to current = %u kbps\n", current / 1000);
			lastChangeUs = nowUs;
			break;
		}
		case Decrease:
		{
			//La descente porte sur le debit ACQUITTE, pas sur notre propre
			//estimation (docs/RATE-CONTROL.md)
			DWORD decreased = (DWORD)(Beta * ackedBps + 0.5);
			if (decreased > 5000)
				decreased -= 5000;
			if (decreased > delayCurrentBitrate && linkCapacity.hasEstimate)
				decreased = (DWORD)(Beta * linkCapacity.estimateKbps * 1000);
			if (decreased < delayCurrentBitrate)
				current = decreased;
			if (ackedBps / 1000 < linkCapacity.LowerBoundKbps())
				linkCapacity.Reset();
			linkCapacity.OnOveruse(ackedBps / 1000);
			Debug("BWE-TX: Decrease rate to current = %u kbps\n", current / 1000);
			//La descente laisse l'etat en Hold : un seul retour au calme
			//relance la montee (meme contrat que le lot 3bis cote reception)
			state = Hold;
			lastChangeUs = nowUs;
			break;
		}
	}
	delayCurrentBitrate = ClampBitrate(current);
}

//--- Etage de perte -----------------------------------------------------------

void SenderBWE::UpdateMinHistory(QWORD nowUs)
{
	//File des minima sur 1 s (temoin UpdateMinHistory)
	while (!minHistory.empty() && nowUs - minHistory.front().first > BweIncreaseIntervalUs)
		minHistory.pop_front();
	while (!minHistory.empty() && minHistory.back().second >= lossBasedTarget)
		minHistory.pop_back();
	minHistory.push_back({ nowUs, lossBasedTarget });
}

bool SenderBWE::UpdateTarget(DWORD bitrate, QWORD nowUs)
{
	//Le delai borne toujours (temoin ApplyTargetLimits/UpdateTargetBitrate)
	if (delayInitialized && delayCurrentBitrate && bitrate > delayCurrentBitrate)
		bitrate = delayCurrentBitrate;
	bitrate = ClampBitrate(bitrate);
	bool changed = bitrate != lossBasedTarget;
	lossBasedTarget = bitrate;

	//Sur le FRONT seulement : a ce plafond l'estimateur peut rester des minutes,
	//une trace par rapport serait du bruit. Sans transport-cc il y monte des
	//qu'il n'y a pas de perte : le voir ici dit que la borne codee en dur (cf.
	//RTPSession::VideoSenderEstimateMaxBps) est ce qui le tient, pas le lien.
	const bool ceiling = bitrate >= maxConfiguredBitrate;
	if (ceiling && !atCeiling)
		Log("-BWE-TX: limite codee en dur de %u kbit/s atteinte, l'estimateur d'emission ne montera pas plus haut [%s]\n",
		    maxConfiguredBitrate / 1000, eventSource ? eventSource->GetName() : "");
	atCeiling = ceiling;
	return changed;
}

bool SenderBWE::UpdateLossEstimate(QWORD nowUs)
{
	if (!lossBasedTarget)
		return false;
	//Phase de depart : 2 s de confiance au delai tant qu'aucune perte
	if (!lastFractionLost && firstReportUs && nowUs - firstReportUs < StartPhaseUs)
	{
		DWORD target = lossBasedTarget;
		if (delayInitialized && delayCurrentBitrate > target)
			target = delayCurrentBitrate;
		if (target != lossBasedTarget)
		{
			minHistory.clear();
			minHistory.push_back({ nowUs, lossBasedTarget });
			return UpdateTarget(target, nowUs);
		}
	}
	UpdateMinHistory(nowUs);
	if (!lastLossReportUs || nowUs - lastLossReportUs >= LossReportTimeoutUs)
		return UpdateTarget(lossBasedTarget, nowUs);

	float loss = lastFractionLost / 256.0f;
	if (loss <= LowLossThreshold)
	{
		//Perte < 2 % : +8 % du minimum sur la derniere seconde, +1 kb/s
		DWORD target = (DWORD)(minHistory.front().second * 1.08 + 0.5) + 1000;
		return UpdateTarget(target, nowUs);
	}
	if (loss <= HighLossThreshold)
		//2-10 % : ne rien faire
		return UpdateTarget(lossBasedTarget, nowUs);
	//> 10 % : descente x(512-perte)/512, au plus une fois par 300 ms + RTT
	if (!hasDecreasedSinceLastLoss
	    && nowUs - lastLossDecreaseUs >= BweDecreaseIntervalUs + (QWORD)rtt * 1000)
	{
		lastLossDecreaseUs = nowUs;
		hasDecreasedSinceLastLoss = true;
		DWORD target = (DWORD)((QWORD)lossBasedTarget * (512 - lastFractionLost) / 512);
		return UpdateTarget(target, nowUs);
	}
	return UpdateTarget(lossBasedTarget, nowUs);
}

//--- Entrees ------------------------------------------------------------------

bool SenderBWE::ProcessFeedback(const std::vector<SentPacketHistory::Result>& results, DWORD lost, QWORD nowUs)
{
	if (results.empty())
		return false;
	//Le rapport lui-meme est un rapport de perte
	DWORD covered = lost + results.size();
	lastFractionLost = (BYTE)std::min<DWORD>(255, covered ? 256 * lost / covered : 0);
	lastLossReportUs = nowUs;
	hasDecreasedSinceLastLoss = false;
	//Coupure de flux : 2 s sans rapport, le detecteur repart a neuf
	if (lastFeedbackUs && nowUs - lastFeedbackUs > StreamTimeoutUs)
		detector.Reset();
	lastFeedbackUs = nowUs;
	if (!firstReportUs)
		firstReportUs = nowUs;

	DWORD before = GetEstimatedBitrate();
	for (const SentPacketHistory::Result& r : results)
	{
		UpdateAckedBitrate(r.recvTimeUs, r.size);
		UpdateSentBitrate(r.sentTimeUs, r.size);
		detector.OnPacket(r.sentTimeUs, r.recvTimeUs, r.size);
	}
	//Les paquets perdus ont ete EMIS aussi : sans eux, un lien a fortes
	//pertes ressemblerait a un emetteur timide et leverait le plafond a tort.
	if (lost)
		UpdateSentBitrate(results.back().sentTimeUs,
				  lost * results.back().size);

	if (detector.GetUsage() == TrendlineDetector::OverUsing)
	{
		//Frein temporel : une descente par temps de reaction
		if (TimeToReduceFurther(nowUs))
			UpdateDelayEstimate(nowUs);
	} else {
		UpdateDelayEstimate(nowUs);
	}

	//Sans cible de perte encore : le delai initialise la cible
	if (!lossBasedTarget && delayInitialized)
		lossBasedTarget = delayCurrentBitrate;
	UpdateLossEstimate(nowUs);

	DWORD after = GetEstimatedBitrate();
	TraceEstimate(nowUs, after != before);
	return after != before;
}

bool SenderBWE::UpdateFractionLost(BYTE fractionLost, QWORD nowUs)
{
	lastFractionLost = fractionLost;
	lastLossReportUs = nowUs;
	hasDecreasedSinceLastLoss = false;
	if (!firstReportUs)
		firstReportUs = nowUs;
	DWORD before = GetEstimatedBitrate();
	if (!lossBasedTarget && delayInitialized)
		lossBasedTarget = delayCurrentBitrate;
	//Sans controleur de delai (pas de transport-cc), la cible s'amorce sur le
	//debit reellement emis. Mesure pas encore prete : elle reste a 0 et le
	//prochain RR reessaie.
	else if (!lossBasedTarget)
		lossBasedTarget = GetSentBitrate();
	UpdateLossEstimate(nowUs);
	DWORD after = GetEstimatedBitrate();
	TraceEstimate(nowUs, after != before);
	return after != before;
}
