#ifndef _AUDIOOUTPUT_H_
#define _AUDIOOUTPUT_H_
#include <mutex>
#include <fifo.h>
#include <audio.h>
#include "vad.h"

// Rééchantillonnage via libswresample (remplace l'ancien AudioTransrater/speexdsp).
// Pointeur opaque : l'en-tête ffmpeg n'est inclus que dans le .cpp.
struct SwrContext;

class PipeAudioOutput :
	public AudioOutput
	
{
public:
	PipeAudioOutput(bool calcVAD);
	virtual ~PipeAudioOutput();
	virtual int PlayBuffer(SWORD *buffer,DWORD size,DWORD frameTime);
	virtual int StartPlaying(DWORD samplerate);
	virtual int StopPlaying();

	virtual DWORD GetNativeRate()		{ return nativeRate;	}
	virtual DWORD GetPlayingRate()		{ return playRate;	}

	int GetSamples(SWORD *buffer,DWORD size,bool min_len=false);
	DWORD GetVAD(DWORD numSamples);
	int Init(DWORD samplerate);
	int End();
private:
	//Mutex
	std::mutex	mutex;

	//Members
	fifo<SWORD,8192>	fifoBuffer;
	int			inited;
	VAD			vad;
	DWORD			acu;
	bool			calcVAD;
	SwrContext		*swr;

	DWORD	playRate;
	DWORD	nativeRate;
};

#endif
