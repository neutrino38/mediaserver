/* 
*File:   remoteratecontrol.cpp
*Author: Sergio
*
*Created on 26 de diciembre de 2012, 12:49
 */

#include "remoteratecontrol.h"
#include <cstdlib>
#include <cmath>
#include "log.h"

//packetCalc/lostCalc : fenetres ALIGNEES (1 s, meme horloge appelant) pour que
//le ratio de pertes compare des choses comparables (§3.3 du diagnostic).
RemoteRateControl::RemoteRateControl() : bitrateCalc(100), fpsCalc(1000), packetCalc(1000), lostCalc(1000)
{
	eventSource = NULL;
	rtt = 0;
	prevTS = 0;
	prevTime = 0;
	prevSize = 0;
	prevTarget = 0;
	curTS = 0;
	curTime = 0;
	curSize = 0;
	curDelta = 0;
	lastFrameTS = 0;
	slope = 8.0/512.0;
	E[0][0] = 100;
	E[0][1] = 0;
	E[1][0] = 0;
	E[1][1] = 1e-1;
	//Constantes du temoin (overuse_estimator.h:59) — les valeurs locales
	//1e-10/1e-2 etaient 1000x/10x l'amont, jamais validees en production.
	processNoise[0] = 1e-13;
	processNoise[1] = 1e-3;
	avgNoise = 0;
	varNoise = 50;
	//Seuil unique : c'etait la valeur de la region MaxUnknown, la seule que le
	//detecteur voyait au demarrage. Il n'est plus fixe par region, cf.
	//SetRateControlRegion.
	threshold = 25;
	prevOffset = 0;
	offset = 0;
	hypothesis = Normal;
	overUseCount = 0;
	lostOverCount = 0;
	absSendTimeCycles = 0;
}

void RemoteRateControl::Update(QWORD time,QWORD ts,DWORD size, bool mark)
{
	//Update bitrate calculator
	bitrateCalc.Update(time, size*8);
	//Update packet count
	packetCalc.Update(time, 1);
	
//	Debug("ts:%u,time:%llu,%u\n",ts,time,size);
	
	//If it is an out of order packet from previous frame
	if (curTS && ts < curTS)
		//Exit
		return;
	
	//Update size of current frame
	curSize += size;
	//And reception time
	curTS	= ts;
	curTime = time;
	
	//If not first packet
	if (prevTime)
		//Increase deltas
		curDelta += (curTime - prevTime) - (curTS - prevTS);
				
	//Add new frame
	if (mark)
	{
		//New frame
		fpsCalc.Update(time,1);
		//§3.4 e : le filtre recoit curDelta LUI-MEME — la premiere difference
		//du delai (t_delta − ts_delta du temoin), accumulee sur l'image. On
		//passait curDelta−prevDelta, la derivee SECONDE : une file qui se
		//remplit lineairement etait invisible (mesure lot 0). Premiere image :
		//pas de periode inter-images encore, on ne filtre pas.
		if (lastFrameTS)
			UpdateKalman(curDelta, curSize - prevSize, (double)(curTS - lastFrameTS));
		lastFrameTS = curTS;
		//reset frame stats
		prevSize = curSize;
		curSize = 0;
		curDelta = 0;
	}
	//Update current stats
	prevTS = curTS;
	prevTime = curTime;
}

