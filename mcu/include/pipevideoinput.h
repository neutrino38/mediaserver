#ifndef _PIPEVIDEOINPUT_H_
#define _PIPEVIDEOINPUT_H_

#include <condition_variable>
#include <mutex>
#include <video.h>
#include "videorescaler.h"

class PipeVideoInput
	: public VideoInput
{
public:
	PipeVideoInput();
	virtual ~PipeVideoInput();

	virtual int   StartVideoCapture(int width,int height,int fps);
	virtual PictPtr GrabFrame(DWORD timeout);
	virtual void  CancelGrabFrame();
	virtual DWORD GetBufferSize();
	virtual int   StopVideoCapture();

	int Init();
	// Publie une trame (mise à l'échelle vers videoWidth x videoHeight si besoin).
	int SetFrame(PictPtr pic);
	int End();

private:
	PictPtr last;
	VideoRescaler resizer;
	int videoWidth;
	int videoHeight;
	int videoSize;
	int videoFPS;
	int imgNew;
	int inited;
	int capturing;

	std::mutex newPicMutex;
	std::condition_variable newPicCond;
};

#endif
