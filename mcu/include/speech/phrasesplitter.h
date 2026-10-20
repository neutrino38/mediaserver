#ifndef SPEECH_PHRASESPLITTER_H
#define SPEECH_PHRASESPLITTER_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * T.140 text, typed character by character, cut into phrases to speak
 * (SPEC TTS §7.1). A pure class: no thread, no clock of its own.
 *
 * U+0008 erases the last character not yet sent. A phrase ends on `.`, `?`,
 * `!` or `…` followed by a blank, on U+2028 or `\n`, or after `silenceMs`
 * without typing. A final punctuation alone does not cut: "3.5" or "ives.fr"
 * stay whole. Text is UTF-8.
 */
class PhraseSplitter
{
public:
	// Mistral advises under 300 words a request: an endless phrase is cut at
	// its last blank before this size.
	static constexpr size_t MaxPhraseBytes = 1500;

	explicit PhraseSplitter(int silenceMs) : silenceMs(silenceMs) {}

	// Text typed at `nowMs`; returns the phrases it completes.
	std::vector<std::string> Add(const std::string& text, uint64_t nowMs);

	// The pending phrase once `silenceMs` passed without typing, else "".
	std::string Tick(uint64_t nowMs);

	// The pending phrase, now. "" when there is none.
	std::string Flush();

	void Clear()	{ pending.clear(); }

	// Typed, not yet cut into a phrase.
	const std::string& Pending() const	{ return pending; }

	void SetSilenceMs(int ms)	{ silenceMs = ms; }

private:
	std::string Cut();

	int		silenceMs;
	std::string	pending;
	uint64_t	lastTypedMs = 0;
};

#endif
