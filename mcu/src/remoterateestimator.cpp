/* 
 * File:   remoterateestimator.cpp
 * Author: Sergio
 * 
 * Created on 8 de marzo de 2013, 10:43
 */

#include <map>
#include <cstdlib>
#include <cmath>
#include "remoterateestimator.h"

//Duree d'initialisation avant la premiere estimation (cf. Update, plus bas).
static const QWORD kInitializationMs = 5000;

//Fenetre de mesure du debit entrant : 1 s, celle du temoin (kBitrateWindow,
//bwe_defines.h ; remote_bitrate_estimator_single_stream.cc). A 200 ms, la
//descente visant beta x debit retenait le PIRE echantillon de 200 ms de
//l'episode au lieu de sa moyenne — un quantile bas, pas une mesure.
RemoteRateEstimator::RemoteRateEstimator() : bitrateAcu(1000)
{
	//Not last estimate
	//Bornes realignees (docs/RATE-CONTROL.md) : l'ancien plancher 128000
	//interdisait d'annoncer un reseau lent, l'ancien plafond 1280000000
	//(coquille probable) n'en etait pas un — temoin : 30 Mb/s, plancher 16 kb/s
	//(arbitrage A1 : en-dessous, mieux vaut geler l'image).
	minConfiguredBitRate	= 16000;
	maxConfiguredBitRate	= 30000000;
	currentBitRate		= 0;
	maxHoldRate		= 0;
	avgMaxBitRate		= -1.0f;
	varMaxBitRate		= 0.4f;
	lastBitRateChange	= 0;
	avgChangePeriod		= 1000.0f;
	lastChange		= 0;
	beta			= 0.9f;
	noiseVar 		= 0;
	rtt			= 200;
	absSendTimeCycles	= 0;
	curTS			= 0;
	//Set initial state and region
	cameFromState		= Decrease;
	state			= Hold;
	region			= RemoteRateControl::MaxUnknown;
	eventSource		= NULL;
}

RemoteRateEstimator::~RemoteRateEstimator()
{
	//Clean all streasm
	for (Streams::iterator it = streams.begin(); it!=streams.end(); ++it)
		//Delete
		delete(it->second);
}
void RemoteRateEstimator::AddStream(DWORD ssrc)
{
	Log("-RemoteRateEstimator adding stream [ssrc:%u]\n",ssrc);

	//Lock
	lock.WaitUnusedAndLock();
	//Create new control
	RemoteRateControl* ctrl = new RemoteRateControl();
	//Set tracer
	ctrl->SetEventSource(eventSource);
	//Add it
	streams[ssrc] = ctrl;
	//Unlock
	lock.Unlock();
}

void RemoteRateEstimator::RemoveStream(DWORD ssrc)
{
	Log("-RemoteRateEstimator removing stream [ssrc:%u]\n",ssrc);
	
	//Lock
	lock.WaitUnusedAndLock();
	Streams::iterator it = streams.find(ssrc);

	//If found
	if (it!=streams.end())
	{
		//Delete
		delete(it->second);
		//REmove
		streams.erase(it);
	}
	
	//Unlock
	lock.Unlock();
}

void RemoteRateEstimator::Update(DWORD ssrc, RTPTimedPacket * packet)
{
	//§3.1 : la taille sort du paquet (vue transport, en-tete compris, comme le
	//temoin) — l'ancien 3e parametre recevait getTimeMS() en production.
	DWORD size = packet->GetSize();
	//Get rtp timestamp in ms
	QWORD ts = packet->GetClockTimestamp();

	if (packet->HasAbsSentTime())
	{
		//Use absolote time instead of rtp time for knowing timestamp at origin
		ts = packet->GetAbsSendTime()+64000*absSendTimeCycles;
		//Check if it has been a wrapp arround, absSendTime has 64s wraps
		if (ts+32000<curTS)
		{
			//Increase cycles
			absSendTimeCycles++;
			//Fix wrap for this one
			ts += 64000;
		}
	}
	
	
	//Update
	Update(packet->GetSSRC(),packet->GetTime(),ts,size, packet->GetMark());
	
	//Store current ts
	curTS = ts;
}

