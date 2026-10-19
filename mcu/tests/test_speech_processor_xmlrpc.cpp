/**
 * test_speech_processor_xmlrpc.cpp — le SpeechProcessor vu du CONTRÔLEUR, par
 * la table `jsr309CmdList` (SPEC SPEECH-PROCESSOR §8.1, §10.8).
 *
 * Le call flow de docs/JSR-309-API.md §9.8 est joué ici appel par appel, dans
 * l'ordre de la doc : si la doc et le code divergent, un test casse. Plus les
 * refus que le contrôleur doit pouvoir lire : port hors de {0, 1}, vidéo,
 * objets inconnus, bruit hors bornes.
 *
 *     ./tests/runtests --gtest_filter='SpeechProcessorXmlRpc*'
 */
#include <gtest/gtest.h>

#include <cstdarg>
#include <cstring>
#include <string>

#include "xmlhandler.h"
#include "../src/jsr309/JSR309Manager.h"

extern XmlHandlerCmd jsr309CmdList[];

namespace
{

typedef xmlrpc_value* (*CommandFunc)(xmlrpc_env*, xmlrpc_value*, void*);

CommandFunc FindCommand(const char* name)
{
	for (int i = 0; jsr309CmdList[i].name; ++i)
		if (strcmp(jsr309CmdList[i].name, name) == 0)
			return jsr309CmdList[i].func;
	return NULL;
}

// Une réponse : returnCode (1 succès, 0 faute, -1 pas de réponse, -2 méthode
// absente), le premier entier de returnVal, et errorMsg.
struct Answer
{
	int		code = -1;
	int		value = -1;
	std::string	error;
};

Answer Call(JSR309Manager& manager, const char* name, const char* format, ...)
{
	Answer answer;
	CommandFunc command = FindCommand(name);
	if (!command)
	{
		answer.code = -2;
		return answer;
	}

	xmlrpc_env env;
	xmlrpc_env_init(&env);

	va_list args;
	va_start(args, format);
	xmlrpc_value* params = NULL;
	const char* tail = NULL;
	xmlrpc_build_value_va(&env, format, args, &params, &tail);
	va_end(args);

	xmlrpc_value* response = command(&env, params, (void*)&manager);
	if (response)
	{
		xmlrpc_value* code = NULL;
		xmlrpc_struct_find_value(&env, response, "returnCode", &code);
		if (code)
		{
			xmlrpc_parse_value(&env, code, "i", &answer.code);
			xmlrpc_DECREF(code);
		}
		xmlrpc_value* values = NULL;
		xmlrpc_struct_find_value(&env, response, "returnVal", &values);
		if (values)
		{
			if (xmlrpc_array_size(&env, values) > 0)
			{
				xmlrpc_value* first = NULL;
				xmlrpc_array_read_item(&env, values, 0, &first);
				if (first)
				{
					xmlrpc_parse_value(&env, first, "i", &answer.value);
					xmlrpc_DECREF(first);
				}
			}
			xmlrpc_DECREF(values);
		}
		xmlrpc_value* message = NULL;
		xmlrpc_struct_find_value(&env, response, "errorMsg", &message);
		if (message)
		{
			//"s" rend la chaîne interne de la valeur, pas une copie : ne pas la libérer
			const char* text = NULL;
			xmlrpc_parse_value(&env, message, "s", &text);
			if (text)
				answer.error = text;
			xmlrpc_DECREF(message);
		}
		xmlrpc_DECREF(response);
	}
	if (params)
		xmlrpc_DECREF(params);
	xmlrpc_env_clean(&env);
	return answer;
}

// Les valeurs de l'API, telles que docs/JSR-309-API.md §4 les publie
const int AUDIO = 0;
const int VIDEO = 1;
const int TEXT = 2;
const int PCMU = 0;
const int OPUS = 98;

class Controller
{
public:
	Controller()	{ manager.Init(&handler, 0); }
	~Controller()	{ manager.End(); }

	int Count(int sessionId)
	{
		std::shared_ptr<MediaSession> session;
		return manager.GetMediaSessionRef(sessionId, session) ? session->GetSpeechProcessorCount() : -1;
	}

	XmlStreamingHandler	handler;
	JSR309Manager		manager;
};

}

