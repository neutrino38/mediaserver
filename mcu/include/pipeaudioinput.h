#ifndef _PIPEAUDIOINPUT_H_
#define _PIPEAUDIOINPUT_H_
#include <mutex>
#include <condition_variable>
#include <deque>
#include <audio.h>

// Rééchantillonnage via libswresample. Pointeur opaque : l'en-tête ffmpeg
// n'est inclus que dans le .cpp.
struct SwrContext;


class PipeAudioInput :
	public AudioInput
{
public:
	PipeAudioInput();
	virtual ~PipeAudioInput();

	virtual SamplesPtr RecFrame(DWORD timeoutMs);
	virtual void CancelRecFrame();
	virtual int StartRecording(DWORD rate);
	virtual int StopRecording();

	virtual DWORD GetNativeRate()		{ return nativeRate;	}
	virtual DWORD GetRecordingRate()	{ return recordRate;	}

	// Publie une trame. Elle porte sa fréquence : un changement en cours de
	// flux est absorbé ici, sans que personne ait à prévenir le pipe.
	int PutFrame(SamplesPtr samples);
	int End();

	// ADAPTATEUR TRANSITOIRE pour les producteurs encore en SWORD* (mixeur,
	// bridge RTMP). Init() ne sert plus qu'à leur déclarer leur fréquence.
	int Init(DWORD rate);
	int PutSamples(SWORD *buffer,DWORD size);

private:
	// Profondeur de la file, en millisecondes : une trame peut faire 10 ou
	// 120 ms, la borner en nombre de trames ne bornerait pas la latence.
	static const DWORD MaxQueuedMs = 500;

	// Rééchantillonne `samples` vers recordRate. Reconfigure le resampler si
	// la fréquence d'entrée a changé. Rend la trame telle quelle si les
	// fréquences coïncident, nullptr en cas d'échec. À appeler sous mutex.
	SamplesPtr Resample(SamplesPtr samples);

	// Durée totale en file, en millisecondes. À appeler sous mutex.
	DWORD QueuedMs() const;

	std::mutex		mutex;
	std::condition_variable	cond;

	std::deque<SamplesPtr>	queue;
	bool		recording;
	bool 		inited;
	bool		canceled;

	SwrContext		*swr;
	DWORD			swrInRate;	// fréquence d'entrée du resampler ouvert
	DWORD			recordRate;
	DWORD			nativeRate;
};

#endif
