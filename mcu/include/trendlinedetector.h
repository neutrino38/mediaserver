#ifndef TRENDLINEDETECTOR_H
#define TRENDLINEDETECTOR_H

#include <deque>
#include "config.h"

//Detecteur de surutilisation cote emetteur (sender_bwe_plan.md, 6.2) :
//groupes d'envoi puis regression lineaire sur le delai lisse, seuil adaptatif.
//Reecriture maison de inter_arrival_delta.cc + trendline_estimator.cc du
//temoin ../webrtc (e12c39e03c), sans tri ni plafond de pente (options
//desactivees par defaut amont) ni detecteur audio separe.
class TrendlineDetector
{
public:
	enum BandwidthUsage
	{
		UnderUsing = 0,
		Normal = 1,
		OverUsing = 2
	};

	static const char* GetName(BandwidthUsage usage)
	{
		switch (usage)
		{
			case UnderUsing: return "UnderUsing";
			case Normal:     return "Normal";
			case OverUsing:  return "OverUsing";
		}
		return "Unknown";
	}

	TrendlineDetector();

	//Un paquet acquitte : instants d'envoi (horloge locale) et d'arrivee
	//(horloge du pair, deroulee par SentPacketHistory), taille transport.
	void OnPacket(QWORD sendTimeUs, QWORD recvTimeUs, DWORD size);
	void Reset();

	BandwidthUsage GetUsage() const	{ return hypothesis; }
	double GetTrend() const		{ return prevTrend; }
	double GetThreshold() const	{ return threshold; }

private:
	void UpdateTrendline(double recvDeltaMs, double sendDeltaMs, QWORD arrivalTimeMs);
	void Detect(double trend, double tsDeltaMs, QWORD nowMs);
	void UpdateThreshold(double modifiedTrend, QWORD nowMs);

	//Groupes d'envoi (temoin inter_arrival_delta.cc) : un groupe = 5 ms
	//d'envoi, une rafale (delai de propagation decroissant) reste soudee.
	struct SendTimeGroup
	{
		bool  started;
		QWORD firstSendUs;
		QWORD sendUs;
		QWORD firstArrivalUs;
		QWORD completeUs;
		DWORD size;
	};

	bool ComputeDeltas(QWORD sendTimeUs, QWORD recvTimeUs, DWORD size,
	                   double& sendDeltaMs, double& recvDeltaMs);
	bool NewTimestampGroup(QWORD recvTimeUs, QWORD sendTimeUs) const;
	bool BelongsToBurst(QWORD recvTimeUs, QWORD sendTimeUs) const;

	SendTimeGroup current;
	SendTimeGroup prev;
	int reorderedGroups;

	//Regression (temoin trendline_estimator.cc)
	struct Timing
	{
		double arrivalMs;
		double smoothedDelayMs;
	};
	std::deque<Timing> history;
	int    numDeltas;
	double firstArrivalMs;
	double accumulatedDelay;
	double smoothedDelay;

	//Detection et seuil adaptatif
	double threshold;
	double prevTrend;
	double timeOverUsing;
	int    overuseCounter;
	QWORD  lastThresholdUpdateMs;
	bool   hasThresholdUpdate;
	BandwidthUsage hypothesis;
};

#endif
