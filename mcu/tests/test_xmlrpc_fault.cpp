/**
 * test_xmlrpc_fault.cpp — une requête XML-RPC mal typée ne doit pas tuer le
 * serveur.
 *
 * Ce que ces tests fixent, et pourquoi ils existent : `xmlrpc_build_value`
 * ASSÈRE et `abort()` le processus quand l'`xmlrpc_env` qu'on lui donne porte
 * déjà une faute. Or c'est l'état normal des handlers après un
 * `xmlrpc_parse_value` en échec — le motif
 * `if(env->fault_occurred) return xmlerror(env,"Fault occurred")` apparaît des
 * dizaines de fois dans `xmlrpcmcu.cpp` et `xmlrpcjsr309.cpp`. Un seul appel mal
 * typé, depuis n'importe quel contrôleur, faisait donc tomber le mediaserver
 * (constaté en recette le 2026-09-02 : `../src/xmlrpc_build.c:355: assertion
 * failed`, SIGABRT).
 *
 * Ces tests ne « vérifient » pas une valeur de retour : sur le code fautif ils
 * n'échouent pas, ils EMPORTENT le binaire de test. C'est la forme la plus nette
 * de rouge qu'un défaut d'abort puisse produire.
 */
#include <gtest/gtest.h>

#include "xmlhandler.h"

namespace {

// Met l'env dans l'état exact où le laisse un handler dont le parse a échoué :
// un paramètre de type struct là où le format attend un entier.
class FaultedEnv
{
public:
	FaultedEnv()
	{
		xmlrpc_env_init(&env);

		xmlrpc_value* bad   = xmlrpc_build_value(&env, "{s:s}", "pas", "un entier");
		xmlrpc_value* array = xmlrpc_build_value(&env, "(Vsiii)", bad, "sonde", 0, 0, 0);

		int a, c, d, e;
		char* b;
		xmlrpc_parse_value(&env, array, "(isiii)", &a, &b, &c, &d, &e);

		xmlrpc_DECREF(bad);
		xmlrpc_DECREF(array);
	}

	~FaultedEnv() { xmlrpc_env_clean(&env); }

	xmlrpc_env env;
};

// Lit `returnCode` d'une réponse, ou -1 si elle n'en porte pas.
static int ReturnCode(xmlrpc_env* env, xmlrpc_value* response)
{
	if (!response)
		return -1;

	xmlrpc_value* code = NULL;
	xmlrpc_struct_find_value(env, response, "returnCode", &code);
	if (!code)
		return -1;

	int value = -1;
	xmlrpc_parse_value(env, code, "i", &value);
	xmlrpc_DECREF(code);
	return value;
}

TEST(XmlRpcFault, TheEnvIsReallyFaultedByAMistypedParameter)
{
	FaultedEnv faulted;

	ASSERT_TRUE(faulted.env.fault_occurred)
		<< "sans faute, les deux tests suivants ne prouveraient rien";
}

// LE test du défaut : sur le code d'avant, cette ligne abort() le binaire.
TEST(XmlRpcFault, AnErrorResponseIsBuiltInsteadOfAborting)
{
	FaultedEnv faulted;

	xmlrpc_value* response = xmlerror(&faulted.env, "Fault occurred");

	ASSERT_NE(response, nullptr);
	EXPECT_FALSE(faulted.env.fault_occurred) << "la faute doit etre soldee";
	EXPECT_EQ(ReturnCode(&faulted.env, response), 0);
	xmlrpc_DECREF(response);
}

// Même piège par l'autre porte : un handler qui n'a pas testé la faute (le cas
// de xmlparsemap) arrive sur le chemin de SUCCÈS. Répondre « returnCode 1 »
// serait mentir, puisqu'il n'a pas lu ce qu'il croit avoir lu.
TEST(XmlRpcFault, ASuccessResponseOnAFaultedEnvAnswersAnError)
{
	FaultedEnv faulted;

	xmlrpc_value* response = xmlok(&faulted.env);

	ASSERT_NE(response, nullptr);
	EXPECT_EQ(ReturnCode(&faulted.env, response), 0) << "surtout pas 1";
	xmlrpc_DECREF(response);
}

// Une réponse normale reste une réponse normale.
TEST(XmlRpcFault, ACleanEnvStillAnswersSuccess)
{
	xmlrpc_env env;
	xmlrpc_env_init(&env);

	xmlrpc_value* response = xmlok(&env);

	ASSERT_NE(response, nullptr);
	EXPECT_EQ(ReturnCode(&env, response), 1);
	xmlrpc_DECREF(response);
	xmlrpc_env_clean(&env);
}

// xmlparsemap : un membre dont la valeur n'est pas une chaine laissait strKey et
// strVal NON INITIALISES, et les inserait dans la map — deux pointeurs sauvages.
TEST(XmlRpcFault, AMapMemberThatIsNotAStringIsNotRead)
{
	xmlrpc_env env;
	xmlrpc_env_init(&env);

	xmlrpc_value* map = xmlrpc_build_value(&env, "{s:i}", "cle", 42);
	ASSERT_NE(map, nullptr);

	Properties props;
	EXPECT_EQ(xmlparsemap(&env, map, props), 0);
	EXPECT_TRUE(props.empty()) << "rien ne doit etre insere depuis un membre illisible";
	EXPECT_TRUE(env.fault_occurred) << "l'appelant doit pouvoir voir la faute";

	xmlrpc_DECREF(map);
	xmlrpc_env_clean(&env);
}

// Et un paramètre qui n'est pas une struct du tout : la taille était
// indéterminée, donc la boucle parcourait un nombre inventé de membres.
TEST(XmlRpcFault, ANonStructParameterIsRefusedBeforeLooping)
{
	xmlrpc_env env;
	xmlrpc_env_init(&env);

	xmlrpc_value* notAStruct = xmlrpc_build_value(&env, "i", 7);
	ASSERT_NE(notAStruct, nullptr);

	Properties props;
	EXPECT_EQ(xmlparsemap(&env, notAStruct, props), 0);
	EXPECT_TRUE(props.empty());

	xmlrpc_DECREF(notAStruct);
	xmlrpc_env_clean(&env);
}

// Les bons membres passent toujours.
TEST(XmlRpcFault, AWellFormedMapIsStillRead)
{
	xmlrpc_env env;
	xmlrpc_env_init(&env);

	xmlrpc_value* map = xmlrpc_build_value(&env, "{s:s,s:s}", "a", "1", "b", "2");
	ASSERT_NE(map, nullptr);

	Properties props;
	EXPECT_EQ(xmlparsemap(&env, map, props), 2);
	EXPECT_EQ(props.GetProperty("a", ""), std::string("1"));
	EXPECT_EQ(props.GetProperty("b", ""), std::string("2"));
	EXPECT_FALSE(env.fault_occurred);

	xmlrpc_DECREF(map);
	xmlrpc_env_clean(&env);
}

} // namespace
