/*
 * File:   videopipe.cpp
 * Author: Sergio
 *
 * Created on 19 de marzo de 2013, 16:08
 */

#include "videopipe.h"
#include "log.h"
#include "tools.h"
#include <stdlib.h>

VideoPipe::VideoPipe()
{
	//No estamos iniciados (inited : init par défaut dans le header — le ctor
	//historique ne l'initialisait PAS, lecture de mémoire indéterminée)
	capturing = false;
	imgNew = 0;
	videoWidth = 0;
	videoHeight = 0;
	videoSize = 0;
	videoFPS = 0;
	inputWidth  = 0;
        inputHeight = 0;

}

VideoPipe::~VideoPipe()
{
}

int VideoPipe::Init()
{
	Log("VideoPipe init\n");

	//Iniciamos
	Locked([this] { inited = true; });

	return true;
}

int VideoPipe::End()
{
	//Terminamos
	Locked([this] { inited = false; });

	//Réveiller un grab en cours (il relivrera `last`, cf. sémantique)
	Signal();

	return true;
}

int VideoPipe::StartVideoCapture(int width,int height,int fps)
{
	Log("-StartVideoCapture [%d,%d,%d]\n",width,height,fps);

	Locked([&] {
		//Almacenamos el tama�o
		videoWidth = width;
		videoHeight = height;
		videoSize = (videoWidth*videoHeight*3)/2;
		videoFPS = fps;

		//El inicio
		imgNew = false;
		last = nullptr;

		//Estamos capturando
		capturing = true;
	});

	return true;
}

int VideoPipe::StopVideoCapture()
{
	Log("-StopVideoCapture\n");

	Locked([this] {
		//Y no estamos capturando
		capturing = false;

		//Clear flags
		last = nullptr;
		imgNew = false;
	});

	return true;
}

PictPtr VideoPipe::GrabFrame(DWORD timeout)
{
	PictPtr pic;

	//Si no estamos iniciados
	if (!Locked([this] { wakeGrab = false; return (bool)inited; }))
	{
		//Logeamos
		Error("VideoPipe no inited, grab failed\n");
		//Salimos
		return nullptr;
	}

	//Attendre une nouvelle image, un CancelGrabFrame ou un End ; le timeout
	//échu relivre `last` (gel d'image, cf. sémantique en tête de classe)
	WaitUntil(timeout, [this] { return imgNew != 0 || wakeGrab || !inited; });

	Locked([&] {
		//Lo vamos a consumir
		imgNew=0;

		//Nos quedamos con la referencia antes de que la cambien (partage refcompté)
		pic=last;
	});

	return pic;
}

void  VideoPipe::CancelGrabFrame()
{
	Locked([this] {
		//No image
		imgNew = false;
		last = nullptr;

		//Réveil explicite du grab en cours
		wakeGrab = true;
	});

	//Se�alamos
	Signal();
}

DWORD VideoPipe::GetBufferSize()
{
	return (videoWidth*videoHeight*3)/2;
}

int VideoPipe::SetVideoSize(int width, int height)
{
	//Set current values
	if ( width != inputWidth && height != inputHeight )
	{
		Log("VideoPipe: size changed to %d x %d\n", width, height );
		inputWidth  = width;
		inputHeight = height;
                sizeChanged = true;
	}
	return 0;
}

int VideoPipe::NextFrame(PictPtr pic)
{
	if (!pic || !pic->GetAVFrame())
		return 0;

	bool delivered = Locked([&] {
		//Si estamos capturamos
		if (!capturing)
			return false;

		// Mise à l'échelle vers la taille de sortie (le rescaler rend une
		// référence si la trame est déjà à la bonne taille). Remplace
		// FrameScaler ; graphe avfilter persistant.
		last = resizer.Rescale(pic, videoWidth, videoHeight, false);

		//Hay imagen
		imgNew = true;
		return true;
	});

	//Se�alamos (hors verrou de travail : Signal reprend celui de Wait)
	if (delivered)
		Signal();

	return 1;
}