//Aligne ligne a ligne sur le temoin ../webrtc (overuse_estimator.cc), les
//divergences §3.4 du diagnostic corrigees une a une — voir rate-control.md.
void RemoteRateControl::UpdateKalman(int deltaTime, int deltaSize, double tsDelta)
{
	//Debug("RemoteRateControl::UpdateKalman() deltas [time:%d size:%d ts:%f]\n",deltaTime, deltaSize, tsDelta);

	//§3.4 f : bruit de processus du temoin, SANS mise a l'echelle 30/fps
	//(l'amont ne la fait pas ; la locale multipliait un reglage deja 1000x
	//trop grand).
	E[0][0] += processNoise[0];
	E[1][1] += processNoise[1];

	if ((hypothesis==OverUsing && offset<prevOffset) || (hypothesis==UnderUsing && offset>prevOffset))
		E[1][1] += 10*processNoise[1];

	const double h[2] =
	{
		(double)deltaSize,
		1.0
	};
	const double Eh[2] =
	{
		E[0][0]*h[0] + E[0][1]*h[1],
		E[1][0]*h[0] + E[1][1]*h[1]
	};

	const double residual = deltaTime-slope*h[0]-offset;

	//Periode d'image minimale sur les 60 dernieres (temoin :
	//UpdateMinFramePeriod, overuse_estimator.cc:105-115) : les images en
	//retard ne doivent pas ETIRER le facteur d'oubli.
	double minFramePeriod = tsDelta;
	if (tsDeltaHist.size() >= 60)
		tsDeltaHist.pop_front();
	for (double oldTsDelta : tsDeltaHist)
		minFramePeriod = fmin(oldTsDelta, minFramePeriod);
	tsDeltaHist.push_back(tsDelta);

	//§3.4 a : filtre remis a l'endroit — le residu normal PASSE, l'aberrant
	//(image cle : hors modele gaussien) est ecrete a ±3σ AVEC son signe. La
	//version locale faisait l'inverse et perdait le signe.
	double residualFiltered = residual;
	const double maxResidual = 3*sqrt(varNoise);
	if (std::fabs(residual) > maxResidual)
		residualFiltered = residual < 0 ? -maxResidual : maxResidual;

	//§3.4 d : le bruit ne se mesure qu'en etat STABLE (temoin l.62-68) —
	//mesurer pendant la congestion, c'est prendre la congestion pour du
	//bruit. La condition etait en commentaire depuis 2013.
	if (hypothesis == Normal)
	{
		// Faster filter during startup to faster adapt to the jitter level
		// of the network. alpha is tuned for 30 frames per second.
		double alpha = 0.01;
		//§3.4 f : bascule a 300 images comme le temoin (10 s a 30 fps)
		if (fpsCalc.GetAcumulated() > 300)
			alpha = 0.002;

		//§3.2 : l'exposant du facteur d'oubli est un TEMPS (la periode
		//inter-images), plus jamais une difference de tailles signee — c'est
		//elle qui rendait varNoise negative puis NaN.
		const double beta = pow(1-alpha, minFramePeriod*30/1000.0);
		avgNoise = beta*avgNoise + (1-beta)*residualFiltered;
		varNoise = beta*varNoise + (1-beta)*(avgNoise-residualFiltered)*(avgNoise-residualFiltered);
		//§3.4 b : plancher du temoin (l.136-138) — varNoise reste une variance.
		if (varNoise < 1)
			varNoise = 1;
	}

	const double denom = varNoise+h[0]*Eh[0]+h[1]*Eh[1];
	const double K[2] =
	{
		Eh[0] / denom,
		Eh[1] / denom
	};
	const double IKh[2][2] =
	{
		{ 1.0-K[0]*h[0] ,    -K[0]*h[1] },
		{    -K[1]*h[0] , 1.0-K[1]*h[1] }
	};

	//§3.4 c : temporaires du temoin (l.80-87) — sans eux les deux dernieres
	//lignes lisaient E[0][0]/E[0][1] DEJA reecrits.
	const double e00 = E[0][0];
	const double e01 = E[0][1];

	// Update state
	E[0][0] = e00*IKh[0][0] + E[1][0]*IKh[0][1];
	E[0][1] = e01*IKh[0][0] + E[1][1]*IKh[0][1];
	E[1][0] = e00*IKh[1][0] + E[1][0]*IKh[1][1];
	E[1][1] = e01*IKh[1][0] + E[1][1]*IKh[1][1];

	//Le controle que l'amont fait en RTC_DCHECK (l.89-98)
	if (!CovarianceIsPositiveSemiDefinite())
		Debug("BWE: covariance no longer positive semi-definite\n");

	slope = slope+K[0]*residual;
	prevOffset = offset;
	offset = offset+K[1]*residual;

	const double T = !fpsCalc.IsEmpty() ? fmin(fpsCalc.GetInstantAvg(),30)*offset : offset;

	//Debug("BWE: Update tdelta:%d,tsdelta:%d,fsdelta:%d,t:%f,threshold:%f,slope:%f,offset:%f,scale:%f,frames:%lld,fps:%llf,residual:%f\n",deltaTime,deltaTS,deltaSize,T,threshold,slope,offset,scaleFactor,fpsCalc.GetInstantAvg(),fpsCalc.GetInstantAvg(),residual);

	//Compare
	if (std::fabs(T)>threshold)
	{
		///Detect overuse
		if (offset>0)
		{
			//LOg
			if (hypothesis!=OverUsing )
			{
				//Check 
				//La bascule exige que le delai CONTINUE de croitre (temoin :
				//overuse_detector.cc, "if (offset >= prev_offset_)"). Sans elle, un
				//a-coup dont l'effet retombe declarait une congestion : mesure du
				//2026-08-18, un retard de 32 ms produit quatre depassements de
				//suite dont le T DECROIT (27,6 -> 27,0 -> 26,4 -> 25,9). Le
				//compteur n'est pas remis a zero ici : si le delai recroit a
				//l'image suivante, la bascule a lieu.
				if (overUseCount>2 && offset>=prevOffset)
				{
					//Formats : long double -> cast double + %f ("%llf" n'existe pas,
					//les valeurs affichees etaient fausses).
					Debug("BWE: Overusing bitrate:%.0f max:%.0f min:%.0f T:%f,threshold:%f\n",(double)bitrateCalc.GetInstantAvg(),(double)bitrateCalc.GetMaxAvg(),(double)bitrateCalc.GetMinAvg(),std::fabs(T),threshold);
					//Overusing
					hypothesis = OverUsing;
					//Reset counter
					overUseCount=0;
				} else if (overUseCount<=2) {
					//Le compteur etait passe en 1er argument SANS % correspondant :
					//tous les champs affiches etaient decales d'un cran.
					Debug("BWE: Overusing candidate %u/3 bitrate:%.0f max:%.0f min:%.0f T:%f,threshold:%f\n",overUseCount,(double)bitrateCalc.GetInstantAvg(),(double)bitrateCalc.GetMaxAvg(),(double)bitrateCalc.GetMinAvg(),std::fabs(T),threshold);
					//increase counter
					overUseCount++;
				}
			}
		} else {
			//Le compteur se remet a zero DEHORS de la garde "si l'hypothese
			//change" : conditionne a elle, il n'etait jamais remis a zero sur un
			//flux deja Normal, et comptait "3 depassements depuis toujours" au
			//lieu de "3 consecutifs" — trois a-coups que des secondes de trafic
			//sain separent declaraient une congestion (temoin : overuse_detector.cc
			//remet time_over_using_ et overuse_counter_ a zero des le retour sous
			//le seuil).
			overUseCount=0;
			//If we change state
			if (hypothesis!=UnderUsing)
			{
				Debug("BWE:  UnderUsing bitrate:%.0f max:%.0f min:%.0f T:%f\n",(double)bitrateCalc.GetInstantAvg(),(double)bitrateCalc.GetMaxAvg(),(double)bitrateCalc.GetMinAvg(),std::fabs(T));
				//Reset bitrate
				bitrateCalc.ResetMinMax();
				//Under using, do nothing until going back to normal
				hypothesis = UnderUsing;
			}
		}
	} else {

		overUseCount=0;
		//If we change state
		if (hypothesis!=Normal)
		{
			//Log
			Debug("BWE:  Normal  bitrate:%.0f max:%.0f min:%.0f\n",(double)bitrateCalc.GetInstantAvg(),(double)bitrateCalc.GetMaxAvg(),(double)bitrateCalc.GetMinAvg());
			//Reset
			bitrateCalc.ResetMinMax();
			//Normal
			hypothesis = Normal;
		}
	}
	if (eventSource) eventSource->SendEvent("rrc.update","[%llu,\"%s\"]",getTimeMS(),GetName(hypothesis));
}


