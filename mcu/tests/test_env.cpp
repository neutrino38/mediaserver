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

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "log.h"
#include "hwprobe.h"

extern "C" {
#include <libavutil/log.h>
}

// libmedikit route ses Log()/Debug()/Error() par des pointeurs de fonctions
// (SetLogFunctions). SANS BRANCHEMENT ILS SONT PERDUS — erreurs comprises —, et
// la suite devient aveugle à la moitié du code qu'elle exerce : une panne du
// sous-module s'y présente comme un test rouge sans le moindre message. C'est ce
// qui a masqué l'échec de configuration du graphe VAAPI du VideoRescaler.
//
// Déclaré à la main plutôt qu'inclus : les extern "C" Log/Debug/Error de
// <medkit/log.h> entrent en conflit avec les inline d'include/log.h (même raison
// que dans main.cpp).
extern "C" void SetLogFunctions(int (*dbg)(const char*, va_list),
				int (*log)(const char*, va_list),
				int (*err)(const char*, va_list));

namespace {

// Chien de garde : un test qui se FIGE ne doit pas figer la suite.
//
// Budget PAR TEST : GTEST_MCU_WATCHDOG_S (défaut 120 s, 0 = désarmé). Aucun
// test légitime de cette suite ne s'en approche, le plus lent tenant en
// quelques secondes.
class TestWatchdog
{
public:
	// Un death test « threadsafe » relance le binaire, donc l'environnement et
	// ce thread : son exit() ne passe pas par TearDown().
	~TestWatchdog() { Stop(); }

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

// Les Log()/Error()/Debug() du mcu vont dans un fichier, pas dans la sortie de
// gtest : GTEST_MCU_LOG donne son chemin (défaut tests/test.log, « - » = stdout).
// Un death test « threadsafe » relance le binaire : l'enfant ajoute au fichier
// au lieu de l'écraser.
void OpenMcuLog(int argc, char** argv)
{
	const char* path = getenv("GTEST_MCU_LOG");
	if (!path)
		path = "tests/test.log";
	if (strcmp(path, "-") == 0)
		return;

	bool child = false;
	for (int i = 1; i < argc; ++i)
		if (strstr(argv[i], "--gtest_internal_run_death_test"))
			child = true;

	FILE* f = fopen(path, child ? "a" : "w");
	if (!f)
	{
		fprintf(stderr, "logs du mcu : impossible d'ouvrir %s (%s), ils restent sur stdout\n",
			path, strerror(errno));
		return;
	}
	Logger::SetOutput(f);
	if (!child)
		printf("logs du mcu dans %s\n", path);
}

int MedkitLogCb(const char *msg, va_list ap)
{
	fprintf(Logger::Output(), "[medkit] ");
	vfprintf(Logger::Output(), msg, ap);
	fflush(Logger::Output());
	return 1;
}

int MedkitDebugCb(const char *msg, va_list ap)
{
	if (!Logger::IsDebugEnabled())
		return 1;

	fprintf(Logger::Output(), "[medkit][DBG] ");
	vfprintf(Logger::Output(), msg, ap);
	fflush(Logger::Output());
	return 1;
}

// ffmpeg écrit sur stderr par défaut : ses messages suivent ceux du mcu.
void FfmpegLogCb(void* ptr, int level, const char* fmt, va_list ap)
{
	static int printPrefix = 1;
	char line[1024];

	if (level > AV_LOG_INFO)
		return;
	av_log_format_line(ptr, level, fmt, ap, line, sizeof(line), &printPrefix);
	Log("[ffmpeg] %s", line);
}

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

		// Les messages de libmedikit sortent avec ceux du mcu, préfixés pour qu'on
		// sache lequel des deux parle. Les Debug() suivent le même interrupteur.
		SetLogFunctions(MedkitDebugCb, MedkitLogCb, MedkitLogCb);
		av_log_set_callback(FfmpegLogCb);

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
	// Même mode que le binaire mcu : RunHwProbeChild relance /proc/self/exe,
	// qui est ici runtests.
	if (argc == 2 && strcmp(argv[1], "--hwprobe") == 0)
	{
		SetLogFunctions(MedkitDebugCb, MedkitLogCb, MedkitLogCb);
		RunHwProbe(stdout);
		return 0;
	}
	OpenMcuLog(argc, argv);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
