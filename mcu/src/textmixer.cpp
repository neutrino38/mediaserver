#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <tools.h>
#include <wchar.h>
#include "log.h"
#include "textmixer.h"
#include "pipetextinput.h"
#include "pipetextoutput.h"


/***********************
* TextMixer
*	Constructor
************************/
TextMixer::TextMixer()
{
}

/***********************
* ~TextMixer
*	Destructor
************************/
TextMixer::~TextMixer()
{
}

/***********************************
* startMixingText
*	Crea el thread de mezclado de text
************************************/
void *TextMixer::startMixingText(void *par)
{
	Log("-MixTextThread [%d]\n",getpid());

	//Obtenemos el parametro
	TextMixer *am = (TextMixer *)par;

	//Bloqueamos las se�ales
	blocksignals();
	
	//Ejecutamos
	pthread_exit((void *)(intptr_t)am->MixText());
}

/***********************************
* MixText
*	Mezcla los texts
************************************/
int TextMixer::MixText()
{
	wchar_t buffer[1024];
	DWORD size=1024;

	Log(">MixText\n");

	//Mientras estemos mezclando
	while(mixingText)
	{
		//Lock list of text mixers
		lstTextsUse.IncUse();

		//Send to all participants
		for (TextSources::iterator it=sources.begin();it!=sources.end();++it)
		{
			//Get text source
			TextSource *text = it->second;

			//Check if it has somethin in the queue
			if (text->output->Length())
			{
				//Read input
				DWORD len = text->output->ReadText(buffer,size);
				//Add to all workers
				for (TextWorkers::iterator w=workers.begin();w!=workers.end();++w)
				{
					//Get worker
					TextMixerWorker *worker = (*w);
					//Check it is not the ixer worker
					if (text->worker!=worker)
						//Write it
						worker->WriteText(text->id,buffer,len);
				}
			}
		}

		//Send private texts
		for (TextPrivates::iterator it=privates.begin();it!=privates.end();++it)
		{
			//Get text source
			TextPrivate *priv = it->second;

			//Check if it has somethin in the queue
			if (priv->output->Length())
			{
				//Read input
				DWORD len = priv->output->ReadText(buffer,size);
				//Get private worked
				//Add to all workers
				TextSources::iterator its = sources.find(priv->to);
				//If it setill exist
				if (its!=sources.end())
					//Write it
					its->second->worker->WriteText(priv->id,buffer,len);
			}
		}

		//Know for each worker
		for (TextWorkers::iterator w=workers.begin();w!=workers.end();++w)
			//Process it
			(*w)->ProcessText();

		//Un lock
		lstTextsUse.DecUse();

		//Tick de 200 ms, interruptible par End() (l'ancien msleep ne l'était
		//pas : l'arrêt attendait la fin du sommeil)
		mixTickWait.WaitSignal(200);
	}

	//Know for each worker
	for (TextWorkers::iterator w=workers.begin();w!=workers.end();++w)
		//Flush any text in the queue
		(*w)->FlushText();

	//Logeamos
	Log("<MixText\n");

	return 1;
}


/***********************
* Init
*	Inicializa el mezclado de text
************************/
int TextMixer::Init()
{
	// Estamos mzclando
	mixingText = true;

	//Réarme le tick si on ré-Init après un End (Cancel est collant)
	mixTickWait.Reset();

	//Y arrancamoe el thread
	createPriorityThread(&mixTextThread,startMixingText,this,0);

	return 1;
}

/***********************
* End
*	Termina el mezclado de text
************************/
int TextMixer::End()
{
	Log(">End textmixer\n");

	//Terminamos con la mezcla
	if (mixingText)
	{
		//Terminamos la mezcla
		mixingText = 0;

		//Réveille le tick tout de suite (arrêt immédiat)
		mixTickWait.Cancel();

		//Y esperamos
		pthread_join(mixTextThread,NULL);
	}

	//Borramos los texts restantes. DeleteMixer EFFACE l'entrée de la map :
	//itérer dessus pendant la suppression invalidait l'itérateur (UB, crash
	//constaté par TextMixerSite.ForwardsTextToOtherParticipantOnly).
	while (!sources.empty())
		//Borramos el text
		DeleteMixer(sources.begin()->first);

	Log("<End textmixer\n");
	
	return 1;
}

