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
	//Contrat EventQueueSweeper/Worker : le destructeur dérivé doit avoir arrêté
	//le thread — DeleteQueueOwners n'existe plus quand ~Worker s'exécute
	StopSweeper();
}


/**************************************
* Init
*	Inicializa la JSR309Manager
**************************************/
int JSR309Manager::Init(XmlStreamingHandler *eventMngr,int queueExpiresSecs)
{
	timeval tv;

	//Bloqueamos
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Estamos iniciados
		inited = true;

		//Get secs
		gettimeofday(&tv,NULL);

		//El id inicial
		maxId = (tv.tv_sec & 0x7FFF) << 16;

		//Store event mngr
		this->eventMngr = eventMngr;
	}

	//Arme le balayeur hors verrou (il prend ce même mutex à son premier tour)
	StartSweeper(eventMngr,queueExpiresSecs,"JSR309Manager");

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

	//Arrête le balayeur AVANT de vider la liste, et hors de tout verrou : le
	//join ne doit jamais se faire sous un mutex que le thread peut vouloir
	//prendre (il prend `mutex` à chaque tour)
	StopSweeper();

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

	//NB : les sessions rattachées à cette file ne sont PAS détruites ici. Le
	//balayeur constatera que leur queueId ne désigne plus rien et ARMERA le
	//délai de grâce (cf. eventqueuesweeper.h, signal 2) : le client garde ainsi
	//une chance de se reconnecter avant de perdre ses sessions.
	return eventMngr->DestroyEventQueue(id);
}

/**************************************
* CollectQueueIds
*	Files référencées par les sessions (EventQueueSweeper)
**************************************/
void JSR309Manager::CollectQueueIds(std::set<int>& ids)
{
	std::lock_guard<std::mutex> lock(mutex);

	for (MediaSessions::iterator it=sessions.begin(); it!=sessions.end(); ++it)
		if (it->second.queueId > 0)
			ids.insert(it->second.queueId);
}

/**************************************
* DeleteQueueOwners
*	Détruit les sessions liées à une file d'événements (EventQueueSweeper)
**************************************/
int JSR309Manager::DeleteQueueOwners(int queueId,const char *reason)
{
	//Extrait les entrées sous verrou : une fois hors de la map, personne ne
	//peut plus en obtenir de nouvelle référence (même principe que
	//DeleteMediaSession)
	std::vector<MediaSessionEntry> expired;

	{
		std::lock_guard<std::mutex> lock(mutex);

		for (MediaSessions::iterator it=sessions.begin(); it!=sessions.end(); )
		{
			if (it->second.queueId == queueId)
			{
				expired.push_back(std::move(it->second));
				it = sessions.erase(it);
			} else
				++it;
		}
	}

	//Terminaison HORS verrou : End() joint des threads (recorders, players)
	//qui peuvent vouloir ce même mutex
	for (std::vector<MediaSessionEntry>::iterator it=expired.begin(); it!=expired.end(); ++it)
	{
		Log("-JSR309Manager: suppression de la session %d [tag:%ls,queue:%d] : %s\n",
			it->id,it->tag.c_str(),queueId,reason);

		if (it->sess)
			it->sess->End();
	}

	//Le dernier shared_ptr détruit chaque session, hors verrou lui aussi
	int count = (int)expired.size();
	expired.clear();

	return count;
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
* GetMediaSessionCount
*	Nombre de sessions vivantes
**************************************/
int JSR309Manager::GetMediaSessionCount()
{
	std::lock_guard<std::mutex> lock(mutex);

	return sessions.size();
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
	{
		//Send new event (la file prend possession)... sauf si la file n'existe
		//plus : AddEvent rend 0 SANS détruire l'événement (fuite historique)
		if (!mngr->AddEvent(queueId,event))
			delete event;
	}
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
