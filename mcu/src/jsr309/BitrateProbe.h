/*
 * BitrateProbe.h — sonde au-dessus de la limite TMMBR/REMB du pair.
 *
 * Un pair Linphone après une congestion ne remonte plus jamais sa limite : son
 * TMMBR vaut 0,7 puis 0,9 fois ce qu'il reçoit, et sa seule voie de remontée —
 * l'estimateur VBE — ignore toute image de moins de 3 paquets. À 55 kb/s une
 * image fait un paquet : il est aveugle, et nous obéissons à la lettre. Séance
 * du 2026-09-02 : 55 kb/s de 720p, 36 s après la restauration d'un lien parfait.
 *
 * La sonde dépasse la limite du pair, par paliers de x1,4 et pendant quelques
 * secondes, pour que son VBE revoie des images de plusieurs paquets et remesure.
 * Son propre code porte alors son TMMBR à l'estimation. C'est un écart délibéré
 * au RFC 5104 (arbitrage mainteneur 2026-09-02), borné trois fois :
 *   - seulement quand c'est la limite du PAIR qui borne l'encodeur, et que notre
 *     propre estimateur d'émission (étage de perte) tient la valeur sondée ;
 *   - jamais au-dessus de la consigne négociée ni de notre estimateur ;
 *   - abandon à la première perte (notre estimateur redescend), avec un
 *     intervalle doublé à chaque échec.
 * Toutes les constantes viennent des sources Linphone (videobandwidthestimator.cc,
 * bandwidthcontroller.c). Débits en kb/s, instants en µs, aucune horloge interne.
 */
#ifndef BITRATEPROBE_H
#define BITRATEPROBE_H

#include "config.h"

class BitrateProbe
{
public:
	//3 paquets par image à 15 im/s (mPacketCountMin = 3) : en dessous, le VBE du
	//pair ne mesure rien et la sonde est invisible.
	static constexpr int   FloorKbps     = 500;
	//Pas de montée : seuil d'acceptation du pair après congestion
	//(NO_INCREASE_THRESHOLD_FOR_CONGESTION = 1.4).
	static constexpr int   StepNum       = 14;
	static constexpr int   StepDen       = 10;
	//Lien propre depuis autant avant d'oser.
	static constexpr QWORD ArmUs         = 5000000;
	//70 mesures à 15 im/s (mMinMeasurements) puis une fenêtre de 5 s (mMinInterval).
	static constexpr QWORD DurationUs    = 6000000;
	//Entre deux sondes ; doublé à chaque échec, remis au minimum quand le pair suit.
	static constexpr QWORD IntervalMinUs = 10000000;
	static constexpr QWORD IntervalMaxUs = 120000000;

	enum Event { None, Start, End, Abort };

	BitrateProbe() { Reset(); }

	void Reset()
	{
		probing       = false;
		probeKbps     = 0;
		peerAtStart   = 0;
		startUs       = 0;
		cleanSinceUs  = 0;
		nextAllowedUs = 0;
		intervalUs    = IntervalMinUs;
		event         = None;
	}

	//Ce que la sonde voudrait envoyer face à `peerKbps`, sous la consigne.
	static int Wanted(int peerKbps, int ceilingKbps)
	{
		int wanted = peerKbps * StepNum / StepDen;
		if (wanted < FloorKbps)
			wanted = FloorKbps;
		if (ceilingKbps > 0 && wanted > ceilingKbps)
			wanted = ceilingKbps;
		return wanted;
	}

	//`target` est le min(consigne, pair, estimateur) déjà calculé par l'appelant.
	//Rend la cible à appliquer ; LastEvent() dit s'il s'est passé quelque chose.
	int Apply(int target, int peerKbps, int bweKbps, int ceilingKbps, QWORD nowUs)
	{
		event = None;
		const int  wanted = Wanted(peerKbps, ceilingKbps);
		const bool bound  = peerKbps > 0 && target == peerKbps;	//c'est le pair qui borne
		const bool room   = wanted > peerKbps;			//il y a quelque chose à gagner
		const bool clean  = bweKbps > 0 && bweKbps >= wanted;	//notre lien porte la sonde

		if (!bound || !room || !clean)
		{
			if (probing)
				Stop(nowUs, /*followed=*/ peerKbps > peerAtStart, Abort);
			cleanSinceUs = 0;
			return target;
		}

		if (probing)
		{
			//Le pair a remesuré et relevé sa limite : la sonde a fait son
			//travail. On s'arrête sur SA valeur — jamais moins que ce qu'il
			//autorise désormais — et la suivante partira de ce palier.
			if (peerKbps > peerAtStart)
			{
				Stop(nowUs, true, End);
				return target;
			}
			if (nowUs - startUs < DurationUs)
				return Clamp(probeKbps, bweKbps, ceilingKbps);
			//Fin de sonde sans que le pair ait bougé.
			Stop(nowUs, false, End);
			return target;
		}

		if (!cleanSinceUs)
			cleanSinceUs = nowUs;
		if (nowUs - cleanSinceUs < ArmUs || nowUs < nextAllowedUs)
			return target;

		probing     = true;
		startUs     = nowUs;
		probeKbps   = wanted;
		peerAtStart = peerKbps;
		event       = Start;
		return Clamp(probeKbps, bweKbps, ceilingKbps);
	}

	bool  IsProbing()  const { return probing;   }
	int   GetProbeKbps() const { return probeKbps; }
	Event LastEvent()  const { return event;     }
	QWORD GetIntervalUs() const { return intervalUs; }

private:
	static int Clamp(int kbps, int bweKbps, int ceilingKbps)
	{
		if (bweKbps > 0 && kbps > bweKbps)
			kbps = bweKbps;
		if (ceilingKbps > 0 && kbps > ceilingKbps)
			kbps = ceilingKbps;
		return kbps;
	}

	void Stop(QWORD nowUs, bool followed, Event why)
	{
		probing = false;
		//Le pair a suivi : on recommence vite. Sinon on espace, pour ne pas
		//harceler un pair qui ne remesure pas — ni un lien qui perd.
		intervalUs    = followed ? IntervalMinUs
			      : (intervalUs * 2 > IntervalMaxUs ? IntervalMaxUs : intervalUs * 2);
		nextAllowedUs = nowUs + intervalUs;
		cleanSinceUs  = 0;
		event         = why;
	}

	bool  probing;
	int   probeKbps;
	int   peerAtStart;
	QWORD startUs;
	QWORD cleanSinceUs;
	QWORD nextAllowedUs;
	QWORD intervalUs;
	Event event;
};

#endif	/* BITRATEPROBE_H */
