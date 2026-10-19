/**
 * test_speech_comp_reference.cpp — the comp prompt against the real Mistral,
 * on ten reference phrases (SPEC §14.4). Out of `make check`: it needs the
 * network and a key. Run it by hand before any change of prompt or model:
 *
 *   MISTRAL_API_KEY_FILE=~/.config/mistral/api.key make -C mcu check-speech-comp
 *
 * COMP_MODEL picks another model.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "speech/compcheck.h"
#include "speech/compsession.h"
#include "speech/mistralchat.h"
#include "speech/speechconfig.h"

using json = nlohmann::json;

namespace
{

// Typographic and straight apostrophes are the same to the voice.
std::string Normalize(std::string text)
{
	for (size_t pos; (pos = text.find("\xE2\x80\x99")) != std::string::npos; )
		text.replace(pos, 3, "'");
	return text;
}

}

TEST(DISABLED_SpeechCompReference, TenFrenchPhrases)
{
	const char* keyFile = getenv("MISTRAL_API_KEY_FILE");
	if (!keyFile)
		GTEST_SKIP() << "MISTRAL_API_KEY_FILE is not set";
	std::ifstream keyStream(keyFile);
	std::string key;
	std::getline(keyStream, key);
	ASSERT_FALSE(key.empty()) << "cannot read " << keyFile;

	const char* model = getenv("COMP_MODEL");
	std::ifstream casesFile(std::string(SPEECH_FIXTURES_DIR) + "/comp/cases.json");
	std::stringstream raw;
	raw << casesFile.rdbuf();
	const json cases = json::parse(raw.str());

	int passed = 0;
	for (const json& item : cases)
	{
		const std::string text = item["text"];
		const MistralChat::Result result = MistralChat::Complete({
			"https://api.mistral.ai", key, model ? model : SpeechConfig().compModel, CompSession::Prompt(),
			json{{"langue", "fr"}, {"contexte", item["context"]}, {"texte", text}}.dump()});
		ASSERT_EQ("", result.error) << text;

		const CompCheck::Result corrected = CompCheck::Apply(text, result.content);
		bool ok = false;
		for (const json& expected : item["expected"])
			ok = ok || Normalize(corrected.text) == expected.get<std::string>();
		passed += ok;
		EXPECT_TRUE(ok) << "[" << item["context"].get<std::string>() << "]" << text << " -> " << corrected.text;
	}
	std::cout << passed << "/" << cases.size() << " phrases corrected as expected" << std::endl;
}
