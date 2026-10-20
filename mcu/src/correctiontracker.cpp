#include "correctiontracker.h"

namespace
{

const std::string LineSeparator = "\xE2\x80\xA8";

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

std::string Trim(const std::string& text)
{
	const size_t first = text.find_first_not_of(" \t");
	if (first == std::string::npos)
		return "";
	return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

}

CorrectionTracker::CorrectionTracker(int silenceMs, bool announce, const std::string& announcement) :
	silenceMs(silenceMs),
	announce(announce),
	announcement(announcement),
	splitter(silenceMs)
{
}

void CorrectionTracker::Speak(std::vector<Action>& actions, const std::string& phrase, const std::string& after)
{
	if (phrase.empty())
		return;
	actions.push_back({Action::Speak, phrase});
	lineSaid += phrase + after;
}

void CorrectionTracker::FinishCorrection(std::vector<Action>& actions, bool endOfLine)
{
	const std::string line = Trim(lineSaid + retyped);
	correcting = false;
	retyped.clear();
	lineSaid = endOfLine || line.empty() ? "" : line + " ";
	if (!line.empty())
		actions.push_back({Action::Speak, announce ? announcement + line : line});
}

std::vector<CorrectionTracker::Action> CorrectionTracker::Add(const std::string& utf8, uint64_t nowMs)
{
	std::vector<Action> actions;
	lastTypedMs = nowMs;

	for (size_t pos = 0; pos < utf8.size(); )
	{
		const size_t length = std::min(CharLength(utf8, pos), utf8.size() - pos);
		const std::string c = utf8.substr(pos, length);
		pos += length;
		const bool endOfLine = c == "\n" || c == LineSeparator;

		if (correcting)
		{
			if (c == "\b")
			{
				if (!retyped.empty())
					EraseLastChar(retyped);
				else
					EraseLastChar(lineSaid);
			}
			else if (endOfLine)
				FinishCorrection(actions, true);
			else if (c != "\r")
				retyped += c;
			continue;
		}

		if (c == "\b" && splitter.Pending().empty())
		{
			//Rien à effacer qui ne soit déjà dit, ou déjà rien du tout
			if (lineSaid.empty())
				continue;
			EraseLastChar(lineSaid);
			correcting = true;
			actions.push_back({Action::Stop, ""});
			continue;
		}

		const bool blank = c == " " || c == "\t";
		for (const std::string& phrase : splitter.Add(c, nowMs))
			Speak(actions, phrase, blank ? " " : "");
		if (endOfLine)
			lineSaid.clear();
	}
	return actions;
}

std::vector<CorrectionTracker::Action> CorrectionTracker::Tick(uint64_t nowMs)
{
	std::vector<Action> actions;
	if (correcting)
	{
		if (nowMs - lastTypedMs >= (uint64_t)silenceMs)
			FinishCorrection(actions, false);
		return actions;
	}
	Speak(actions, splitter.Tick(nowMs), "");
	return actions;
}

void CorrectionTracker::Clear()
{
	splitter.Clear();
	lineSaid.clear();
	retyped.clear();
	correcting = false;
}
