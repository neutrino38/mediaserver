#include <algorithm>
#include <cmath>
#include "trendlinedetector.h"

//Constantes du temoin, fichier:valeur verifies sur ../webrtc e12c39e03c
static const QWORD  SendTimeGroupLengthUs   = 5000;	//delay_based_bwe.cc:43
static const QWORD  BurstDeltaThresholdUs   = 5000;	//inter_arrival_delta.cc:23
static const QWORD  MaxBurstDurationUs      = 100000;	//inter_arrival_delta.cc:24
static const int    ReorderedResetThreshold = 3;	//inter_arrival_delta.h:27
static const double SmoothingCoef           = 0.9;	//trendline_estimator.cc:36
static const double ThresholdGain           = 4.0;	//trendline_estimator.cc:37
static const size_t WindowSize              = 20;	//trendline_estimator.h:29
static const double KUp                     = 0.0087;	//trendline_estimator.cc:173
static const double KDown                   = 0.039;	//trendline_estimator.cc:174
static const double OverusingTimeThresholdMs = 10;	//trendline_estimator.cc:98
static const int    MinNumDeltas            = 60;	//trendline_estimator.cc:99
static const int    DeltaCounterMax         = 1000;	//trendline_estimator.cc:100
static const double MaxAdaptOffsetMs        = 15.0;	//trendline_estimator.cc:97
static const double InitialThresholdMs      = 12.5;	//trendline_estimator.cc:176

TrendlineDetector::TrendlineDetector()
{
	Reset();
}

void TrendlineDetector::Reset()
{
	current = SendTimeGroup();
	prev = SendTimeGroup();
	reorderedGroups = 0;
	history.clear();
	numDeltas = 0;
	firstArrivalMs = -1;
	accumulatedDelay = 0;
	smoothedDelay = 0;
	threshold = InitialThresholdMs;
	prevTrend = 0;
	timeOverUsing = -1;
	overuseCounter = 0;
	lastThresholdUpdateMs = 0;
	hasThresholdUpdate = false;
	hypothesis = Normal;
}

bool TrendlineDetector::NewTimestampGroup(QWORD recvTimeUs, QWORD sendTimeUs) const
{
	if (!current.started)
		return false;
	if (BelongsToBurst(recvTimeUs, sendTimeUs))
		return false;
	return sendTimeUs - current.firstSendUs > SendTimeGroupLengthUs;
}

bool TrendlineDetector::BelongsToBurst(QWORD recvTimeUs, QWORD sendTimeUs) const
{
	long long arrivalDelta = (long long)recvTimeUs - (long long)current.completeUs;
	long long sendDelta = (long long)sendTimeUs - (long long)current.sendUs;
	if (!sendDelta)
		return true;
	long long propagationDelta = arrivalDelta - sendDelta;
	if (propagationDelta < 0 && arrivalDelta <= (long long)BurstDeltaThresholdUs
	    && recvTimeUs - current.firstArrivalUs < MaxBurstDurationUs)
		return true;
	return false;
}

bool TrendlineDetector::ComputeDeltas(QWORD sendTimeUs, QWORD recvTimeUs, DWORD size,
                                      double& sendDeltaMs, double& recvDeltaMs)
{
	bool calculated = false;
	if (!current.started)
	{
		current.started = true;
		current.sendUs = sendTimeUs;
		current.firstSendUs = sendTimeUs;
		current.firstArrivalUs = recvTimeUs;
		current.size = 0;
	} else if (current.firstSendUs > sendTimeUs) {
		//Reordonne : ignore
		return false;
	} else if (NewTimestampGroup(recvTimeUs, sendTimeUs)) {
		//Premier paquet d'un groupe suivant : l'echantillon precedent est pret
		if (prev.started)
		{
			sendDeltaMs = ((long long)current.sendUs - (long long)prev.sendUs) / 1000.0;
			recvDeltaMs = ((long long)current.completeUs - (long long)prev.completeUs) / 1000.0;
			if (recvDeltaMs < 0)
			{
				//Groupes arrives dans le desordre
				if (++reorderedGroups >= ReorderedResetThreshold)
					Reset();
				return false;
			}
			reorderedGroups = 0;
			calculated = true;
		}
		prev = current;
		current.firstSendUs = sendTimeUs;
		current.sendUs = sendTimeUs;
		current.firstArrivalUs = recvTimeUs;
		current.size = 0;
	} else {
		current.sendUs = std::max(current.sendUs, sendTimeUs);
	}
	current.size += size;
	current.completeUs = recvTimeUs;
	return calculated;
}

