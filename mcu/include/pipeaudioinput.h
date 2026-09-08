#ifndef _PIPEAUDIOINPUT_H_
#define _PIPEAUDIOINPUT_H_
#include <mutex>
#include <condition_variable>
#include <deque>
#include <audio.h>
#include "audioresampler.h"


class PipeAudioInput :
	public AudioInput
{
public:
	PipeAudioInput();
	virtual ~PipeAudioInput() = default;

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

	// Durée totale en file, en millisecondes. À appeler sous mutex.
	DWORD QueuedMs() const;

	std::mutex		mutex;
	std::condition_variable	cond;

	std::deque<SamplesPtr>	queue;
	bool		recording;
	bool 		inited;
	bool		canceled;

	// Conversion vers recordRate ; utilisé sous `mutex`. Objet partagé avec
	// l'encodeur audio JSR-309, qui en a besoin sans la file (lot 3).
	AudioResampler		resampler;
	DWORD			recordRate;
	DWORD			nativeRate;
};

#endif
