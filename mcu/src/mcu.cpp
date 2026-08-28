#include <stdlib.h>
#include <map>
#include <cstring>
#include "log.h"
#include "mcu.h"
#include "rtmpparticipant.h"
#include "stringparser.h"


/**************************************
* MCU
*	Constructor
**************************************/
MCU::MCU()
{
	//No event mngr
	eventMngr = NULL;
	//Not inited
	inited = false;
}

/**************************************
* ~MCU
*	Destructur
**************************************/
MCU::~MCU()
{
	//End just in case
	End();
}


/**************************************
* Init
*	Inicializa la mcu
**************************************/
int MCU::Init(XmlStreamingHandler *eventMngr,int queueExpiresSecs)
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
		maxId = (tv.tv_sec & 0x1FFF);

		//Store event mngr
		this->eventMngr = eventMngr;
	}

	//Arme le balayeur hors verrou (il prend ce même mutex à chaque tour)
	StartSweeper(eventMngr,queueExpiresSecs,"MCU");

	//Salimos
	return 1;
}

/**************************************
* End
*	Termina la mcu
**************************************/
int MCU::End()
{
	Log(">End MCU\n");

	//Arrête le balayeur AVANT de vider la liste, et hors de tout verrou : le
	//join ne doit jamais se faire sous un mutex que le thread peut vouloir
	//prendre (il prend `mutex` à chaque tour)
	StopSweeper();

	//Extrae las conferencias bajo lock y las termina fuera : conf->End() joint
	//des threads (participants, mixeurs) qui peuvent vouloir CE mutex — p.ex.
	//onParticipantMediaTimeout. Les terminer sous le verrou pouvait bloquer.
	Conferences ended;
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Dejamos de estar iniciados
		inited = false;

		//Vaciamos las listas
		ended.swap(conferences);
		tags.clear();
	}

	//Paramos las conferencias fuera del lock ; el ultimo shared_ptr las destruye
	for (Conferences::iterator it=ended.begin(); it!=ended.end(); ++it)
		it->second.conf->End();

	Log("<End MCU\n");

	//Salimos
	return false;
}

int MCU::CreateEventQueue()
{
	//Check mngr
	if (!eventMngr)
		//Error
		return Error("Event manager not set!\n");

	//Create it
	return eventMngr->CreateEventQueue();
}

int MCU::DeleteEventQueue(int id)
{
	//Check mngr
	if (!eventMngr)
		//Error
		return Error("Event manager not set!\n");

	//NB : les conférences rattachées à cette file ne sont PAS détruites ici. Le
	//balayeur constatera que leur queueId ne désigne plus rien et ARMERA le
	//délai de grâce (cf. eventqueuesweeper.h, signal 2) : le contrôleur garde
	//ainsi une chance de se reconnecter avant de perdre ses conférences.
	return eventMngr->DestroyEventQueue(id);
}

/**************************************
* CollectQueueIds
*	Files référencées par les conférences (EventQueueSweeper)
**************************************/
void MCU::CollectQueueIds(std::set<int>& ids)
{
	std::lock_guard<std::mutex> lock(mutex);

	for (Conferences::iterator it=conferences.begin(); it!=conferences.end(); ++it)
		if (it->second.queueId > 0)
			ids.insert(it->second.queueId);
}

/**************************************
* DeleteQueueOwners
*	Détruit les conférences liées à une file d'événements (EventQueueSweeper)
**************************************/
int MCU::DeleteQueueOwners(int queueId,const char *reason)
{
	//Extrait les entrées sous verrou : une fois hors de la map, personne ne
	//peut plus en obtenir de nouvelle référence (même principe que
	//DeleteConference)
	std::vector<std::shared_ptr<MultiConf> > expired;

	{
		std::lock_guard<std::mutex> lock(mutex);

		for (Conferences::iterator it=conferences.begin(); it!=conferences.end(); )
		{
			if (it->second.queueId == queueId)
			{
				Log("-MCU: suppression de la conference %d [tag:%ls,queue:%d] : %s\n",
					it->first,it->second.conf->GetTag().c_str(),queueId,reason);

				//Le tag doit partir avec la conférence, sinon il bloque la map
				//des tags (et les événements se routent par tag)
				tags.erase(it->second.conf->GetTag());

				expired.push_back(std::move(it->second.conf));
				it = conferences.erase(it);
			} else
				++it;
		}
	}

	//Terminaison HORS verrou : End() joint des threads (participants, mixeurs)
	//qui peuvent vouloir ce même mutex
	for (std::vector<std::shared_ptr<MultiConf> >::iterator it=expired.begin(); it!=expired.end(); ++it)
		if (*it)
			(*it)->End();

	//Le dernier shared_ptr détruit chaque conférence, hors verrou lui aussi
	int count = (int)expired.size();
	expired.clear();

	return count;
}

