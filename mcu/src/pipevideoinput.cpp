#include "log.h"
#include "pipevideoinput.h"
#include "tools.h"
#include <stdlib.h>

PipeVideoInput::PipeVideoInput()
{
	//inited = false;
	capturing = false;
	imgNew = 0;
	videoWidth = 0;
	videoHeight = 0;
	videoSize = 0;
	videoFPS = 0;
}

PipeVideoInput::~PipeVideoInput()
{
}

int PipeVideoInput::Init()
{
	Log("PipeVideoInput init\n");

	//Protegemos
	std::lock_guard<std::mutex> lock(newPicMutex);

	//Iniciamos
	inited = true;

	return true;
}

int PipeVideoInput::End()
{
	//Protegemos
	{
		std::lock_guard<std::mutex> lock(newPicMutex);

		//Terminamos
		inited = false;
	}

	//Se�alizamos la condicion
	newPicCond.notify_all();

	return true;
}

int PipeVideoInput::StartVideoCapture(int width,int height,int fps)
{
	Log("-StartVideoCapture [%d,%d,%d]\n",width,height,fps);

	//Protegemos
	std::lock_guard<std::mutex> lock(newPicMutex);

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

	return true;
}

int PipeVideoInput::StopVideoCapture()
{
	Log("-StopVideoCapture\n");

	//Protegemos
	std::lock_guard<std::mutex> lock(newPicMutex);

	//Y no estamos capturando
	capturing = false;

	//Clear flags
	last = nullptr;
	imgNew = false;

	return true;
}

PictPtr PipeVideoInput::GrabFrame(DWORD timeout)
{
	//Bloqueamos para ver si hay un nuevo picture
	std::unique_lock<std::mutex> lock(newPicMutex);

	//Si no estamos iniciados
	if (!inited || !capturing)
	{
		//Logeamos
		Error("PipeVideoInput no inited or not capturing, grab failed\n");
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
			if (newPicCond.wait_until(lock, std::chrono::system_clock::from_time_t(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec)) == std::cv_status::timeout)
			{
				//Timeout
				Error("PipeVideoInput grab timeout\n");
				return nullptr;
			}
		} else {
			//Wait ad infinitum
			newPicCond.wait(lock);
		}
	}

	//Lo vamos a consumir
	imgNew=0;

	//Devolvemos la ultima trama (partage refcompté : survit tant que l'encodeur la tient)
	return last;
}

void  PipeVideoInput::CancelGrabFrame()
{
	//Protegemos
	std::lock_guard<std::mutex> lock(newPicMutex);

	//No image
	imgNew = false;
	last = nullptr;

	//Se�alamos
	newPicCond.notify_all();

	//Unloco mutex
}

DWORD PipeVideoInput::GetBufferSize()
{
	return (videoWidth*videoHeight*3)/2;
}

int PipeVideoInput::SetFrame(PictPtr pic)
{
	if (!pic || !pic->GetAVFrame())
		return 0;

	//Protegemos
	std::lock_guard<std::mutex> lock(newPicMutex);

	//Si estamos capturamos
	if (capturing)
	{
		// Mise à l'échelle vers la taille de l'encodeur (le rescaler rend une
		// simple référence si la trame est déjà à la bonne taille). Remplace
		// FrameScaler ; graphe avfilter persistant.
		last = resizer.Rescale(pic, videoWidth, videoHeight, false);

		//Hay imagen
		imgNew = true;
		//Se�alamos
		newPicCond.notify_all();
	}
	return 1;
}
