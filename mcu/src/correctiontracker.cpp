#include "correctiontracker.h"

#include <algorithm>
#include <cctype>

namespace
{

const std::string LineSeparator = "\xE2\x80\xA8";
const std::string Ellipsis = "\xE2\x80\xA6";

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

// Ce qui fait partir une phrase, et la borne de celle qu'une correction redit
bool IsPhraseEnd(const std::string& c)
{
	return c == "." || c == "?" || c == "!" || c == ":" || c == Ellipsis;
}

bool IsBlank(const std::string& c)
{
	return c == " " || c == "\t";
}

std::string Trim(const std::string& text)
{
	const size_t first = text.find_first_not_of(" \t");
	if (first == std::string::npos)
		return "";
	return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Une lettre ou un chiffre : le second point de « .. » n'est pas une phrase.
bool HasWords(const std::string& text)
{
	for (size_t pos = 0; pos < text.size(); )
	{
		const size_t length = std::min(CharLength(text, pos), text.size() - pos);
		const std::string c = text.substr(pos, length);
		pos += length;
		if (length > 1 ? c != Ellipsis : isalnum((unsigned char)c[0]) != 0)
			return true;
	}
	return false;
}

size_t CountWords(const std::string& text)
{
	size_t words = 0;
	bool inWord = false;
	for (char c : text)
	{
		const bool blank = c == ' ' || c == '\t';
		if (!blank && !inWord)
			++words;
		inWord = !blank;
	}
	return words;
}

// Le début de la phrase qui contient `pos` : juste après la ponctuation qui
// précède, ou le début de la ligne.
size_t SentenceStart(const std::string& line, size_t pos)
{
	size_t start = 0;
	for (size_t i = 0; i < pos && i < line.size(); )
	{
		const size_t length = std::min(CharLength(line, i), line.size() - i);
		i += length;
		if (i <= pos && IsPhraseEnd(line.substr(i - length, length)))
			start = i;
	}
	return start;
}

}

CorrectionTracker::CorrectionTracker(int stableMs, bool announce, const std::string& announcement) :
	stableMs(stableMs),
	announce(announce),
	announcement(announcement)
{
}

void CorrectionTracker::SpeakFrom(std::vector<Action>& actions, size_t from)
{
	const std::string phrase = Trim(line.substr(std::min(from, line.size())));
	said = line.size();
	if (HasWords(phrase))
		actions.push_back({Action::Speak, phrase});
}

void CorrectionTracker::FinishCorrection(std::vector<Action>& actions)
{
	correcting = false;
	SpeakFrom(actions, correctionStart);
}

std::vector<CorrectionTracker::Action> CorrectionTracker::Add(const std::string& utf8, uint64_t nowMs)
{
	std::vector<Action> actions;

	for (size_t pos = 0; pos < utf8.size(); )
	{
		const size_t length = std::min(CharLength(utf8, pos), utf8.size() - pos);
		const std::string c = utf8.substr(pos, length);
		pos += length;

		if (c == "\r")
			continue;

		if (c == "\n" || c == LineSeparator)
		{
			if (correcting)
				FinishCorrection(actions);
			else
				SpeakFrom(actions, said);
			line.clear();
			said = 0;
			continue;
		}

		if (c == "\b")
		{
			//Rien à effacer : la ligne précédente est close
			if (line.empty())
				continue;
			EraseLastChar(line);
			if (line.size() < said)
			{
				said = line.size();
				const size_t start = SentenceStart(line, line.size());
				if (!correcting)
				{
					correcting = true;
					correctionStart = start;
					actions.push_back({Action::Stop, ""});
					if (announce)
						actions.push_back({Action::Speak, announcement});
				}
				else if (start < correctionStart)
					correctionStart = start;
			}
			if (correcting)
				lastChangeMs = nowMs;
			continue;
		}

		line += c;
		if (correcting)
		{
			lastChangeMs = nowMs;
			continue;
		}
		if (IsPhraseEnd(c))
			SpeakFrom(actions, said);
		else if (IsBlank(c) && CountWords(line.substr(said)) > MaxPendingWords)
			SpeakFrom(actions, said);
	}
	return actions;
}

std::vector<CorrectionTracker::Action> CorrectionTracker::Tick(uint64_t nowMs)
{
	std::vector<Action> actions;
	if (correcting && nowMs - lastChangeMs >= (uint64_t)stableMs)
		FinishCorrection(actions);
	return actions;
}

void CorrectionTracker::Clear()
{
	line.clear();
	said = 0;
	correcting = false;
}
