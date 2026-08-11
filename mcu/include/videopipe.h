/*
 * File:   videopipe.h
 * Author: Sergio
 *
 * Created on 19 de marzo de 2013, 16:08
 */

#ifndef VIDEOPIPE_H
#define	VIDEOPIPE_H

#include "video.h"
#include "videorescaler.h"
#include "wait.h"

//Pont poussé/tiré entre mixeur et encodeur vidéo, bâti sur la primitive Wait
//(cf. wait.h). SÉMANTIQUE PIÈGE préservée (test_wait_sites.cpp) : sur timeout
//sans nouvelle image, GrabFrame RELIVRE la dernière trame (gel d'image) ;
//End() pendant un grab rend encore `last`, le NULL n'arrive qu'au grab
//suivant.
class VideoPipe :
	public VideoOutput,
	public VideoInput,
	protected ::Wait
{
public:
	VideoPipe();
	~VideoPipe();
	int Init();
	/** VideoInput */
	virtual int   StartVideoCapture(int width,int height,int fps);
	virtual PictPtr GrabFrame(DWORD timeout);
	virtual void  CancelGrabFrame();
	virtual DWORD GetBufferSize();
	virtual int   StopVideoCapture();
	/** VideoOutput */
	virtual int NextFrame(PictPtr pic);
	virtual int SetVideoSize(int width,int height);
	int End();
        virtual DWORD GetNativeWidth() { return inputWidth; }
        virtual DWORD GetNativeHeight() { return inputHeight; }


private:

	PictPtr last;
	VideoRescaler resizer;
	int videoWidth;
	int videoHeight;
	int inputWidth;
	int inputHeight;
	int videoSize;
	int videoFPS;
	int imgNew;
	int inited = false;
	int capturing;
	//Réveil explicite du grab en cours (CancelGrabFrame), consommé à l'entrée
	bool wakeGrab = false;
};

#endif	/* VIDEOPIPE_H */
