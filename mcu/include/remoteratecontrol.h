/* 
 * File:   remoteratecontrol.h
 * Author: Sergio
 *
 * Created on 26 de diciembre de 2012, 12:49
 */

#ifndef REMOTERATECONTROL_H
#define	REMOTERATECONTROL_H

#include "config.h"
#include "acumulator.h"
#include "eventstreaminghandler.h"
#include <deque>

class RemoteRateControl
{

public:
	enum BandwidthUsage
	{
		UnderUsing = 0,
		Normal = 1,
		OverUsing = 2
	};

	enum Region {
		MaxUnknown,
		AboveMax,
		NearMax,
		BelowMax
	};

	static const char * GetName(BandwidthUsage usage)
	{
		switch (usage)
		{
			case UnderUsing:
				return "UnderUsing";
			case Normal:
				return "Normal";
			case OverUsing:
				return "OverUsing";
		}
		return "Unknown";
	}
	
	static const char * GetName(Region region)
	{
		switch (region)
		{
			case MaxUnknown:
				return "MaxUnknown";
			case AboveMax:
				return "AboveMax";
			case NearMax:
				return "NearMax";
			case BelowMax:
				return "BelowMax";
		}
		return "Unknown";
	}
public:
	RemoteRateControl();
	void Update(QWORD time,QWORD ts,DWORD size, bool mark);
	bool UpdateRTT(DWORD rtt);
	//now : la MEME horloge (ms) que Update — plus de getTime() µs interne (§3.3)
	bool UpdateLost(DWORD num, QWORD now);
	void SetRateControlRegion(Region region);
	BandwidthUsage GetUsage()	{ return hypothesis; }
	double GetNoise()		{ return varNoise;   }
	//Observabilite pour les tests (lot 0 du chantier rate-control) :
	//l'invariant que l'amont verifie par RTC_DCHECK (overuse_estimator.cc:90-93).
	bool CovarianceIsPositiveSemiDefinite() const
	{
		return E[0][0]+E[1][1]>=0 && E[0][0]*E[1][1]-E[0][1]*E[1][0]>=0 && E[0][0]>=0;
	}
	void SetEventSource(EvenSource *eventSource) {	this->eventSource = eventSource; }

private:
	void UpdateKalman(int deltaTime, int deltaSize, double tsDelta);
private:
	EvenSource *eventSource;
	Acumulator bitrateCalc;
	Acumulator fpsCalc;
	Acumulator packetCalc;
	Acumulator lostCalc;
	DWORD rtt;
	DWORD absSendTimeCycles;
	QWORD prevTS;
	QWORD prevTime;
	DWORD prevSize;
	QWORD curTS;
	QWORD curTime;
	DWORD curSize;
	DWORD prevTarget;
	int64_t curDelta;
	//Horodatage media (ms) de la derniere image close : donne la periode
	//inter-images, l'exposant du facteur d'oubli du bruit (temoin :
	//overuse_estimator.cc:105-115, UpdateMinFramePeriod).
	QWORD lastFrameTS;
	std::deque<double> tsDeltaHist;
	double slope;
	double offset;
	double E[2][2];
	double processNoise[2];
	double avgNoise;
	double varNoise;
	double threshold;
	double prevOffset;
	BandwidthUsage hypothesis;
	//Un compteur PAR chemin de detection : le delai est juge a chaque image
	//(~30 Hz), les pertes a chaque rapport RTCP (~1 Hz). Partages, le premier
	//efface l'accumulation du second trente fois par seconde.
	int overUseCount;
	int lostOverCount;
};

#endif	/* REMOTERATECONTROL_H */

