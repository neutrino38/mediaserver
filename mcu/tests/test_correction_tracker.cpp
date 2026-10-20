/**
 * test_correction_tracker.cpp — la frappe T.140 découpée en phrases à dire,
 * et ses corrections (SPEC SPEECH-PROCESSOR §7.5, §11.4). Classe pure.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "correctiontracker.h"

namespace
{

typedef CorrectionTracker::Action Action;

const std::string BS = "\x08";
const std::string LS = "\xE2\x80\xA8";

Action Say(const std::string& text)	{ return {Action::Speak, text}; }
Action Stop()				{ return {Action::Stop, ""}; }

std::vector<Action> Type(CorrectionTracker& tracker, const std::string& text, uint64_t nowMs)
{
	return tracker.Add(text, nowMs);
}

}

TEST(CorrectionTracker, UnePhrasePartSurUnePonctuationSuivieDUnBlanc)
{
	CorrectionTracker tracker(1200, true);
	EXPECT_TRUE(Type(tracker, "Bonjour", 0).empty());
	EXPECT_TRUE(Type(tracker, ".", 10).empty()) << "la ponctuation seule ne coupe pas";
	EXPECT_EQ(std::vector<Action>{Say("Bonjour.")}, Type(tracker, " ", 20));
}

TEST(CorrectionTracker, UnePhrasePartApresLeSilenceDeFrappe)
{
	CorrectionTracker tracker(1200, true);
	Type(tracker, "un instant", 0);
	EXPECT_TRUE(tracker.Tick(1199).empty());
	EXPECT_EQ(std::vector<Action>{Say("un instant")}, tracker.Tick(1200));
	EXPECT_TRUE(tracker.Tick(5000).empty());
}

TEST(CorrectionTracker, UnRetourArriereSurDuTextePasEncoreDitNeCorrigeRien)
{
	CorrectionTracker tracker(1200, true);
	EXPECT_TRUE(Type(tracker, "je sus" + BS + "is", 0).empty());
	EXPECT_EQ(std::vector<Action>{Say("je suis")}, Type(tracker, LS, 10));
	EXPECT_FALSE(tracker.IsCorrecting());
}

TEST(CorrectionTracker, UnRetourArriereDansDuTexteDitOuvreUneCorrection)
{
	CorrectionTracker tracker(1200, true);
	EXPECT_EQ(std::vector<Action>{Say("deux pizas.")}, Type(tracker, "deux pizas. ", 0));

	// Le premier retour arrière remonte dans la phrase dite : stop tout de suite
	EXPECT_EQ(std::vector<Action>{Stop()}, Type(tracker, BS, 100));
	EXPECT_TRUE(tracker.IsCorrecting());
	EXPECT_TRUE(Type(tracker, BS + BS + BS + BS + BS + BS, 200).empty());

	// La ligne corrigée part en fin de ligne, annoncée
	Type(tracker, "pizzas pour ce soir.", 300);
	EXPECT_EQ(std::vector<Action>{Say("Correction : deux pizzas pour ce soir.")}, Type(tracker, LS, 400));
	EXPECT_FALSE(tracker.IsCorrecting());
}

TEST(CorrectionTracker, UneCorrectionSansFinDeLigneSeFermeSurLeSilence)
{
	CorrectionTracker tracker(1200, false);
	Type(tracker, "il fait bau. ", 0);
	Type(tracker, BS + BS + BS + BS, 100);
	Type(tracker, "eau.", 200);
	EXPECT_TRUE(tracker.Tick(1399).empty());
	EXPECT_EQ(std::vector<Action>{Say("il fait beau.")}, tracker.Tick(1400)) << "sans annonce";
}

TEST(CorrectionTracker, UnRetourArriereEnDebutDeLigneEstSansEffet)
{
	CorrectionTracker tracker(1200, true);
	EXPECT_EQ(std::vector<Action>{Say("fin")}, Type(tracker, "fin" + LS, 0));
	EXPECT_TRUE(Type(tracker, BS, 10).empty()) << "la ligne précédente est close";
	EXPECT_FALSE(tracker.IsCorrecting());
}

TEST(CorrectionTracker, ClearOublieLaLigne)
{
	CorrectionTracker tracker(1200, true);
	Type(tracker, "dit. pas dit", 0);
	tracker.Clear();
	EXPECT_TRUE(tracker.Tick(5000).empty());
	EXPECT_TRUE(Type(tracker, BS, 5000).empty()) << "plus rien de dit sur la ligne";
}
