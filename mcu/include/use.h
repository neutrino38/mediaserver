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
		std::lock_guard<std::mutex> guard(mutex);
		cont ++;
	}

	void DecUse()
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (cont > 0) cont --;
		cond.notify_one();
	}

	bool WaitUnusedAndLock()
	{
		lock.lock();
		std::unique_lock<std::mutex> guard(mutex);
		cond.wait(guard, [this] { return cont == 0; });
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

		if (timeout)
		{
			if (!cond.wait_for(guard, std::chrono::milliseconds(timeout),
					[this] { return cont == 0; }))
			{
				//Timeout : tout relâcher (le guard relâche mutex)
				guard.unlock();
				lock.unlock();
				return 0;
			}
		}
		else
			cond.wait(guard, [this] { return cont == 0; });

		//Rester verrouillé au retour : Unlock() relâchera
		guard.release();
		return 1;
	}

	void Unlock()
	{
		mutex.unlock();
		lock.unlock();
	}

private:
	std::mutex		mutex;
	//Sérialise les écrivains entre eux (tenu de WaitUnusedAndLock à Unlock)
	std::mutex		lock;
	std::condition_variable	cond;
	int			cont = 0;
};

#endif
