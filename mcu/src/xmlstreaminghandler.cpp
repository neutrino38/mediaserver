#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <log.h>
#include <list>
#include "xmlstreaminghandler.h"
#include "tools.h"
#include "use.h"


XmlEventQueue::XmlEventQueue()
{
	//Le compte à rebours d'expiration démarre à la création : le client a
	//`idle` pour venir poller la file qu'il vient de demander.
	lastPoller = std::chrono::steady_clock::now();
}

XmlEventQueue::~XmlEventQueue()
{
	//Drainer un éventuel WaitForEvent avant de détruire la liste (les
	//prédicats la consultent) ; le ~Wait refera à blanc.
	CancelAndDrain();

	//While events
	while (!events.empty())
	{
		//delet first
		delete(events.front());
		//And remove it froom server
		events.pop_front();
	}
}

void XmlEventQueue::AddEvent(XmlEvent *event)
{
	//Add event
	Locked([&] { events.push_back(event); });

	//Signal
	Signal();
}

bool XmlEventQueue::WaitForEvent(DWORD timeout)
{
	//if we are cancel : false SEULEMENT à l'entrée
	if (IsCanceled())
		//canceled
		return false;

	//Attendre un événement ; timeout et Cancel-pendant-l'attente rendent
	//true quand même (keep-alive du long-poll, Cancel en deux temps)
	WaitUntil(timeout, [this] { return !events.empty(); });

	return true;
}

xmlrpc_value* XmlEventQueue::PeekXMLEvent(xmlrpc_env *env)
{
	return Locked([&]() -> xmlrpc_value* {
		//Get event
		if (!events.empty())
			//Retreive firs
			return events.front()->GetXmlValue(env);
		return NULL;
	});
}

void XmlEventQueue::AttachPoller()
{
	Locked([&] {
		pollers++;
		//Horodater aussi à l'attache : si le poller reste des heures, la file
		//n'est de toute façon pas expirable tant que pollers > 0
		lastPoller = std::chrono::steady_clock::now();
	});
}

void XmlEventQueue::DetachPoller()
{
	Locked([&] {
		if (pollers > 0) pollers--;
		//Départ du dernier poller : c'est d'ici que court le délai de grâce
		lastPoller = std::chrono::steady_clock::now();
	});
}

bool XmlEventQueue::IsPolled(std::chrono::milliseconds idle)
{
	return Locked([&] {
		//Poller attaché : vivant, sans discussion
		if (pollers > 0) return true;
		//Sinon : détachement (ou création) assez récent ?
		return (std::chrono::steady_clock::now() - lastPoller) <= idle;
	});
}

void XmlEventQueue::PopEvent()
{
	Locked([&] {
		//Get event
		if (!events.empty())
		{
			//delet first
			delete(events.front());
			//And remove it froom server
			events.pop_front();
		}
	});
}

/**************************************
* XmlStreamingHandler
*	Constructor
*************************************/
XmlStreamingHandler::XmlStreamingHandler()
{
	timeval tv;
	
	//Get secs
	gettimeofday(&tv,NULL);

	//El id inicial
	maxId = (tv.tv_sec & 0x7FFF) << 16;

}


int XmlStreamingHandler::DestroyAllQueues()
{
	for (EventQueues::iterator it = queues.begin(); it!=queues.end(); ++it)
		//Delete queue
		it->second->Cancel();

	listUse.WaitUnusedAndLock();
	for (EventQueues::iterator it = queues.begin(); it!=queues.end(); ++it)
		//Delete queue
		delete(it->second);
	//Empty
	queues.clear();
	//Unlock
	listUse.Unlock();
	return 0;
}

/**************************************

/**************************************
* XmlStreamingHandler
*	Destructor
*************************************/
XmlStreamingHandler::~XmlStreamingHandler()
{
	DestroyAllQueues();
}

/**************************************
* CreateEventQueue
*	Create an event queue and return id
*************************************/
int XmlStreamingHandler::CreateEventQueue()
{
	//Create queue
	XmlEventQueue *queue = new XmlEventQueue();

	//Get lock on list
	listUse.WaitUnusedAndLock();

	//Inc id and get id
	DWORD id = maxId++;

	//Appand
	queues[id] = queue;

	//Unlock
	listUse.Unlock();

	//Return queue id
	return id;
}

int XmlStreamingHandler::AddEvent(DWORD id,XmlEvent *event)
{
	//We are using the list
	listUse.IncUse();

	//Find queue
	EventQueues::iterator it = queues.find(id);

	//If not found
	if (it==queues.end())
	{
		//Not using it anymore
		listUse.DecUse();
		//Mandamos error
		return 0;
	}

	//Get queue
	XmlEventQueue *queue = it->second;

	//Inc queue usage
	queue->IncUse();

	//Not using it anymore the list
	listUse.DecUse();

	//Add the event to the queue
	queue->AddEvent(event);

	//Stop using queue
	queue->DecUse();

	//Done
	return 1;
}



