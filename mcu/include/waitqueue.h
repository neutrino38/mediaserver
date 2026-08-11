/*
 * File:   waitqueue.h
 * Author: Sergio
 *
 * Created on 28 de septiembre de 2011, 0:20
 *
 * File d'attente bloquante (T pointeur), réécrite sur std::mutex /
 * std::condition_variable (chantier modernisation n°2). Sémantique historique
 * PRÉSERVÉE (figée par mcu/tests/test_wait_primitives.cpp) :
 *   - un Add() qui précède le Wait() n'est JAMAIS perdu (la file est re-testée
 *     avant de dormir) ;
 *   - Cancel() est collant : tout Wait ultérieur échoue, même file non vide
 *     (Pop/Peek directs restent servis), jusqu'à Reset() qui efface le cancel
 *     ET vide la file ;
 *   - Wait(0) = attente infinie ; Pop/Peek sur file vide = NULL.
 * Corrections vs l'implémentation pthread d'origine :
 *   - Cancel() réveille TOUS les waiters (pthread_cond_signal n'en réveillait
 *     qu'un) ;
 *   - le destructeur annule puis DRAINE les waiters avant de libérer
 *     mutex/condition (l'ancien les détruisait sous un waiter = UB) ;
 *   - Skip() sur file vide est un no-op (l'ancien pop_front() = UB).
 */

#ifndef WAITQUEUE_H
#define	WAITQUEUE_H
#include "config.h"
#include "use.h"

#include <chrono>
#include <condition_variable>
#include <list>
#include <mutex>

template<typename T>
class WaitQueue : public Use
{
public:
	WaitQueue() = default;
	WaitQueue(const WaitQueue&) = delete;
	WaitQueue& operator=(const WaitQueue&) = delete;

	virtual ~WaitQueue()
	{
		//Annule puis draine les waiters avant de libérer mutex/condition
		std::unique_lock<std::mutex> lock(mutex);
		cancel = true;
		cond.notify_all();
		drained.wait(lock, [this] { return waiters == 0; });
	}

	void Add(T obj)
	{
		std::lock_guard<std::mutex> lock(mutex);
		events.push_back(obj);
		cond.notify_one();
	}

	void Cancel()
	{
		std::lock_guard<std::mutex> lock(mutex);
		cancel = true;
		cond.notify_all();
	}

	//timeout en ms, 0 = infini. true si la file a (eu) quelque chose ou si
	//signalé ; false si timeout ou annulé.
	bool Wait(DWORD timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);

		//Déjà annulé : échec immédiat
		if (cancel)
			return false;

		//Un Add antérieur n'est jamais perdu
		if (!events.empty())
			return true;

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

	T Peek()
	{
		T val = NULL;
		std::lock_guard<std::mutex> lock(mutex);
		if (!events.empty())
			val = events.front();
		return val;
	}

	T Pop()
	{
		T val = NULL;
		std::lock_guard<std::mutex> lock(mutex);
		if (!events.empty())
		{
			val = events.front();
			events.pop_front();
		}
		return val;
	}

	void Skip()
	{
		std::lock_guard<std::mutex> lock(mutex);
		//No-op sur file vide (l'historique faisait pop_front d'une liste vide)
		if (!events.empty())
			events.pop_front();
	}

	void Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		events.clear();
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		events.clear();
		cancel = false;
	}

	DWORD Length()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return events.size();
	}

	bool IsCanceled()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return cancel;
	}

private:
	//Comptage RAII des waiters : garantit le drain du destructeur même sur
	//sortie anticipée. Construit/détruit sous le verrou de la méthode.
	struct WaiterScope
	{
		WaitQueue& q;
		WaiterScope(WaitQueue& q) : q(q) { ++q.waiters; }
		~WaiterScope()
		{
			if (--q.waiters == 0)
				q.drained.notify_all();
		}
	};

	typedef std::list<T> ObjectList;

	ObjectList		events;
	bool			cancel	= false;
	int			waiters	= 0;
	std::mutex		mutex;
	std::condition_variable	cond;
	std::condition_variable	drained;
};

#endif	/* WAITQUEUE_H */