void RemoteRateEstimator::Update(DWORD ssrc,QWORD now,QWORD ts,DWORD size, bool mark)
{
	//Log("-Update [ssrc:%u,now:%lu,last:%u,ts:%lu,size:%u,inwindow:%d\n",ssrc,now,lastChange,ts,size,bitrateAcu.IsInWindow());
	//Lock
	lock.WaitUnusedAndLock();

	//Acumulate bitrate
	bitrateAcu.Update(now,size*8);

	//Get global usage for all streams
	RemoteRateControl::BandwidthUsage usage = RemoteRateControl::UnderUsing;

	//Check if we need to target
	bool streamOverusing = false;

	//Reset noise
	noiseVar = 0;
	
	//Check if it was an unknown stream
	if (streams.find(ssrc)==streams.end())
	{
		//Create new control
		RemoteRateControl* ctrl = new RemoteRateControl();
		//Set tracer
		ctrl->SetEventSource(eventSource);
		//Add it
		streams[ssrc] = ctrl;
	}

	//For each one
	for (Streams::iterator it = streams.begin(); it!=streams.end(); ++it)
	{
		//Get control
		RemoteRateControl *ctrl = it->second;
		//Get stream usage
		RemoteRateControl::BandwidthUsage streamUsage = ctrl->GetUsage();
		//Check if it is the new one
		if (it->first == ssrc)
		{
			//Update it
			ctrl->Update(now,ts,size, mark);
			//Check if pacekt triggered overuse
			if (streamUsage!=RemoteRateControl::OverUsing && ctrl->GetUsage()==RemoteRateControl::OverUsing)
			{
				//We are overusing now
				streamOverusing = true;
				//Set usage
				usage = RemoteRateControl::OverUsing;
			}
		} 
		//Get worst
		if (usage<streamUsage)
			//Set it
			usage = streamUsage;
		//Get noise var and sum up
		noiseVar += ctrl->GetNoise();
	}
	
	//Normalize
	noiseVar = streams.size() ? noiseVar/streams.size() : 0;

	//Initialisation : le temps de remplir les accumulateurs avant de prononcer
	//une estimation, aligne sur le temoin (kInitializationTime, 5 s,
	//aimd_rate_control.cc). Les 60 s qui s'y ajoutaient — un « TMMBR skipping
	//delay » — n'ont plus d'objet : l'emission est desormais verrouillee par la
	//negociation et amortie par le RembThrottler (lot 2), ce n'est plus a
	//l'estimateur de se taire une minute. Elles rendaient surtout tout appel de
	//moins de 61,5 s de video INOBSERVABLE : aucune reestimation periodique,
	//donc aucune trace « BWE: estimation » (constate en mesure, 2026-08-17).
	if (!lastChange)
		lastChange = now + kInitializationMs;

	bool estimated = false;

	//Only update once per second or when the stream starts to overuse
	if (lastChange+1000<now)
	{
		//Update
		Update(usage,streamOverusing,now);
		//Reset min max
		bitrateAcu.ResetMinMax();
		estimated = true;
	} else if ( streamOverusing) {
		//Update but not reset
		Update(usage,streamOverusing,now);
		estimated = true;
	}

	DWORD estimation = estimated ? GetEstimatedBitrateUnlocked() : 0;

	//Unloc
	lock.Unlock();

	if (!estimated)
		return;

	//La consigne se publie sous le verrou LECTEUR — ni sous l'ecrivain, ni sans
	//verrou. Les deux autres choix ont chacun leur panne :
	//  - sous l'ecrivain, le premier REMB pendait le thread RTP pour toujours :
	//    le listener rappelle l'estimateur (RTPSession::onTargetBitrateRequested
	//    -> GetSSRCs, pour nommer les flux couverts) et IncUse attend l'ecrivain ;
	//  - sans verrou, RemoveListener aboutit PENDANT la notification et
	//    ~RTPSession libere la session sous nos pieds. L'estimateur est un membre
	//    de l'Endpoint (jsr309/Endpoint.h), partage par ses jambes : le thread RTP
	//    de l'une notifie les sessions des autres, que le thread XML-RPC detruit
	//    par EndpointDelete. C'est une ecriture apres liberation, donc un tas
	//    corrompu et un crash differe, ailleurs (mesure du 2026-08-17).
	//IncUse tient les deux bouts : il est reentrant, et il bloque RemoveListener
	//le temps de la notification. La liste n'est donc PAS copiee — un listener
	//retire entre-temps n'y est simplement plus.
	lock.IncUse();
	for (Listener* l : listeners)
		//Send it
		l->onTargetBitrateRequested(estimation);
	lock.DecUse();
}

