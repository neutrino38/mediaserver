/*
 * worker.h — classe de base des « classes actives » du mcu.
 *
 * Le motif « membre pthread_t + trampoline statique run(void*) →
 * pthread_exit(Run()) + flag booléen d'arrêt + join dans End/Stop » est
 * recopié dans ~27 classes du serveur (mixeurs, smoothers, encodeurs,
 * participants, serveurs). Worker le factorise sur std::thread + la
 * primitive Wait (wait.h) :
 *
 *   class TextMixer : public Worker
 *   {
 *       int Init() { return StartThread(); }
 *       int End()  { StopThread(); ... }
 *       int Run() override
 *       {
 *           while (IsThreadRunning())
 *           {
 *               ... une passe de travail ...
 *               wait.WaitSignal(200);   // tick annulable par StopThread
 *           }
 *           return 0;
 *       }
 *   };
 *
 * Contrat :
 *  - StartThread() lance le thread (au plus un) et réarme `wait` — un
 *    redémarrage après StopThread() fonctionne. false si déjà lancé.
 *  - StopThread() lève le flag, annule `wait` (réveil immédiat de tout
 *    WaitSignal/WaitUntil en cours) et JOINT le thread. Idempotent.
 *  - Run() boucle sur IsThreadRunning() et dort via `wait` — jamais de
 *    msleep/poll non réveillable.
 *  - Le destructeur DÉRIVÉ doit appeler StopThread() (ou avoir arrêté le
 *    thread) : au moment où ~Worker s'exécute, le Run() dérivé n'existe
 *    déjà plus — le filet de ~Worker joint le thread mais ne peut pas
 *    empêcher l'UB si Run() tourne encore à cet instant.
 *
 * NB : createPriorityThread n'appliquait AUCUNE priorité (le
 * pthread_setschedparam est commenté depuis toujours) — Worker n'en
 * propose donc pas.
 *
 * Conçu pour le chantier wait-primitive-unification ; les sites candidats
 * sont inventoriés dans la fiche mémoire du chantier. AUCUN site n'est
 * encore converti (décision mainteneur 2026-08-11 : classe + tests d'abord).
 */

#ifndef WORKER_H
#define	WORKER_H

#include "config.h"
#include "log.h"
#include "wait.h"

#include <atomic>
#include <thread>

class Worker
{
public:
	virtual ~Worker()
	{
		//Filet : le dérivé aurait dû arrêter son thread (cf. contrat).
		if (thread.joinable())
		{
			Error("-Worker: thread encore actif a la destruction — le destructeur derive doit appeler StopThread()\n");
			running = false;
			wait.Cancel();
			thread.join();
		}
	}

protected:
	Worker() = default;
	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;

	//Démarre le thread (au plus un). Réarme `wait` : un redémarrage après
	//StopThread fonctionne. false si le thread est déjà lancé (appeler
	//StopThread d'abord, même si Run s'est terminé de lui-même).
	bool StartThread()
	{
		if (thread.joinable())
			return false;
		wait.Reset();
		running = true;
		thread = std::thread([this] { Run(); });
		return true;
	}

	//Demande l'arrêt (flag + réveil de `wait`) et joint le thread.
	//Idempotent, sans effet si le thread ne tourne pas.
	void StopThread()
	{
		if (!thread.joinable())
			return;
		running = false;
		wait.Cancel();
		thread.join();
	}

	//Corps du thread. Boucler sur IsThreadRunning() et dormir via `wait`.
	virtual int Run() = 0;

	//Flag d'arrêt, à consulter dans la boucle de Run().
	bool IsThreadRunning() const { return running; }

	//Attente annulable par StopThread, réveillable par wait.Signal() ;
	//état partagé sous wait.Locked(), prédicats via wait.WaitUntil().
	::Wait wait;

private:
	std::thread		thread;
	std::atomic<bool>	running{false};
};

#endif	/* WORKER_H */
