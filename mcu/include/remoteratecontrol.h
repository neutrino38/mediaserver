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
	//Duree de validite d'une conclusion episodique (perte, RTT) sans nouvelle
	//confirmation : deux periodes de rapport RTCP.
	static const QWORD EpisodicTtlMs = 2000;

	RemoteRateControl();
	void Update(QWORD time,QWORD ts,DWORD size, bool mark);
	bool UpdateRTT(DWORD rtt);
	//now : la MEME horloge (ms) que Update — plus de getTime() µs interne (§3.3)
	bool UpdateLost(DWORD num, QWORD now);
	void SetRateControlRegion(Region region);
	//Une hypothese par chemin de detection, composee ici : chacun repond a un
	//signal independant, a sa propre cadence, et aucun n'a autorite pour effacer
	//la conclusion d'un autre. Une seule surutilisation suffit a contraindre ;
	//sinon c'est le detecteur de delai qui parle, seul a distinguer Normal
	//d'UnderUsing.
	BandwidthUsage GetUsage()
	{
		if (hypothesis==OverUsing || lostHypothesis==OverUsing || rttHypothesis==OverUsing)
			return OverUsing;
		return hypothesis;
	}
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
	//Hypothese du detecteur par delai. Celles des deux autres chemins vivent a
	//part : partagee, elle etait reecrite a chaque image (~30 Hz) et effacait la
	//congestion vue par les pertes des la premiere image au delai sain.
	BandwidthUsage hypothesis;
	BandwidthUsage lostHypothesis;
	BandwidthUsage rttHypothesis;
	//Instant (ms) de la derniere confirmation de chacun des deux chemins
	//episodiques. Leur conclusion EXPIRE sans confirmation : un rapport RTCP peut
	//ne jamais revenir — mesure du 2026-08-18, un seul rapport de perte en
	//4,9 minutes — et une hypothese qui ne retombe pas cloue l'estimation au
	//plancher. Avant que chaque chemin ne porte la sienne, la sortie etait
	//fortuite : le detecteur de delai les reecrivait a chaque image.
	QWORD lostOverAt;
	QWORD rttOverAt;
	//Un compteur PAR chemin de detection : le delai est juge a chaque image
	//(~30 Hz), les pertes a chaque rapport RTCP (~1 Hz). Partages, le premier
	//efface l'accumulation du second trente fois par seconde.
	int overUseCount;
	int lostOverCount;
};

#endif	/* REMOTERATECONTROL_H */