/***********************
* CreateMixer
*	Crea una nuevo source de text para mezclar
************************/
int TextMixer::CreateMixer(int id,std::wstring &name)
{
	Log(">CreateMixer text [%d]\n",id);

	//Protegemos la lista
	lstTextsUse.WaitUnusedAndLock();

	//Miramos que si esta
	if (sources.find(id)!=sources.end())
	{
		//Desprotegemos la lista
		lstTextsUse.Unlock();
		return Error("Text sourecer already existed\n");
	}

	//Creamos el source
	TextSource *text = new TextSource();

	//Set id
	text->id = id;

	//POnemos el input y el output (co-propriété shared_ptr, Point 1 / C-4)
	text->input  = std::make_shared<PipeTextInput>();
	text->output = std::make_shared<PipeTextOutput>();
	//Create the worker
	text->worker = new TextMixerWorker();

	//Set name
	text->name = name;

	//Add source to the list
	sources[id] = text;
	
	Log("-Text [%d,%ls]\n",text->id,text->name.c_str());

	//Desprotegemos la lista
	lstTextsUse.Unlock();

	//Y salimos
	Log("<CreateMixer text\n");

	return true;
}

/***********************
* InitMixer
*	Inicializa un text
*************************/
int TextMixer::InitMixer(int id)
{
	Log(">Init mixer [%d]\n",id);

	//Protegemos la lista
	lstTextsUse.WaitUnusedAndLock();

	//Buscamos el text source
	TextSources::iterator it = sources.find(id);

	//Si no esta	
	if (it == sources.end())
	{
		//Desprotegemos
		lstTextsUse.Unlock();
		//Salimos
		return Error("Mixer not found\n");
	}

	//Obtenemos el text source
	TextSource *text = it->second;

	//INiciamos los pipes
	text->input->Init();
	text->output->Init();

	//Init worker
	text->worker->Init();

	//Set participant as reader for the worker
	text->worker->AddReader(id,text->input.get());

	Log("-Text [%d,%ls]\n",text->id,text->name.c_str());

	//Add as writter to all the other participants
	for (TextWorkers::iterator it=workers.begin();it!=workers.end();++it)
		//Add writter
		(*it)->AddWritter(text->id,text->name,true);

	//Set all other participants as writters for the user
	for (TextSources::iterator it=sources.begin();it!=sources.end();++it)
	{
		//Get source
		TextSource *source = it->second;
		Log("[%d,%d]\n",text->id,source->id);
		//Check if it is us
		if (source->id!=text->id)
			//Add writer
			text->worker->AddWritter(source->id,source->name,true);
	}

	//Add the worker to the list
	workers.insert(text->worker);

	//Desprotegemos
	lstTextsUse.Unlock();

	Log("<Init mixer [%d]\n",id);

	//Si esta devolvemos el input
	return true;
}


/***********************
* EndMixer
*	Finaliza un text
*************************/
int TextMixer::EndMixer(int id)
{
	//Protegemos la lista
	lstTextsUse.WaitUnusedAndLock();

	//Buscamos el text source
	TextSources::iterator it = sources.find(id);

	//Si no esta	
	if (it == sources.end())
	{
		//Desprotegemos
		lstTextsUse.Unlock();
		//Salimos
		return false;
	}

	//Obtenemos el text source
	TextSource *text = it->second;

	//Remove as writter to all the other participants
	for (TextWorkers::iterator it=workers.begin();it!=workers.end();++it)
		//Add writter
		(*it)->RemoveWritter(text->id);

	//Remove from the workers
	workers.erase(text->worker);

	//End the mixer
	text->worker->End();

	//Terminamos
	text->input->End();
	text->output->End();

	//Desprotegemos
	lstTextsUse.Unlock();

	//Si esta devolvemos el input
	return true;;
}

