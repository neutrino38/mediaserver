/**
 * test_endpoint_teardown.cpp — ordre de destruction des membres d'un Endpoint
 * JSR309.
 *
 * Ce que ce test fige : un Endpoint se détruit sans écrire dans un membre déjà
 * mort. Les RTPSession de ses ports pointent sur `estimator`/`estimator2` et sur
 * `eventSource` (Init → SetRemoteRateEstimator, ctor → SetEventSource), or les
 * membres se détruisent dans l'ORDRE INVERSE de leur déclaration : ces trois
 * membres DOIVENT donc être déclarés AVANT `ports`/`ports2`.
 *
 * Déclarés après, ils mouraient avant les ports et ~RTPSession appelait
 * remoteRateEstimator->RemoveListener(this) sur un estimateur détruit — un
 * erase() dans un std::set libéré, donc un tas corrompu à CHAQUE EndpointDelete
 * et un abort différé très loin de là (en production : le SSL_free du DTLS).
 *
 * ATTENTION — sans instrumentation, ce test passe MÊME avec le défaut : écrire
 * dans un objet fraîchement libéré ne se voit pas. Il n'a de valeur de garde-fou
 * que sous AddressSanitizer :
 *
 *     cd mcu && make clean && make check ASAN=yes
 *
 * (puis rebâtir sans ASAN=yes : les objets partagent le même répertoire.)
 */
#include <gtest/gtest.h>

#include "../src/jsr309/Endpoint.h"

namespace {

// Un Endpoint audio+vidéo+texte parcourt les trois ports ET les deux
// estimateurs (ports2[Video] reçoit estimator2), donc tous les liens en cause.
TEST(EndpointTeardown, UnEndpointSeDetruitSansEcrireDansUnMembreMort)
{
	Endpoint* endpoint = new Endpoint(L"cx-test",
	                                  /*audioSupported=*/true,
	                                  /*videoSupported=*/true,
	                                  /*textSupported=*/true);

	// Init est ce qui pose les liens session -> estimateur.
	ASSERT_EQ(0, endpoint->Init());

	endpoint->End();
	delete endpoint;
}

// Sans Init, aucun lien vers les estimateurs n'est posé : la destruction doit
// évidemment rester propre. Sert de témoin — si celui-ci casse aussi, le défaut
// n'est pas dans l'ordre des membres.
TEST(EndpointTeardown, UnEndpointJamaisInitSeDetruitAussi)
{
	Endpoint endpoint(L"cx-test-noinit", true, true, true);
}

} // namespace