TEST(SpeechProcessorXmlRpc, LesSeptMethodesSontRecensees)
{
	for (const char* name : {"SpeechProcessorCreate", "SpeechProcessorDelete", "SpeechProcessorPortSetCodec",
				 "SpeechProcessorSetComfortNoise", "SpeechProcessorAttachToEndpoint",
				 "EndpointAttachToSpeechProcessor", "SpeechProcessorDettach"})
		EXPECT_TRUE(FindCommand(name) != NULL) << name;
}

// docs/JSR-309-API.md §9.8, cas 3 : texte, voix et vidéo des deux côtés. La
// vidéo va d'endpoint à endpoint ; l'audio et le texte passent par le
// SpeechProcessor, qui les relaie d'un port à l'autre.
TEST(SpeechProcessorXmlRpc, LeCallFlowDeLaDocSeJoue)
{
	Controller c;
	JSR309Manager& m = c.manager;

	const Answer s = Call(m, "MediaSessionCreate", "(si)", "appel-sourd", 0);
	ASSERT_EQ(1, s.code);
	const int S = s.value;
	const Answer a = Call(m, "EndpointCreate", "(isbbb)", S, "A", 1, 1, 1);
	const Answer b = Call(m, "EndpointCreate", "(isbbb)", S, "B", 1, 1, 1);
	ASSERT_EQ(1, a.code);
	ASSERT_EQ(1, b.code);
	const int A = a.value;
	const int B = b.value;

	// 1. Le SpeechProcessor, avant toute attache
	const Answer sp = Call(m, "SpeechProcessorCreate", "(is)", S, "transcription");
	ASSERT_EQ(1, sp.code) << sp.error;
	const int SP = sp.value;

	// 2. Le codec de chaque port, d'après le SDP de sa jambe
	EXPECT_EQ(1, Call(m, "SpeechProcessorPortSetCodec", "(iiii)", S, SP, 0, PCMU).code);
	EXPECT_EQ(1, Call(m, "SpeechProcessorPortSetCodec", "(iiii)", S, SP, 1, OPUS).code);

	// 3. Les deux sens de chaque jambe, audio et texte
	for (int media : {AUDIO, TEXT})
	{
		EXPECT_EQ(1, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 0, A, media).code);
		EXPECT_EQ(1, Call(m, "EndpointAttachToSpeechProcessor", "(iiiii)", S, A, SP, 0, media).code);
		EXPECT_EQ(1, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 1, B, media).code);
		EXPECT_EQ(1, Call(m, "EndpointAttachToSpeechProcessor", "(iiiii)", S, B, SP, 1, media).code);
	}

	// 4. La vidéo, directement d'un endpoint à l'autre
	EXPECT_EQ(1, Call(m, "EndpointAttachToEndpoint", "(iiii)", S, A, B, VIDEO).code);
	EXPECT_EQ(1, Call(m, "EndpointAttachToEndpoint", "(iiii)", S, B, A, VIDEO).code);

	// 5. Le bruit de confort, si le défaut ne convient pas
	EXPECT_EQ(1, Call(m, "SpeechProcessorSetComfortNoise", "(iiii)", S, SP, 1, -60).code);

	// Terminaison : le SpeechProcessor avant les endpoints
	for (int media : {AUDIO, TEXT})
	{
		EXPECT_EQ(1, Call(m, "EndpointDettach", "(iii)", S, A, media).code);
		EXPECT_EQ(1, Call(m, "EndpointDettach", "(iii)", S, B, media).code);
	}
	EXPECT_EQ(1, Call(m, "EndpointDettach", "(iii)", S, A, VIDEO).code);
	EXPECT_EQ(1, Call(m, "EndpointDettach", "(iii)", S, B, VIDEO).code);
	EXPECT_EQ(1, c.Count(S));
	EXPECT_EQ(1, Call(m, "SpeechProcessorDelete", "(ii)", S, SP).code);
	EXPECT_EQ(0, c.Count(S));
	EXPECT_EQ(1, Call(m, "EndpointDelete", "(ii)", S, A).code);
	EXPECT_EQ(1, Call(m, "EndpointDelete", "(ii)", S, B).code);
	EXPECT_EQ(1, Call(m, "MediaSessionDelete", "(i)", S).code);
}

