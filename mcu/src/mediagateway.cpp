/* 
 * File:   mediagateway.cpp
 * Author: Sergio
 * 
 * Created on 22 de diciembre de 2010, 18:10
 */
#include "log.h"
#include "mediagateway.h"

MediaGateway::MediaGateway()
{	
	//No event mngr
	eventMngr = NULL;
	
	queueId = 0;
}

MediaGateway::~MediaGateway()
{
}


/**************************************
* Init
*	Inititalize the media gateway server
**************************************/
bool MediaGateway::Init(XmlStreamingHandler *p_eventMngr)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Estamos iniciados
	inited = true;

	//El id inicial
	maxId=100;
	
	//Store event mngr
	this->eventMngr = p_eventMngr;
	
	
	//Salimos
	return inited;
}

/**************************************
* End
*	Ends media gateway server
**************************************/
bool MediaGateway::End()
{
	Log(">End MediaGateway\n");

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Dejamos de estar iniciados
	inited = false;

	//Paramos las sesions
	for (MediaBridgeEntries::iterator it=bridges.begin(); it!=bridges.end(); it++)
		//La paramos (el shared_ptr destruye la sesion al vaciar la map)
		it->second.session->End();

	//Clear the MediaGateway list
	bridges.clear();

	Log("<End MediaGateway\n");

	//Salimos
	return !inited;
}

/**************************************
* CreateMediaBridge
*	Create a media bridge session
**************************************/
DWORD MediaGateway::CreateMediaBridge(const std::wstring &name)
{
	Log("-CreateBroadcast [name:\"%ls\"]\n",name.c_str());


	//Creamos la session
	std::shared_ptr<MediaBridgeSession> session = std::make_shared<MediaBridgeSession>();

	//Obtenemos el id
	DWORD sessionId = maxId++;

	session->setSessionId(sessionId);

	//Creamos la entrada
	MediaBridgeEntry entry;

	//Guardamos los datos
	entry.id 	= sessionId;
	entry.name 	= name;
        entry.session 	= session;

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//a�adimos a la lista
	bridges[sessionId] = entry;

	return sessionId;
}

/**************************************
 * SetMediaBridgeInputToken
 *	Associates a token with a media bridge for input
 *	In case there is already a pin associated with that session it fails.
 **************************************/
bool MediaGateway::SetMediaBridgeInputToken(DWORD id,const std::wstring &token)
{
	Log(">SetMediaBridgeInputToken [id:%d,token:\"%ls\"]\n",id,token.c_str());

	bool res = false;

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Get the MediaGateway session entry
	MediaBridgeEntries::iterator it = bridges.find(id);

	//Check it
	if (it==bridges.end())
	{
		//Broadcast not found
		Error("Media bridge session not found\n");
		//Exit
		goto end;
	}

	//Add it
	it->second.session->AddInputToken(token);

	//Everything was ok
	res = true;

end:
	Log("<SetMediaBridgeInputToken\n");

	return res;
}

/**************************************
 * SetMediaBridgeOutputToken
 *	Associates a token with a media bridge for input
 *	In case there is already a pin associated with that session it fails.
 **************************************/
bool MediaGateway::SetMediaBridgeOutputToken(DWORD id,const std::wstring &token)
{
	Log(">SetMediaBridgeOutputToken [id:%d,token:\"%ls\"]\n",id,token.c_str());

	bool res = false;

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Get the MediaGateway session entry
	MediaBridgeEntries::iterator it = bridges.find(id);

	//Check it
	if (it==bridges.end())
	{
		//Broadcast not found
		Error("Media bridge session not found\n");
		//Exit
		goto end;
	}

	//Add it
	it->second.session->AddOutputToken(token);

	//Everything was ok
	res = true;

end:
	Log("<SetMediaBridgeOutputToken\n");

	return res;
}

/**************************************
* GetMediaBridgeRef
*	Obtiene una referencia a una sesion
**************************************/
bool MediaGateway::GetMediaBridgeRef(DWORD id,std::shared_ptr<MediaBridgeSession> &session)
{
	Log(">GetMediaBridgeRef [%d]\n",id);

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Get it
	MediaBridgeEntries::iterator it = bridges.find(id);

	//SI no esta
	if (it==bridges.end())
	{
		//Y salimos
		return Error("Session no encontrada [%d]\n",id);
	}

	//Y obtenemos la referencia compartida a la sesion
	session = it->second.session;

	Log("<GetMediaBridgeRef \n");

	return true;
}

/**************************************
* DeleteSession
*	Inicializa el servidor de FLV
**************************************/
bool MediaGateway::DeleteMediaBridge(DWORD id)
{
	Log(">DeleteMediaBridge [%d]\n",id);

	std::shared_ptr<MediaBridgeSession> session;

	//Extrae la sesion bajo lock : una vez fuera de la map, nadie puede obtener
	//nuevas referencias (remplaza el //TODO numRef == 0 nunca implementado, C-8)
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Find sessionerence
		MediaBridgeEntries::iterator it = bridges.find(id);

		//Check if we found it or not
		if (it==bridges.end())
		{
			//Y salimos
			return Error("Session no encontrada [%d]\n",id);
		}

		//Get sessionerence
		session = std::move(it->second.session);

		//Remove entry from list
		bridges.erase(it);
	}

	Log("-Ending session [%d]\n",id);

	//End session : idempotente, y seguro aunque un handler siga teniendo una
	//referencia — el ultimo shared_ptr destruira el objeto.
	session->End();

	Log("<DeleteMediaBridge [%d]\n",id);

	//Exit
	return true;
}

std::shared_ptr<RTMPNetConnection> MediaGateway::Connect(const std::wstring& appName,RTMPNetConnection::Listener* listener)
{
	std::shared_ptr<MediaBridgeSession> sess;
	wchar_t *stopwcs;

	//Skip the mcu part and find the conf Id
	int i = appName.find(L"/");

	//Check if
	if (i<0)
	{
		//Noting found
		Error("Wrong format for app name\n");
		//Exit
		return nullptr;
	}

	//Get conf id
	int confId = wcstol(appName.substr(i+1).c_str(),&stopwcs,10);

	//Get conference
	if(!GetMediaBridgeRef(confId,sess))
	{
		//No conference found
		Error("MediaBridge not found [confId:%d]\n",confId);
		//Exit
		return nullptr;
	}

	//Connect
	sess->Connect(listener);

	//Return conf : el shared_ptr mantiene la sesion viva mientras dure la conexion RTMP (C-8)
	return sess;
}

int MediaGateway::CreateEventQueue()
{
	//Check mngr
	if (!eventMngr)
		//Error
		return Error("Event manager not set!\n");

	//Create it
	queueId= eventMngr->CreateEventQueue();
	Log("< return mediaGateway queueId=%i\n",queueId);
	return queueId;
}

int MediaGateway::DeleteEventQueue(int id)
{
	//Check mngr
	if (!eventMngr)
		//Error
		return Error("Event manager not set!\n");

	//Create it
	return eventMngr->DestroyEventQueue(id);
}

XmlStreamingHandler* MediaGateway::getEventMngr()
{
	//Check mngr
	if (!eventMngr)
		//Error
		return NULL;

	//Create it
	return eventMngr;
}


int  MediaGateway::getQueueId()
{
	return queueId;
}
void  MediaGateway::setQueueId(int p_queueId)
{
	this->queueId=p_queueId;
}
