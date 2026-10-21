#ifndef TEXTMUX_H
#define TEXTMUX_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

/**
 * TextMux — plusieurs sources de texte T.140 vers une seule sortie
 * (SPEC SPEECH-PROCESSOR §9).
 *
 * Le T.140 n'a pas de multiplexage : une lettre tapée au milieu d'un delta de
 * reconnaissance mélangerait deux phrases. Les règles :
 *
 *  - une seule source TIENT la sortie à un instant donné ;
 *  - elle rend la main en fin de ligne (U+2028 ou '\n'), ou après
 *    `releaseMs` sans caractère d'elle ;
 *  - l'autre attend dans son tampon, sans limite : rien ne se perd ;
 *  - à chaque changement de source, un U+2028 est inséré si la ligne en
 *    cours n'est pas vide ;
 *  - un U+0008 n'efface que du texte de SA source dans la ligne en cours ;
 *    s'il remonterait dans le texte de l'autre source, ou au-delà du début
 *    de la ligne, il est jeté.
 *
 * Classe pure : pas d'horloge propre (l'heure est passée à chaque appel), pas
 * de thread, pas de verrou. Entrée et sortie en UTF-8. Les BOM (U+FEFF), qui
 * servent de keep-alive T.140, sont jetés à l'entrée.
 */
class TextMux
{
public:
	static constexpr int		Sources = 2;
	static constexpr uint64_t	DefaultReleaseMs = 1000;

	static constexpr char32_t	LineSeparator = 0x2028;
	static constexpr char32_t	Backspace = 0x0008;
	static constexpr char32_t	Bom = 0xFEFF;

	explicit TextMux(uint64_t releaseMs = DefaultReleaseMs);

	// Ajoute du texte d'une source. Une séquence UTF-8 coupée entre deux
	// appels est recollée.
	void Push(int source, const std::string& utf8, uint64_t nowMs);

	// Ce qui doit sortir maintenant, en UTF-8 ; vide s'il n'y a rien.
	std::string Pull(uint64_t nowMs);

	// La source qui tient la sortie, -1 si aucune.
	int Holder() const		{ return holder; }
	// Du texte attend-il encore ?
	bool IsEmpty() const;

	static void AppendUtf8(std::string& out, char32_t c);

private:
	struct Source
	{
		std::deque<char32_t>	pending;
		std::string		partial;	// séquence UTF-8 incomplète
		uint64_t		lastInputMs = 0;
		uint64_t		queuedOrder = 0;	// rang dans la file d'attente
	};

	void Emit(std::string& out, int source, char32_t c);
	static bool IsEndOfLine(char32_t c)	{ return c == LineSeparator || c == '\n'; }

	const uint64_t		releaseMs;
	Source			sources[Sources];
	int			holder = -1;
	uint64_t		order = 0;
	// Source de chaque caractère de la ligne en cours.
	std::vector<int>	line;
};

#endif