// docs/JSR-309-API.md §9.8, cas 2 : texte seul d'un côté, voix de l'autre.
// Chaque média est facultatif sur un port.
TEST(SpeechProcessorXmlRpc, UnPortPeutNePorterQueLeTexte)
{
	Controller c;
	JSR309Manager& m = c.manager;

	const int S = Call(m, "MediaSessionCreate", "(si)", "texte-seul", 0).value;
	const int A = Call(m, "EndpointCreate", "(isbbb)", S, "A", 0, 0, 1).value;
	const int B = Call(m, "EndpointCreate", "(isbbb)", S, "B", 1, 0, 0).value;
	const int SP = Call(m, "SpeechProcessorCreate", "(is)", S, "texte-voix").value;

	EXPECT_EQ(1, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 0, A, TEXT).code);
	EXPECT_EQ(1, Call(m, "EndpointAttachToSpeechProcessor", "(iiiii)", S, A, SP, 0, TEXT).code);
	EXPECT_EQ(1, Call(m, "SpeechProcessorPortSetCodec", "(iiii)", S, SP, 1, PCMU).code);
	EXPECT_EQ(1, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 1, B, AUDIO).code);
	EXPECT_EQ(1, Call(m, "EndpointAttachToSpeechProcessor", "(iiiii)", S, B, SP, 1, AUDIO).code);

	// L'endpoint A n'a pas d'audio : rien à attacher
	EXPECT_EQ(0, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 0, A, AUDIO).code);

	EXPECT_EQ(1, Call(m, "SpeechProcessorDettach", "(iiii)", S, SP, 0, TEXT).code);
	EXPECT_EQ(1, Call(m, "SpeechProcessorDelete", "(ii)", S, SP).code);
	EXPECT_EQ(1, Call(m, "MediaSessionDelete", "(i)", S).code);
}

TEST(SpeechProcessorXmlRpc, LesRefusDisentQuoiCorriger)
{
	Controller c;
	JSR309Manager& m = c.manager;

	const int S = Call(m, "MediaSessionCreate", "(si)", "refus", 0).value;
	const int A = Call(m, "EndpointCreate", "(isbbb)", S, "A", 1, 1, 1).value;
	const int SP = Call(m, "SpeechProcessorCreate", "(is)", S, "sp").value;

	Answer video = Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 0, A, VIDEO);
	EXPECT_EQ(0, video.code);
	EXPECT_NE(std::string::npos, video.error.find("audio and text only")) << video.error;
	video = Call(m, "EndpointAttachToSpeechProcessor", "(iiiii)", S, A, SP, 0, VIDEO);
	EXPECT_EQ(0, video.code);
	EXPECT_EQ(0, Call(m, "SpeechProcessorDettach", "(iiii)", S, SP, 0, VIDEO).code);

	const Answer port = Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 2, A, AUDIO);
	EXPECT_EQ(0, port.code);
	EXPECT_NE(std::string::npos, port.error.find("0 (A) or 1 (B)")) << port.error;
	EXPECT_EQ(0, Call(m, "SpeechProcessorPortSetCodec", "(iiii)", S, SP, -1, PCMU).code);

	const Answer noise = Call(m, "SpeechProcessorSetComfortNoise", "(iiii)", S, SP, 0, 6);
	EXPECT_EQ(0, noise.code);
	EXPECT_NE(std::string::npos, noise.error.find("dBov")) << noise.error;

	EXPECT_EQ(0, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP + 100, 0, A, AUDIO).code);
	EXPECT_EQ(0, Call(m, "SpeechProcessorAttachToEndpoint", "(iiiii)", S, SP, 0, A + 100, AUDIO).code);
	EXPECT_EQ(0, Call(m, "SpeechProcessorDelete", "(ii)", S, SP + 100).code);
	EXPECT_EQ(0, Call(m, "SpeechProcessorCreate", "(is)", S + 100, "sp").code);

	// Un appel mal typé n'abat pas le serveur et ne réussit pas : le handler
	// rend la faute posée dans l'env, que XmlHandler sert au client
	// (test_xmlrpc_fault.cpp)
	EXPECT_NE(1, Call(m, "SpeechProcessorAttachToEndpoint", "(iii)", S, SP, 0).code);

	// MediaSessionDelete emporte le SpeechProcessor restant
	EXPECT_EQ(1, Call(m, "MediaSessionDelete", "(i)", S).code);
}
