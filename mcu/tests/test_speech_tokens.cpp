/**
 * test_speech_tokens.cpp — streaming tokens (SPEC §6.2): opaque, single use,
 * one function, bounded life.
 */
#include <gtest/gtest.h>

#include <set>
#include <string>
#include "speech/tokenstore.h"
#include "speech/usage.h"

TEST(SpeechTokens, ATokenIs43Base64UrlCharacters)
{
	TokenStore store;
	std::set<std::string> seen;
	for (int i = 0; i < 100; ++i)
	{
		const TokenStore::Token token = store.Create(SpeechFunction::Tts, 60, "-", 1000);
		ASSERT_EQ(TokenStore::ValueLength, token.value.size());
		EXPECT_EQ(std::string::npos, token.value.find_first_not_of(
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"));
		EXPECT_TRUE(seen.insert(token.value).second);
	}
}

TEST(SpeechTokens, ATokenServesOnce)
{
	TokenStore store;
	const TokenStore::Token token = store.Create(SpeechFunction::Asr, 60, "clinique-nord", 1000);

	TokenStore::Token consumed;
	ASSERT_EQ(TokenStore::Refusal::None, store.Consume(token.value, SpeechFunction::Asr, 1001, consumed));
	EXPECT_EQ("clinique-nord", consumed.account);
	EXPECT_EQ(TokenStore::Refusal::Unknown, store.Consume(token.value, SpeechFunction::Asr, 1002, consumed));
}

TEST(SpeechTokens, ATokenOfAnotherFunctionIsRefusedButNotBurnt)
{
	TokenStore store;
	const TokenStore::Token token = store.Create(SpeechFunction::Tts, 60, "-", 1000);

	TokenStore::Token consumed;
	EXPECT_EQ(TokenStore::Refusal::WrongFunction, store.Consume(token.value, SpeechFunction::Comp, 1001, consumed));
	EXPECT_EQ(TokenStore::Refusal::None, store.Consume(token.value, SpeechFunction::Tts, 1001, consumed));
}

TEST(SpeechTokens, AnExpiredTokenIsUnknownAndCounted)
{
	TokenStore store;
	const TokenStore::Token token = store.Create(SpeechFunction::Tts, 10, "-", 1000);
	EXPECT_EQ(1010, token.expiresAt);

	TokenStore::Token consumed;
	EXPECT_EQ(TokenStore::Refusal::Unknown, store.Consume(token.value, SpeechFunction::Tts, 1010, consumed));

	const TokenStore::Counters counters = store.GetCounters(1010);
	EXPECT_EQ(0u, counters.live);
	EXPECT_EQ(1u, counters.issued);
	EXPECT_EQ(0u, counters.consumed);
	EXPECT_EQ(1u, counters.expiredUnused);
}

TEST(SpeechTokens, AnUnknownValueIsRefused)
{
	TokenStore store;
	TokenStore::Token consumed;
	EXPECT_EQ(TokenStore::Refusal::Unknown, store.Consume("nope", SpeechFunction::Tts, 1000, consumed));
}

TEST(SpeechTokens, CountersFollowTheLifeOfTokens)
{
	TokenStore store;
	const TokenStore::Token a = store.Create(SpeechFunction::Asr, 60, "-", 1000);
	store.Create(SpeechFunction::Asr, 60, "-", 1000);

	TokenStore::Token consumed;
	store.Consume(a.value, SpeechFunction::Asr, 1001, consumed);

	const TokenStore::Counters counters = store.GetCounters(1001);
	EXPECT_EQ(1u, counters.live);
	EXPECT_EQ(2u, counters.issued);
	EXPECT_EQ(1u, counters.consumed);
	EXPECT_EQ(0u, counters.expiredUnused);
}

TEST(SpeechUsage, AnAccountAppearsFromItsFirstToken)
{
	UsageCounters usage;
	usage.Register("clinique-nord");
	usage.Register("clinique-nord");
	usage.Register("-");

	const auto accounts = usage.ByAccount();
	ASSERT_EQ(2u, accounts.size());
	EXPECT_EQ(1u, accounts.count("clinique-nord"));
	EXPECT_EQ(0u, usage.Total().ttsChars);
}
