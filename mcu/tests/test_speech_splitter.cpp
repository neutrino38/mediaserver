/**
 * test_speech_splitter.cpp — the phrase splitter (SPEC TTS §7.1, §11.3) and
 * the PCM converter of the tts function.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>
#include "speech/pcmconverter.h"
#include "speech/phrasesplitter.h"

using Phrases = std::vector<std::string>;

TEST(SpeechSplitter, FinalPunctuationCutsOnlyOnceABlankFollows)
{
	PhraseSplitter splitter(1500);

	EXPECT_EQ(Phrases{}, splitter.Add("Bonjour.", 0));
	EXPECT_EQ((Phrases{"Bonjour.", "Ça va ?"}), splitter.Add(" Ça va ? Oui", 10));
	EXPECT_EQ("Oui", splitter.Flush());
}

TEST(SpeechSplitter, EachKindOfEndGivesOnePhrase)
{
	PhraseSplitter splitter(1500);

	EXPECT_EQ(Phrases{"Un."}, splitter.Add("Un. ", 0));
	EXPECT_EQ(Phrases{"Deux"}, splitter.Add("Deux\n", 0));
	EXPECT_EQ(Phrases{"Trois"}, splitter.Add("Trois\xE2\x80\xA8", 0));
	EXPECT_EQ(Phrases{"Quatre ?"}, splitter.Add("Quatre ? ", 0));
	EXPECT_EQ(Phrases{"Cinq\xE2\x80\xA6"}, splitter.Add("Cinq\xE2\x80\xA6 ", 0));
}

TEST(SpeechSplitter, ABackspaceErasesBeforeSending)
{
	PhraseSplitter splitter(1500);

	splitter.Add("abc\bd.", 0);
	EXPECT_EQ("abd.", splitter.Flush());

	splitter.Add("je sus\bis", 0);
	EXPECT_EQ("je suis", splitter.Flush());
}

TEST(SpeechSplitter, ABackspaceErasesAWholeUtf8Character)
{
	PhraseSplitter splitter(1500);

	splitter.Add("caf\xC3\xA9\b\xC3\xA8", 0);
	EXPECT_EQ("caf\xC3\xA8", splitter.Flush());
}

TEST(SpeechSplitter, ASentPhraseIsNoLongerCorrected)
{
	PhraseSplitter splitter(1500);

	EXPECT_EQ(Phrases{"Fini."}, splitter.Add("Fini. ", 0));
	splitter.Add("\b\b\bok", 0);
	EXPECT_EQ("ok", splitter.Flush());
}

TEST(SpeechSplitter, TextWithoutPunctuationLeavesAfterTheSilenceNeverBefore)
{
	PhraseSplitter splitter(1500);

	splitter.Add("sans ponctuation", 1000);
	EXPECT_EQ("", splitter.Tick(2499));
	EXPECT_EQ("sans ponctuation", splitter.Tick(2500));
	EXPECT_EQ("", splitter.Tick(9000));
}

TEST(SpeechSplitter, NumbersAndNamesWithADotStayWhole)
{
	PhraseSplitter splitter(1500);

	EXPECT_EQ(Phrases{}, splitter.Add("Il est 3.5 sur ives.fr", 0));
	EXPECT_EQ("Il est 3.5 sur ives.fr", splitter.Flush());
}

TEST(SpeechSplitter, AnEndlessPhraseIsCutAtABlank)
{
	PhraseSplitter splitter(1500);
	std::string text;
	while (text.size() < PhraseSplitter::MaxPhraseBytes + 100)
		text += "mot ";

	const Phrases phrases = splitter.Add(text, 0);
	ASSERT_EQ(1u, phrases.size());
	EXPECT_LT(phrases[0].size(), PhraseSplitter::MaxPhraseBytes);
	EXPECT_EQ(' ', text[phrases[0].size()]);
	EXPECT_NE("", splitter.Flush());
}

TEST(SpeechSplitter, BlankPhrasesAreDropped)
{
	PhraseSplitter splitter(1500);

	EXPECT_EQ(Phrases{}, splitter.Add("  \n\n", 0));
	EXPECT_EQ("", splitter.Flush());
}

TEST(SpeechPcmConverter, A440HzSineAt24kHzComesOutAt16kHz)
{
	const int inRate = 24000;
	std::string input(inRate * sizeof(float), '\0');
	float* f = (float*)&input[0];
	for (int i = 0; i < inRate; ++i)
		f[i] = 0.5f * std::sin(2 * M_PI * 440.0 * i / inRate);

	PcmConverter converter(inRate);
	ASSERT_TRUE(converter.IsValid());

	// Cut inside a float on purpose.
	std::string out = converter.Convert(input.substr(0, 1001));
	out += converter.Convert(input.substr(1001));
	out += converter.Drain();

	const size_t samples = out.size() / 2;
	EXPECT_NEAR(16000.0, (double)samples, 32.0);

	const int16_t* s = (const int16_t*)out.data();
	int crossings = 0, peak = 0;
	for (size_t i = 1; i < samples; ++i)
	{
		if ((s[i-1] < 0) != (s[i] < 0))
			++crossings;
		peak = std::max(peak, std::abs((int)s[i]));
	}
	EXPECT_NEAR(880, crossings, 4);
	EXPECT_NEAR(16384, peak, 600);
}
