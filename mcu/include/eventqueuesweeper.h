/*
 * eventqueuesweeper.h — expiration des objets rattachés à une file d'événements
 * dont le contrôleur ne donne plus signe de vie.
 *
 * Le long-poll d'un contrôleur sur sa file d'événements EST sa preuve de vie :
 * il est rétabli en moins d'une seconde après une coupure et le serveur y émet
 * un keep-alive toutes les 30 s. Conception et arbitrages :
 * `jsr309_session_expiry_plan.md` §7.
 *
 * DEUX signaux, un seul délai de grâce (`XmlEventQueue::DefaultExpiresSecs`,
 * réglable par `--event-queue-expires`) :
 *
 *   1. file TOUJOURS LÀ mais plus lue depuis le délai — le contrôleur est mort
 *      sans prévenir. On détruit ses objets, puis la file. Le délai de grâce a
 *      déjà couru : tant que la file existe, il suffisait de venir la lire.
 *   2. file DÉTRUITE explicitement (`EventQueueDelete`) alors que des objets la
 *      référencent encore — ce n'est PAS une destruction immédiate : le
 *      balayeur ARME le délai de grâce à la première constatation et ne détruit
 *      les objets qu'à son échéance, pour laisser au contrôleur une chance de
 *      revenir. Couvre aussi les `queueId` qui n'ont jamais désigné de file.
 *
 * Dans les deux cas, un contrôleur qui revient à temps annule le compte à
 * rebours de lui-même (reprise du long-poll, ou disparition de la référence).
 *
 * Deux services suivent ce cycle de vie — `JSR309Manager` (ses `MediaSession`)
 * et `MCU` (ses conférences) — pour des objets différents mais avec EXACTEMENT
 * la même politique. Cette classe porte la politique (cadence, seuils, ordre
 * « objets d'abord, file ensuite », armement, journalisation) ; le service ne
 * fournit que le lien objets ↔ file :
 *
 *   class MCU : ..., private EventQueueSweeper
 *   {
 *       int Init(XmlStreamingHandler *mngr,int expiresSecs)
 *       {
 *           ...                                    // hors verrou du service !
 *           StartSweeper(mngr,expiresSecs,"MCU");
 *       }
 *       int End()
 *       {
 *           StopSweeper();                         // EN PREMIER, hors verrou
 *           ...
 *       }
 *       virtual void CollectQueueIds(std::set<int>& ids);
 *       virtual int  DeleteQueueOwners(int queueId,const char *reason);
 *   };
 *
 * Contrat :
 *  - `StartSweeper` doit être appelé HORS du verrou du service (le balayeur le
 *    prend à son premier tour) ; un délai <= 0 le laisse désarmé, ce qui rend
 *    le comportement historique (rien n'est jamais détruit d'office, et
 *    `EventQueueDelete` n'a aucun effet de bord sur les objets).
 *  - `StopSweeper` doit être appelé EN PREMIER dans le `End()` du service et
 *    HORS de tout verrou : il JOINT le thread, qui peut être en train de
 *    prendre ce verrou. Le destructeur du service doit l'avoir fait (contrat
 *    `Worker` : au moment de `~Worker`, les méthodes du dérivé n'existent plus).
 *  - `CollectQueueIds` ajoute les `queueId` (> 0) que les objets du service
 *    référencent, sous SON verrou. Appelé à chaque tour : rester bon marché.
 *  - `DeleteQueueOwners` est appelé HORS de tout verrou détenu par le
 *    balayeur : le service y extrait ses entrées sous SON verrou puis appelle
 *    `End()` dessus dehors — un `End()` joint des threads qui peuvent vouloir
 *    ce même verrou. Il rend le nombre d'objets détruits (journalisation).
 */

#ifndef EVENTQUEUESWEEPER_H
#define	EVENTQUEUESWEEPER_H

#include "log.h"
#include "worker.h"
#include "xmlstreaminghandler.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <vector>

class EventQueueSweeper : protected Worker
{
public:
	//Période de balayage en exploitation. Elle descend avec le délai de grâce
	//(un petit délai serait sinon vu un tour trop tard).
	static const int MaxSweepPeriodSecs = 10;

protected:
	//`who` nomme le service dans les logs (durée de vie : littéral).
	void StartSweeper(XmlStreamingHandler *eventMngr,int expiresSecs,const char *who)
	{
		sweepWho	= who;
		sweepEventMngr	= eventMngr;
		queueExpires	= std::chrono::milliseconds(expiresSecs>0 ? expiresSecs*1000 : 0);
		sweepPeriod	= std::min(queueExpires,
					std::chrono::milliseconds(MaxSweepPeriodSecs*1000));

		if (!queueExpires.count())
		{
			Log("-%s: expiration par event queue DESACTIVEE\n",sweepWho);
			return;
		}

		Log("-%s: expiration par event queue armee [grace:%lldms,balayage:%lldms]\n",
			sweepWho,(long long)queueExpires.count(),(long long)sweepPeriod.count());

		StartThread();
	}