bool RemoteRateControl::UpdateRTT(DWORD rtt)
{
	//Check difference
	if (this->rtt>40 && rtt>this->rtt*1.50)
	{	
		//Overusing
		hypothesis = OverUsing;
		//Reset counter
		overUseCount=0;
	}
	
	//Update RTT
	this->rtt = rtt;

	//Debug
	Debug("BWE: UpdateRTT rtt:%dms hipothesis:%s\n",rtt,GetName(hypothesis));

	if (eventSource) 
	{
		eventSource->SendEvent("rrc.rtt","[%llu,\"%s\",\"%d\"]",getTimeMS(),GetName(hypothesis),rtt);
		Debug("BWE: for stream %s\n", eventSource->GetName() );
	}

	//Return if we are overusing now
	return hypothesis==OverUsing;
}

bool RemoteRateControl::UpdateLost(DWORD num, QWORD now)
{
	//§3.3 : meme horloge que les paquets (celle de l'appelant, en ms) — la
	//version locale melait un getTime() en µs a des fenetres en ms, gonflant
	//le ratio au point que quelques pertes isolees valaient congestion
	//(mesure lot 0 : bascule au 5e rapport d'UNE perte).
	lostCalc.Update(now,num);

	//If we are in window
	if (packetCalc.IsInWindow() && lostCalc.IsInWindow())
	{
		//Get packets
		long double packets = packetCalc.GetInstantAvg();
		long double lost    = lostCalc.GetInstantAvg();

		//Check lost is more than 2.5%
		if (lost*1000/(packets+lost)>25)
		{
			//Check
			if (lostOverCount>2)
			{
				//Overusing
				hypothesis = OverUsing;
				//Reset counter
				lostOverCount=0;
				//Reset lost counter
				lostCalc.Reset(now);
				//Debug
				Debug("BWE: UpdateLost lost:%u hipothesis:%s,packets:%.1f,lost:%.1f\n",num,GetName(hypothesis),(double)packets,(double)lost);
			} else {
				//increase counter
				lostOverCount++;
			}
		}
	}

	//L'evenement envoyait le RTT a la place du nombre de pertes.
	if (eventSource) eventSource->SendEvent("rrc.lost","[%llu,\"%s\",\"%u\"]",getTimeMS(),GetName(hypothesis),num);

	//true if overusing
	return hypothesis==OverUsing;
}

void RemoteRateControl::SetRateControlRegion(Region region)
{
	//Debug
	Debug("BWE: SetRateControlRegion %s\n",GetName(region));

	//La region ne fixe PLUS le seuil. Elle le faisait tomber a 12 ms des que
	//l'estimation approchait son maximum connu, ce qui fermait un cercle :
	//OverUsing -> Decrease -> NearMax -> seuil 12 -> OverUsing plus facile
	//encore, et l'estimation ne franchissait jamais son propre maximum (mesure
	//du 2026-08-18 : 1210 kb/s annonces pour 1804 kb/s recus). La region reste
	//le pilote du facteur de montee, cote RemoteRateEstimator.
}
