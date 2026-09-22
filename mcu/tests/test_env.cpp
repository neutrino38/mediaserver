/**
 * test_env.cpp — infrastructure commune de la suite gtest du mediaserver (mcu).
 *
 * Contrairement à libmedikit (qui exige SetLogFunctions() sous peine de segfault),
 * le mcu fournit ses propres Log()/Debug()/Error() *inline* et autonomes
 * (mcu/include/log.h) : aucun câblage de callback n'est nécessaire ici. On se
 * contente donc de désactiver le mode debug par défaut (les handlers WebSocket
 * appellent Debug() abondamment) pour garder la sortie des tests lisible, et
 * d'exposer un smoke test qui valide la chaîne compile/link/exécution
 * gtest ↔ objets mcu.
 *
 * On fournit ici notre PROPRE main() plutôt que -lgtest_main. L'Environment global
 * est enregistré via un initialiseur statique (AddGlobalTestEnvironment se contente
 * d'empiler l'objet, sûr avant InitGoogleTest).
 */
#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "log.h"

namespace {

// Chien de garde : un test qui se FIGE ne doit pas figer la suite.
//
// Mesurer la durée d'un arrêt ne protège que d'un arrêt lent. Quand une course
// de démontage se déclenche — le pont texte WebSocket l'a fait le 2026-09-22,
// et le démontage RTMP est connu pour bloquer une fois sur dix —, un thread
// tourne, le join ne rend jamais la main, et `make check` attend sans fin sans
// même dire où. Ce chien de garde nomme le test et tue le processus : un échec
// se lit, un gel ne se lit pas.
//
// Budget PAR TEST : GTEST_MCU_WATCHDOG_S (défaut 120 s, 0 = désarmé). Aucun
// test légitime de cette suite ne s'en approche, le plus lent tenant en
// quelques secondes.
class TestWatchdog
{
public:
	void Start()
	{
		const char* s = getenv("GTEST_MCU_WATCHDOG_S");
		budgetSecs = s ? atoi(s) : 120;
		if (budgetSecs <= 0)
			return;
		running = true;
		thread = std::thread([this] { Run(); });
	}

	void Stop()
	{
		if (!running)
			return;
		running = false;
		thread.join();
	}

private:
	void Run()
	{
		std::string current;
		int secs = 0;

		while (running)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));

			const ::testing::TestInfo* info =
				::testing::UnitTest::GetInstance()->current_test_info();
			const std::string name = info
				? std::string(info->test_suite_name()) + "." + info->name()
				: std::string();

			//Un autre test : on repart de zéro
			if (name != current)
			{
				current = name;
				secs = 0;
				continue;
			}
			if (current.empty() || ++secs < budgetSecs)
				continue;

			fprintf(stderr,
				"\n[CHIEN DE GARDE] %s tourne depuis %d s : la suite est figee.\n"
				"[CHIEN DE GARDE] abandon du processus (voir GTEST_MCU_WATCHDOG_S).\n",
				current.c_str(), secs);
			fflush(stderr);
			abort();
		}
	}

	std::thread thread;
	std::atomic<bool> running{false};
	int budgetSecs = 0;
};

TestWatchdog g_watchdog;

// Environment global : SetUp() une fois avant tous les tests.
class McuEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		g_watchdog.Start();

		// Silence les Debug() des handlers (échange WebSocket surtout).
		// Passer GTEST_MCU_DEBUG=1 dans l'environnement pour tout tracer.
		const char* dbg = getenv("GTEST_MCU_DEBUG");
		Logger::EnableDebug(dbg && dbg[0]=='1');

		// Comme main.cpp : écrire dans un socket que le pair a fermé est un cas
		// NORMAL de fin de connexion. Sans cette ligne, le SIGPIPE par défaut tue
		// le binaire de test là où le serveur, lui, continue.
		signal(SIGPIPE, SIG_IGN);
	}

	void TearDown() override
	{
		g_watchdog.Stop();
	}
};

::testing::Environment* const g_mcuEnv =
	::testing::AddGlobalTestEnvironment(new McuEnvironment);

} // namespace

// --- Smoke test : valide compile + link + exécution gtest contre les objets mcu.
TEST(Smoke, LogFunctionsWork)
{
	// Si on arrive ici, le binaire de test a démarré : un Log() ne doit pas crasher.
	Log("[smoke] mcu gtest harness up\n");
	SUCCEED();
}

// main() propre à l'exécutable (cf. en-tête).
int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