/***********************
* DeleteMixer
*	Borra una fuente de text
************************/
int TextMixer::DeleteMixer(int id)
{
	Log("-DeleteMixer text [%d]\n",id);

	//Protegemos la lista
	lstTextsUse.WaitUnusedAndLock();

	//Lo buscamos
	TextSources::iterator it = sources.find(id);

	//SI no ta
	if (it == sources.end())
	{
		//DDesprotegemos la lista
		lstTextsUse.Unlock();
		//Salimos
		return Error("Text source not found\n");
	}

	//Obtenemos el text source
	TextSource *text = it->second;

	//Lo quitamos de la lista
	sources.erase(it);

	//Desprotegemos la lista
	lstTextsUse.Unlock();

	//Les pipes sont des shared_ptr : rendus quand le dernier stream les relâche
	//(Point 1 / C-4). Le worker (raw) est détruit avant les shared_ptr : il
	//détient un pointeur brut vers input, mais son propre thread est arrêté par
	//DetachAllReaders/End dans EndMixer.
	delete text->worker;
	delete text;

	return 0;
}

/***********************
* GetInput
*	Obtiene el input para un id
************************/
TextInput* TextMixer::GetInput(int id)
{
	//Protegemos la lista
	lstTextsUse.IncUse();

	//Buscamos el text source
	TextSources::iterator it = sources.find(id);

	//Obtenemos el input
	TextInput *input = NULL;

	//Si esta
	if (it != sources.end())
		input = (TextInput*)it->second->input.get();

	//Desprotegemos
	lstTextsUse.DecUse();

	//Si esta devolvemos el input
	return input;
}

/***********************
* GetSharedInput
*	Copie de shared_ptr sur le pipe d'entrée (co-propriété, Point 1 / C-4)
************************/
std::shared_ptr<TextInput> TextMixer::GetSharedInput(int id)
{
	lstTextsUse.IncUse();
	TextSources::iterator it = sources.find(id);
	std::shared_ptr<TextInput> input;
	if (it != sources.end())
		input = it->second->input;
	lstTextsUse.DecUse();
	return input;
}

/***********************
* GetOutput
*	Obtiene el output para un id
************************/
TextOutput* TextMixer::GetOutput(int id)
{
	//Protegemos la lista
	lstTextsUse.IncUse();

	//Buscamos el text source
	TextSources::iterator it = sources.find(id);

	//Obtenemos el output
	TextOutput *output = NULL;

	//Si esta
	if (it != sources.end())
		//Store it
		output = it->second->output.get();

	//if still not found
	if (!output)
	{
		//Check if it is private
		TextPrivates::iterator it = privates.find(id);
		//If it exist
		if (it!=privates.end())
			//Store it
			output = it->second->output.get();
	}

	//Desprotegemos
	lstTextsUse.DecUse();

	//Si esta devolvemos el input
	return output;
}

/***********************
* GetSharedOutput
*	Copie de shared_ptr sur le pipe de sortie (sources puis privates), Point 1
************************/
std::shared_ptr<TextOutput> TextMixer::GetSharedOutput(int id)
{
	lstTextsUse.IncUse();
	std::shared_ptr<TextOutput> output;
	//Chercher d'abord dans les sources
	TextSources::iterator it = sources.find(id);
	if (it != sources.end())
		output = it->second->output;
	//Sinon dans les privates
	if (!output)
	{
		TextPrivates::iterator itp = privates.find(id);
		if (itp != privates.end())
			output = itp->second->output;
	}
	lstTextsUse.DecUse();
	return output;
}


