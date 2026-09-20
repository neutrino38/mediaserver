#ifndef _USE_H_
#define _USE_H_
#include "tools.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

/*
 * Compteur d'usage lecteurs/écrivain historique du mcu, réécrit sur
 * std::mutex/std::condition_variable (dernier îlot pthread de la
 * synchronisation). Sémantique historique PRÉSERVÉE, figée par
 * mcu/tests/test_use.cpp :
 *  - IncUse RÉENTRANT (simple compteur, tout thread, plusieurs fois) ;
 *  - PAS de priorité écrivain : les IncUse passent PENDANT qu'un
 *    WaitUnusedAndLock attend — seule la section effectivement tenue les
 *    bloque (c'est pour ces deux points que std::shared_mutex est
 *    disqualifié : réentrance lecteur indéfinie, préférence écrivain) ;
 *  - UNE SEULE exception à la ligne précédente, le PASSAGE DE TÉMOIN : quand le
 *    compteur tombe à zéro alors qu'un écrivain attend, les nouveaux IncUse
 *    patientent le temps que cet écrivain prenne la main. Sans lui, un écrivain
 *    réveillé depuis un futex perd la course contre un lecteur qui reprend sa
 *    lecture dans la microseconde suivante, et il la perd à TOUS les tours :
 *    c'est ainsi qu'un thread réacteur RTP est resté 34 s dans
 *    RTPSession::ChangeStream (appel du 2026-09-18). La réentrance n'en souffre
 *    pas — le témoin n'existe qu'à compteur NUL, donc quand aucun thread ne
 *    tient de lecture, et un IncUse imbriqué trouve toujours le compteur > 0 ;
 *  - écrivains sérialisés entre eux par le second mutex `lock` ;
 *  - WaitUnusedAndLock RETOURNE EN TENANT mutex+lock (relâchés par Unlock,
 *    depuis le MÊME thread) — d'où le unique_lock::release() ;
 *  - variante timée : 1 = verrouillé, 0 = timeout (tout est relâché).
 *    L'ancien -1 (erreur système pthread) disparaît : les appelants testent
 *    « != 1 » ou la vérité, 0 les couvre.
 */
class Use
{
public:
	Use() = default;
	~Use() = default;

	void IncUse()
	{
		std::unique_lock<std::mutex> guard(mutex);
		//Passage de témoin (cf. en-tête) : un écrivain vient d'être réveillé et
		//n'a pas encore pris la main. Ne pas lui passer devant.
		handoffCond.wait(guard, [this] { return !handoff; });
		cont ++;
	}

	void DecUse()
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (cont > 0) cont --;
		//Dernier lecteur sorti alors qu'un écrivain attend : le témoin lui est
		//réservé jusqu'à ce qu'il prenne la main.
		if (cont == 0 && writers > 0)
			handoff = true;
		cond.notify_one();
	}

	bool WaitUnusedAndLock()
	{
		lock.lock();
		std::unique_lock<std::mutex> guard(mutex);
		writers ++;
		cond.wait(guard, [this] { return cont == 0; });
		writers --;
		//Témoin consommé : à partir d'ici c'est `mutex`, tenu jusqu'à Unlock,
		//qui écarte les lecteurs.
		handoff = false;
		//Rester verrouillé au retour : Unlock() relâchera
		guard.release();
		return true;
	}

        /**
         * Wait during x ms
         * @param timeout timeout to wait in ms (0 = infini)
         * @return 1 = Ok, locked ; 0 = timeout (tout est relâché)
         **/
        int WaitUnusedAndLock(DWORD timeout)
	{
		lock.lock();
		std::unique_lock<std::mutex> guard(mutex);
		writers ++;

		if (timeout)
		{
			if (!cond.wait_for(guard, std::chrono::milliseconds(timeout),
					[this] { return cont == 0; }))
			{
				writers --;
				//Témoin rendu : l'écrivain qui l'avait réservé abandonne, les
				//lecteurs mis en attente pour lui n'ont plus personne à attendre.
				handoff = false;
				//Timeout : tout relâcher (le guard relâche mutex)
				guard.unlock();
				handoffCond.notify_all();
				lock.unlock();
				return 0;
			}
		}
		else
			cond.wait(guard, [this] { return cont == 0; });

		writers --;
		handoff = false;
		//Rester verrouillé au retour : Unlock() relâchera
		guard.release();
		return 1;
	}

	void Unlock()
	{
		mutex.unlock();
		lock.unlock();
		//Les lecteurs arrêtés par le témoin attendent sur cette variable, pas
		//sur `mutex` : sans ce réveil ils dorment jusqu'au prochain DecUse.
		handoffCond.notify_all();
	}

private:
	std::mutex		mutex;
	//Sérialise les écrivains entre eux (tenu de WaitUnusedAndLock à Unlock)
	std::mutex		lock;
	std::condition_variable	cond;
	//Lecteurs retenus le temps qu'un écrivain réveillé prenne la main
	std::condition_variable	handoffCond;
	int			cont = 0;
	//Écrivains en attente (0 ou 1 : `lock` les sérialise)
	int			writers = 0;
	bool			handoff = false;
};

/**
 * Garde RAII sur le côté LECTEUR d'un Use (IncUse/DecUse).
 *
 * Rendre la main sans DecUse laisse le compteur à jamais non nul : tout
 * WaitUnusedAndLock ultérieur attend pour toujours. Un `return` anticipé dans une
 * fonction gardée suffit à le faire, et il n'y a aucune trace pour le dire — d'où
 * ce garde plutôt que des paires posées à la main.
 *
 * Lecteur seulement : le côté écrivain (WaitUnusedAndLock/Unlock) rend la main EN
 * TENANT deux mutex, avec une sémantique de transfert qu'un garde de portée
 * décrirait mal.
 */
class ScopedUse
{
public:
	explicit ScopedUse(Use& use) : use(use)	{ use.IncUse(); }
	~ScopedUse()				{ use.DecUse(); }

	ScopedUse(const ScopedUse&) = delete;
	ScopedUse& operator=(const ScopedUse&) = delete;

private:
	Use&	use;
};

#endif
