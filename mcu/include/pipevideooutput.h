#ifndef _PIPVIDEOOUTPUT_H_
#define _PIPVIDEOOUTPUT_H_
#include <condition_variable>
#include <mutex>
#include <video.h>

class PipeVideoOutput :
	public VideoOutput
{
public:
	//Verrou et condition PARTAGÉS avec le VideoMixer propriétaire
	PipeVideoOutput(std::mutex* mutex, std::condition_variable* cond);
	virtual ~PipeVideoOutput();

	virtual int NextFrame(PictPtr pic);
	virtual int SetVideoSize(int width,int height);

	// Dernière trame publiée (partage refcompté, plus de memcpy).
	PictPtr	GetFrame();
	int	IsChanged(DWORD version);
	int 	GetWidth()	{ return videoWidth;		};
	int 	GetHeight()	{ return videoHeight;		};
	int	Init();
	int	End();

	bool	SizeHasChanged(DWORD version);

private:
	PictPtr	last;
	int 	videoWidth;
	int	videoHeight;
	bool	isChanged;
	bool	versionChanged;
	bool	sizeChange;
	int 	inited;
	DWORD	version;

	std::mutex*		 videoMixerMutex;
	std::condition_variable* videoMixerCond;
};

#endif