//Frein temporel du temoin (aimd_rate_control.cc, TimeToReduceFurther) : une
//descente n'est rejouee que si le temps d'une reaction s'est ecoule, ou si le
//debit s'est effondre sous la moitie de la consigne. Sans lui, chaque rapport
//de perte ou de RTT relance toute la machine — mesure du 2026-08-18 : 102
//reestimations en 5,1 s, une toutes les 12 ms, et l'estimation qui retient le
//pire echantillon de l'episode au lieu de sa moyenne.
bool RemoteRateEstimator::TimeToReduceFurther(QWORD now) const
{
	const QWORD interval = rtt < 10 ? 10 : (rtt > 200 ? 200 : rtt);
	if (now >= lastChange + interval)
		return true;
	//Effondrement franc : on ne fait pas attendre une chute de moitie.
	if (currentBitRate)
		return bitrateAcu.GetInstantAvg() < 0.5 * currentBitRate;
	return false;
}

void RemoteRateEstimator::Update(RemoteRateControl::BandwidthUsage usage, bool reactNow, QWORD now)
{
	//reactNow dit que la surutilisation vient d'apparaitre. Sur un FRONT on
	//reagit tout de suite ; sur un NIVEAU repete, on attend le frein. Le
	//parametre existait depuis l'origine sans etre lu.
	if (usage==RemoteRateControl::OverUsing && !reactNow && !TimeToReduceFurther(now))
		return;

	//Blindage §3.1 bis : un instant qui recule (appelant defaillant, horloge
	//melangee) ne doit plus empoisonner avgChangePeriod par soustraction non
	//signee ni produire une conversion double->DWORD hors plage.
	if (now < lastChange)
		now = lastChange;

	// If it is the first estimation
	if (!currentBitRate)
		//Init to maximum
		currentBitRate = bitrateAcu.GetMaxAvg();

	//Calculate difference from last update
	QWORD changePeriod = now - lastChange;

	//Update last changed
	lastChange = now;

	//calculate average period
	avgChangePeriod = 0.9f * avgChangePeriod + 0.1f * changePeriod;

	//Modify state depending on the bandwidht state
	switch (usage)
	{
		case RemoteRateControl::Normal:
			if (state == Hold)
			{
				//Change now
				lastBitRateChange = now;
				//Swicth to increase
				ChangeState(Increase);
			} else if (state == Decrease) {
				ChangeState(Hold);
			}
			break;
		case RemoteRateControl::OverUsing:
			if (state == Increase)
				//Decrease
				ChangeState(Hold);
			else if (state == Hold)
				//Decrease
				ChangeState(Decrease);
			break;
		case RemoteRateControl::UnderUsing:
			if (region==RemoteRateControl::NearMax && state != Hold)
			{
				ChangeState(Hold);
			}
			else if (state!=Increase)
			{
				//Change now
				lastBitRateChange = now;
				//Swicth to increase
				ChangeState(Increase);
			}
			break;
	}

	//Get current estimation
	DWORD current = currentBitRate;
	//Get incoming bitrate
	float incomingBitRate = bitrateAcu.GetInstantAvg();

	//Fenetre du plafond glissant (deque monotone : le front est le max).
	while (!incomingMaxWindow.empty() && incomingMaxWindow.front().first + IncreaseLimitWindowMs < now)
		incomingMaxWindow.pop_front();
	//Fenetre videe par l'expiration ci-dessus, pas par une baisse mesuree : aucun
	//paquet n'est arrive depuis plus de IncreaseLimitWindowMs (renegociation,
	//coupure ICE...). Ce n'est pas un signal de congestion, juste une absence de
	//mesure — mesure du 2026-08-26 (mcu-gris.log) : une reprise au meme debit
	//qu'avant la coupure s'est vue plafonnee au plancher (16 kb/s) le temps
	//d'une remontee AIMD de plusieurs dizaines de secondes, ecran gris a l'appui.
	const bool windowWasEmpty = incomingMaxWindow.empty();
	while (!incomingMaxWindow.empty() && incomingMaxWindow.back().second <= incomingBitRate)
		incomingMaxWindow.pop_back();
	incomingMaxWindow.emplace_back(now, incomingBitRate);
	const float windowedIncomingBitRate = incomingMaxWindow.front().second;
	// Calculate the max bit rate std dev given the normalized
	// variance and the current incoming bit rate.
	float stdMaxBitRate = sqrt(varMaxBitRate * avgMaxBitRate);

	if (stdMaxBitRate<avgMaxBitRate*0.03)
		stdMaxBitRate = avgMaxBitRate*0.03;	

	bool recovery = false;

	//Depending on curren state
	switch (state)
	{
		case Hold:
			maxHoldRate = fmax(maxHoldRate, incomingBitRate);
			break;
		case Increase:
		{
			if (avgMaxBitRate >= 0)
			{
				if (incomingBitRate > avgMaxBitRate + 3 * stdMaxBitRate)
				{
					ChangeRegion(RemoteRateControl::MaxUnknown);
					UpdateMaxBitRateEstimate(fmax(currentBitRate,incomingBitRate));
				} else if (incomingBitRate > avgMaxBitRate + 2.5 * stdMaxBitRate) {
					ChangeRegion(RemoteRateControl::AboveMax);
				} else if (incomingBitRate > avgMaxBitRate - 3 * stdMaxBitRate) {
					ChangeRegion(RemoteRateControl::NearMax);
				} else {
					ChangeRegion(RemoteRateControl::BelowMax);
				}
			}

			const DWORD responseTime = (DWORD) (avgChangePeriod + 0.5f) + rtt + 300;
			double alpha = RateIncreaseFactor(now, lastBitRateChange, responseTime);

			current = (DWORD) (current * alpha) + 8000;

			if (maxHoldRate > 0 && beta * maxHoldRate > current)
			{
				current = (DWORD) (beta * maxHoldRate);
				UpdateMaxBitRateEstimate(fmax(currentBitRate,incomingBitRate));
				ChangeRegion(RemoteRateControl::NearMax);
				recovery = true;
			}

			maxHoldRate = 0;
			Debug("BWE: Increase rate to current = %u kbps\n", current / 1000);
			lastBitRateChange = now;
			break;
		}
		case Decrease:
			// Set bit rate to something slightly lower than max
			// to get rid of any self-induced delay.
			current = (DWORD) (beta * incomingBitRate + 0.5);
			if (current > currentBitRate)
			{
				// Avoid increasing the rate when over-using.
				if (region != RemoteRateControl::MaxUnknown)
					current = (DWORD) (beta * avgMaxBitRate + 0.5f);
				current = fmin(current, currentBitRate);
			}


			if (avgMaxBitRate<0 || incomingBitRate > avgMaxBitRate - 3 * stdMaxBitRate )
			{
				ChangeRegion(RemoteRateControl::NearMax);
				UpdateMaxBitRateEstimate(fmax(currentBitRate,incomingBitRate));
			} else {
				ChangeRegion(RemoteRateControl::BelowMax);
			}	

			Debug("BWE: Decrease rate to current = %u kbps\n", current / 1000);
		
			lastBitRateChange = now;
			//« Stay on hold until the pipes are cleared » (aimd_rate_control.cc:310-311).
			//Sans cette ligne, l'etat restait Decrease : aucune transition ne l'en
			//sortait, le retour au calme le menait a Hold et il fallait un SECOND
			//tick Normal pour relancer la montee. Un lien qui alterne n'en offre
			//jamais deux de suite — mesure du 2026-08-19 en boucle ouverte,
			//estimation figee 17 s a 871 kb/s pour 1500 a 1860 kb/s recus, puis
			//descendant encore a chaque tick sur un accumulateur qui baisse.
			ChangeState(Hold);
			break;
	}
	
	//Plafond GLISSANT du temoin (aimd_rate_control.cc:238-239, :266) : on
	//n'annonce pas plus de 1,5 fois ce qu'on recoit, mais on SUIT ce debit au
	//lieu de se figer. L'ancien code posait current = currentBitRate, un gel
	//dont on ne sortait plus : mesure du 2026-08-18, estimation immobile a
	//3841 kb/s pendant 90 s pour 2465 kb/s recus — c'est ce gel qui rendait la
	//re-montee inobservable.
	//
	//La borne est le max FENETRE de l'entrant, pas l'entrant instantane : nos
	//pairs obeissent a la lettre (Linphone cale son emission sur le TMMBR,
	//Chrome plafonne sur le REMB), donc suivre un trou d'emission d'une seconde
	//le transformait en plafond dont on ne sortait qu'a +8 %/s — 20 s
	//d'oscillation mesures le 2026-08-22 sur un lien sain, sans un seul
	//OverUsing. Une baisse DURABLE est toujours suivie (la fenetre se vide en
	//IncreaseLimitWindowMs) et une congestion reelle passe par Decrease.
	if (!recovery && !windowWasEmpty && (incomingBitRate > 100000 || current > 150000))
	{
		const DWORD increaseLimit = (DWORD)(1.5 * windowedIncomingBitRate) + 10000;
		if (current > increaseLimit)
		{
			current = increaseLimit;
			lastBitRateChange = now;
		}
	}

	//Update
	currentBitRate = current;

	//Chec min
	if (currentBitRate<minConfiguredBitRate)
		//Set minimun
		currentBitRate = minConfiguredBitRate;
	//Chec max
	if (currentBitRate>maxConfiguredBitRate)
		//Set maximum
		currentBitRate = maxConfiguredBitRate;

	//Formats : DWORD -> %u, long double -> cast double + %f ("%llf" n'existe pas,
	//les valeurs affichees etaient fausses).
	//"stream=" nomme la patte (tag du participant / nom de l'endpoint) : sans lui
	//un appel a deux pattes melange deux series dans le meme journal et le
	//depouillement du lot 3 (mcu/tests/tools/) ne peut pas les separer.
	Debug("BWE: estimation stream=%s state=%s region=%s usage=%s currentBitRate=%u current=%u incoming=%.0f min=%.0f max=%.0f\n",eventSource?eventSource->GetName():"",GetName(state),RemoteRateControl::GetName(region),RemoteRateControl::GetName(usage),currentBitRate/1000,current/1000,(double)incomingBitRate/1000,(double)bitrateAcu.GetMinAvg()/1000,(double)bitrateAcu.GetMaxAvg()/1000);

	if (eventSource)
		eventSource->SendEvent
		(
			"rre",
			"[%llu,\"%s\",\"%s\",%u,%u,%u,%u,%u]",
			now,
			GetName(state),
			RemoteRateControl::GetName(region),
			(DWORD)currentBitRate/1000,
			avgMaxBitRate>0?(DWORD)avgMaxBitRate/1000:0,
			(DWORD)incomingBitRate/1000,
			bitrateAcu.IsInMinMaxWindow()?(DWORD)bitrateAcu.GetMinAvg()/1000:0,
			bitrateAcu.IsInMinMaxWindow()?(DWORD)bitrateAcu.GetMaxAvg()/1000:0
		);

	//La notification des listeners appartient a l'appelant, qui l'emet apres
	//avoir relache le verrou : cette fonction s'execute sous le verrou ecrivain.
}