	void StopSweeper()
	{
		StopThread();
	}

	//Les queueId (> 0) référencés par les objets du service, sous son verrou.
	virtual void CollectQueueIds(std::set<int>& ids) = 0;

	//Détruit les objets du service rattachés à cette file. Rend leur nombre.
	virtual int DeleteQueueOwners(int queueId,const char *reason) = 0;

private:
	virtual int Run()
	{
		Log(">%s: balayeur d'expiration\n",sweepWho);

		while (IsThreadRunning())
		{
			//Tick annulable : StopSweeper réveille immédiatement
			wait.WaitSignal((DWORD)sweepPeriod.count());

			if (!IsThreadRunning())
				break;

			Sweep();
		}

		Log("<%s: balayeur d'expiration\n",sweepWho);

		return 0;
	}

	void Sweep()
	{
		if (!sweepEventMngr)
			return;

		//--- Signal 1 : files toujours là, mais que plus personne ne lit ------
		//Ne rend que des ids, donc rien ne pendouille si une file disparaît
		//d'ici sa destruction ci-dessous.
		std::vector<DWORD> idles = sweepEventMngr->GetIdleQueues(queueExpires);

		for (std::vector<DWORD>::iterator it=idles.begin(); it!=idles.end(); ++it)
		{
			int queueId = (int)*it;

			//Les objets d'abord : l'ordre garantit qu'aucun événement de fin
			//n'est publié dans une file déjà détruite
			int count = DeleteQueueOwners(queueId,"controleur absent du long-poll");

			Log("-%s: file d'evenements %d sans poller depuis plus de %llds, "
			    "destruction [objets:%d]\n",
				sweepWho,queueId,(long long)(queueExpires.count()/1000),count);

			//Puis la file elle-même (couvre aussi les files nues abandonnées)
			sweepEventMngr->DestroyEventQueue((DWORD)queueId);

			//Plus d'orphelinat en cours pour cette file
			orphans.erase(queueId);
		}

		//--- Signal 2 : objets rattachés à une file qui n'existe plus ---------
		//EventQueueDelete explicite (ou queueId jamais valide) : on ARME le
		//délai de grâce plutôt que de détruire, pour laisser au contrôleur une
		//chance de se reconnecter.
		std::set<int> ids;
		CollectQueueIds(ids);

		Clock::time_point now = Clock::now();

		for (std::set<int>::iterator it=ids.begin(); it!=ids.end(); ++it)
		{
			int queueId = *it;

			//La file est là : rien à reprocher (qu'elle soit lue ou non est
			//l'affaire du signal 1)
			if (sweepEventMngr->HasQueue((DWORD)queueId))
			{
				orphans.erase(queueId);
				continue;
			}

			Orphans::iterator oit = orphans.find(queueId);

			//Première constatation : armement
			if (oit==orphans.end())
			{
				orphans[queueId] = now;
				Log("-%s: file d'evenements %d detruite mais encore referencee, "
				    "armement du delai de grace de %llds\n",
					sweepWho,queueId,(long long)(queueExpires.count()/1000));
				continue;
			}

			//Délai écoulé : le contrôleur n'est pas revenu
			if (now - oit->second > queueExpires)
			{
				int count = DeleteQueueOwners(queueId,
						"file d'evenements detruite, delai de grace ecoule");

				Log("-%s: delai de grace ecoule pour la file d'evenements %d, "
				    "destruction [objets:%d]\n",sweepWho,queueId,count);

				orphans.erase(oit);
			}
		}

		//--- Purge : plus aucun objet ne référence cette file ----------------
		for (Orphans::iterator oit=orphans.begin(); oit!=orphans.end(); )
		{
			if (ids.find(oit->first)==ids.end())
				oit = orphans.erase(oit);
			else
				++oit;
		}
	}

private:
	typedef std::chrono::steady_clock		Clock;
	//Files détruites mais encore référencées -> date d'armement du délai
	typedef std::map<int,Clock::time_point>		Orphans;

private:
	const char		*sweepWho	= "EventQueueSweeper";
	XmlStreamingHandler	*sweepEventMngr	= NULL;
	std::chrono::milliseconds queueExpires{0};
	std::chrono::milliseconds sweepPeriod{0};
	//Accédé par le seul thread du balayeur
	Orphans			orphans;
};

#endif	/* EVENTQUEUESWEEPER_H */
