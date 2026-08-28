#ifndef _AUDIORESAMPLER_H_
#define _AUDIORESAMPLER_H_
#include <audio.h>

// Rééchantillonnage via libswresample. Pointeur opaque : l'en-tête ffmpeg
// n'est inclus que dans le .cpp.
struct SwrContext;

// Rééchantillonneur mono S16, ouvert paresseusement sur la fréquence portée par
// la trame et sur celle que le consommateur demande. Sorti de PipeAudioInput au
// lot 3 de `jsr309_transcode_sans_thread.md` : le transcodeur audio n'a plus de
// file entre son décodeur et son encodeur, mais il a toujours besoin de la
// conversion. Non copiable (possède un contexte ffmpeg).
class AudioResampler
{
public:
	AudioResampler();
	~AudioResampler();

	AudioResampler(const AudioResampler&)            = delete;
	AudioResampler& operator=(const AudioResampler&) = delete;

	// Rend `samples` converti à `outRate`. La trame telle quelle si les
	// fréquences coïncident ou si `outRate` vaut 0 ; nullptr en cas d'échec.
	// La trame fait foi : si SA fréquence change, le contexte est rouvert.
	SamplesPtr Resample(SamplesPtr samples, DWORD outRate);

	// Ferme le contexte : la prochaine trame rouvre.
	void Reset();

private:
	SwrContext	*swr;
	DWORD		inRate;		// fréquence d'entrée du contexte ouvert
	DWORD		outRate;	// fréquence de sortie du contexte ouvert
};

#endif
