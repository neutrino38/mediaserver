#include "speech/phrasesplitter.h"

#include <algorithm>

namespace
{

const std::string LineSeparator = "\xE2\x80\xA8";	// U+2028
const std::string Ellipsis = "\xE2\x80\xA6";		// U+2026

bool IsBlank(const std::string& text)
{
	return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool EndsWithFinalPunctuation(const std::string& text)
{
	if (text.empty())
		return false;
	const char last = text.back();
	if (last == '.' || last == '?' || last == '!')
		return true;
	return text.size() >= Ellipsis.size()
		&& text.compare(text.size() - Ellipsis.size(), Ellipsis.size(), Ellipsis) == 0;
}

// Length of the UTF-8 character starting at `text[pos]`.
size_t CharLength(const std::string& text, size_t pos)
{
	const unsigned char lead = text[pos];
	if (lead >= 0xF0) return 4;
	if (lead >= 0xE0) return 3;
	if (lead >= 0xC0) return 2;
	return 1;
}

void EraseLastChar(std::string& text)
{
	if (text.empty())
		return;
	size_t pos = text.size() - 1;
	while (pos > 0 && ((unsigned char)text[pos] & 0xC0) == 0x80)
		--pos;
	text.erase(pos);
}

}

std::vector<std::string> PhraseSplitter::Add(const std::string& text, uint64_t nowMs)
{
	std::vector<std::string> phrases;
	lastTypedMs = nowMs;

	for (size_t pos = 0; pos < text.size(); )
	{
		const size_t length = std::min(CharLength(text, pos), text.size() - pos);
		const std::string c = text.substr(pos, length);
		pos += length;

		if (c == "\b")
			EraseLastChar(pending);
		else if (c == "\n" || c == LineSeparator)
		{
			const std::string phrase = Cut();
			if (!phrase.empty())
				phrases.push_back(phrase);
		}
		else if ((c == " " || c == "\t") && EndsWithFinalPunctuation(pending))
		{
			const std::string phrase = Cut();
			if (!phrase.empty())
				phrases.push_back(phrase);
		}
		else if (c != "\r")
		{
			pending += c;
			if (pending.size() >= MaxPhraseBytes)
			{
				const size_t blank = pending.find_last_of(" \t");
				std::string rest = blank == std::string::npos ? "" : pending.substr(blank + 1);
				if (blank != std::string::npos)
					pending.erase(blank);
				const std::string phrase = Cut();
				if (!phrase.empty())
					phrases.push_back(phrase);
				pending = rest;
			}
		}
	}
	return phrases;
}

std::string PhraseSplitter::Tick(uint64_t nowMs)
{
	if (pending.empty() || nowMs - lastTypedMs < (uint64_t)silenceMs)
		return "";
	return Cut();
}

std::string PhraseSplitter::Flush()
{
	return Cut();
}

std::string PhraseSplitter::Cut()
{
	std::string phrase;
	phrase.swap(pending);
	if (IsBlank(phrase))
		return "";
	phrase.erase(0, phrase.find_first_not_of(" \t"));
	phrase.erase(phrase.find_last_not_of(" \t") + 1);
	return phrase;
}
