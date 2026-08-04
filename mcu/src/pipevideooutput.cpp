#include "log.h"
#include "pipevideooutput.h"
#include <string.h>
#include <stdlib.h>

PipeVideoOutput::PipeVideoOutput(pthread_mutex_t* mutex, pthread_cond_t* cond)
{
	//Nos quedamos con los mutex
	videoMixerMutex = mutex;
	videoMixerCond  = cond;

	//Ponemos el cambio
	inited		= false;
	isChanged	= false;
	versionChanged	= false;
	version		= -1;
	videoWidth	= 0;
	videoHeight	= 0;
	keepAspect = true;
	sizeChange = false;
}

PipeVideoOutput::~PipeVideoOutput()
{
	// `last` (PictPtr) se libère tout seul.
}

bool PipeVideoOutput::SizeHasChanged(DWORD version)
{
    if (sizeChange)
    {
	// We return true until the version changres
        if (versionChanged) sizeChange = false;
	return true;
    }
    else
    {
        return false;
    }
}

int PipeVideoOutput::NextFrame(PictPtr pic)
{
	//Check pic
	if (!pic || !pic->GetAVFrame())
		return Error("-PipeVideoOuput called with null frame");

	//Check if wer are inited
	if (!inited)
		//Exit
		return Error("-PipeVideoOutput calling NextFrame without been inited\n");

	//Bloqueamos
	pthread_mutex_lock(videoMixerMutex);

	// Publie une référence de la trame (zéro-copie, plus de memcpy).
	last = pic;

	//Ponemos el cambio
	isChanged = true;

	//Se�alizamos
	pthread_cond_signal(videoMixerCond);

	//Y desbloqueamos
	pthread_mutex_unlock(videoMixerMutex);

	return true;
}

int PipeVideoOutput::SetVideoSize(int width,int height)
{
	//Check it it is the same size
	if ((videoWidth==width) && (videoHeight==height))
		//Not changed
		return 0;

	//Lock
	Log("-SetVideoSize: inbound video size changed to %dx%d.\n", width, height);
	pthread_mutex_lock(videoMixerMutex);

	//Store size
	videoWidth = width;
	videoHeight= height;
	sizeChange = true;
	//Unlock
	pthread_mutex_unlock(videoMixerMutex);

	//Changed
	return 1;
}

PictPtr PipeVideoOutput::GetFrame()
{
	//QUitamos el cambio
	isChanged = false;

	//Y devolvemos la ultima trama (partage refcompté)
	return last;
}

int PipeVideoOutput::Init()
{
	//Iniciamos
	inited = true;

	return true;
}

int PipeVideoOutput::End()
{
	//Terminamos
	inited = false;

	return true;
}

int PipeVideoOutput::IsChanged(DWORD version)
{
	//If not inited
	if (!inited)
		//Not changed
		return false;
	//Check if we are asking for the same answer than before
	if (this->version==version)
		//Return previous chanded
		return versionChanged;
	//Store version number
	this->version = version;
	//Store value for change associated to that version
	versionChanged = isChanged;
	//Have we changed?
	return isChanged;
};
