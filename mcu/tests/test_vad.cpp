/**
 * test_vad.cpp — détection de voix (VAD), sur libfvad.
 *
 * La VAD ne coupe pas d'audio : elle désigne qui parle. Son 0/1 est cumulé par
 * `PipeAudioOutput::PlayFrame`, et c'est ce cumul qui choisit le locuteur affiché
 * en mosaïque. Trois choses sont donc gardées ici, et aucune ne se lit dans les
 * signatures :
 *
 *  - le silence numérique n'est PAS voisé, la parole l'est, aux quatre fréquences
 *    que fvad accepte — 48 kHz compris. `vad.h` déclarait autrefois 48 kHz non
 *    supporté alors que `vad.cpp` l'acceptait : la VAD ne tournait jamais sur les
 *    jambes à 48 kHz ;
 *  - `SetMode()` a un effet RÉEL. C'est ce qui avait été perdu sur Debian, où
 *    l'APM 1.x de webrtc-audio-processing n'exposait plus le réglage
 *    d'agressivité : le mode s'écrivait, sans rien changer. Un tel test échoue
 *    si `SetMode()` redevient un no-op ;
 *  - la fréquence se change en cours de route sur la MÊME instance. fvad la porte
 *    sur l'instance, pas sur l'appel.
 *
 * Les signaux sont synthétiques et déterministes : aucun `rand()`, donc aucune
 * dépendance à l'implémentation de la libc.
 */
#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "vad.h"

