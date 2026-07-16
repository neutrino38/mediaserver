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
	//Inicializamos los mutex
	pthread_mutex_init(&newPicMutex,0);
	pthread_cond_init(&newPicCond,0);

	//No estamos iniciados
	//inited = false;
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
	//Liberamos los mutex
	pthread_mutex_destroy(&newPicMutex);
	pthread_cond_destroy(&newPicCond);
}

int VideoPipe::Init()
{
	Log("VideoPipe init\n");

	//Protegemos
	pthread_mutex_lock(&newPicMutex);

	//Iniciamos
	inited = true;

	//Protegemos
	pthread_mutex_unlock(&newPicMutex);

	return true;
}

int VideoPipe::End()
{
	//Protegemos
	pthread_mutex_lock(&newPicMutex);

	//Terminamos
	inited = false;

	//Se�alizamos la condicion
	pthread_cond_signal(&newPicCond);

	//Protegemos
	pthread_mutex_unlock(&newPicMutex);

	return true;
}

int VideoPipe::StartVideoCapture(int width,int height,int fps)
{
	Log("-StartVideoCapture [%d,%d,%d]\n",width,height,fps);

	//Protegemos
	pthread_mutex_lock(&newPicMutex);

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

	//Desprotegemos
	pthread_mutex_unlock(&newPicMutex);

	return true;
}

int VideoPipe::StopVideoCapture()
{
	Log("-StopVideoCapture\n");

	//Protegemos
	pthread_mutex_lock(&newPicMutex);

	//Y no estamos capturando
	capturing = false;

	//Clear flags
	last = nullptr;
	imgNew = false;

	//Desprotegemos
	pthread_mutex_unlock(&newPicMutex);

	return true;
}

PictPtr VideoPipe::GrabFrame(DWORD timeout)
{
	PictPtr pic;

	//Bloqueamos para ver si hay un nuevo picture
	pthread_mutex_lock(&newPicMutex);

	//Si no estamos iniciados
	if (!inited)
	{
		//Logeamos
		Error("VideoPipe no inited, grab failed\n");
		//Desbloqueamos
		pthread_mutex_unlock(&newPicMutex);
		//Salimos
		return nullptr;
	}

	//Miramos a ver si hay un nuevo pict
	if(imgNew==0)
	{
		//If timeout has been specified
		if (timeout)
		{
			timespec   ts;
			//Calculate timeout
			calcTimout(&ts,timeout);
			//wait
			pthread_cond_timedwait(&newPicCond,&newPicMutex,&ts);
		} else {
			//Wait ad infinitum
			pthread_cond_wait(&newPicCond,&newPicMutex);
		}
	}

	//Lo vamos a consumir
	imgNew=0;

	//Nos quedamos con la referencia antes de que la cambien (partage refcompté)
	pic=last;

	//Y liberamos el mutex
	pthread_mutex_unlock(&newPicMutex);

	return pic;
}

void  VideoPipe::CancelGrabFrame()
{
	//Protegemos
	pthread_mutex_lock(&newPicMutex);

	//No image
	imgNew = false;
	last = nullptr;

	//Se�alamos
	pthread_cond_signal(&newPicCond);

	//Unloco mutex
	pthread_mutex_unlock(&newPicMutex);

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

	//Protegemos
	pthread_mutex_lock(&newPicMutex);

	//Si estamos capturamos
	if (capturing)
	{
		// Mise à l'échelle vers la taille de sortie (le rescaler rend une
		// référence si la trame est déjà à la bonne taille). Remplace
		// FrameScaler ; graphe avfilter persistant.
		last = resizer.Rescale(pic, videoWidth, videoHeight, false);

		//Hay imagen
		imgNew = true;
		//Se�alamos
		pthread_cond_signal(&newPicCond);
	}

	//Y desbloqueamos
	pthread_mutex_unlock(&newPicMutex);

	return 1;
}
