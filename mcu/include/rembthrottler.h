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
 *   - tout le reste (hausse, ou variation dans le bruit) attend la période de
 *     hausse depuis la dernière annonce — 200 ms en REMB, que Chrome attend
 *     périodique. En TMMBR il n'y a PAS de période : la limite est collante
 *     (RFC 5104), redire une valeur voisine n'apprend rien au pair, et Linphone
 *     détruit et recrée son encodeur VP8 à chaque TMMBR de valeur différente
 *     (msvideoqualitycontroller.c, vp8.c enc_set_configuration) — une trame clé
 *     toutes les 2,6 s mesurée le 2026-08-30 avec la période de 5 s. Seul un pas
 *     franc de hausse part, et la baisse n'est « franche » qu'à 10 % (un pas
 *     d'AIMD vaut 15 %, le bruit du plafond glissant 3 %) ;
 *   - une HAUSSE que le pair DEPASSE deja ne part pas : ce n'est plus notre
 *     limite qui le borne mais sa propre negociation, donc monter le plafond ne
 *     changera rien a ce qu'il emet — alors que le seul fait de recevoir un
 *     TMMBR de valeur differente lui fait repiocher taille et cadence. Mesure du
 *     2026-09-02, appel sans degradation : sur 6 annonces, 4 annoncaient plus
 *     que le debit reellement recu, et chacune a fait basculer la definition de
 *     la source entre VGA et 720p (0,03 a 0,94 s apres l'envoi) ;
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

	//Ce qui mérite une annonce, par dialecte.
	struct Policy
	{
		//Période après laquelle une hausse (ou du bruit) part quand même ;
		//0 = jamais par le temps.
		QWORD raiseIntervalMs;
		//Pas de hausse qui part sans attendre ; 0 = aucun.
		DWORD raiseStepPercent;
		//Seuil de la baisse franche : lastSent >= bitrate * seuil / 100.
		DWORD dropThresholdPercent;
	};
	static constexpr Policy RembPolicy  = { SendIntervalMs, 0, (DWORD)SendThresholdPercent };
	static constexpr Policy TmmbrPolicy = { 0, 20, 110 };
	//Valeur d'« aucun plafond externe » et de « rien encore annoncé ».
	static constexpr DWORD NoLimit = 0xFFFFFFFF;
	//Tolérance sur « le pair respecte notre limite » : il faut qu'il la dépasse
	//de plus de 5 % pour qu'on le déclare tenu par sa propre négociation. En
	//dessous, il peut être en train de l'appliquer, et la lever l'informe.
	static constexpr QWORD PeerOverLimitPercent = 105;

	RembThrottler()
	{
		Reset();
	}

	//Le dialecte négocié choisit sa politique. Survit à Reset() : c'est une
	//propriété de la négociation, pas de l'état.
	void SetPolicy(const Policy& p)
	{
		policy = p;
	}

	//Remet l'amortisseur à l'état neuf : la prochaine annonce part sans attendre.
	void Reset()
	{
		lastSent     = NoLimit;
		lastSendTime = 0;
		hasSent      = false;
		maxBitrate   = NoLimit;
		peerBitrate  = 0;
	}

	//Débit réellement reçu du pair, en bps ; 0 = inconnu, aucun filtrage. Posé
	//avant la décision, il sert à reconnaître une hausse qui n'apprend rien.
	void SetPeerBitrate(DWORD bitrate)
	{
		peerBitrate = bitrate;
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
		if (hasSent)
		{
			const bool drop = (QWORD)bitrate * policy.dropThresholdPercent / 100 <= (QWORD)lastSent;
			const bool step = policy.raiseStepPercent
				&& (QWORD)bitrate * 100 >= (QWORD)lastSent * (100 + policy.raiseStepPercent)
				&& RaiseIsInformative();
			const bool period = policy.raiseIntervalMs
				&& now >= lastSendTime + policy.raiseIntervalMs;
			if (!drop && !step && !period)
				return false;
		}

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

	//Une hausse apprend-elle quelque chose au pair ? Non s'il DÉPASSE déjà la
	//limite qu'on lui a annoncée : le plafond effectif est alors le sien, et
	//monter le nôtre ne changera pas ce qu'il émet. Vrai quand on ne mesure
	//rien (on n'invente pas) et quand il respecte la limite, car la lever est
	//précisément ce qui le libère.
	bool RaiseIsInformative() const
	{
		if (!peerBitrate || lastSent == NoLimit)
			return true;
		return (QWORD)peerBitrate * 100 <= (QWORD)lastSent * PeerOverLimitPercent;
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
	DWORD	peerBitrate;
	Policy	policy = RembPolicy;
};

#endif	/* REMBTHROTTLER_H */