namespace {

//Parole de synthese : 12 harmoniques d'un fondamental a 120 Hz. Le detecteur
//WebRTC decide sur la repartition d'energie en six bandes, pas sur une forme
//d'onde : une telle somme d'harmoniques lui presente le meme profil qu'un son
//voise.
static std::vector<SWORD> Parole(DWORD rate, DWORD ms, double amplitude)
{
	std::vector<SWORD> buffer(rate*ms/1000);
	for (size_t i = 0; i < buffer.size(); i++)
	{
		double t = (double)i/rate;
		double s = 0;
		for (int h = 1; h <= 12; h++)
			s += sin(2*M_PI*120*h*t)/h;
		buffer[i] = (SWORD)(amplitude*s);
	}
	return buffer;
}

static std::vector<SWORD> Silence(DWORD rate, DWORD ms)
{
	return std::vector<SWORD>(rate*ms/1000, 0);
}

//Joue le buffer par trames de 10 ms et compte les trames voisees. C'est la
//granularite reelle : CalcVad agrege par OU logique sur ce qu'on lui passe.
static int CompterTramesVoisees(VAD& vad, const std::vector<SWORD>& buffer, DWORD rate)
{
	DWORD chunk = rate/100;
	int voisees = 0;
	for (size_t off = 0; off + chunk <= buffer.size(); off += chunk)
		if (vad.CalcVad(const_cast<SWORD*>(&buffer[off]), chunk, rate))
			voisees++;
	return voisees;
}

const DWORD FREQUENCES[] = { 8000, 16000, 32000, 48000 };

//Traine du detecteur : apres de la parole, il continue de declarer voise un
//silence numerique pendant 100 ms en mode agressif (celui du mediaserver) et
//150 ms en mode qualite. Mesure sur le commit epingle du sous-module, identique
//aux quatre frequences. C'est du lissage, pas un defaut : en mosaique, la
//fenetre du locuteur ne clignote pas entre deux syllabes.
const DWORD TRAINE_MS = 150;

TEST(Vad, LeSilenceNEstPasVoise)
{
	for (DWORD rate : FREQUENCES)
	{
		VAD vad;
		std::vector<SWORD> silence = Silence(rate, 500);
		EXPECT_EQ(0, CompterTramesVoisees(vad, silence, rate))
			<< "silence declare voise a " << rate << " Hz";
	}
}

TEST(Vad, LaParoleEstVoiseeAuxQuatreFrequences)
{
	for (DWORD rate : FREQUENCES)
	{
		VAD vad;
		std::vector<SWORD> parole = Parole(rate, 500, 6000);
		EXPECT_GT(CompterTramesVoisees(vad, parole, rate), 40)
			<< "parole non detectee a " << rate << " Hz";
	}
}

//Le piege corrige : les deux fonctions doivent s'accorder sur la meme liste.
TEST(Vad, IsRateSupportedEtCalcVadSAccordent)
{
	VAD vad;
	for (DWORD rate : FREQUENCES)
	{
		EXPECT_TRUE(vad.IsRateSupported(rate)) << rate << " Hz devrait etre supporte";
		std::vector<SWORD> parole = Parole(rate, 100, 6000);
		EXPECT_EQ(1, vad.CalcVad(&parole[0], parole.size(), rate))
			<< "CalcVad refuse " << rate << " Hz que IsRateSupported accepte";
	}

	//44100 Hz n'est pas une frequence native du detecteur
	EXPECT_FALSE(vad.IsRateSupported(44100));
	std::vector<SWORD> parole = Parole(44100, 100, 6000);
	EXPECT_EQ(0, vad.CalcVad(&parole[0], parole.size(), 44100));
}

//Ce test est le garde-fou de la migration : si SetMode() redevient sans effet,
//les deux comptes deviennent egaux et il echoue.
TEST(Vad, LeModeChangeLaDecisionSurUnSignalLimite)
{
	//Amplitude choisie au bord de la decision : trop faible pour un mode
	//agressif, suffisante pour le mode qualite.
	const double LIMITE = 640;

	VAD qualite;
	ASSERT_TRUE(qualite.SetMode(VAD::QUALITY));
	int voiseesQualite = CompterTramesVoisees(qualite, Parole(8000, 1000, LIMITE), 8000);

	VAD agressive;
	ASSERT_TRUE(agressive.SetMode(VAD::VERYAGGRESIVE));
	int voiseesAgressive = CompterTramesVoisees(agressive, Parole(8000, 1000, LIMITE), 8000);

	EXPECT_GT(voiseesQualite, voiseesAgressive)
		<< "SetMode() sans effet : " << voiseesQualite << " trames en QUALITY, "
		<< voiseesAgressive << " en VERYAGGRESIVE";
}

//fvad porte la frequence sur l'instance. Sans la reecrire au changement, une
//trame de 480 echantillons serait lue comme 60 ms de 8 kHz, longueur invalide,
//et la decision serait perdue en silence.
TEST(Vad, LaFrequenceChangeSurLaMemeInstance)
{
	VAD vad;

	std::vector<SWORD> a8k = Parole(8000, 100, 6000);
	ASSERT_EQ(1, vad.CalcVad(&a8k[0], a8k.size(), 8000));

	std::vector<SWORD> a48k = Parole(48000, 100, 6000);
	EXPECT_EQ(1, vad.CalcVad(&a48k[0], a48k.size(), 48000));

	//Et le retour : une trame de 80 echantillons serait une longueur invalide
	//si l'instance etait restee a 48 kHz.
	EXPECT_EQ(1, vad.CalcVad(&a8k[0], a8k.size(), 8000));
}

TEST(Vad, UneTramePlusCourteQueDixMillisecondesEstIgnoree)
{
	VAD vad;
	std::vector<SWORD> parole = Parole(8000, 5, 6000);
	EXPECT_EQ(0, vad.CalcVad(&parole[0], parole.size(), 8000));
}

TEST(Vad, GetVadRendLaDerniereDecision)
{
	VAD vad;

	std::vector<SWORD> parole = Parole(8000, 100, 6000);
	ASSERT_EQ(1, vad.CalcVad(&parole[0], parole.size(), 8000));
	EXPECT_EQ(1, vad.GetVAD());

	//Trame a trame, comme le fait PipeAudioOutput : un seul appel portant tout
	//le silence rendrait 1, CalcVad agregeant par OU logique et la traine
	//couvrant ses premieres trames.
	std::vector<SWORD> silence = Silence(8000, TRAINE_MS + 100);
	CompterTramesVoisees(vad, silence, 8000);
	EXPECT_EQ(0, vad.GetVAD());
}

} // namespace