double RemoteRateEstimator::RateIncreaseFactor(QWORD now, QWORD last, DWORD reactionTime) const
{
	// alpha = 1.02 + B ./ (1 + exp(b*(tr - (c1*s2 + c2))))
	// Parameters
	const double B = 0.0407;
	const double b = 0.0025;
	const double c1 = -6700.0 / (33 * 33);
	const double c2 = 800.0;
	const double d = 0.85;

	double alpha = 1.005 + B / (1 + exp(b * (d * reactionTime - (c1 * noiseVar + c2))));

	if (alpha < 1.005)
		alpha = 1.005;
	else if (alpha > 1.3)
		alpha = 1.3;

	if (last)
		alpha = pow(alpha, (now - last) / 1000.0);

	if (region == RemoteRateControl::NearMax)
		// We're close to our previous maximum. Try to stabilize the
		// bit rate in this region, by increasing in smaller steps.
		alpha = alpha - (alpha - 1.0) / 2.0;
	else if (region == RemoteRateControl::MaxUnknown)
		alpha = alpha + (alpha - 1.0) * 4.0;
	else if (region == RemoteRateControl::BelowMax)
		alpha = alpha + (alpha - 1.0) * 2.0;

	return alpha;
}

void RemoteRateEstimator::UpdateChangePeriod(QWORD now)
{
	QWORD changePeriod = 0;
	if (lastChange)
		changePeriod = now - lastChange;
	lastChange = now;
	avgChangePeriod = 0.9f * avgChangePeriod + 0.1f * changePeriod;
}

