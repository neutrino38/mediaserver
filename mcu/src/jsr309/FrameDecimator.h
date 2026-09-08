/*
 * FrameDecimator.h — pas de décimation d'un transcodeur vidéo inline.
 *
 * Depuis le lot 4 de `jsr309_transcode_sans_thread.md`, l'encodeur tourne sur
 * le thread de démux de la source. S'il est plus lent que la cadence
 * d'arrivée, ce thread prend du retard, le RTPBuffer de la jambe jette des
 * PAQUETS, le décodeur y voit des pertes et réclame une trame clé au pair
 * chaque seconde (appel du 2026-08-29 : 10 697 paquets jetés, 57 FIR en 64 s).
 *
 * Cette politique rend au transcodeur la propriété qu'avait VideoPipe : un
 * encodeur trop lent perd des IMAGES, jamais des paquets. Elle choisit un pas
 * k — une image encodée sur k — tel que le coût d'encodage tienne dans le
 * budget que laisse la source :
 *
 *     k = ceil( coût / (budget × UsableShare) )
 *
 * où `coût` est la moyenne glissante du temps d'encodage d'une image et
 * `budget` l'écart moyen entre deux images de la source. La part utilisable
 * laisse au décodage et au démux ce qui n'est pas de l'encodage.
 *
 * Asymétrie voulue : le pas MONTE tout de suite (le RTPBuffer jette après
 * 500 ms, il n'y a pas le temps d'attendre) et ne REDESCEND qu'après
 * RecoveryUs de coût continûment sous le seuil (chaque changement de cadence
 * coûte une trame clé à la réouverture de l'encodeur).
 */
#ifndef FRAMEDECIMATOR_H
#define FRAMEDECIMATOR_H

#include "config.h"

class FrameDecimator
{
public:
	//Part du budget d'une image que l'encodage peut occuper : 4/5. Le reste
	//est le décodage et le démux, qui tournent sur le même thread.
	static constexpr QWORD UsableShareNum = 4;
	static constexpr QWORD UsableShareDen = 5;
	//Pas maximal : au-delà, la sortie tombe sous ~1 im/s à 15 im/s de source,
	//et c'est l'encodeur lui-même qu'il faut changer, pas le pas.
	static constexpr int MaxStep = 15;
	//Échantillons nécessaires avant la première décision : la moyenne d'un
	//seul coût, c'est le coût de la première image, qui contient l'ouverture
	//de l'encodeur.
	static constexpr int MinSamples = 8;
	//Durée pendant laquelle le coût doit rester sous le seuil avant que le pas
	//redescende.
	static constexpr QWORD RecoveryUs = 3000000;
	//Un échantillon au-delà de OutlierFactor × la moyenne est ramené à cette
	//borne : la trame clé qui suit une réouverture coûte plusieurs fois une
	//trame inter, et ne dit rien du régime.
	static constexpr QWORD OutlierFactor = 3;
	//Hystérésis du seuil : le pas monte quand le coût dépasse la part
	//utilisable, il ne redescend que quand le coût tient dans 7/10 de cette
	//part. Avec un seul seuil, un coût qui oscille de ±2 ms autour de lui fait
	//battre le pas — appel du 2026-08-29 : 31-34 ms pour 32 ms utilisables,
	//74 réouvertures de x264 en 20 min, une trame clé toutes les 16 s.
	static constexpr QWORD DownShareNum = 7;
	static constexpr QWORD DownShareDen = 10;

	FrameDecimator() { Reset(); }

	void Reset()
	{
		step = 1;
		costUs = 0;
		budgetUs = 0;
		samples = 0;
		belowSinceUs = 0;
	}

	//Un encodage vient de coûter `encodeUs` pour une source dont l'écart moyen
	//entre images vaut `frameBudgetUs`. Rend true si le pas a changé.
	bool Observe(QWORD encodeUs, QWORD frameBudgetUs, QWORD nowUs)
	{
		if (!frameBudgetUs)
			return false;
		budgetUs = frameBudgetUs;

		if (samples >= MinSamples && costUs && encodeUs > costUs*OutlierFactor)
			encodeUs = costUs*OutlierFactor;

		//Moyenne glissante exponentielle, constante 1/8 : assez courte pour
		//suivre une montée en charge, assez longue pour lisser une image.
		if (!samples)
			costUs = encodeUs;
		else
			costUs = (costUs*7 + encodeUs)/8;
		samples++;

		if (samples < MinSamples)
			return false;

		const QWORD usable = budgetUs*UsableShareNum/UsableShareDen;
		const int neededUp = StepFor(usable);
		if (neededUp > step)
		{
			step = neededUp;
			belowSinceUs = 0;
			return true;
		}

		//Pour redescendre, le coût doit tenir dans une part plus petite : c'est
		//ce qui sépare les deux seuils.
		const int neededDown = StepFor(usable*DownShareNum/DownShareDen);
		if (neededDown >= step)
		{
			belowSinceUs = 0;
			return false;
		}

		//neededDown < step : redescendre seulement après RecoveryUs de calme.
		if (!belowSinceUs)
		{
			belowSinceUs = nowUs ? nowUs : 1;
			return false;
		}
		if (nowUs - belowSinceUs < RecoveryUs)
			return false;

		step = neededDown;
		belowSinceUs = 0;
		return true;
	}

	int	GetStep() const		{ return step;		}
	QWORD	GetCostUs() const	{ return costUs;	}
	QWORD	GetBudgetUs() const	{ return budgetUs;	}
	bool	IsSaturated() const	{ return step == MaxStep; }

private:
	//Pas nécessaire pour que le coût courant tienne dans `share` µs par image.
	int StepFor(QWORD share) const
	{
		int needed = share ? (int)((costUs + share - 1)/share) : MaxStep;
		if (needed < 1)
			needed = 1;
		if (needed > MaxStep)
			needed = MaxStep;
		return needed;
	}

	int	step;
	QWORD	costUs;
	QWORD	budgetUs;
	int	samples;
	QWORD	belowSinceUs;
};

#endif	/* FRAMEDECIMATOR_H */
