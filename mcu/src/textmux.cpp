#include "textmux.h"

TextMux::TextMux(uint64_t releaseMs) :
	releaseMs(releaseMs)
{
}

void TextMux::AppendUtf8(std::string& out, char32_t c)
{
	if (c < 0x80)
		out.push_back((char)c);
	else if (c < 0x800)
	{
		out.push_back((char)(0xC0 | (c >> 6)));
		out.push_back((char)(0x80 | (c & 0x3F)));
	}
	else if (c < 0x10000)
	{
		out.push_back((char)(0xE0 | (c >> 12)));
		out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (c & 0x3F)));
	}
	else
	{
		out.push_back((char)(0xF0 | (c >> 18)));
		out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
		out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (c & 0x3F)));
	}
}

void TextMux::Push(int source, const std::string& utf8, uint64_t nowMs)
{
	if (source < 0 || source >= Sources)
		return;

	Source& s = sources[source];
	const std::string bytes = s.partial + utf8;
	s.partial.clear();

	const bool wasEmpty = s.pending.empty();
	bool text = false;

	for (size_t i = 0; i < bytes.size(); )
	{
		const unsigned char lead = bytes[i];
		size_t length;
		char32_t c;
		if (lead < 0x80)		{ length = 1; c = lead; }
		else if ((lead & 0xE0) == 0xC0)	{ length = 2; c = lead & 0x1F; }
		else if ((lead & 0xF0) == 0xE0)	{ length = 3; c = lead & 0x0F; }
		else if ((lead & 0xF8) == 0xF0)	{ length = 4; c = lead & 0x07; }
		else
		{
			//Octet de continuation orphelin : on le saute
			++i;
			continue;
		}

		//Séquence coupée en fin de paquet : le reste viendra avec le suivant
		if (i + length > bytes.size())
		{
			s.partial = bytes.substr(i);
			break;
		}

		bool valid = true;
		for (size_t k = 1; k < length; ++k)
		{
			const unsigned char next = bytes[i + k];
			if ((next & 0xC0) != 0x80)
			{
				valid = false;
				break;
			}
			c = (c << 6) | (next & 0x3F);
		}
		if (!valid)
		{
			++i;
			continue;
		}
		i += length;

		//Le BOM est le keep-alive du T.140 : ce n'est pas du texte
		if (c == Bom)
			continue;

		s.pending.push_back(c);
		text = true;
	}

	if (!text)
		return;
	s.lastInputMs = nowMs;
	if (wasEmpty)
		s.queuedOrder = ++order;
}

bool TextMux::IsEmpty() const
{
	for (const Source& s : sources)
		if (!s.pending.empty())
			return false;
	return true;
}

void TextMux::Emit(std::string& out, int source, char32_t c)
{
	if (c == Backspace)
	{
		//N'efface que son propre texte, et jamais au-delà du début de la ligne
		if (line.empty() || line.back() != source)
			return;
		line.pop_back();
		AppendUtf8(out, c);
		return;
	}

	AppendUtf8(out, c);
	if (IsEndOfLine(c))
		line.clear();
	else
		line.push_back(source);
}

std::string TextMux::Pull(uint64_t nowMs)
{
	std::string out;

	for (;;)
	{
		if (holder == -1)
		{
			//La source qui attend depuis le plus longtemps prend la main
			int chosen = -1;
			for (int i = 0; i < Sources; ++i)
				if (!sources[i].pending.empty()
				    && (chosen == -1 || sources[i].queuedOrder < sources[chosen].queuedOrder))
					chosen = i;
			if (chosen == -1)
				break;

			//Changement de source au milieu d'une ligne : on la termine
			if (!line.empty() && line.back() != chosen)
				Emit(out, chosen, LineSeparator);
			holder = chosen;
		}

		Source& s = sources[holder];
		bool endOfLine = false;
		while (!s.pending.empty() && !endOfLine)
		{
			const char32_t c = s.pending.front();
			s.pending.pop_front();
			Emit(out, holder, c);
			endOfLine = IsEndOfLine(c);
		}

		if (endOfLine)
		{
			//Fin de ligne : la main passe. Ce qui reste de cette source repasse
			//derrière ce qui attend déjà.
			if (!s.pending.empty())
				s.queuedOrder = ++order;
			holder = -1;
			continue;
		}

		//Tampon vide : la source garde la main tant qu'elle n'a pas été muette
		//`releaseMs`.
		if (nowMs - s.lastInputMs < releaseMs)
			break;
		holder = -1;
		if (IsEmpty())
			break;
	}

	return out;
}
