/*
 * File:   JSR309Manager.cpp
 * Author: Sergio
 *
 * Created on 8 de septiembre de 2011, 13:06
 */
#include "log.h"
#include "amf.h"
#include "JSR309Manager.h"
#include "xmlstreaminghandler.h"


JSR309Manager::JSR309Manager()
{
	//Init id
	maxId = 1;
	//NO manager
	eventMngr = NULL;
	//Not inited
	inited = false;
}

JSR309Manager::~JSR309Manager()
{
}


/**************************************
* Init
*	Inicializa la JSR309Manager
**************************************/
int JSR309Manager::Init(XmlStreamingHandler *eventMngr)
{
	timeval tv;

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Estamos iniciados
	inited = true;

	//Get secs
	gettimeofday(&tv,NULL);

	//El id inicial
	maxId = (tv.tv_sec & 0x7FFF) << 16;

	//Store event mngr
	this->eventMngr = eventMngr;

	//Salimos
	return 1;
}

/**************************************
* End
*	Termina la JSR309Manager
**************************************/
int JSR309Manager::End()
{
	Log(">End JSR309Manager\n");

	//Extrae las sesiones bajo lock y las termina fuera : End() puede esperar
	//threads (recorders, players) que necesitan otros mutex.
	MediaSessions ended;
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Dejamos de estar iniciados
		inited = false;

		//Vaciamos la lista
		ended.swap(sessions);

		//NO manager
		eventMngr = NULL;
	}

	//Paramos las sesiones fuera del lock ; el ultimo shared_ptr las destruye
	for (MediaSessions::iterator it=ended.begin(); it!=ended.end(); ++it)
		it->second.sess->End();

	Log("<End JSR309Manager\n");

	//Salimos
	return false;
}

int JSR309Manager::CreateEventQueue()
{
	//Check mngr
	if (!eventMngr)
		//Error
		return Error("Event manager not set!\n");

	//Create it
	return eventMngr->CreateEventQueue();
}

int JSR309Manager::DeleteEventQueue(int id)
{
	//Check mngr
	if (!eventMngr)
		//Error
		return Error("Event manager not set!\n");

	//Create it
	return eventMngr->DestroyEventQueue(id);
}


/**************************************
* CreateMediaSession
*	Inicia una conferencia
**************************************/
int JSR309Manager::CreateMediaSession(std::wstring tag,int queueId)
{
	Log(">CreateMediaSession\n");

	//Obtenemos el id
	int sessId = maxId++;

	//Creamos la multi (make_shared : requis pour weak_from_this / RecorderTimer)
	std::shared_ptr<MediaSession> sess = std::make_shared<MediaSession>(tag);

	//Donne à la session sa back-reference vers le manager et son id, pour qu'elle
	//puisse publier directement des événements (players, recorders, endpoints).
	sess->SetEventHandler(sessId, this);

	//Set listener
	sess->SetListener(this,NULL);

	//INit it
	sess->Init();

	//Creamos la entrada
	MediaSessionEntry entry;

	//Guardamos los datos
	entry.id 	= sessId;
	entry.tag 	= tag;
	entry.sess 	= sess;
	entry.queueId	= queueId;

	//Bloqueamos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//a�adimos a la lista
		sessions[sessId] = entry;
	}

	Log("<CreateMediaSession [%d]\n",sessId);

	return sessId;
}

/**************************************
* GetMediaSessionRef
*	Obtiene una referencia a una conferencia
**************************************/
int JSR309Manager::GetMediaSessionRef(int id,std::shared_ptr<MediaSession> &sess)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find confernce
	MediaSessions::iterator it = sessions.find(id);

	//SI no esta
	if (it==sessions.end())
		//Y salimos
		return Error("Media session not found [%d]\n",id);

	//Y obtenemos la referencia compartida a la sesion
	sess = it->second.sess;

	return true;
}

