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

std::string Repeat(const std::string& text, int count)
{
	std::string out;
	for (int i = 0; i < count; ++i)
		out += text;
	return out;
}

}

TEST(CorrectionTracker, UnPointDeclencheDesQuIlEstTape)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_TRUE(Type(tracker, "Bonjour", 0).empty());
	EXPECT_EQ(std::vector<Action>{Say("Bonjour.")}, Type(tracker, ".", 10));
	EXPECT_TRUE(Type(tracker, " ", 20).empty()) << "le blanc qui suit ne redit rien";
}

TEST(CorrectionTracker, LesAutresPonctuationsFinalesDeclenchentAussi)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_EQ(std::vector<Action>{Say("Ça va ?")}, Type(tracker, "Ça va ?", 0));
	EXPECT_EQ(std::vector<Action>{Say("Oui !")}, Type(tracker, " Oui !", 10));
	EXPECT_EQ(std::vector<Action>{Say("Bof\xE2\x80\xA6")}, Type(tracker, " Bof\xE2\x80\xA6", 20));
	EXPECT_EQ(std::vector<Action>{Say("Voici la liste :")}, Type(tracker, " Voici la liste :", 30));
}

// 14:32:46 le 2026-10-21 : « l'a », 1,2 s de pause, et la synthèse disait
// « Il avait l'a » puis « ir fermé. ».
TEST(CorrectionTracker, UnSilenceNeCoupeJamaisUnMot)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_TRUE(Type(tracker, "Il avait l'a", 0).empty());
	EXPECT_TRUE(tracker.Tick(60000).empty());
	EXPECT_EQ(std::vector<Action>{Say("Il avait l'air fermé.")}, Type(tracker, "ir fermé.", 61000));
}

TEST(CorrectionTracker, PlusDeCinqMotsPartentSurLeBlanc)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_TRUE(Type(tracker, "un deux trois quatre cinq ", 0).empty()) << "cinq mots attendent";
	EXPECT_TRUE(Type(tracker, "six", 10).empty()) << "un mot en cours ne part pas";
	EXPECT_EQ(std::vector<Action>{Say("un deux trois quatre cinq six")}, Type(tracker, " ", 20));
	EXPECT_TRUE(Type(tracker, "sept huit ", 30).empty()) << "le compte repart de zéro";
}

TEST(CorrectionTracker, UneFinDeLigneDitLeReste)
{
	CorrectionTracker tracker(2000, true);
	Type(tracker, "a demain", 0);
	EXPECT_EQ(std::vector<Action>{Say("a demain")}, Type(tracker, LS, 10));
	EXPECT_TRUE(Type(tracker, LS, 20).empty()) << "une ligne vide ne dit rien";
}

TEST(CorrectionTracker, UnePonctuationSeuleNEstPasUnePhrase)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_EQ(std::vector<Action>{Say("Ah.")}, Type(tracker, "Ah.", 0));
	EXPECT_TRUE(Type(tracker, ".", 10).empty());
	EXPECT_TRUE(Type(tracker, " .", 20).empty());
}

TEST(CorrectionTracker, UnRetourArriereSurDuTextePasEncoreDitNeCorrigeRien)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_TRUE(Type(tracker, "je sus" + BS + "is", 0).empty());
	EXPECT_EQ(std::vector<Action>{Say("je suis")}, Type(tracker, LS, 10));
	EXPECT_FALSE(tracker.IsCorrecting());
}

TEST(CorrectionTracker, LeRetourArriereDitCorrectionPuisLaPhraseStable)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_EQ(std::vector<Action>{Say("deux pizas.")}, Type(tracker, "deux pizas.", 0));

	// Le premier retour arrière remonte dans la phrase dite : stop et annonce
	EXPECT_EQ((std::vector<Action>{Stop(), Say("Correction")}), Type(tracker, BS, 100));
	EXPECT_TRUE(tracker.IsCorrecting());
	EXPECT_TRUE(Type(tracker, Repeat(BS, 5), 200).empty());

	// La ponctuation ne déclenche rien tant que la ligne bouge
	EXPECT_TRUE(Type(tracker, "pizzas pour ce soir.", 300).empty());
	EXPECT_TRUE(tracker.Tick(2299).empty());
	EXPECT_EQ(std::vector<Action>{Say("deux pizzas pour ce soir.")}, tracker.Tick(2300));
	EXPECT_FALSE(tracker.IsCorrecting());
	EXPECT_TRUE(tracker.Tick(9000).empty());
}

TEST(CorrectionTracker, SeuleLaPhraseToucheeEstRedite)
{
	CorrectionTracker tracker(2000, true);
	Type(tracker, "Il fait beau. Il pleut.", 0);
	Type(tracker, Repeat(BS, 6), 100);
	Type(tracker, "neige.", 200);
	EXPECT_EQ(std::vector<Action>{Say("Il neige.")}, tracker.Tick(2200));
}

TEST(CorrectionTracker, UnRetourArrierePlusLoinElargitLaPhraseRedite)
{
	CorrectionTracker tracker(2000, false);
	Type(tracker, "Il fait beau. Il pleut.", 0);
	EXPECT_EQ(std::vector<Action>{Stop()}, Type(tracker, BS, 100)) << "sans annonce";
	Type(tracker, Repeat(BS, 14), 200);
	Type(tracker, "chaud. Il vente.", 300);
	EXPECT_EQ(std::vector<Action>{Say("Il fait chaud. Il vente.")}, tracker.Tick(2300));
}

TEST(CorrectionTracker, UneFinDeLigneClotLaCorrectionTouteDeSuite)
{
	CorrectionTracker tracker(2000, true);
	Type(tracker, "il fait bau.", 0);
	Type(tracker, Repeat(BS, 3), 100);
	Type(tracker, "eau.", 200);
	EXPECT_EQ(std::vector<Action>{Say("il fait beau.")}, Type(tracker, LS, 300));
	EXPECT_FALSE(tracker.IsCorrecting());
}

// 14:33:25 le 2026-10-21 : le blanc tapé après une phrase dite était oublié, et
// sept retours arrière en effaçaient un de trop : « l'airun peu fermé ».
TEST(CorrectionTracker, LaLigneEstCelleQueVoitLAppelant)
{
	CorrectionTracker tracker(2000, false);
	Type(tracker, "Il avait l'air fermé.", 0);
	Type(tracker, " ", 100);
	Type(tracker, Repeat(BS, 7), 200);
	Type(tracker, "un peu fermé.", 300);
	EXPECT_EQ(std::vector<Action>{Say("Il avait l'air un peu fermé.")}, tracker.Tick(2300));
}

TEST(CorrectionTracker, UnRetourArriereEnDebutDeLigneEstSansEffet)
{
	CorrectionTracker tracker(2000, true);
	EXPECT_EQ(std::vector<Action>{Say("fin")}, Type(tracker, "fin" + LS, 0));
	EXPECT_TRUE(Type(tracker, BS, 10).empty()) << "la ligne précédente est close";
	EXPECT_FALSE(tracker.IsCorrecting());
}

TEST(CorrectionTracker, ClearOublieLaLigne)
{
	CorrectionTracker tracker(2000, true);
	Type(tracker, "dit. pas dit", 0);
	tracker.Clear();
	EXPECT_TRUE(Type(tracker, LS, 5000).empty());
	EXPECT_TRUE(Type(tracker, BS, 5000).empty()) << "plus rien de dit sur la ligne";
}