void RemoteRateEstimator::UpdateMaxBitRateEstimate(float incomingBitRate)
{
	const float alpha = 0.10f;
	
	if (avgMaxBitRate == -1.0f)
		avgMaxBitRate = incomingBitRate;
	else
		avgMaxBitRate = (1 - alpha) * avgMaxBitRate + alpha * incomingBitRate;

	// Estimate the max bit rate variance and normalize the variance with the average max bit rate.
	const float norm = fmax(avgMaxBitRate, 1.0f);

	varMaxBitRate = (1 - alpha) * varMaxBitRate + alpha * (avgMaxBitRate - incomingBitRate) * (avgMaxBitRate - incomingBitRate) / norm;

	// 0.4 ~= 14 kbit/s at 500 kbit/s
	if (varMaxBitRate < 0.4f)
		varMaxBitRate = 0.4f;

	// 2.5f ~= 35 kbit/s at 500 kbit/s
	if (varMaxBitRate > 2.5f)
		varMaxBitRate = 2.5f;

}

DWORD RemoteRateEstimator::GetEstimatedBitrateUnlocked() const
{
	//Retun estimation
	return bitrateAcu.IsInWindow() ? currentBitRate : 0;
}

DWORD RemoteRateEstimator::GetIncomingBitrate()
{
	//Meme verrou lecteur que GetEstimatedBitrate : l'accumulateur est ecrit par
	//le thread qui recoit les paquets.
	lock.IncUse();
	DWORD incoming = bitrateAcu.IsInWindow() ? (DWORD)bitrateAcu.GetInstantAvg() : 0;
	lock.DecUse();
	return incoming;
}
DWORD RemoteRateEstimator::GetEstimatedBitrate()
{
	//Lecteur : trois threads lisent sans verrou jusqu'ici (revue rate-control)
	lock.IncUse();
	DWORD estimation = GetEstimatedBitrateUnlocked();
	lock.DecUse();
	return estimation;
}

