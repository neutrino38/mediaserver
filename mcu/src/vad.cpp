#include "vad.h"
#include "log.h"

/*
 * fvad rend une decision par trame de 10, 20 ou 30 ms, en int16 mono.
 * CalcVad decoupe le buffer recu en trames de 10 ms et agrege la decision
 * (OU logique), de facon a renvoyer le meme 0/1 par appel que les
 * implementations precedentes.
 */

VAD::VAD()
{
	//No vad decision yet
	last = 0;
	//Aucune frequence posee sur l'instance (fvad prend 8000 Hz par defaut)
	sampleRate = 0;

	//Create the fvad instance
	fvad = fvad_new();
	if (!fvad)
	{
		Error("VAD: could not create fvad instance.\n");
		return;
	}

	//Set aggressive mode (comportement historique)
	SetMode(VERYAGGRESIVE);
}

VAD::~VAD()
{
	if (fvad)
		fvad_free(fvad);
}

int VAD::CalcVad(SWORD* buffer,DWORD size,DWORD rate)
{
	//Check we have an instance
	if (!fvad)
		return 0;

	//Only these four rates are accepted by fvad
	if (!IsRateSupported(rate))
		return Error("VAD: Cannot use sample rate = %u for VAD.\n", rate);

	//La frequence est portee par l'instance, pas par l'appel : on ne la reecrit
	//que lorsqu'elle change. fvad_set_sample_rate ne touche pas a l'etat du
	//detecteur, elle ne fait que choisir la fonction de decision.
	if (rate != sampleRate)
	{
		if (fvad_set_sample_rate(fvad,(int)rate)!=0)
			return Error("VAD: fvad rejected sample rate = %u.\n", rate);
		sampleRate = rate;
	}

	//Number of samples in a 10ms mono frame
	DWORD chunk = rate / 100;

	//Not enough data for a single frame
	if (size < chunk)
		return 0;

	int voice = 0;

	//Process the buffer in 10ms chunks (le reliquat < 10ms est ignore). On ne
	//sort pas de la boucle des qu'une trame est voisee : le detecteur est a
	//etat, et sauter des trames fausserait son estimation du bruit.
	for (DWORD off = 0; off + chunk <= size; off += chunk)
		//Accumulate voice decision
		if (fvad_process(fvad,buffer+off,chunk) > 0)
			voice = 1;

	//Store and return
	last = voice;
	return voice;
}

bool VAD::SetMode(Mode mode)
{
	if (!fvad)
		return false;

	//L'enum a les memes valeurs ET les memes noms que les modes fvad :
	//0 quality, 1 low bitrate, 2 aggressive, 3 very aggressive.
	return fvad_set_mode(fvad,(int)mode) == 0;
}

int VAD::GetVAD()
{
	return last;
}
