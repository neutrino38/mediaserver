/**
 * test_textmux.cpp — les règles du multiplexeur de texte (SPEC SPEECH-PROCESSOR
 * §9, §11.2). Classe pure : ni thread, ni horloge réelle, l'heure est donnée.
 *
 * Source 0 : le texte tapé de l'autre côté (relais) ; source 1 : la
 * transcription (ASR). Les noms n'importent pas à la classe.
 */
#include <gtest/gtest.h>

#include <string>
#include "textmux.h"

namespace
{

const std::string LS = "\xE2\x80\xA8";	// U+2028
const std::string BS = "\x08";		// U+0008
const std::string BOM = "\xEF\xBB\xBF";	// U+FEFF

}

TEST(TextMux, UneSourceSeulePasseTelleQuelle)
{
	TextMux mux;
	mux.Push(0, "Bonjour", 0);
	EXPECT_EQ("Bonjour", mux.Pull(0));
	EXPECT_EQ("", mux.Pull(20));
	mux.Push(0, " à tous", 40);
	EXPECT_EQ(" à tous", mux.Pull(40));
}

TEST(TextMux, LAutreSourceAttendLaFinDeLaLigne)
{
	TextMux mux;
	mux.Push(0, "je tape", 0);
	EXPECT_EQ("je tape", mux.Pull(0));
	EXPECT_EQ(0, mux.Holder());

	// La voix arrive en pleine frappe : elle attend.
	mux.Push(1, "on parle", 100);
	EXPECT_EQ("", mux.Pull(100));

	mux.Push(0, " encore" + LS, 200);
	EXPECT_EQ(" encore" + LS + "on parle", mux.Pull(200));
	EXPECT_EQ(1, mux.Holder());
}

TEST(TextMux, LaMainPasseApresUneSecondeDeSilence)
{
	TextMux mux;
	mux.Push(0, "phrase sans fin", 0);
	EXPECT_EQ("phrase sans fin", mux.Pull(0));
	mux.Push(1, "voix", 300);

	EXPECT_EQ("", mux.Pull(999));
	// Le changement de source au milieu d'une ligne la termine.
	EXPECT_EQ(LS + "voix", mux.Pull(1000));
}

TEST(TextMux, PasDeSautDeLigneSurUneLigneVide)
{
	TextMux mux;
	mux.Push(0, "ligne" + LS, 0);
	mux.Push(1, "suite", 0);
	EXPECT_EQ("ligne" + LS + "suite", mux.Pull(0));
}

TEST(TextMux, LeRetourArriereNeTraversePasUneFrontiereDeSource)
{
	TextMux mux;
	mux.Push(1, "voix", 0);
	EXPECT_EQ("voix", mux.Pull(0));
	EXPECT_EQ("", mux.Pull(1000));

	// La frappe reprend la main sur la ligne de la voix : saut de ligne.
	mux.Push(0, "ab", 1000);
	EXPECT_EQ(LS + "ab", mux.Pull(1000));

	// Deux retours arrière effacent "ab" ; le troisième remonterait au-delà
	// du début de la ligne : jeté.
	mux.Push(0, BS + BS + BS + "c", 1100);
	EXPECT_EQ(BS + BS + "c", mux.Pull(1100));
}

TEST(TextMux, UnChangementDeSourceOuvreUneLigneQueLeRetourArriereNeRemontePas)
{
	TextMux mux;
	mux.Push(0, "a", 0);
	EXPECT_EQ("a", mux.Pull(0));
	// La source 0 se tait, la 1 prend la main : le saut de ligne inséré est
	// une frontière que son second retour arrière ne franchit pas.
	mux.Push(1, "x" + BS + BS, 2000);
	EXPECT_EQ(LS + "x" + BS, mux.Pull(2000));
}

TEST(TextMux, RienNeSePerdQuandLesDeuxSourcesSAlternent)
{
	TextMux mux;
	mux.Push(0, "un" + LS + "deux" + LS, 0);
	mux.Push(1, "A" + LS + "B" + LS, 0);

	std::string out;
	for (uint64_t t = 0; t < 5000; t += 20)
		out += mux.Pull(t);

	// La main passe à chaque fin de ligne, dans l'ordre d'attente.
	EXPECT_EQ("un" + LS + "A" + LS + "deux" + LS + "B" + LS, out);
	EXPECT_TRUE(mux.IsEmpty());
}

TEST(TextMux, UnCaractereCoupeEntreDeuxPaquetsEstRecolle)
{
	TextMux mux;
	const std::string e = "\xC3\xA9";	// é
	mux.Push(0, "caf" + e.substr(0, 1), 0);
	EXPECT_EQ("caf", mux.Pull(0));
	mux.Push(0, e.substr(1), 20);
	EXPECT_EQ(e, mux.Pull(20));
}

TEST(TextMux, LeBomNEstPasDuTexte)
{
	TextMux mux;
	mux.Push(0, "a", 0);
	EXPECT_EQ("a", mux.Pull(0));
	mux.Push(1, "voix", 0);
	// Un keep-alive ne prolonge pas la main de la source 0.
	mux.Push(0, BOM, 900);
	EXPECT_EQ(LS + "voix", mux.Pull(1000));
}

TEST(TextMux, UneSourceHorsBornesEstIgnoree)
{
	TextMux mux;
	mux.Push(-1, "x", 0);
	mux.Push(TextMux::Sources, "y", 0);
	EXPECT_EQ("", mux.Pull(0));
	EXPECT_TRUE(mux.IsEmpty());
}
