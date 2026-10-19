#include "speech/compcheck.h"

#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{

size_t CharLength(unsigned char lead)
{
	if (lead >= 0xF0) return 4;
	if (lead >= 0xE0) return 3;
	if (lead >= 0xC0) return 2;
	return 1;
}

// A letter or a digit: ASCII alphanumerics, and every non-ASCII character
// but the general punctuation block (U+2000 to U+206F: …, ‘ ’, “ ”, U+2028)
// and the Latin-1 signs before À.
bool IsWordChar(const std::string& text, size_t pos)
{
	const unsigned char c = text[pos];
	if (c < 0x80)
		return isalnum(c);
	if (c == 0xC2)
		return false;
	if (c == 0xE2 && pos + 1 < text.size() && (unsigned char)text[pos+1] == 0x80)
		return false;
	return c >= 0xC3;
}

bool IsWordStart(const std::string& text, size_t pos)
{
	if (pos == 0)
		return true;
	size_t prev = pos - 1;
	while (prev > 0 && ((unsigned char)text[prev] & 0xC0) == 0x80)
		--prev;
	return !IsWordChar(text, prev);
}

bool IsWordEnd(const std::string& text, size_t pos)
{
	return pos >= text.size() || !IsWordChar(text, pos);
}

size_t Utf16Length(const std::string& text, size_t from, size_t to)
{
	size_t units = 0;
	for (size_t pos = from; pos < to; pos += CharLength(text[pos]))
		units += CharLength(text[pos]) == 4 ? 2 : 1;
	return units;
}

// The letters and digits of `text`, lower-cased in ASCII, one entry per
// character: what a spelling correction may change only a little.
std::vector<std::string> Letters(const std::string& text)
{
	std::vector<std::string> letters;
	for (size_t pos = 0; pos < text.size(); pos += CharLength(text[pos]))
		if (IsWordChar(text, pos))
		{
			std::string c = text.substr(pos, CharLength(text[pos]));
			if (c.size() == 1)
				c[0] = tolower((unsigned char)c[0]);
			letters.push_back(c);
		}
	return letters;
}

size_t EditDistance(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
	std::vector<size_t> row(b.size() + 1);
	for (size_t j = 0; j <= b.size(); ++j)
		row[j] = j;
	for (size_t i = 1; i <= a.size(); ++i)
	{
		size_t diagonal = row[0];
		row[0] = i;
		for (size_t j = 1; j <= b.size(); ++j)
		{
			const size_t above = row[j];
			row[j] = std::min({row[j] + 1, row[j-1] + 1, diagonal + (a[i-1] == b[j-1] ? 0 : 1)});
			diagonal = above;
		}
	}
	return row[b.size()];
}

// A correction changes a few letters; more is a rewriting.
bool IsCorrection(const std::string& from, const std::string& to)
{
	const std::vector<std::string> before = Letters(from);
	return EditDistance(before, Letters(to)) <= std::max<size_t>(2, before.size() / 3);
}

}

CompCheck::Result CompCheck::Apply(const std::string& text, const std::string& answer)
{
	Result result;
	result.text = text;

	const json parsed = json::parse(answer, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("corrections") || !parsed["corrections"].is_array())
		return result;
	result.valid = true;

	// Changes are found left to right in the text sent, then applied.
	std::vector<std::pair<size_t,size_t>> spans;	// bytes in `text`
	size_t searchFrom = 0;
	for (const json& pair : parsed["corrections"])
	{
		if (!pair.is_object() || !pair.contains("from") || !pair.contains("to")
		    || !pair["from"].is_string() || !pair["to"].is_string())
			continue;
		const std::string from = pair["from"];
		const std::string to = pair["to"];
		if (from.empty() || to.empty() || from == to)
			continue;
		if (!IsCorrection(from, to))
			continue;

		size_t found = std::string::npos;
		for (size_t pos = text.find(from, searchFrom); pos != std::string::npos; pos = text.find(from, pos + 1))
			if (IsWordStart(text, pos) && IsWordEnd(text, pos + from.size()))
			{
				found = pos;
				break;
			}
		if (found == std::string::npos)
			continue;

		spans.push_back({found, found + from.size()});
		result.changes.push_back({Utf16Length(text, 0, found), Utf16Length(text, 0, found + from.size()), from, to});
		searchFrom = found + from.size();
	}

	std::string corrected;
	size_t copied = 0;
	for (size_t i = 0; i < spans.size(); ++i)
	{
		corrected += text.substr(copied, spans[i].first - copied);
		corrected += result.changes[i].to;
		copied = spans[i].second;
	}
	corrected += text.substr(copied);
	result.text = corrected;
	return result;
}