/***********************
* CreatePrivate
*	Create a private text source for one participant
************************/
int TextMixer::CreatePrivate(int id,int to,std::wstring &name)
{
	Log(">CreatePrivate text [%d,%d]\n",id,to);

	//Protegemos la lista
	lstTextsUse.WaitUnusedAndLock();

	//Miramos que si esta
	if (privates.find(id)!=privates.end())
	{
		//Desprotegemos la lista
		lstTextsUse.Unlock();
		//Error
		return Error("Private sourecer already existed\n");
	}

	//Create the private text
	TextPrivate *priv = new TextPrivate();

	//Set id
	priv->id = id;
	//Set target mixer
	priv->to = to;
	//Create output (co-propriété shared_ptr, Point 1)
	priv->output = std::make_shared<PipeTextOutput>();
	//Set name
	priv->name = name;

	//Add private the list
	privates[id] = priv;
	
	//Desprotegemos la lista
	lstTextsUse.Unlock();

	//Y salimos
	Log("<CreateMixer text\n");

	return true;
}

/***********************
* InitPrivate
*	Inicializa un text
*************************/
int TextMixer::InitPrivate(int id)
{
	Log(">Init private [%d]\n",id);

	//Protegemos la lista
	lstTextsUse.IncUse();

	//Buscamos el text source
	TextPrivates::iterator it = privates.find(id);

	//Si no esta
	if (it == privates.end())
	{
		//Desprotegemos
		lstTextsUse.DecUse();
		//Salimos
		return Error("Mixer not found\n");
	}

	//Obtenemos el text source
	TextPrivate *priv = it->second;

	//INiciamos los pipes
	priv->output->Init();

	//Find private target
	TextSources::iterator itSource=sources.find(priv->to);
	
	//if found
	if (itSource!=sources.end())
		//Add writer
		itSource->second->worker->AddWritter(id,priv->name,false);

	//Desprotegemos
	lstTextsUse.DecUse();

	Log("<Init private [%d]\n",id);

	//Si esta devolvemos el input
	return true;
}


/***********************
* EndPrivate
*	Finaliza un text
*************************/
int TextMixer::EndPrivate(int id)
{
	//Protegemos la lista
	lstTextsUse.IncUse();

	//Buscamos el text source
	TextPrivates::iterator it = privates.find(id);

	//Si no esta
	if (it == privates.end())
	{
		//Desprotegemos
		lstTextsUse.DecUse();
		//Salimos
		return false;
	}

	//Obtenemos el text source
	TextPrivate *priv = it->second;

	//Find private target
	TextSources::iterator itSource=sources.find(priv->to);

	//If target found
	if (itSource!=sources.end())
		//Add writer
		itSource->second->worker->RemoveWritter(priv->id);

	//Terminamos
	priv->output->End();

	//Desprotegemos
	lstTextsUse.DecUse();

	//Si esta devolvemos el input
	return true;;
}

/***********************
* DeletePrivate
*	Borra una fuente de text
************************/
int TextMixer::DeletePrivate(int id)
{
	Log("-DeletePrivate text [%d]\n",id);

	//Protegemos la lista
	lstTextsUse.WaitUnusedAndLock();

	///Buscamos el text source
	TextPrivates::iterator it = privates.find(id);

	//Si no esta
	if (it == privates.end())
	{
		//Desprotegemos
		lstTextsUse.Unlock();
		//Salimos
		return false;
	}

	//Obtenemos el text source
	TextPrivate *priv = it->second;

	//Lo quitamos de la lista
	privates.erase(it);

	//Desprotegemos la lista
	lstTextsUse.Unlock();

	//priv->output est un shared_ptr : rendu quand le dernier détenteur le
	//relâche (Point 1). La struct conteneur est libérée ici.
	delete priv;

	return 0;
}
/***********************
* GetPrivateOutput
*	Obtiene el output para un id
************************/
TextOutput* TextMixer::GetPrivateOutput(int id)
{
	//Protegemos la lista
	lstTextsUse.IncUse();

	//Buscamos el text source
	TextPrivates::iterator it = privates.find(id);

	//Obtenemos el output
	TextOutput *output = NULL;

	//Si esta
	if (it != privates.end())
		output = it->second->output.get();

	//Desprotegemos
	lstTextsUse.DecUse();

	//Si esta devolvemos el input
	return output;
}
