/**
 * test_use.cpp — caractérisation de Use (use.h), le compteur d'usage
 * lecteurs/écrivain historique du mcu, AVANT sa migration vers
 * std::mutex/std::condition_variable (dernier îlot pthread de la
 * synchronisation).
 *
 * Sémantiques à préserver STRICTEMENT (c'est pour elles que std::shared_mutex
 * est disqualifié — réentrance lecteur indéfinie et préférence écrivain) :
 *   - IncUse est RÉENTRANT : un même thread peut compter plusieurs fois ;
 *   - PAS de priorité écrivain : un IncUse passe PENDANT qu'un
 *     WaitUnusedAndLock attend (seule la section tenue le bloque) ;
 *   - sauf le PASSAGE DE TÉMOIN : à l'instant où le compteur tombe à zéro pour
 *     un écrivain qui attend, les nouveaux IncUse patientent jusqu'à ce qu'il
 *     ait pris la main. Sans lui, une boucle de lecture serrée affame
 *     l'écrivain indéfiniment ;
 *   - une fois WaitUnusedAndLock rendu, les IncUse bloquent jusqu'à Unlock ;
 *   - les écrivains sont sérialisés entre eux (second mutex) ;
 *   - WaitUnusedAndLock(ms) rend 1 = verrouillé, 0 = timeout (tout est
 *     relâché : un lecteur repasse aussitôt) ; 0 ms = attente infinie ;
 *   - Unlock est appelé par le thread qui a obtenu le verrou.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "use.h"

namespace {

typedef std::chrono::steady_clock Clock;

static long ElapsedMs(const Clock::time_point& t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// IncUse réentrant : deux IncUse du même thread, l'écrivain n'obtient le
// verrou qu'après les DEUX DecUse.
TEST(UsePrimitive, IncUseIsReentrant)
{
	Use use;
	use.IncUse();
	use.IncUse();

	std::atomic<long> acquiredAt{-1};
	Clock::time_point t0 = Clock::now();
	std::thread writer([&]() {
		use.WaitUnusedAndLock();
		acquiredAt = ElapsedMs(t0);
		use.Unlock();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	EXPECT_EQ(acquiredAt.load(), -1);	// toujours en attente (cont=2)
	use.DecUse();
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	EXPECT_EQ(acquiredAt.load(), -1);	// toujours en attente (cont=1)
	use.DecUse();
	writer.join();
	EXPECT_GE(acquiredAt.load(), 100);	// obtenu après le 2e DecUse
}

// PAS de priorité écrivain : pendant qu'un écrivain ATTEND, un nouveau
// lecteur passe sans bloquer.
TEST(UsePrimitive, ReadersPassWhileWriterWaits)
{
	Use use;
	use.IncUse();	// lecteur initial : l'écrivain va attendre

	std::atomic<bool> locked{false};
	std::thread writer([&]() {
		use.WaitUnusedAndLock();
		locked = true;
		use.Unlock();
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	ASSERT_FALSE(locked.load());	// l'écrivain attend bien

	//Un NOUVEAU lecteur passe immédiatement (sémantique historique)
	Clock::time_point t0 = Clock::now();
	use.IncUse();
	EXPECT_LT(ElapsedMs(t0), 100);

	//Libérer les deux lecteurs : l'écrivain obtient enfin le verrou
	use.DecUse();
	use.DecUse();
	writer.join();
	EXPECT_TRUE(locked.load());
}

// Une fois le verrou TENU, les lecteurs bloquent jusqu'à Unlock.
TEST(UsePrimitive, HeldLockBlocksReaders)
{
	Use use;
	ASSERT_TRUE(use.WaitUnusedAndLock());	// rien d'utilisé : immédiat

	std::atomic<long> readerAt{-1};
	Clock::time_point t0 = Clock::now();
	std::thread reader([&]() {
		use.IncUse();
		readerAt = ElapsedMs(t0);
		use.DecUse();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	EXPECT_EQ(readerAt.load(), -1);	// bloqué tant que le verrou est tenu
	use.Unlock();
	reader.join();
	EXPECT_GE(readerAt.load(), 100);
}

// Deux écrivains : sérialisés (le second n'entre qu'après l'Unlock du premier).
TEST(UsePrimitive, WritersAreSerialized)
{
	Use use;
	ASSERT_TRUE(use.WaitUnusedAndLock());

	std::atomic<long> secondAt{-1};
	Clock::time_point t0 = Clock::now();
	std::thread writer2([&]() {
		use.WaitUnusedAndLock();
		secondAt = ElapsedMs(t0);
		use.Unlock();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	EXPECT_EQ(secondAt.load(), -1);
	use.Unlock();
	writer2.join();
	EXPECT_GE(secondAt.load(), 100);
}

// Variante à timeout : 0 = timeout, tout est relâché (un lecteur repasse) ;
// 1 = verrouillé quand l'usage retombe à temps.
TEST(UsePrimitive, TimedVariantTimesOutAndReleases)
{
	Use use;
	use.IncUse();

	Clock::time_point t0 = Clock::now();
	EXPECT_EQ(use.WaitUnusedAndLock(150), 0);	// timeout
	long ms = ElapsedMs(t0);
	EXPECT_GE(ms, 100);
	EXPECT_LT(ms, 5000);

	//Tout est relâché après le timeout : un lecteur passe sans bloquer
	Clock::time_point t1 = Clock::now();
	use.IncUse();
	EXPECT_LT(ElapsedMs(t1), 100);
	use.DecUse();
	use.DecUse();

	//Et quand l'usage retombe, la variante timée verrouille (retour 1)
	EXPECT_EQ(use.WaitUnusedAndLock(1000), 1);
	use.Unlock();
}

// NON-RÉGRESSION : un écrivain ne doit pas être affamé par un lecteur qui
// reprend sa lecture aussitôt relâchée. C'est la forme exacte de
// RTPEndpoint::MultiplexLoop — IncUse, attente bornée, DecUse, et on recommence
// — contre RTPSession::ChangeStream. Sans le passage de témoin, l'écrivain,
// réveillé depuis un futex, perd la course à chaque tour : mesuré 34 s sur un
// thread réacteur RTP le 2026-09-18, jusqu'au raccrochage.
TEST(UsePrimitive, WriterIsNotStarvedByATightReaderLoop)
{
	Use use;
	std::atomic<bool> stop{false};
	std::atomic<bool> inside{false};
	std::mutex	  m;
	std::condition_variable cv;

	//UN lecteur, comme une session RTP n'a qu'un consommateur : prise du
	//compteur, attente bornée, relâche, et on recommence sans respirer.
	std::thread reader([&]() {
		while (!stop)
		{
			use.IncUse();
			inside = true;
			{
				std::unique_lock<std::mutex> l(m);
				cv.wait_for(l, std::chrono::milliseconds(20));
			}
			use.DecUse();
		}
	});

	int  starved = 0;
	long worst   = 0;

	for (int round = 0; round < 20; ++round)
	{
		//L'écrivain doit arriver PENDANT une lecture : c'est le seul cas où il
		//doit gagner une course, et le seul où l'ancienne version le perdait.
		inside = false;
		while (!inside)
			std::this_thread::yield();

		Clock::time_point t0 = Clock::now();
		int  ret = use.WaitUnusedAndLock(2000);
		long ms  = ElapsedMs(t0);

		if (ms > worst)
			worst = ms;
		if (ret != 1 || ms >= 1000)
			++starved;
		if (ret)
			use.Unlock();
	}

	stop = true;
	cv.notify_all();
	reader.join();

	EXPECT_EQ(starved, 0) << "écrivain affamé, pire attente " << worst << " ms";
}

// Le DecUse pendant l'attente timée débloque avant l'échéance.
TEST(UsePrimitive, TimedVariantAcquiresWhenFreed)
{
	Use use;
	use.IncUse();
	std::thread releaser([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		use.DecUse();
	});
	Clock::time_point t0 = Clock::now();
	EXPECT_EQ(use.WaitUnusedAndLock(5000), 1);
	EXPECT_LT(ElapsedMs(t0), 4000);
	use.Unlock();
	releaser.join();
}

// Tempête lecteurs/écrivains : jamais de lecteur dans la section exclusive.
TEST(UsePrimitive, StressReadersVsWriters)
{
	Use use;
	std::atomic<int>  readers{0};
	std::atomic<bool> stop{false};
	std::atomic<int>  violations{0};

	std::thread readerTh[3];
	for (int i = 0; i < 3; ++i)
		readerTh[i] = std::thread([&]() {
			while (!stop)
			{
				use.IncUse();
				++readers;
				std::this_thread::yield();
				--readers;
				use.DecUse();
				//Respiration : garde la durée du test prévisible avec trois
				//lecteurs et trente prises de verrou. La boucle SANS
				//respiration est couverte par
				//WriterIsNotStarvedByATightReaderLoop.
				std::this_thread::sleep_for(std::chrono::microseconds(500));
			}
		});

	for (int k = 0; k < 30; ++k)
	{
		use.WaitUnusedAndLock();
		if (readers.load() != 0)
			++violations;
		std::this_thread::yield();
		if (readers.load() != 0)
			++violations;
		use.Unlock();
	}
	stop = true;
	for (int i = 0; i < 3; ++i)
		readerTh[i].join();
	EXPECT_EQ(violations.load(), 0);
}

// ScopedUse : le DecUse doit partir à la sortie de portée, y compris par un
// retour anticipé. C'est tout l'intérêt du garde — un DecUse manqué laisse le
// compteur non nul et fait attendre POUR TOUJOURS le prochain écrivain.
TEST(UsePrimitive, ScopedUseReleasesOnEarlyReturn)
{
	Use use;

	// Fonction qui rend la main au milieu de la portée gardée.
	auto readerWithEarlyReturn = [&use](bool bailOut) {
		ScopedUse scoped(use);
		if (bailOut)
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	};

	readerWithEarlyReturn(true);
	readerWithEarlyReturn(false);

	// Si un seul DecUse manquait, ce WaitUnusedAndLock n'aboutirait jamais :
	// la variante timée transforme l'interblocage en échec lisible.
	EXPECT_EQ(1, use.WaitUnusedAndLock(1000)) << "un DecUse a été perdu";
	use.Unlock();
}

// Le garde tient bien le verrou pendant sa portée : un écrivain doit attendre.
TEST(UsePrimitive, ScopedUseBlocksWriterWhileAlive)
{
	Use use;
	std::atomic<bool> locked{false};
	std::thread writer;

	{
		ScopedUse scoped(use);

		writer = std::thread([&]() {
			use.WaitUnusedAndLock();
			locked = true;
			use.Unlock();
		});

		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		EXPECT_FALSE(locked.load()) << "l'écrivain est passé pendant la portée gardée";
	}
	// Sortie de portée : le DecUse est parti, l'écrivain doit entrer.

	writer.join();
	EXPECT_TRUE(locked.load());
}

} // namespace