void TrendlineDetector::OnPacket(QWORD sendTimeUs, QWORD recvTimeUs, DWORD size)
{
	double sendDeltaMs = 0, recvDeltaMs = 0;
	if (ComputeDeltas(sendTimeUs, recvTimeUs, size, sendDeltaMs, recvDeltaMs))
		UpdateTrendline(recvDeltaMs, sendDeltaMs, recvTimeUs / 1000);
}

void TrendlineDetector::UpdateTrendline(double recvDeltaMs, double sendDeltaMs, QWORD arrivalTimeMs)
{
	const double deltaMs = recvDeltaMs - sendDeltaMs;
	numDeltas = std::min(numDeltas + 1, DeltaCounterMax);
	if (firstArrivalMs < 0)
		firstArrivalMs = (double)arrivalTimeMs;

	accumulatedDelay += deltaMs;
	smoothedDelay = SmoothingCoef * smoothedDelay + (1 - SmoothingCoef) * accumulatedDelay;

	history.push_back({ (double)arrivalTimeMs - firstArrivalMs, smoothedDelay });
	if (history.size() > WindowSize)
		history.pop_front();

	//Regression lineaire simple : la pente s'interprete comme
	//(debit emis - capacite) / capacite (temoin trendline_estimator.cc:229)
	double trend = prevTrend;
	if (history.size() == WindowSize)
	{
		double sumX = 0, sumY = 0;
		for (const Timing& t : history)
		{
			sumX += t.arrivalMs;
			sumY += t.smoothedDelayMs;
		}
		double avgX = sumX / history.size();
		double avgY = sumY / history.size();
		double numerator = 0, denominator = 0;
		for (const Timing& t : history)
		{
			numerator += (t.arrivalMs - avgX) * (t.smoothedDelayMs - avgY);
			denominator += (t.arrivalMs - avgX) * (t.arrivalMs - avgX);
		}
		if (denominator != 0)
			trend = numerator / denominator;
	}

	Detect(trend, sendDeltaMs, arrivalTimeMs);
}

void TrendlineDetector::Detect(double trend, double tsDeltaMs, QWORD nowMs)
{
	if (numDeltas < 2)
	{
		hypothesis = Normal;
		return;
	}
	const double modifiedTrend = std::min(numDeltas, MinNumDeltas) * trend * ThresholdGain;
	if (modifiedTrend > threshold)
	{
		if (timeOverUsing < 0)
			//Demarrage : on suppose une surutilisation depuis la moitie de
			//l'intervalle ecoule
			timeOverUsing = tsDeltaMs / 2;
		else
			timeOverUsing += tsDeltaMs;
		overuseCounter++;
		if (timeOverUsing > OverusingTimeThresholdMs && overuseCounter > 1)
		{
			if (trend >= prevTrend)
			{
				timeOverUsing = 0;
				overuseCounter = 0;
				hypothesis = OverUsing;
			}
		}
	} else if (modifiedTrend < -threshold) {
		timeOverUsing = -1;
		overuseCounter = 0;
		hypothesis = UnderUsing;
	} else {
		timeOverUsing = -1;
		overuseCounter = 0;
		hypothesis = Normal;
	}
	prevTrend = trend;
	UpdateThreshold(modifiedTrend, nowMs);
}

void TrendlineDetector::UpdateThreshold(double modifiedTrend, QWORD nowMs)
{
	if (!hasThresholdUpdate)
	{
		lastThresholdUpdateMs = nowMs;
		hasThresholdUpdate = true;
	}
	//Une excursion au-dela de seuil+15 ms n'adapte pas : ce serait regler le
	//seuil sur une chute de capacite (temoin trendline_estimator.cc:311-316)
	if (fabs(modifiedTrend) > threshold + MaxAdaptOffsetMs)
	{
		lastThresholdUpdateMs = nowMs;
		return;
	}
	const double k = fabs(modifiedTrend) < threshold ? KDown : KUp;
	const QWORD maxTimeDeltaMs = 100;
	QWORD timeDeltaMs = std::min(nowMs - lastThresholdUpdateMs, maxTimeDeltaMs);
	threshold += k * (fabs(modifiedTrend) - threshold) * timeDeltaMs;
	threshold = std::clamp(threshold, 6.0, 600.0);
	lastThresholdUpdateMs = nowMs;
}
