#ifndef _PIPEAUDIOINPUT_H_
#define _PIPEAUDIOINPUT_H_
#include <mutex>
#include <condition_variable>
#include <audio.h>
#include <fifo.h>

// Rééchantillonnage via libswresample (remplace l'ancien AudioTransrater/speexdsp).
// Pointeur opaque : l'en-tête ffmpeg n'est inclus que dans le .cpp.
struct SwrContext;


class PipeAudioInput : 
	public AudioInput
{
public:
	PipeAudioInput();
	~PipeAudioInput();
	virtual int RecBuffer(SWORD *buffer,DWORD size);
	virtual void CancelRecBuffer();
	virtual int StartRecording(DWORD rate);
	virtual int StopRecording();

	virtual DWORD GetNativeRate()		{ return nativeRate;	}
	virtual DWORD GetRecordingRate()	{ return recordRate;	}
	
	int Init(DWORD rate);
	int PutSamples(SWORD *buffer,DWORD size);
	int End();

private:
	//Los mutex y condiciones
	std::mutex		mutex;
	std::condition_variable	cond;

	//Members
	fifo<SWORD,4096>	fifoBuffer;
	int		recording;
	int 		inited;
	int		canceled;
	
	
	SwrContext		*swr;
	DWORD			recordRate;
	DWORD			nativeRate;
};

#endif