/**************************************
* CreateConference
*	Inicia una conferencia
**************************************/
int MCU::CreateConference(std::wstring tag,int queueId)
{
	//Log
	Log(">CreateConference [tag:%ls,queueId:%d]\n",tag.c_str(),queueId);

	//Create the multiconf
	std::shared_ptr<MultiConf> conf = std::make_shared<MultiConf>(tag);

	//Set us as listeners
	conf->SetListener(this);

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Get the id
	int confId = maxId++;

	//Guardamos los datos
	ConferenceEntry entry;
	entry.conf 	= conf;
	entry.queueId	= queueId;

	//a�adimos a la lista
	conferences[confId] = entry;
	//Add to tags
	tags[tag] = confId;

	Log("<CreateConference [%d]\n",confId);

	return confId;
}

/**************************************
* GetConferenceRef
*	Obtiene una referencia a una conferencia
**************************************/
int MCU::GetConferenceRef(int id,std::shared_ptr<MultiConf> &conf)
{
	Log("-GetConferenceRef [%d]\n",id);

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find confernce
	Conferences::iterator it = conferences.find(id);

	//SI no esta
	if (it==conferences.end())
	{
		//Y salimos
		return Error("Conference not found [%d]\n",id);
	}

	//Y obtenemos la referencia compartida a la conferencia
	conf = it->second.conf;

	return true;
}

/**************************************
* GetConferenceID
*	Get conference Id by tag
**************************************/
int MCU::GetConferenceId(const std::wstring& tag)
{
	Log("-GetConferenceId [%ls]\n",tag.c_str());

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find id by tag
	ConferenceTags::iterator it = tags.find(tag);

	//Check if found
	if (it==tags.end())
	{
		//Y salimos
		return Error("Conference tag not found [%ls]\n",tag.c_str());
	}

	//Get id
	return it->second;
}

/**************************************
* DeleteConference
*	Inicializa la mcu
**************************************/
int MCU::DeleteConference(int id)
{
	Log(">DeleteConference [%d]\n",id);

	std::shared_ptr<MultiConf> conf;

	//Extrae la conferencia bajo lock : una vez fuera de la map, nadie puede obtener
	//nuevas referencias (remplace el flag enabled + el polling numRef/sleep(2))
	{
		std::lock_guard<std::mutex> lock(mutex);

		//Find conference
		Conferences::iterator it = conferences.find(id);

		//Check if we found it or not
		if (it==conferences.end())
			//Y salimos
			return Error("Conference not found [%d]\n",id);

		Log("-Disabling conference [%d] tag=[%s]\n",id, it->second.conf->GetTag().c_str());

		//Get conference from ref entry
		conf = std::move(it->second.conf);

		//Remove tag
		tags.erase(conf->GetTag());

		//Remove from list
		conferences.erase(it);
	}

	//End conference : idempotente, y seguro aunque un handler siga teniendo una
	//referencia — el ultimo shared_ptr destruira el objeto.
	conf->End();

	Log("<DeleteConference [%d]\n",id);

	//Exit
	return true;
}

