/*
 * File:   rembthrottler.h
 *
 * Amortisseur du feedback de débit sortant (chantier rate-control, lot 2).
 *
 * L'estimateur de réception change d'avis à chaque réévaluation ; le pair, lui,
 * n'a pas besoin d'un paquet RTCP par soubresaut. La règle est celle du témoin
 * (../webrtc, modules/congestion_controller/remb_throttler.cc), réécrite en
 * style maison plutôt que recopiée (arbitrage A5 du plan) :
 *
 *   - une BAISSE de plus de 3 % part TOUT DE SUITE — c'est le message urgent,
 *     le lien sature et le pair doit ralentir ;
 *   - tout le reste (hausse, ou variation dans le bruit) attend 200 ms depuis
 *     la dernière annonce ;
 *   - un plafond venu d'AILLEURS (l'autre patte d'un relais, lot 5) se compose
 *     par min() avec la mesure locale : on annonce le plus contraint des deux.
 *
 * La classe ne connaît ni socket ni horloge : l'appelant lui passe l'instant
 * (getTimeMS() en production, une horloge simulée dans les tests) et reçoit un
 * booléen « émettre maintenant » plus la valeur à annoncer. Elle sert les deux
 * chemins d'émission — REMB (draft-alvestrand-rmcat-remb-03) et TMMBR
 * (RFC 5104) — parce que la question qu'elle tranche, « faut-il redire au pair
 * combien il peut envoyer », est la même dans les deux formats.
 */

#ifndef REMBTHROTTLER_H
#define	REMBTHROTTLER_H

#include "config.h"

class RembThrottler
{
public:
	//Période minimale entre deux annonces qui ne sont PAS une baisse franche.
	static constexpr QWORD SendIntervalMs = 200;
	//Une annonce ne devance sa période que si elle descend de plus de 3 %.
	static constexpr QWORD SendThresholdPercent = 103;
	//Valeur d'« aucun plafond externe » et de « rien encore annoncé ».
	static constexpr DWORD NoLimit = 0xFFFFFFFF;

	RembThrottler()
	{
		Reset();
	}

	//Remet l'amortisseur à l'état neuf : la prochaine annonce part sans attendre.
	void Reset()
	{
		lastSent     = NoLimit;
		lastSendTime = 0;
		hasSent      = false;
		maxBitrate   = NoLimit;
	}

	/**
	 * La mesure locale a changé (estimateur de réception).
	 * @param bitrate  la nouvelle estimation, en bps
	 * @param now      l'instant, en ms
	 * @param out      [sortie] le débit à annoncer au pair, plafond compris
	 * @return true s'il faut émettre maintenant
	 */
	bool OnEstimateChanged(DWORD bitrate, QWORD now, DWORD& out)
	{
		//Une hausse — ou une variation dans le bruit — attend son tour ; seule
		//une baisse de plus de 3 % devance la période.
		if (hasSent
		    && (QWORD)bitrate * SendThresholdPercent / 100 > (QWORD)lastSent
		    && now < lastSendTime + SendIntervalMs)
			return false;

		//C'est la mesure qui est mémorisée, pas la valeur émise : le plafond
		//externe est une composition, il ne doit pas faire oublier ce que la
		//patte mesure réellement.
		lastSent     = bitrate;
		lastSendTime = now;
		hasSent      = true;

		out = Compose(bitrate);
		return true;
	}

	/**
	 * Un plafond posé de l'extérieur (la patte opposée d'un relais, lot 5).
	 * Un plafond qui DESCEND part tout de suite ; un plafond qui remonte, ou
	 * qui ne mord pas sur la mesure locale, attend la période.
	 * @return true s'il faut émettre maintenant
	 */
	bool SetMaxBitrate(DWORD bitrate, QWORD now, DWORD& out)
	{
		maxBitrate = bitrate;

		//Rien de neuf à dire au pair si le plafond ne mord pas sur ce qu'on a
		//déjà annoncé, et que la période n'est pas écoulée.
		if (hasSent && now < lastSendTime + SendIntervalMs && lastSent <= maxBitrate)
			return false;

		lastSendTime = now;
		hasSent      = true;

		out = Compose(lastSent);
		return true;
	}

	//Le débit à annoncer pour une mesure locale donnée : le plus contraint de
	//la mesure et du plafond externe. Sans effet de bord — le chemin périodique
	//(SendSenderReport) redit la valeur courante sans rouvrir la décision.
	DWORD Compose(DWORD bitrate) const
	{
		return bitrate < maxBitrate ? bitrate : maxBitrate;
	}

	//La dernière mesure annoncée, NoLimit tant que rien n'est parti.
	DWORD GetLastSent()  const { return lastSent;   }
	DWORD GetMaxBitrate() const { return maxBitrate; }

private:
	DWORD	lastSent;
	QWORD	lastSendTime;
	bool	hasSent;
	DWORD	maxBitrate;
};

#endif	/* REMBTHROTTLER_H */
