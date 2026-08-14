/*
 * File:   waitqueue.h
 * Author: Sergio
 *
 * Created on 28 de septiembre de 2011, 0:20
 *
 * File d'attente bloquante (T pointeur) = la primitive Wait + une liste.
 * Toute la mécanique de synchronisation (verrou, condition, cancel collant,
 * drainage des waiters à la destruction) vit dans Wait (cf. wait.h) ; ici il
 * n'y a QUE la liste, protégée via Locked() et attendue via WaitUntil().
 *
 * Sémantique historique PRÉSERVÉE (figée par mcu/tests/test_wait_primitives.cpp) :
 *   - un Add() qui précède le Wait() n'est JAMAIS perdu (attente à prédicat
 *     « file non vide ») ;
 *   - Cancel() est collant : tout Wait ultérieur échoue, même file non vide
 *     (Pop/Peek directs restent servis), jusqu'à Reset() qui efface le cancel
 *     ET vide la file ;
 *   - Wait(0) = attente infinie ; Pop/Peek sur file vide = NULL ;
 *   - Skip() sur file vide = no-op.
 * Nuance vs l'implémentation d'origine : Wait() ne rend true que si la file
 * est réellement non vide (attente à prédicat) — un réveil sans donnée ne
 * « réussit » plus, ce qui évite aux consommateurs un Pop() NULL.
 */

#ifndef WAITQUEUE_H
#define	WAITQUEUE_H
#include "config.h"
#include "use.h"
#include "wait.h"

#include <list>

template<typename T>
class WaitQueue : public Use, protected ::Wait
{
public:
	WaitQueue() = default;

	virtual ~WaitQueue()
	{
		//Drainer les waiters TANT QUE la liste (consultée par le prédicat
		//de Wait()) est encore vivante ; le ~Wait refera à blanc.
		CancelAndDrain();
	}

	//La mécanique d'annulation est celle de Wait
	using ::Wait::Cancel;
	using ::Wait::IsCanceled;

	void Add(T obj)
	{
		Locked([&] { events.push_back(obj); });
		Signal();
	}

	//timeout en ms, 0 = infini. true si la file est non vide ;
	//false si timeout ou annulé.
	bool Wait(DWORD timeout)
	{
		return WaitUntil(timeout, [this] { return !events.empty(); });
	}

	T Peek()
	{
		return Locked([this]() -> T {
			return events.empty() ? (T)NULL : events.front();
		});
	}

	//timeout en ms : 0 = ne PAS attendre (pop non bloquant). ATTENTION, ce n'est
	//pas la convention de Wait() ci-dessus, ou 0 signifie « infini » : les neuf
	//appels historiques Pop() veulent le pop non bloquant, d'ou ce defaut.
	//
	//Rend NULL des que la file est vide au moment du retrait — expiration du
	//timeout, annulation, ou pop a vide. La garde est indispensable et pas
	//defensive : Wait() rend false sur timeout ET sur Cancel, donc le chemin
	//NORMAL passe ici file vide, et front() sur un deque vide est un
	//comportement indefini.
	T Pop(DWORD timeout = 0)
	{
		if (timeout > 0)
			Wait(timeout);

		return Locked([this]() -> T {
			if (events.empty())
				return (T)NULL;
			T val = events.front();
			events.pop_front();
			return val;
		});
	}

	void Skip()
	{
		//No-op sur file vide (l'historique faisait pop_front d'une liste vide)
		Locked([this] {
			if (!events.empty())
				events.pop_front();
		});
	}

	void Clear()
	{
		Locked([this] { events.clear(); });
	}

	void Reset()
	{
		Locked([this] { events.clear(); });
		::Wait::Reset();
	}

	DWORD Length()
	{
		return Locked([this]() -> DWORD { return events.size(); });
	}

private:
	std::list<T> events;
};

#endif	/* WAITQUEUE_H */