std::shared_ptr<RTMPNetConnection> MCU::Connect(const std::wstring& appName,RTMPNetConnection::Listener* listener)
{
	int confId = 0;
	std::shared_ptr<MultiConf> conf;
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

	//Get type
	std::wstring type = appName.substr(0,i);

	//Get arg
	std::wstring arg = appName.substr(i+1);

	//Check type
	if (type.compare(L"mcutag")==0)
		//Get by tag
		confId = GetConferenceId(arg);
	else
		//Fet conf Id
		confId = wcstol(arg.c_str(),&stopwcs,10);

	//Get conference
	if(!GetConferenceRef(confId,conf))
	{
		//No conference found
		Error("Conference not found [confId:%d]\n",confId);
		//Exit
		return nullptr;
	}

	//Connect
	conf->Connect(listener);

	//Return conf : el shared_ptr mantiene la conferencia viva mientras dure la conexion RTMP
	return conf;
}

int MCU::GetConferenceList(ConferencesInfo& lst)
{
	Log(">GetConferenceList\n");

	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//For each conference
	for (Conferences::iterator it = conferences.begin(); it!=conferences.end(); ++it)
	{
		ConferenceInfo info;
		//Set data
		info.id = it->first;
		info.name = it->second.conf->GetTag();
		info.numPart = it->second.conf->GetNumParticipants();
		//Append new info
		lst[it->first] = info;
	}

	Log("<GetConferenceList\n");

	return true;
}

/**************************************
* GetLoad
*	Nombre de conférences et de participants, sans journalisation
**************************************/
void MCU::GetLoad(int& conferences,int& participants)
{
	std::lock_guard<std::mutex> lock(mutex);

	conferences  = this->conferences.size();
	participants = 0;

	for (Conferences::iterator it = this->conferences.begin(); it!=this->conferences.end(); ++it)
		participants += it->second.conf->GetNumParticipants();
}

/**************************************
* onWebSocketConnection (S5)
*	La porte WebSocket de l'API conférence : /mcu/<confId>/<token>. Résout la
*	conférence (GetConferenceRef) puis délègue la résolution du token au
*	MultiConf — le miroir exact de JSR309Manager::onWebSocketConnection.
**************************************/
void MCU::onWebSocketConnection(const HTTPRequest &request, WebSocket *ws)
{
	Log("MCU: incoming WebSocket connection to %s\n", request.GetRequestURI().c_str());

	// The URL must look (assuming confId 1234 and token deadbeef) as follows:
	//   /mcu/1234/deadbeef
	std::string url = request.GetRequestURI();
	StringParser parser(url);

	if (!parser.MatchString("/mcu"))
	{
		ws->Reject(404, "Not found");
		return;
	}

	if (!parser.ParseChar('/'))
	{
		Error("MCU::onWebSocketConnection() | bad URL: no /mcu/ => HTTP 400\n");
		ws->Reject(400, "Bad Request");
		return;
	}

	if (!parser.ParseInteger())
	{
		Error("MCU::onWebSocketConnection() | bad URL: missing conference ID.\n");
		ws->Reject(400, "Bad Request");
		return;
	}

	int confId = (int) parser.GetIntegerValue();

	if (!parser.ParseChar('/'))
	{
		Error("MCU::onWebSocketConnection() | bad URL: no /mcu/confId/ => HTTP 400\n");
		ws->Reject(400, "Bad Request no sep before token");
		return;
	}

	if (!parser.ParseToken())
	{
		Error("MCU::onWebSocketConnection(): cannot extract token from URL %s\n", url.c_str());
		ws->Reject(400, "Bad Request cannot extract token");
		return;
	}

	std::string token = parser.GetValue();

	std::shared_ptr<MultiConf> conf;
	if (!GetConferenceRef(confId, conf))
	{
		Error("MCU::onWebSocketConnection() | no such conference %d\n", confId);
		ws->Reject(404, "No such conference");
		return;
	}

	conf->onNewMediaConnection(ws, token);
}

