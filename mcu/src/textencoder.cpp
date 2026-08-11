#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <set>
#include <list>
#include "textencoder.h"
#include "log.h"
#include "tools.h"
#include "text.h"


/**********************************
* TextEncoder
*	Constructor
***********************************/
TextEncoder::TextEncoder()
{
	//Not encoding
	encodingText=0;
	//Create mutex
}

/*******************************
* ~TextEncoder
*	Destructor.
********************************/
TextEncoder::~TextEncoder()
{
	//If still running
	if (encodingText)
		//End
		End();
	//Destroy mutex
}

/***************************************
* Init
*	Inicializa los devices
***************************************/
int TextEncoder::Init(TextInput *input)
{
	Log(">Init text encoder\n");

	//Nos quedamos con los puntericos
	textInput  = input;

	//Y aun no estamos mandando nada
	encodingText=0;

	Log("<Init text encoder\n");

	return 1;
}

/***************************************
* startencodingText
*	Helper function
***************************************/
/***************************************
* StartSending
*	Comienza a mandar a la ip y puertos especificados
***************************************/
int TextEncoder::StartEncoding()
{
	Log(">Start encoding text\n");

	//Si estabamos mandando tenemos que parar
	if (encodingText)
		//paramos
		StopEncoding();

	encodingText=1;

	//Start thread
	StartThread();

	Log("<StartSending text [%d]\n",encodingText);

	return 1;
}
/***************************************
* End
*	Termina la conferencia activa
***************************************/
int TextEncoder::End()
{
	//If encoding
	if (encodingText)
		//Terminamos de enviar
		StopEncoding();

	return 1;
}


/***************************************
* StopEncoding
* 	Termina el envio
****************************************/
int TextEncoder::StopEncoding()
{
	Log(">StopEncoding Text [0x%x]\n",this);

	//Esperamos a que se cierren las threads de envio
	if (encodingText)
	{
		//paramos
		encodingText=0;

		Log("-Cancel text [0x%x]\n",textInput);

		//Cancel any pending grab
		textInput->Cancel();

		//Y esperamos
		StopThread();
	}

	Log("<StopEncoding Text\n");

	return 1;
}

/*******************************************
* Encode
*	Capturamos el text y lo mandamos
*******************************************/
int TextEncoder::Encode()
{
	//Mientras tengamos que capturar
	while(encodingText)
	{
		//Wait until there is a frame
		TextFrame *frame = textInput->GetFrame(0);

		//Check framce
		if (!frame)
			//next one
			continue;

		//If it has content
		if (frame->GetWLength())
		{
			//RELAIS BRUT du T.140 : on transmet la trame telle que le mixeur
			//texte l'a produite, c'est-a-dire le FLUX INCREMENTAL (caracteres
			//frappes depuis la derniere trame, retours arriere 0x08 compris).
			//
			//Cette boucle accumulait auparavant le texte (scroll + line) et
			//emettait a chaque fois la CHAINE COMPLETE depuis le debut. Or le
			//seul consommateur de ces trames est MP4Recorder, dont la piste de
			//sous-titres (Text2Subtitle, dans libmedkit) est DEJA un
			//accumulateur : le texte etait donc accumule deux fois et le
			//fichier MP4 contenait "[nom] tw[nom] tws[nom] twst..." au lieu de
			//"[nom] test". Text2Subtitle traite par ailleurs exactement les
			//memes cas particuliers (BOM 0xFEFF, retour arriere, 0xFFFD,
			//delimiteurs de ligne) que l'accumulation supprimee ici.
			std::unique_lock<std::mutex> mutexLock(mutex);
			//For each listener
			for (Listeners::iterator it=listeners.begin(); it!=listeners.end(); ++it)
				//Call listener
				(*it)->onMediaFrame(*frame);
			//unlock
			mutexLock.unlock();
		}

		//Delete frame -- y compris les trames vides, qui fuyaient jusqu'ici
		delete(frame);
	}

	//Salimos
        Log("<Encode Text\n");
	
	pthread_exit(0);
}

bool TextEncoder::AddListener(MediaFrame::Listener *listener)
{
	//Lock
	std::unique_lock<std::mutex> mutexLock(mutex);

	//Add to set
	listeners.insert(listener);

	//unlock
	mutexLock.unlock();

	return true;
}

bool TextEncoder::RemoveListener(MediaFrame::Listener *listener)
{
	//Lock
	std::unique_lock<std::mutex> mutexLock(mutex);

	//Search
	Listeners::iterator it = listeners.find(listener);

	//If found
	if (it!=listeners.end())
		//End
		listeners.erase(it);

	//Unlock
	mutexLock.unlock();

	return true;
}