void RemoteRateEstimator::GetSSRCs(std::list<DWORD> &ssrcs)
{
	//Lecteur : la map est modifiee par AddStream/RemoveStream/Update
	lock.IncUse();
	//For each one
	for (Streams::iterator it = streams.begin();  it!=streams.end(); ++it)
		//add ssrc
		ssrcs.push_back(it->first);
	lock.DecUse();
}
void RemoteRateEstimator::ChangeState(State newState)
{
	Debug("BWE: ChangeState from:%s to:%s\n",GetName(state),GetName(newState));
	//Store values
	cameFromState = state;
	state = newState;
}

void RemoteRateEstimator::ChangeRegion(RemoteRateControl::Region newRegion)
{
	Debug("BWE: Change region to:%s\n",RemoteRateControl::GetName(newRegion));
	//Store new region
	region = newRegion;
	//Calculate new beta
	switch (region)
	{
		case RemoteRateControl::AboveMax:
		case RemoteRateControl::MaxUnknown:
			beta = 0.9f;
			break;
		case RemoteRateControl::NearMax:
			beta = 0.95f;
			break;
		case RemoteRateControl::BelowMax:
			beta = 0.85f;
	}
	//Set it on controls
	for (Streams::iterator it = streams.begin(); it!=streams.end(); ++it)
		//Set region on control
		 it->second->SetRateControlRegion(newRegion);
}