/**************************************
* DeleteMediaSession
*	Inicializa la JSR309Manager
**************************************/
int JSR309Manager::DeleteMediaSession(int id)
{
	Log(">DeleteMediaSession [%d]\n",id);

	std::shared_ptr<MediaSession> sess;

	//Extrae la sesion bajo lock : una vez fuera de la map, nadie puede obtener
	//nuevas referencias (remplace le flag enabled + WaitUnusedAndLock)
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Find conference
		MediaSessions::iterator it = sessions.find(id);

		//Check if we found it or not
		if (it==sessions.end())
			//Y salimos
			return Error("Media session not found [%d]\n",id);

		//Get conference from ref entry
		sess = std::move(it->second.sess);

		//Remove from list
		sessions.erase(it);
	}

	//End conference : idempotent, et sûr même si un handler tient encore une
	//référence — le dernier shared_ptr détruira l'objet.
	if (sess)
		sess->End();

	Log("<DeleteMediaSession [%d]\n",id);

	//Exit
	return true;
}

int JSR309Manager::PostEvent(int sessionId,int eventContextId , JSR309Event *event)
{
	Debug(">Post Event\n");

	std::shared_ptr<MediaSession> sess;

	//Bloqueamos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Find confernce
		MediaSessions::iterator it = sessions.find(sessionId);

		//SI no esta
		if (it==sessions.end())
		{
			//Prend possession de l'événement : pas de fuite
			delete event;
			//Y salimos
			return Error("Media session not found [%d]\n",sessionId);
		}

		sess = it->second.sess;
	}

	//Résout le contexte hors du lock manager (prend le mutex de la session)
	std::shared_ptr<JSR309EventContext> evtctx = sess->GetEventContext(eventContextId);

	if (!evtctx)
	{
		delete event;
		return Error("Event context not found [%d]\n",eventContextId);
	}

	event->FillEvent(*evtctx);

	Debug("<PostEvent\n");

	//Remise à la file (copie tag/queueId sous lock manager)
	return DeliverEvent(sessionId,event);
}

int JSR309Manager::DeliverEvent(int sessionId, JSR309Event *event)
{
	std::wstring tag;
	int queueId;
	XmlStreamingHandler *mngr;

	//Copie tag/queueId sous lock : plus aucun accès à l'entrée après unlock (C-3)
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Find confernce
		MediaSessions::iterator it = sessions.find(sessionId);

		//SI no esta
		if (it==sessions.end())
		{
			delete event;
			return Error("Media session not found [%d]\n",sessionId);
		}

		tag	= it->second.tag;
		queueId	= it->second.queueId;
		mngr	= eventMngr;
	}

	event->SetSessionTag(tag);

	if (mngr)
		//Send new event (la file prend possession)
		mngr->AddEvent(queueId,event);
	else
		delete event;

	return 1;
}

void JSR309Manager::onWebSocketConnection(const HTTPRequest &request, WebSocket *ws)
{
	::Log("JSR309Manager: incoming WebSocket connection to %s\n", request.GetRequestURI().c_str());
	std::shared_ptr<MediaSession> sess;
	std::string token;

	int sessionId;

	// Get the URL which must look (assuming conferenceId 1234 and userId 5678) as follows:
	//   /bfcp/1234/5678
	std::string url = request.GetRequestURI();
	StringParser parser(url);

	// Check the URL.
	if (! parser.MatchString("/jsr309"))
	{
		ws->Reject(404, "Not found");
		return;
	}

	// Extract sessionId
	if (! parser.ParseChar('/'))
	{
		::Error("JSR309::onWebSocketConnection() | bad URL: no /jsr309/ => HTTP 400\n");
		ws->Reject(400, "Bad Request");
		return;
	}
	if (! parser.ParseInteger()) {
		::Error("jsr309::onWebSocketConnection() | bad URL: missing session ID.\n");
		ws->Reject(400, "Bad Request");
		return;
	}

	sessionId = (int) parser.GetIntegerValue();

	if (! parser.ParseChar('/'))
	{
		Error("jsr309::onWebSocketConnection() | bad URL: no no /jsr309/sessionId/ => HTTP 400\n");
		ws->Reject(400, "Bad Request no sep before token");
		return;
	}

	if (! parser.ParseToken())
	{
		Error("jsr309::onWebSocketConnection(): cannot textract token from URL %s\n", url.c_str());
		ws->Reject(400, "Bad Request cannot extract token");
		return;
	}

	token = parser.GetValue();
	if ( GetMediaSessionRef(sessionId, sess) )
	{
	    sess->onNewMediaConnection(ws, token);
	}
	else
	{
	    Error("jsr309::onWebSocketConnection() | no such session %d\n", sessionId);
	    ws->Reject(404, "No such media session");
	}
}
