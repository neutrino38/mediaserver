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
 * ATTENTION : on fournit ici notre PROPRE main() plutôt que -lgtest_main. En
 * effet, la suite se lie contre libwebrtc_audio_processing.so (VAD), qui exporte
 * un symbole `main` parasite (un petit outil interne « RTP timing file »). Comme
 * gtest_main est lui aussi une bibliothèque partagée, l'éditeur de liens dynamique
 * résolvait le `main` indéfini de l'exécutable vers celui de la .so webrtc (première
 * dans l'ordre de lien) au lieu de gtest — et la suite ne lançait jamais aucun test.
 * Un main() défini DANS l'exécutable est une définition forte : il l'emporte
 * toujours. L'Environment global est enregistré via un initialiseur statique
 * (AddGlobalTestEnvironment se contente d'empiler l'objet, sûr avant InitGoogleTest).
 */
#include <gtest/gtest.h>

#include "log.h"

namespace {

// Environment global : SetUp() une fois avant tous les tests.
class McuEnvironment : public ::testing::Environment
{
public:
	void SetUp() override
	{
		// Silence les Debug() des handlers (échange WebSocket surtout).
		// Passer GTEST_MCU_DEBUG=1 dans l'environnement pour tout tracer.
		const char* dbg = getenv("GTEST_MCU_DEBUG");
		Logger::EnableDebug(dbg && dbg[0]=='1');
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

// main() propre à l'exécutable (cf. en-tête : contourne le main parasite exporté
// par libwebrtc_audio_processing.so).
int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
