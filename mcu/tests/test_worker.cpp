/**
 * test_worker.cpp — Worker (worker.h) : la classe de base des classes
 * actives (std::thread + Wait), créée pour le chantier
 * wait-primitive-unification AVANT toute conversion de site.
 *
 * Fixe le contrat : démarrage/arrêt/redémarrage, arrêt IMMÉDIAT même en
 * plein sommeil (l'annulation du Wait interrompt le tick), réveil anticipé
 * par Signal, double Start refusé, Stop sans Start inoffensif, destruction
 * pendant que le thread tourne sûre SI le destructeur dérivé appelle
 * StopThread (le contrat documenté).
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "worker.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Worker de test : compte ses passes, dort tickMs entre deux.
struct TickWorker : public Worker
{
	std::atomic<int>  ticks{0};
	std::atomic<bool> exited{false};
	DWORD tickMs;

	explicit TickWorker(DWORD tickMs = 50) : tickMs(tickMs) {}
	~TickWorker() override { StopThread(); }	// le contrat

	bool Start() { return StartThread(); }
	void Stop()  { StopThread(); }
	void Poke()  { wait.Signal(); }

	int Run() override
	{
		while (IsThreadRunning())
		{
			++ticks;
			wait.WaitSignal(tickMs);
		}
		exited = true;
		return 0;
	}
};

// Attend qu'un compteur atteigne `target` (échéance large).
static bool WaitTicks(TickWorker& w, int target, long deadlineMs = 3000)
{
	Clock::time_point t0 = Clock::now();
	while (w.ticks < target && ElapsedMs(t0) < deadlineMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	return w.ticks >= target;
}

TEST(WorkerBase, StartRunsAndStopJoins)
{
	TickWorker w(50);
	ASSERT_TRUE(w.Start());
	EXPECT_TRUE(WaitTicks(w, 2));
	w.Stop();
	EXPECT_TRUE(w.exited.load());

	// Le thread est bien arrêté : le compteur ne bouge plus.
	int frozen = w.ticks;
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	EXPECT_EQ(w.ticks.load(), frozen);
}

// L'arrêt interrompt le sommeil au lieu d'en attendre la fin.
TEST(WorkerBase, StopInterruptsLongSleep)
{
	TickWorker w(60000);	// tick d'une minute
	ASSERT_TRUE(w.Start());
	ASSERT_TRUE(WaitTicks(w, 1));

	Clock::time_point t0 = Clock::now();
	w.Stop();
	EXPECT_LT(ElapsedMs(t0), 1000);
	EXPECT_TRUE(w.exited.load());
}

TEST(WorkerBase, RestartAfterStopWorks)
{
	TickWorker w(30);
	ASSERT_TRUE(w.Start());
	ASSERT_TRUE(WaitTicks(w, 2));
	w.Stop();

	// Redémarrage : Reset du Wait interne (le Cancel est collant).
	int before = w.ticks;
	w.exited = false;
	ASSERT_TRUE(w.Start());
	EXPECT_TRUE(WaitTicks(w, before + 2));
	w.Stop();
	EXPECT_TRUE(w.exited.load());
}

TEST(WorkerBase, DoubleStartRefusedAndStopIdempotent)
{
	TickWorker w(50);
	w.Stop();			// Stop sans Start : inoffensif
	ASSERT_TRUE(w.Start());
	EXPECT_FALSE(w.Start());	// déjà lancé
	w.Stop();
	w.Stop();			// double Stop : inoffensif
}

// Signal réveille le tick sans l'arrêter (passe suivante immédiate).
TEST(WorkerBase, SignalWakesLoopEarly)
{
	TickWorker w(60000);
	ASSERT_TRUE(w.Start());
	ASSERT_TRUE(WaitTicks(w, 1));

	Clock::time_point t0 = Clock::now();
	w.Poke();
	EXPECT_TRUE(WaitTicks(w, 2, 2000));	// bien avant la minute
	EXPECT_LT(ElapsedMs(t0), 2000);
	w.Stop();
}

// Destruction pendant que le thread tourne : sûre parce que le destructeur
// dérivé applique le contrat (StopThread avant de mourir).
TEST(WorkerBase, DestructionWhileRunningIsSafe)
{
	Clock::time_point t0 = Clock::now();
	{
		TickWorker w(60000);
		ASSERT_TRUE(w.Start());
		ASSERT_TRUE(WaitTicks(w, 1));
	}	// ~TickWorker → StopThread
	EXPECT_LT(ElapsedMs(t0), 3000);
}

} // namespace
