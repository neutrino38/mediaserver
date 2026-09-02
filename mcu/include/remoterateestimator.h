/* 
 * File:   remoterateestimator.h
 * Author: Sergio
 *
 * Created on 8 de marzo de 2013, 10:43
 */

#ifndef REMOTERATEESTIMATOR_H
#define	REMOTERATEESTIMATOR_H

#include "use.h"
#include "remoteratecontrol.h"
#include "rtp.h"
#include <deque>
#include <set>

class RemoteRateEstimator
{
public:
	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		virtual void onTargetBitrateRequested(DWORD bitrate) = 0;
	};
public:
	enum State {
		Hold,
		Increase,
		Decrease
	};

	const char * GetName(State state)
	{
		switch (state)
		{
			case Hold:
				return "Hold";
			case Increase:
				return "Increase";
			case Decrease:
				return "Decrease";
		}
		return "Unknown";
	}
public:
	RemoteRateEstimator();
	~RemoteRateEstimator();
	//Plusieurs sessions partagent un estimateur (RTPParticipant) : chacune
	//s'inscrit/se desinscrit, plus de "dernier SetListener gagne".
	void AddListener(Listener *listener);
	void RemoveListener(Listener *listener);
	void AddStream(DWORD ssrc);
	void RemoveStream(DWORD ssrc);
	void UpdateRTT(DWORD ssrc,DWORD rtt, QWORD now);
	void UpdateLost(DWORD ssrc,DWORD lost, QWORD now);
	//La taille vient du paquet lui-meme (GetSize()) : le 3e parametre qui
	//recevait un horodatage a disparu.
	void Update(DWORD ssrc, RTPTimedPacket* packet);
	void Update(DWORD ssrc,QWORD now,QWORD ts,DWORD size, bool mark);
	DWORD GetEstimatedBitrate();
	//Debit REELLEMENT recu du pair, en bps ; 0 si la fenetre de mesure n'est pas
	//pleine. Il dit si la limite qu'on a annoncee borne encore le pair, ou s'il
	//est deja tenu par sa propre negociation (cf. RembThrottler).
	DWORD GetIncomingBitrate();
	void GetSSRCs(std::list<DWORD> &ssrcs);
	void SetTemporalMaxLimit(DWORD limit);
	void SetTemporalMinLimit(DWORD limit);
	void SetEventSource(EvenSource *eventSource) {	this->eventSource = eventSource; }
	EvenSource* GetEventSource() {	return eventSource; }
private:
	DWORD GetEstimatedBitrateUnlocked() const;
	double RateIncreaseFactor(QWORD now, QWORD last, DWORD reactionTime) const;
	void Update(RemoteRateControl::BandwidthUsage usage,bool reactNow,QWORD now);
	bool TimeToReduceFurther(QWORD now) const;
	void UpdateChangePeriod(QWORD now);
	void UpdateMaxBitRateEstimate(float incomingBitRateKbps);
	void ChangeState(State newState);
	void ChangeRegion(RemoteRateControl::Region newRegion);
private:
	typedef std::map<DWORD,RemoteRateControl*> Streams;
private:
	std::set<Listener*> listeners;
	EvenSource*	eventSource;
	Acumulator	bitrateAcu;
	Streams		streams;
	Use		lock;
	DWORD minConfiguredBitRate;
	DWORD maxConfiguredBitRate;
	DWORD currentBitRate;
	DWORD maxHoldRate;
	float avgMaxBitRate;
	float varMaxBitRate;
	State state;
	State cameFromState;
	RemoteRateControl::Region region;
	QWORD lastBitRateChange;
	DWORD noiseVar;
	QWORD curTS;
	DWORD absSendTimeCycles;
	float avgChangePeriod;
	QWORD lastChange;
	float beta;
	DWORD rtt;
	//Fenetre du plafond glissant : le debit entrant MAXIMAL des dernieres
	//IncreaseLimitWindowMs, tenu en deque monotone decroissante (le front est
	//le max). Un trou d'emission de la source ne doit pas faire chuter
	//l'annonce sans signal de congestion — mesure du 2026-08-22 (alice_bob_1) :
	//lien sain a 87 Mb/s, zero OverUsing, et l'annonce qui suivait chaque
	//reouverture d'encodeur du pair vers le bas, oscillation entretenue de 20 s.
	static constexpr QWORD IncreaseLimitWindowMs = 5000;
	std::deque<std::pair<QWORD,float>> incomingMaxWindow;
};

#endif	/* REMOTERATEESTIMATOR_H */

