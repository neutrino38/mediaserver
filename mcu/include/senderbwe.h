#ifndef SENDERBWE_H
#define SENDERBWE_H

#include <deque>
#include <vector>
#include "config.h"
#include "sentpackethistory.h"
#include "trendlinedetector.h"

class EvenSource;

//Estimateur de bande passante cote emetteur, v1 (sender_bwe_plan.md, 6.2).
//Entree : les paquets acquittes d'un rapport transport-cc (apparies par
//SentPacketHistory), la fraction perdue et le RTT des RR. Sortie : un debit
//cible = min(etage de perte, plafond du controleur de delai), borne.
//Reecriture maison des modules temoin ../webrtc e12c39e03c : delay_based_bwe,
//aimd_rate_control + link_capacity_estimator, bitrate_estimator, et l'etage
//de perte historique 2 %/10 % de send_side_bandwidth_estimation. Hors v1 :
//sondage, ALR, LossBasedBweV2, fenetre de congestion.
class SenderBWE
{
public:
	SenderBWE();

	//Paquets acquittes d'un rapport, ordonnes par arrivee, plus le compte de
	//paquets declares perdus par ce rapport (le feedback transport-cc PORTE
	//les pertes : c'est lui qui fait tiquer l'etage de perte, comme chez le
	//temoin — les RR ne sont qu'un second canal). Rend true si la cible a change.
	bool ProcessFeedback(const std::vector<SentPacketHistory::Result>& results, DWORD lost, QWORD nowUs);
	//Fraction perdue d'un RR sur notre flux sortant (unites de 1/256)
	bool UpdateFractionLost(BYTE fractionLost, QWORD nowUs);
	void UpdateRTT(DWORD rttMs);

	void SetStartBitrate(DWORD bitrate, QWORD nowUs);
	void SetMinMaxBitrate(DWORD min, DWORD max);
	//Nomme la patte dans les traces BWE-TX (meme source que l'estimateur RX)
	void SetEventSource(EvenSource* eventSource)	{ this->eventSource = eventSource; }

	bool  HasEstimate() const	{ return lossBasedTarget > 0; }
	DWORD GetEstimatedBitrate() const;
	DWORD GetDelayBasedLimit() const	{ return delayCurrentBitrate; }
	DWORD GetAckedBitrate() const	{ return ackedBitrate > 0 ? (DWORD)(ackedBitrate * 1000) : 0; }
	DWORD GetSentBitrate() const	{ return sentBitrate > 0 ? (DWORD)(sentBitrate * 1000) : 0; }
	TrendlineDetector::BandwidthUsage GetUsage() const	{ return detector.GetUsage(); }
	const char* GetStateName() const;

private:
	//--- Controleur de delai : detecteur + AIMD + memoire de capacite ---
	enum State { Hold, Increase, Decrease };

	void  UpdateDelayEstimate(QWORD nowUs);
	void  ChangeState(TrendlineDetector::BandwidthUsage usage, QWORD nowUs);
	bool  TimeToReduceFurther(QWORD nowUs) const;
	DWORD ClampBitrate(DWORD bitrate) const;
	double NearMaxIncreaseRateBpsPerSecond() const;

	//Memoire de capacite (temoin link_capacity_estimator.cc) : moyenne
	//glissante alimentee a chaque surutilisation, bornes a +/-3 sigma
	struct LinkCapacity
	{
		bool   hasEstimate;
		double estimateKbps;
		double deviationKbps;
		void   Reset()			{ hasEstimate = false; deviationKbps = 0.4; }
		void   OnOveruse(double ackedKbps);
		double UpperBoundKbps() const;
		double LowerBoundKbps() const;
	};

	TrendlineDetector detector;
	State  state;
	DWORD  delayCurrentBitrate;	//b/s ; 0 = pas encore initialise
	bool   delayInitialized;
	QWORD  firstThroughputUs;
	QWORD  lastChangeUs;
	LinkCapacity linkCapacity;
	QWORD  lastFeedbackUs;		//coupure de flux : 2 s sans rapport = reset

	//--- Debit acquitte (temoin bitrate_estimator.cc) ---
	void UpdateAckedBitrate(QWORD nowUs, DWORD bytes);
	double ackedBitrate;		//kb/s ; <0 = pas encore d'echantillon
	double ackedVar;
	QWORD  ackedWindowMs;
	QWORD  ackedPrevMs;
	bool   hasAckedPrev;
	DWORD  ackedSum;

	//--- Etage de perte (temoin send_side_bandwidth_estimation.cc) ---
	//La cible suit <2 % -> +8 % du minimum sur 1 s ; 2-10 % -> rien ;
	//>10 % -> x(512-perte)/512, au plus une fois par 300 ms + RTT.
	bool UpdateLossEstimate(QWORD nowUs);
	void UpdateMinHistory(QWORD nowUs);
	bool UpdateTarget(DWORD bitrate, QWORD nowUs);

	DWORD lossBasedTarget;		//b/s ; 0 = pas de cible encore
	BYTE  lastFractionLost;
	QWORD lastLossReportUs;
	QWORD lastLossDecreaseUs;
	bool  hasDecreasedSinceLastLoss;
	QWORD firstReportUs;		//phase de depart : 2 s de confiance au delai
	std::deque<std::pair<QWORD, DWORD>> minHistory;

	//Trace : au changement de cible, et au moins une fois par seconde —
	//une serie echantillonnee aux seuls changements biaise le depouillement
	//(dispersion, oscillation) du lot 3 etendu aux traces BWE-TX.
	//Debit reellement EMIS, fenetre glissante sur les instants d'emission des
	//paquets rapportes (pertes comprises, a la taille moyenne du rapport).
	//C'est lui qui separe le regime auto-limite (emis << cible : notre
	//encodeur borne, le plafond 1,5 x l'acquitte ne prouve rien) du regime
	//limite par le reseau (emis ~= cible : le plafond est legitime).
	void UpdateSentBitrate(QWORD sentUs, DWORD bytes);
	bool IsSelfLimited(DWORD currentBps) const;
	double sentBitrate;		//kb/s, -1 tant que la fenetre n'est pas pleine
	QWORD sentPrevMs;
	bool  hasSentPrev;
	QWORD sentSum;
	QWORD sentWindowMs;

	void TraceEstimate(QWORD nowUs, bool changed);
	QWORD lastTraceUs;
	EvenSource* eventSource;

	DWORD rtt;			//ms
	DWORD minConfiguredBitrate;
	DWORD maxConfiguredBitrate;
};

#endif