void MCU::onParticipantRequestFPU(MultiConf *conf,int partId)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find the conference by tag : si ya no esta, se esta destruyendo, ignoramos el evento
	ConferenceTags::iterator tit = tags.find(conf->GetTag());
	if (tit==tags.end())
		return;
	Conferences::iterator it = conferences.find(tit->second);
	if (it==conferences.end())
		return;

	//Check Event and event queue
	if (eventMngr && it->second.queueId>0)
	{
		//Send new event (la file prend possession... sauf si elle n'existe plus :
		//AddEvent rend 0 SANS detruire l'evenement)
		XmlEvent *event = new ::PlayerRequestFPUEvent(it->first,conf->GetTag(),partId);
		if (!eventMngr->AddEvent(it->second.queueId,event))
			delete event;
	}
}

/**********************
* onParticipantMediaTimeout / onParticipantMediaConnected
*	P7/S1-S2. Meme forme que onParticipantRequestFPU : on retrouve la conference
*	par son tag (si elle n'y est plus, elle se detruit, on ignore) et on empile
*	l'evenement dans la file du controleur.
***********************/
void MCU::onParticipantMediaTimeout(MultiConf *conf,int partId,MediaFrame::Type media,MediaFrame::MediaRole role)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find the conference by tag
	ConferenceTags::iterator tit = tags.find(conf->GetTag());
	if (tit==tags.end())
		return;
	Conferences::iterator it = conferences.find(tit->second);
	if (it==conferences.end())
		return;

	//Check Event and event queue
	if (eventMngr && it->second.queueId>0)
	{
		//Send new event (la file prend possession... sauf si elle n'existe plus)
		XmlEvent *event = new ::ParticipantMediaEvent(MCU::ParticipantMediaTimeout,it->first,conf->GetTag(),partId,(int)media,(int)role);
		if (!eventMngr->AddEvent(it->second.queueId,event))
			delete event;
	}
}

void MCU::onParticipantMediaConnected(MultiConf *conf,int partId,MediaFrame::Type media,MediaFrame::MediaRole role)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find the conference by tag
	ConferenceTags::iterator tit = tags.find(conf->GetTag());
	if (tit==tags.end())
		return;
	Conferences::iterator it = conferences.find(tit->second);
	if (it==conferences.end())
		return;

	//Check Event and event queue
	if (eventMngr && it->second.queueId>0)
	{
		//Send new event (la file prend possession... sauf si elle n'existe plus)
		XmlEvent *event = new ::ParticipantMediaEvent(MCU::ParticipantMediaConnected,it->first,conf->GetTag(),partId,(int)media,(int)role);
		if (!eventMngr->AddEvent(it->second.queueId,event))
			delete event;
	}
}

void MCU::onParticipantRequestDocSharing(MultiConf *conf,int partId,std::wstring status)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Find the conference by tag : si ya no esta, se esta destruyendo, ignoramos el evento
	ConferenceTags::iterator tit = tags.find(conf->GetTag());
	if (tit==tags.end())
		return;
	Conferences::iterator it = conferences.find(tit->second);
	if (it==conferences.end())
		return;

	//Check Event and event queue
	if (eventMngr && it->second.queueId>0)
	{
		//Send new event (la file prend possession... sauf si elle n'existe plus)
		XmlEvent *event = new ::PlayerRequestDocSharingEvent(it->first,conf->GetTag(),partId,status);
		if (!eventMngr->AddEvent(it->second.queueId,event))
			delete event;
	}
}

int MCU::onFileUploaded(const char* url, const char *filename)
{
	Log("-File upload for %s\n",url);

	std::shared_ptr<MultiConf> conf;

	//Skip the first path
	const char *sep = url + strlen("/upload/mcu/app/");

	//If not found
	if (!sep)
		//not found
		return 404;

	//Convert to wstring
	UTF8Parser parser;

	if (!parser.Parse((BYTE*)sep,strlen(sep)))
	{
		//Error
		Error("Error parsing conference tag\n");
		//Error
		return 500;
	}
	
	//Get id by tag
	int confId = GetConferenceId(parser.GetWString());

	//Get conference
	if(!GetConferenceRef(confId,conf))
	{
		//Error
		Error("Conference does not exist\n");
		//Not found
		return 404;
	}

	//Display it
	int ret = conf->AppMixerDisplayImage(filename) ? 200 : 500;

	//REturn result
	return ret;
}
