#ifndef CORRECTIONTRACKER_H
#define CORRECTIONTRACKER_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * CorrectionTracker — la frappe T.140 d'un port, découpée en phrases à dire,
 * avec ses corrections (SPEC SPEECH-PROCESSOR §7.5).
 *
 * Il tient une copie exacte de la ligne affichée chez l'appelant, et la part
 * déjà envoyée à la synthèse. Une phrase part :
 *  - sur `.`, `?`, `!`, `:` ou `…`, dès qu'il est tapé ;
 *  - sur une fin de ligne (U+2028 ou `\n`) ;
 *  - sur un blanc, quand plus de `MaxPendingWords` mots attendent.
 * Jamais sur un silence : un mot n'est pas coupé en deux.
 *
 * Un retour arrière qui ne touche que du texte pas encore dit l'efface. Un
 * retour arrière qui remonte dans du texte déjà dit ouvre une CORRECTION :
 *  - Stop part tout de suite, puis l'annonce si elle est demandée ;
 *  - la phrase touchée, depuis la ponctuation qui la précède, est redite
 *    quand la ligne n'a pas changé pendant `stableMs`, ou en fin de ligne.
 *
 * Classe pure : ni thread, ni horloge, ni réseau. Entrée et sortie en UTF-8.
 */
class CorrectionTracker
{
public:
	struct Action
	{
		enum Kind { Speak, Stop };
		Kind		kind;
		std::string	text;

		bool operator==(const Action& other) const	{ return kind == other.kind && text == other.text; }
	};

	static constexpr int DefaultStableMs = 2000;
	static constexpr size_t MaxPendingWords = 5;

	CorrectionTracker(int stableMs, bool announce, const std::string& announcement = "Correction");

	std::vector<Action> Add(const std::string& utf8, uint64_t nowMs);
	std::vector<Action> Tick(uint64_t nowMs);

	// Oublie la ligne en cours : ce qui était tapé ne sera pas dit.
	void Clear();

	bool IsCorrecting() const	{ return correcting; }

private:
	void SpeakFrom(std::vector<Action>& actions, size_t from);
	void FinishCorrection(std::vector<Action>& actions);

	const int		stableMs;
	const bool		announce;
	const std::string	announcement;
	// La ligne telle qu'elle s'affiche, et combien d'octets en sont dits
	std::string		line;
	size_t			said = 0;
	bool			correcting = false;
	// Où commence la phrase à redire
	size_t			correctionStart = 0;
	uint64_t		lastChangeMs = 0;
};

#endif
