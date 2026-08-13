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
 *    Appelé DEPUIS le thread lui-même, il baisse le flag mais REFUSE de se
 *    joindre (journalisé) : un join sur soi tuerait le processus sur un abort().
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
		//Filet : le dérivé aurait dû arrêter son thread (cf. contrat). Passe par
		//StopThread() plutôt que de rejouer sa séquence : un seul chemin de join,
		//donc un seul endroit qui porte le garde-fou anti-auto-join.
		if (thread.joinable())
		{
			Error("-Worker: thread encore actif a la destruction — le destructeur derive doit appeler StopThread()\n");
			StopThread();
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
	//
	//L'arrêt est demandé AVANT toute autre considération : même appelé sans thread,
	//ou depuis le thread lui-même, `running` retombe et `wait` est annulé — donc la
	//boucle de Run() sortira. Seul le join est conditionnel.
	void StopThread()
	{
		running = false;
		wait.Cancel();

		//Un join() sur son PROPRE thread lève std::system_error (EDEADLK). Rien ne
		//le rattrape ici, donc le processus meurt sur un abort() dont la trace ne
		//nomme pas le coupable. Et le chemin existe : Stop() est appelé depuis des
		//callbacks de Joinable::Listener (onEndStream, par exemple), qui s'exécutent
		//sur le thread de la SOURCE — et une source peut être alimentée par notre
		//propre boucle (un port de mixeur nourri par output->PlayBuffer()).
		//
		//Refuser et le dire est strictement meilleur que tomber : le drapeau est
		//déjà baissé, la boucle sort d'elle-même, et le thread reste joignable — le
		//prochain StartThread() le verra et refusera, ce qui laisse dans le journal
		//les deux lignes qui racontent l'histoire au lieu d'un core sans contexte.
		if (thread.get_id() == std::this_thread::get_id())
		{
			Error("-Worker: StopThread() appele depuis le thread lui-meme ; join() ignore\n");
			return;
		}

		if (thread.joinable())
			thread.join();
	}

	//Corps du thread. Boucler sur IsThreadRunning() et dormir via `wait`.
	virtual int Run() = 0;

	//Flag d'arrêt, à consulter dans la boucle de Run().
	inline bool IsThreadRunning() const { return running; }

	//Attente annulable par StopThread, réveillable par wait.Signal() ;
	//état partagé sous wait.Locked(), prédicats via wait.WaitUntil().
	::Wait wait;

private:
	std::thread		thread;
	std::atomic<bool>	running{false};
};

#endif	/* WORKER_H */
