#ifndef CORRECTIONTRACKER_H
#define CORRECTIONTRACKER_H

#include <cstdint>
#include <string>
#include <vector>
#include "speech/phrasesplitter.h"

/**
 * CorrectionTracker — la frappe T.140 d'un port, découpée en phrases à dire,
 * avec ses corrections (SPEC SPEECH-PROCESSOR §7.5).
 *
 * Les phrases sont coupées comme dans speech-server (PhraseSplitter), avec un
 * silence de frappe à `silenceMs`. Un retour arrière qui ne touche que du
 * texte pas encore dit l'efface. Un retour arrière qui remonte dans du texte
 * déjà dit ouvre une CORRECTION :
 *  - l'action Stop part tout de suite, pour couper ce qui reste à dire ;
 *  - les retours arrière suivants effacent la ligne, la frappe la complète ;
 *  - en fin de ligne, ou après `silenceMs` sans frappe, la ligne corrigée
 *    part en une phrase, précédée de l'annonce si elle est demandée.
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

	static constexpr int DefaultSilenceMs = 1200;

	CorrectionTracker(int silenceMs, bool announce, const std::string& announcement = "Correction : ");

	std::vector<Action> Add(const std::string& utf8, uint64_t nowMs);
	std::vector<Action> Tick(uint64_t nowMs);

	// Oublie la ligne en cours : ce qui était tapé ne sera pas dit.
	void Clear();

	bool IsCorrecting() const	{ return correcting; }

private:
	void Speak(std::vector<Action>& actions, const std::string& phrase, const std::string& after);
	void FinishCorrection(std::vector<Action>& actions, bool endOfLine);

	const int		silenceMs;
	const bool		announce;
	const std::string	announcement;
	PhraseSplitter		splitter;
	// La ligne en cours, telle qu'elle a déjà été dite
	std::string		lineSaid;
	bool			correcting = false;
	std::string		retyped;
	uint64_t		lastTypedMs = 0;
};

#endif