std::vector<DWORD> XmlStreamingHandler::GetIdleQueues(std::chrono::milliseconds idle)
{
	std::vector<DWORD> idles;

	//We are using the list (interdit toute modification de la map le temps du
	//parcours : les écrivains passent par listUse.WaitUnusedAndLock)
	listUse.IncUse();

	for (EventQueues::iterator it = queues.begin(); it!=queues.end(); ++it)
		if (!it->second->IsPolled(idle))
			idles.push_back(it->first);

	//Not using it anymore
	listUse.DecUse();

	return idles;
}

bool XmlStreamingHandler::HasQueue(DWORD id)
{
	//We are using the list
	listUse.IncUse();

	bool found = (queues.find(id)!=queues.end());

	//Not using it anymore
	listUse.DecUse();

	return found;
}

int XmlStreamingHandler::DestroyEventQueue(DWORD id)
{
	Log("-Destroy event queue [id:%d]\n",id);
	
	//Get lock
	listUse.WaitUnusedAndLock();

	//Find queue
	EventQueues::iterator it = queues.find(id);

	//If not found
	if (it==queues.end())
	{
		//Not using it anymore
		listUse.Unlock();
		//Mandamos error
		return Error("Event queue not found\n");
	}

	//Get queue
	XmlEventQueue *queue = it->second;

	//Remove it from the queue
	queues.erase(it);

	//Unlock queue
	listUse.Unlock();

	//Cancel any pending activity
	queue->Cancel();

	//Wait until it is not used anymore
	queue->WaitUnusedAndLock();

	//Delete queu
	delete(queue);
	
	//Done
	return 1;
}

/**************************************
* ProcessRequest
*	Procesa una peticion
*************************************/
int XmlStreamingHandler::ProcessRequest(TRequestInfo *req,TSession * const ses)
{
	XmlEvent* event;
	xmlrpc_env env;
	timeval tv;

	Log(">ProcessRequest [uri:%s]\n",req->uri);

	//Block signals
	blocksignals();

	//Init timer
	getUpdDifTime(&tv);

	//Get las
	char *i = strrchr((char*)req->uri,'/');

	//Check if it was not found
	if (!i)
		//Mandamos error
		return XmlRpcServer::SendError(ses, 404, "Not found");

	//Get queue id
	DWORD id = atoi(i+1);

	//We are using the list
	listUse.IncUse();

	//Find queue
	EventQueues::iterator it = queues.find(id);

	//If not found
	if (it==queues.end())
	{
		//Not using it anymore
		listUse.DecUse();
		//Mandamos error
		return XmlRpcServer::SendError(ses, 404, "Not found");
	}

	//Get queue
	XmlEventQueue *queue = it->second;

	//Inc queue usage
	queue->IncUse();

	//Ce long-poll est la preuve de vie du client : tant qu'il est attaché, la
	//file (et les sessions qui lui sont liées) ne peut pas expirer.
	queue->AttachPoller();

	//Not using it anymore
	listUse.DecUse();

	//Creamos un enviroment
	xmlrpc_env_init(&env);

	//Set the content type
	ResponseContentType(ses, (char*)"text/xml; charset=\"utf-8\"");

	//Chunked output
	ResponseChunked(ses);

	//Send OK
	ResponseStatus(ses,200);

	//Start writing
	ResponseWriteStart(ses);

	//Get next events
	while(queue->WaitForEvent(30000))
	{
		//Get xml rpc object value
		xmlrpc_value *val = queue->PeekXMLEvent(&env);

		//If no event
		if (!val)
		{
			//Send keep alive;
			if (!ResponseWriteBody(ses,"\r\n",2))
				//Close on error
				break;
			//Wait again
			continue;
		}

		//Create mem block
		xmlrpc_mem_block *output = xmlrpc_mem_block_new(&env, 0);

		//Serialize
		xmlrpc_serialize_response(&env,output,val);

		//Free value
		xmlrpc_DECREF(val);

		//Send it
		if (!ResponseWriteBody(ses,XMLRPC_MEMBLOCK_CONTENTS(char, output), XMLRPC_MEMBLOCK_SIZE(char, output)))
			//Close on error
			break;
		//Liberamos
		XMLRPC_MEMBLOCK_FREE(char, output);

		//Remove event from queue
		queue->PopEvent();
	}

	//End it
	ResponseWriteEnd(ses);

	//Plus de poller : le délai de grâce d'expiration démarre ici (la
	//reconnexion du client, en principe sous la seconde, le réarme)
	queue->DetachPoller();

	//Dec queue usage
	queue->DecUse();

	Log("<ProcessRequest [time:%llu]\n",getDifTime(&tv)/1000);

	return 1;
}

