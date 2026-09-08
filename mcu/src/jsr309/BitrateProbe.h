/*
 * BitrateProbe.h — sonde au-dessus de la limite TMMBR/REMB du pair.
 *
 * Un pair Linphone après une congestion ne remonte plus jamais sa limite : son
 * TMMBR vaut 0,7 puis 0,9 fois ce qu'il reçoit, et sa seule voie de remontée —
 * l'estimateur VBE — ignore toute image de moins de 3 paquets. À 55 kb/s une
 * image fait un paquet : il est aveugle, et nous obéissons à la lettre. Séance
 * du 2026-09-02 : 55 kb/s de 720p, 36 s après la restauration d'un lien parfait.
 *
 * La sonde dépasse la limite du pair, par paliers de x1,4 et pendant 15 s, pour
 * que son VBE revoie des images de plusieurs paquets et remesure. L'encodeur
 * doit alors REMPLIR la cible (VideoEncoder::SetFillBudget) : un encodeur H264
 * en CRF laissé à lui-même n'émettait que ~250 kb/s pour une sonde à 500
 * (séance du 2026-09-02, mesure chez le pair), et le pair n'avait rien à suivre.
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
	//Le VBE du pair ignore toute image de moins de 3 paquets (mPacketCountMin) :
	//plancher = 3 paquets pleins par image à la cadence courante, plus 25 % pour
	//la crête VBV à 90 % de la consigne et l'entête RTP. 607 kb/s à 15 im/s.
	static constexpr int   PacketsPerFrame = 3;
	static constexpr int   FloorMarginNum  = 5;
	static constexpr int   FloorMarginDen  = 4;
	//Pas de montée : seuil d'acceptation du pair après congestion
	//(NO_INCREASE_THRESHOLD_FOR_CONGESTION = 1.4).
	static constexpr int   StepNum       = 14;
	static constexpr int   StepDen       = 10;
	//Lien propre depuis autant avant d'oser.
	static constexpr QWORD ArmUs         = 5000000;
	//L'estimation du pair est le 10e centile de ses 200 dernières images qualifiées
	//(mMaxMeasurements, trust 90 %) : il faut en renouveler plus de 180 à la
	//cadence sondée (12 s à 15 im/s), puis attendre sa période de 5 s (mMinInterval).
	static constexpr QWORD DurationUs    = 15000000;
	//Entre deux sondes ; doublé à chaque échec, remis au minimum quand le pair suit.
	static constexpr QWORD IntervalMinUs = 10000000;
	static constexpr QWORD IntervalMaxUs = 120000000;
	//Le pair n'applique une estimation que si elle dépasse d'au moins 40 % ce
	//qu'il REÇOIT ; pendant la sonde il reçoit l'estimation, donc il ne relève
	//sa limite qu'une fois la sonde finie (6 s après, mesuré le 2026-09-02). On
	//guette la hausse encore ce temps-là après la fin.
	static constexpr QWORD FollowGraceUs = 10000000;

	enum Event { None, Start, End, Abort, Followed };

	BitrateProbe() { Reset(); }

	void Reset()
	{
		probing       = false;
		probeKbps     = 0;
		peerAtStart   = 0;
		startUs       = 0;
		endedUs       = 0;
		cleanSinceUs  = 0;
		nextAllowedUs = 0;
		intervalUs    = IntervalMinUs;
		event         = None;
	}

	static int Floor(int fps)
	{
		if (fps < 1)
			fps = 1;
		return PacketsPerFrame * RTPPAYLOADSIZE * 8 * fps / 1000 * FloorMarginNum / FloorMarginDen;
	}

	//Ce que la sonde voudrait envoyer face à `peerKbps`, sous la consigne.
	static int Wanted(int peerKbps, int ceilingKbps, int fps)
	{
		int wanted = peerKbps * StepNum / StepDen;
		if (wanted < Floor(fps))
			wanted = Floor(fps);
		if (ceilingKbps > 0 && wanted > ceilingKbps)
			wanted = ceilingKbps;
		return wanted;
	}

	//`target` est le min(consigne, pair, estimateur) déjà calculé par l'appelant.
	//Rend la cible à appliquer ; LastEvent() dit s'il s'est passé quelque chose.
	int Apply(int target, int peerKbps, int bweKbps, int ceilingKbps, int fps, QWORD nowUs)
	{
		event = None;

		//Hausse du pair dans le délai de grâce après une sonde : c'est elle qui
		//l'a obtenue. L'échelle repart vite, du nouveau palier.
		if (!probing && endedUs)
		{
			if (nowUs - endedUs > FollowGraceUs)
				endedUs = 0;
			else if (peerKbps > peerAtStart)
			{
				endedUs       = 0;
				intervalUs    = IntervalMinUs;
				nextAllowedUs = nowUs + IntervalMinUs;
				event         = Followed;
			}
		}

		const int  wanted = Wanted(peerKbps, ceilingKbps, fps);
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
				Stop(nowUs, true, Followed);
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
	int   GetPeerAtStart() const { return peerAtStart; }
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
		endedUs       = followed ? 0 : nowUs;
		event         = why;
	}

	bool  probing;
	int   probeKbps;
	int   peerAtStart;
	QWORD startUs;
	QWORD endedUs;
	QWORD cleanSinceUs;
	QWORD nextAllowedUs;
	QWORD intervalUs;
	Event event;
};

#endif	/* BITRATEPROBE_H */