void RemoteRateEstimator::UpdateRTT(DWORD ssrc, DWORD rtt, QWORD now)
{
	//Lock
	lock.WaitUnusedAndLock();
	//Update
	this->rtt = rtt;
	//Find stream
	Streams::iterator it = streams.find(ssrc);
	//If found
	if (it!=streams.end())
	{
		//Le FRONT se calcule ici, comme le temoin le fait avec prior_state
		//(remote_bitrate_estimator_single_stream.cc:111-121) : ces deux chemins
		//rendent un niveau, seul l'appelant sait si la surutilisation vient
		//d'apparaitre. Le front passe, le niveau repete attend le frein.
		const bool wasOverusing = it->second->GetUsage()==RemoteRateControl::OverUsing;
		if (it->second->UpdateRTT(rtt, now))
			Update(it->second->GetUsage(),!wasOverusing, now);
	}
	//Unlock
	lock.Unlock();
}

void RemoteRateEstimator::UpdateLost(DWORD ssrc, DWORD lost, QWORD now)
{
	//Lock
	lock.WaitUnusedAndLock();
	//Find stream
	Streams::iterator it = streams.find(ssrc);
	//If found
	if (it!=streams.end())
	{
		const bool wasOverusing = it->second->GetUsage()==RemoteRateControl::OverUsing;
		//Set it (meme horloge que les paquets, §3.3)
		if (it->second->UpdateLost(lost, now))
			Update(it->second->GetUsage(),!wasOverusing, now);
	}
	//Unlock
	lock.Unlock();
}


void RemoteRateEstimator::SetTemporalMaxLimit(DWORD limit)
{
	//Ecrivain : la borne est lue par Update sous verrou
	lock.WaitUnusedAndLock();
	//Check if reseting
	if (limit)
	{
		//Le garde n'exclut plus que l'absurde : avec le plancher a 16 kb/s,
		//un maximum de 64 kb/s — un reseau lent — est enfin annonçable (§6).
		if( limit > minConfiguredBitRate)
		{
			Log("-RemoteRateEstimator::SetTemporalMaxLimit() %u\n", limit);
			//Set maximun bitrate
			maxConfiguredBitRate = limit;
		}
		else
		{
			Log("-RemoteRateEstimator::SetTemporalMaxLimit() ignored %u\n", limit);
		}
	}
	else
	{
		//Set default max (temoin : 30 Mb/s)
		maxConfiguredBitRate = 30000000;
		Log("-RemoteRateEstimator::SetTemporalMaxLimit() maximized %u\n", maxConfiguredBitRate);
	}
	lock.Unlock();
}

void RemoteRateEstimator::SetTemporalMinLimit(DWORD limit)
{
	//Ecrivain : la borne est lue par Update sous verrou
	lock.WaitUnusedAndLock();
	//Check if reseting
	if (limit)
	{
		if( limit < maxConfiguredBitRate)
		{
			Log("-RemoteRateEstimator::SetTemporalMinLimit %u\n", limit);
			//Set minimum bitrate
			minConfiguredBitRate = limit;
		}
		else
		{
			Log("-RemoteRateEstimator::SetTemporalMinLimit ignored %u\n", limit);
		}
	}
	else
	{
		//Set default min
		minConfiguredBitRate = 16000;
		Log("-RemoteRateEstimator::SetTemporalMinLimit minimized %u\n", minConfiguredBitRate);
	}
	lock.Unlock();
}

void RemoteRateEstimator::AddListener(Listener *listener)
{
	if (!listener)
		return;
	//Ecrivain : le set est parcouru par Update sous verrou
	lock.WaitUnusedAndLock();
	listeners.insert(listener);
	lock.Unlock();
}

void RemoteRateEstimator::RemoveListener(Listener *listener)
{
	lock.WaitUnusedAndLock();
	listeners.erase(listener);
	lock.Unlock();
}
