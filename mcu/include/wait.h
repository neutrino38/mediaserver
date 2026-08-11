/*
 * File:   wait.h
 * Author: Sergio
 *
 * Created on 27 de mayo de 2013, 16:33
 *
 * Attente signalable/annulable avec timeout, réécrite sur std::mutex /
 * std::condition_variable (chantier modernisation n°2). Sémantique historique
 * PRÉSERVÉE (figée par mcu/tests/test_wait_primitives.cpp) :
 *   - Signal() sans waiter est PERDU (pure variable de condition, aucun état) ;
 *   - Cancel() est collant : tout WaitSignal ultérieur échoue ;
 *   - WaitSignal(0) = attente infinie ; true si signalé, false si timeout/annulé.
 * Corrections vs l'implémentation pthread d'origine :
 *   - Cancel() réveille TOUS les waiters (l'ancien pthread_cond_signal n'en
 *     réveillait qu'un, les autres purgeaient leur plein timeout) ;
 *   - le destructeur annule puis DRAINE les waiters avant de libérer
 *     mutex/condition (l'ancien détruisait le mutex sous un waiter = UB).
 * Évolutions (unification du motif, cf. wait-primitive-unification) :
 *   - WaitUntil(timeout, pred) : attente sur prédicat évalué SOUS le verrou ;
 *   - Locked(f) : traitement sous le verrou de l'attente (état partagé de la
 *     classe utilisatrice), à réveiller ensuite via Signal().
 */

#ifndef WAIT_H
#define	WAIT_H

#include "config.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

class Wait
{
public:
	Wait() = default;
	Wait(const Wait&) = delete;
	Wait& operator=(const Wait&) = delete;

	virtual ~Wait()
	{
		//Annule puis draine : on ne libère mutex/condition qu'une fois le
		//dernier waiter effectivement sorti de WaitSignal/WaitUntil
		CancelAndDrain();
	}

	void Signal()
	{
		//Sous le verrou : un Signal concurrent d'une entrée en attente est
		//soit vu avant le sommeil, soit délivré au waiter — jamais entre-deux
		std::lock_guard<std::mutex> lock(mutex);
		cond.notify_one();
	}

	void Cancel()
	{
		std::lock_guard<std::mutex> lock(mutex);
		cancel = true;
		cond.notify_all();
	}

	//Réarme après un Cancel (le cancel est collant sinon)
	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		cancel = false;
	}

	bool IsCanceled()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return cancel;
	}

	//timeout en ms, 0 = infini. true si signalé (ou réveil intempestif,
	//comme l'historique) ; false si timeout ou annulé.
	bool WaitSignal(DWORD timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);

		//Déjà annulé : échec immédiat
		if (cancel)
			return false;

		WaiterScope scope(*this);

		bool ok;
		if (timeout)
			ok = (cond.wait_for(lock, std::chrono::milliseconds(timeout)) == std::cv_status::no_timeout);
		else
		{
			cond.wait(lock);
			ok = true;
		}

		return ok && !cancel;
	}

	//Attend que le prédicat (évalué SOUS le verrou) devienne vrai — l'état
	//qu'il consulte doit être muté via Locked() + Signal(). true si le
	//prédicat est satisfait ; false si timeout ou annulé.
	template <class Pred>
	bool WaitUntil(DWORD timeout, Pred pred)
	{
		std::unique_lock<std::mutex> lock(mutex);

		if (cancel)
			return false;

		WaiterScope scope(*this);

		auto stop = [&] { return cancel || pred(); };
		bool ok;
		if (timeout)
			ok = cond.wait_for(lock, std::chrono::milliseconds(timeout), stop);
		else
		{
			cond.wait(lock, stop);
			ok = true;
		}

		return ok && !cancel;
	}

	//Exécute un traitement sous le verrou de l'attente et retourne son
	//résultat. Réveiller ensuite via Signal() si l'état modifié conditionne
	//un WaitUntil en cours.
	template <class F>
	auto Locked(F f) -> decltype(f())
	{
		std::lock_guard<std::mutex> lock(mutex);
		return f();
	}

protected:
	//Annule et attend la sortie du dernier waiter. Idempotent. Une classe
	//dérivée dont les waiters consultent SES membres (prédicats WaitUntil)
	//DOIT l'appeler dans son propre destructeur, avant que ses membres ne
	//soient détruits — le ~Wait le refera à blanc.
	void CancelAndDrain()
	{
		std::unique_lock<std::mutex> lock(mutex);
		cancel = true;
		cond.notify_all();
		drained.wait(lock, [this] { return waiters == 0; });
	}

private:
	//Comptage RAII des waiters : garantit le drain du destructeur même sur
	//sortie anticipée. Construit/détruit sous le verrou de la méthode.
	struct WaiterScope
	{
		Wait& w;
		WaiterScope(Wait& w) : w(w) { ++w.waiters; }
		~WaiterScope()
		{
			if (--w.waiters == 0)
				w.drained.notify_all();
		}
	};

	std::mutex		mutex;
	std::condition_variable	cond;
	std::condition_variable	drained;
	bool			cancel	= false;
	int			waiters	= 0;
};

#endif	/* WAIT_H */
