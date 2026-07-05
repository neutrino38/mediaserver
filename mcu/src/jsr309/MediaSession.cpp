#include "log.h"
#include "MediaSession.h"
#include "JSR309Manager.h"
#include <errno.h>
#include <time.h>

/*****************************************************************************
 * RecorderTimer : minuterie de durée max d'un enregistrement (reason=1)
 *****************************************************************************/
RecorderTimer::RecorderTimer(MediaSession* session,int recorderId,DWORD maxDurationMs)
{
	this->session       = session;
	this->recorderId    = recorderId;
	this->maxDurationMs  = maxDurationMs;
	wakeup      = false;
	stopClaimed = false;
	started     = false;
	pthread_mutex_init(&mutex,NULL);
	pthread_cond_init(&cond,NULL);
}

RecorderTimer::~RecorderTimer()
{
	//Réveille le thread (annulation) et attend sa fin
	pthread_mutex_lock(&mutex);
	wakeup = true;
	pthread_cond_signal(&cond);
	bool joinable = started;
	pthread_mutex_unlock(&mutex);

	if (joinable)
		pthread_join(thread,NULL);

	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&mutex);
}

void RecorderTimer::Start()
{
	pthread_mutex_lock(&mutex);
	started = true;
	pthread_mutex_unlock(&mutex);
	pthread_create(&thread,NULL,run,this);
}

bool RecorderTimer::ClaimStop()
{
	bool claimed = false;
	pthread_mutex_lock(&mutex);
	if (!stopClaimed)
	{
		stopClaimed = true;
		claimed = true;
	}
	pthread_mutex_unlock(&mutex);
	return claimed;
}

void* RecorderTimer::run(void* arg)
{
	((RecorderTimer*)arg)->Run();
	return NULL;
}

void RecorderTimer::Run()
{
	//Calcule l'échéance absolue (CLOCK_REALTIME, comme attend pthread_cond_timedwait)
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME,&deadline);
	deadline.tv_sec  += maxDurationMs/1000;
	deadline.tv_nsec += (long)(maxDurationMs%1000)*1000000L;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec  += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	//Attend l'échéance ou une annulation
	pthread_mutex_lock(&mutex);
	int rc = 0;
	while (!wakeup && rc != ETIMEDOUT)
		rc = pthread_cond_timedwait(&cond,&mutex,&deadline);
	bool timedOut = (!wakeup && rc == ETIMEDOUT);
	pthread_mutex_unlock(&mutex);

	//Échéance atteinte sans annulation : on tente de prendre l'arrêt
	if (timedOut && ClaimStop())
		session->onRecorderMaxDuration(recorderId);
}

MediaSession::MediaSession(std::wstring tag)
{
	//Init
	maxEndpointId = 1;
	maxPlayersId = 1;
	maxRecordersId = 1;
	maxAudioMixerId = 1;
	maxVideoMixerId = 1;
	maxVideoTranscoderId = 1;
	maxEventContextId = 1;
	//No hay manager de eventos todavia
	eventMngr = NULL;
	sessionId = 0;
	//Store it
	this->tag = tag;
}

void MediaSession::SetEventHandler(int sessionId, JSR309Manager* mngr)
{
	//Store back-reference vers le manager pour pouvoir publier des événements
	this->sessionId = sessionId;
	this->eventMngr  = mngr;
}

int MediaSession::PostEvent(int eventContextId, JSR309Event* ev)
{
	//Sans manager câblé, on ne peut rien publier : on libère l'événement pour
	//éviter une fuite mémoire.
	if (!eventMngr || sessionId <= 0)
	{
		delete ev;
		return 0;
	}
	//Le manager (et in fine la file d'événements) prend possession de l'événement.
	return eventMngr->PostEvent(sessionId, eventContextId, ev);
}

MediaSession::~MediaSession()
{
	End();
}

void MediaSession::SetListener(MediaSession::Listener *listener,void* param)
{
	//Store values
	this->listener = listener;
	this->param = param;
}

int MediaSession::Init()
{
	Log("-Init media session\n");
	
	//Inited
	return 1;
}

int MediaSession::End()
{
	Log(">End media session\n");

	//Annule toutes les minuteries de recorder (annule + join des threads) AVANT de
	//libérer les recorders, pour qu'aucun thread de minuterie n'y accède encore.
	for (RecorderTimers::iterator it=recorderTimers.begin(); it!=recorderTimers.end(); ++it)
		delete(it->second);
	recorderTimers.clear();

	//Delete all recorders
	for (Recorders::iterator it=recorders.begin(); it!=recorders.end(); ++it)
		//Delete object
		delete(it->second);
	//Clean map
	recorders.clear();

	//End all endpoints
	for (Endpoints::iterator it=endpoints.begin(); it!=endpoints.end(); ++it)
		//End it
		it->second->End();

	//Delete all players
	for (Players::iterator it=players.begin(); it!=players.end(); ++it)
		//Delete object
		delete(it->second);
	//Clean map
	players.clear();

	//Delete all video transcoders
	for (VideoTranscoders::iterator it=videoTranscoders.begin(); it!=videoTranscoders.end(); ++it)
		//Delete object
		delete(it->second);

	//Clean map
	videoTranscoders.clear();

	//Delete all endpoints
	for (Endpoints::iterator it=endpoints.begin(); it!=endpoints.end(); ++it)
		//Delete object
		delete(it->second);

	//Clean map
	endpoints.clear();

	eventContexts.clear();
	
	Log("<End media session\n");

	return 1;
}


int MediaSession::PlayerCreate(std::wstring tag)
{
    //Create ID
    int playerId = maxPlayersId++;
	//Create player
	Player* player = new Player(tag);
	//Set event listener
	player->SetListener(this,(void*)(intptr_t)playerId);
	//Append the player
	players[playerId] = player;

	int eventContextId = maxEventContextId++;
	eventContexts[eventContextId]= new JSR309EventContext( playerId, MediaFrame::Video, MediaFrame::VIDEO_MAIN);
	player->SetEventContextId(MediaFrame::Video,eventContextId);
	//Mémorise le contexte pour publier les événements de cycle de vie du player
	playerEventCtx[playerId] = eventContextId;

	//Return it
	return playerId;
	
}

int MediaSession::PlayerOpen(int playerId,const char* filename)
{
        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second;

        return player->Open(filename);
}

int MediaSession::PlayerPlay(int playerId)
{
        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second;

        //Start playback
        int res = player->Play();

        //Publie PlayerStartedEvent si le démarrage a réussi
        if (res)
        {
                EventCtxMap::iterator ctx = playerEventCtx.find(playerId);
                if (ctx != playerEventCtx.end())
                        PostEvent(ctx->second, new PlayerStartedEvent(player->GetTag()));
        }

        return res;
}

int MediaSession::PlayerSeek(int playerId,QWORD time)
{
        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second;

        return player->Seek(time);
}

int MediaSession::PlayerStop(int playerId)
{
        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second;

        return player->Stop();
}

int MediaSession::PlayerClose(int playerId)
{
        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second;

        return player->Close();
}

int MediaSession::PlayerDelete(int playerId)
{
        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second;
		
        //Remove from list
        players.erase(it);

        //Relete player
        delete(player);

        return 1;
}

int MediaSession::RecorderCreate(std::wstring tag)
{
        //Create ID
        int recorderId = maxRecordersId++;
	//Create recorder
	Recorder* recorder = new Recorder(tag);
	//Append the recorder
        recorders[recorderId] = recorder;

	//Contexte d'événement du recorder, symétrique à celui des players
	int eventContextId = maxEventContextId++;
	eventContexts[eventContextId] = new JSR309EventContext( recorderId, MediaFrame::Video, MediaFrame::VIDEO_MAIN);
	recorderEventCtx[recorderId] = eventContextId;

        //Return it
        return recorderId;
}

int MediaSession::RecorderRecord(int recorderId,const char* filename,DWORD maxDuration)
{
        //Get recorder
        Recorders::iterator it = recorders.find(recorderId);
        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;
	//create recording
        if (!recorder->Create(filename))
		//Error
		return Error("-Could not create file");
	//Start recording
	int res = recorder->Record();

	//Sur succès : publie RecorderStartedEvent et arme la minuterie de durée max
	if (res)
	{
		EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
		if (ctx != recorderEventCtx.end())
			PostEvent(ctx->second, new RecorderStartedEvent(recorder->GetTag()));

		//Durée max demandée : (ré)arme la minuterie d'arrêt automatique
		if (maxDuration > 0)
		{
			//Supprime une éventuelle minuterie précédente
			RecorderTimers::iterator t = recorderTimers.find(recorderId);
			if (t != recorderTimers.end())
			{
				delete t->second;
				recorderTimers.erase(t);
			}
			//Arme la nouvelle minuterie
			RecorderTimer* timer = new RecorderTimer(this,recorderId,maxDuration);
			recorderTimers[recorderId] = timer;
			timer->Start();
		}
	}
	return res;
}

int MediaSession::RecorderStop(int recorderId)
{
	//Get recorder
        Recorders::iterator it = recorders.find(recorderId);
        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;

	//Récupère et détache l'éventuelle minuterie de durée max
	RecorderTimer* timer = NULL;
	RecorderTimers::iterator t = recorderTimers.find(recorderId);
	if (t != recorderTimers.end())
	{
		timer = t->second;
		recorderTimers.erase(t);
	}

	//Détermine qui pilote l'arrêt : si la minuterie a déjà déclenché (reason=1),
	//elle a déjà fermé le fichier et publié l'événement.
	bool claimed = timer ? timer->ClaimStop() : true;

	//Détruit la minuterie (annule + join du thread)
	if (timer)
		delete timer;

	//La durée max a déjà arrêté l'enregistrement : plus rien à faire
	if (!claimed)
		return 1;

	//Arrêt explicite
        int res = recorder->Close();

	//Publie RecorderStoppedEvent (reason=0, explicite)
	EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
	if (ctx != recorderEventCtx.end())
		PostEvent(ctx->second, new RecorderStoppedEvent(recorder->GetTag(), RecorderStoppedEvent::Explicit));

	return res;
}

int MediaSession::RecorderDelete(int recorderId)
{
        //Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;

	//Annule et détruit l'éventuelle minuterie AVANT de libérer le recorder
	//(le join garantit que le thread de minuterie n'utilise plus le recorder).
	RecorderTimers::iterator t = recorderTimers.find(recorderId);
	if (t != recorderTimers.end())
	{
		delete t->second;
		recorderTimers.erase(t);
	}

        //Remove from list
        recorders.erase(it);

        //Relete player
        delete(recorder);

        return 1;
}

void MediaSession::onRecorderMaxDuration(int recorderId)
{
	//Appelé depuis le thread RecorderTimer à l'expiration de la durée max.
	//NB : ne touche PAS la map recorderTimers (nettoyée par RecorderStop/Delete).
	Recorders::iterator it = recorders.find(recorderId);
	if (it == recorders.end())
		return;
	Recorder* recorder = it->second;

	//Arrêt automatique de l'enregistrement
	recorder->Close();

	//Publie RecorderStoppedEvent (reason=1, durée max atteinte)
	EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
	if (ctx != recorderEventCtx.end())
		PostEvent(ctx->second, new RecorderStoppedEvent(recorder->GetTag(), RecorderStoppedEvent::MaxDuration));
}

int MediaSession::RecorderAttachToAudioMixerPort(int recorderId,int mixerId,int portId)
{
	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;

	 //Get Player
        AudioMixers::iterator itMixer = audioMixers.find(mixerId);

        //If not found
        if (itMixer==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");

	 //Get it
        AudioMixerResource* audioMixer = itMixer->second;

	//Attach
	return recorder->Attach(MediaFrame::Audio,audioMixer->GetJoinable(portId));
}

int MediaSession::RecorderAttachToVideoMixerPort(int recorderId,int mixerId,int portId)
{
	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;

	 //Get Player
        VideoMixers::iterator itMixer = videoMixers.find(mixerId);

        //If not found
        if (itMixer==videoMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");

	 //Get it
        VideoMixerResource* videoMixer = itMixer->second;

	//And attach
	return recorder->Attach(MediaFrame::Video,videoMixer->GetJoinable(portId));
}

int MediaSession::RecorderAttachToEndpoint(int recorderId,int endpointId,MediaFrame::Type media)
{
	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;

	//Get source endpoint
        Endpoints::iterator itEndpoints = endpoints.find(endpointId);

        //If not found
        if (itEndpoints==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* source = itEndpoints->second;

	//Attach
	return recorder->Attach(media,source->GetJoinable(media));
}

int MediaSession::RecorderDettach(int recorderId,MediaFrame::Type media)
{
	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second;

	//Attach
	return recorder->Dettach(media);
}


Endpoint* MediaSession::GetEndpoint(int endpointId) 
{
	
	Endpoints::iterator it = endpoints.find(endpointId);

	//If not found
	if (it == endpoints.end())
	{
		//Exit
		Error("Endpoint [%d] not found\n", endpointId);
		return NULL;
	}
	
	//Get it
	return (Endpoint*) it->second;

}

Player*	 MediaSession::GetPlayer(int playerId) 
{
	
	Players::iterator it = players.find(playerId);

	//If not found
	if (it == players.end())
	{
		//Exit
		Error("player [%d] not found\n", playerId);
		return NULL;
	}
	
	//Get it
	return (Player*) it->second;

}

JSR309EventContext*	MediaSession::GetEventContext(int EventContextId) 
{
	
	EventContexts::iterator it = eventContexts.find(EventContextId);

	//If not found
	if (it == eventContexts.end())
	{
		//Exit
		Error("event context [%d] not found\n", EventContextId);
		return NULL;
	}
	
	//Get it
	return (JSR309EventContext*) it->second;

}

int MediaSession::EndpointCreate(std::wstring name,bool audioSupported,bool videoSupported,bool textSupport)
{
    //Create endpoint
    Endpoint* endpoint = new Endpoint(name,audioSupported,videoSupported,textSupport);
	
	//Init it
	endpoint->Init();
	
	//Create ID
    int endpointId = maxEndpointId++;
	int eventContextId;
	//Log endpoint tag name
	Log("-EndpointCreate [%d,%ls]\n",endpointId,endpoint->GetName().c_str());
	
	//Append
	endpoints[endpointId] = endpoint;
	
	if (audioSupported)
	{
		eventContextId = maxEventContextId++;
		eventContexts[eventContextId]= new JSR309EventContext( endpointId,MediaFrame::Audio, MediaFrame::VIDEO_MAIN);
		endpoint->SetEventContextId(MediaFrame::Audio, MediaFrame::VIDEO_MAIN, eventContextId);
	}
	if (videoSupported)
	{
		eventContextId = maxEventContextId++;
		eventContexts[eventContextId]= new JSR309EventContext( endpointId,MediaFrame::Video, MediaFrame::VIDEO_MAIN);
		endpoint->SetEventContextId(MediaFrame::Video, MediaFrame::VIDEO_MAIN, eventContextId);
	}
	if(textSupport)
	{
		eventContextId = maxEventContextId++;
		eventContexts[eventContextId]= new JSR309EventContext( endpointId,MediaFrame::Text, MediaFrame::VIDEO_MAIN);
		endpoint->SetEventContextId(MediaFrame::Text, MediaFrame::VIDEO_MAIN, eventContextId);
	}
	
	//Return it
    return endpointId;

}
int MediaSession::EndpointDelete(int endpointId)
{
	//Get Player
	Endpoints::iterator it = endpoints.find(endpointId);

	//If not found
	if (it==endpoints.end())
			//Exit
			return Error("Endpoint not found\n");
	//Get it
	Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointDelete [%ls]\n",endpoint->GetName().c_str());

	//Remove from list
	endpoints.erase(it);

	//End it
	endpoint->End();

	//Relete endpoint
	delete(endpoint);

	return 1;
}

int MediaSession::EndpointSetLocalCryptoSDES(int endpointId,MediaFrame::Type media,const char *suite,const char* key)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Call it
	return endpoint->SetLocalCryptoSDES(media,suite,key);
}

int MediaSession::EndpointSetRemoteCryptoSDES(int endpointId,MediaFrame::Type media,const char *suite,const char* key)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Call it
	return endpoint->SetRemoteCryptoSDES(media,suite,key);
}

int MediaSession::EndpointSetRemoteCryptoDTLS(int endpointId,MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Call it
	Log("-EndpointSetRemoteCryptoDTLS: endpoint %ls and media %s.\n", endpoint->GetName().c_str(), MediaFrame::TypeToString(media));
	return endpoint->SetRemoteCryptoDTLS(media,setup,hash,fingerprint);
}


int MediaSession::EndpointSetLocalSTUNCredentials(int endpointId,MediaFrame::Type media,const char *username,const char* pwd)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	Log("-EndpointSetLocalSTUNCredentials: endpoint %ls and media %s.\n", endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Call it
	return endpoint->SetLocalSTUNCredentials(media,username,pwd);
}

int MediaSession::EndpointSetRemoteSTUNCredentials(int endpointId,MediaFrame::Type media,const char *username,const char* pwd)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;
	Log("-EndpointSetRemoteSTUNCredentials: endpoint %ls and media %s.\n", endpoint->GetName().c_str(), MediaFrame::TypeToString(media));
	//Call it
	return endpoint->SetRemoteSTUNCredentials(media,username,pwd);
}

int MediaSession::EndpointSetRTPProperties(int endpointId,MediaFrame::Type media,const Properties& properties)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;
	//Call it
	return endpoint->SetRTPProperties(media,properties);
}

//Endpoint Video functionality
int MediaSession::EndpointStartSending(int endpointId,MediaFrame::Type media,char *sendVideoIp,int sendVideoPort,RTPMap& rtpMap)
{
        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointStartSending [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	return endpoint->StartSending(media,sendVideoIp, sendVideoPort, rtpMap);
}

int MediaSession::EndpointAddICECandidate(int endpointId,MediaFrame::Type media,const char* candidate)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Délègue au endpoint (trickle ICE Niveau 1)
	return endpoint->AddICECandidate(media,candidate);
}

int MediaSession::EndpointStartRTPTimeout(int endpointId,MediaFrame::Type media,DWORD timeoutMs)
{
        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Délègue au endpoint (watchdog d'inactivité RTP - gap 5)
	return endpoint->ArmRTPTimeout(media,timeoutMs);
}

int MediaSession::EndpointStopSending(int endpointId,MediaFrame::Type media)
{
        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointStopSending [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	return endpoint->StopSending(media);
}

int MediaSession::EndpointStartReceiving(int endpointId,MediaFrame::Type media,RTPMap& rtpMap)
{
        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointStartReceiving [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	return endpoint->StartReceiving(media,rtpMap);
}
int MediaSession::EndpointStopReceiving(int endpointId,MediaFrame::Type media)
{
        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointStopReceiving [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));
	
	//Execute
	return endpoint->StopReceiving(media);
}

int MediaSession::EndpointRequestUpdate(int endpointId,MediaFrame::Type media)
{
	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointRequestUpdate [%ls]\n",endpoint->GetName().c_str());

	//Execute
	return endpoint->RequestUpdate(media);
}
int MediaSession::EndpointAttachToPlayer(int endpointId,int playerId,MediaFrame::Type media)
{
	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointAttachToPlayer [%ls]\n",endpoint->GetName().c_str());

	 //Get Player
        Players::iterator itPlayer = players.find(playerId);

        //If not found
        if (itPlayer==players.end())
                //Exit
                return Error("Player not found\n");
	
	 //Get it
        Player* player = itPlayer->second;
	
	//Attach
	return endpoint->Attach(media,MediaFrame::VIDEO_MAIN,player->GetJoinable(media));
}

int MediaSession::EndpointAttachToAudioMixerPort(int endpointId,int mixerId,int portId)
{
	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointAttachToAudioMixerPort [%ls]\n",endpoint->GetName().c_str());

	 //Get Player
        AudioMixers::iterator itMixer = audioMixers.find(mixerId);

        //If not found
        if (itMixer==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");

	 //Get it
        AudioMixerResource* audioMixer = itMixer->second;

	//Attach
	return endpoint->Attach(MediaFrame::Audio,MediaFrame::VIDEO_MAIN,audioMixer->GetJoinable(portId));
}

int MediaSession::EndpointAttachToVideoMixerPort(int endpointId,int mixerId,int portId)
{
	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointAttachToVideoMixerPort [%ls]\n",endpoint->GetName().c_str());

	 //Get Player
        VideoMixers::iterator itMixer = videoMixers.find(mixerId);

        //If not found
        if (itMixer==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found\n");

	 //Get it
        VideoMixerResource* videoMixer = itMixer->second;

	//And attach
	return endpoint->Attach(MediaFrame::Video,MediaFrame::VIDEO_MAIN,videoMixer->GetJoinable(portId));
}

int MediaSession::EndpointAttachToVideoTranscoder(int endpointId,int videoTranscoderId)
{
	//Get endpoint
        Endpoint* endpoint = GetEndpoint(endpointId);

        //If not found
        if (endpoint == NULL)
                //Exit
                return 0;

	 //Get Video transcoder
        VideoTranscoders::iterator itTranscoder = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (itTranscoder==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found[%d]\n",videoTranscoderId);

	 //Get it
        VideoTranscoder* videoTranscoder = itTranscoder->second;

	//Log endpoint tag name
	Log("-EndpointAttachToVideoTranscoder [endpoint:%ls,transcoder:%ls]\n",endpoint->GetName().c_str(),videoTranscoder->GetName().c_str());

	//And attach
	return endpoint->Attach(MediaFrame::Video,MediaFrame::VIDEO_MAIN,videoTranscoder);
}

int MediaSession::EndpointAttachToEndpoint(int endpointId,int sourceId,MediaFrame::Type media)
{
	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointAttachToEndpoint [%ls] for media\n",endpoint->GetName().c_str());

	//Get source endpoint
        it = endpoints.find(sourceId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* source = it->second;

	Log("-EndpointAttachToEndpoint: activating TS transparency.\n");
	endpoint->SetRTPTsTransparency(media, true, MediaFrame::VIDEO_MAIN);
	source->SetRTPTsTransparency(media, true, MediaFrame::VIDEO_MAIN);
	//Attach
	return endpoint->Attach(media,MediaFrame::VIDEO_MAIN,source->GetJoinable(media));
}

int MediaSession::EndpointDettach(int endpointId,MediaFrame::Type media)
{
	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");

	//Get it
        Endpoint* endpoint = it->second;

	//Log endpoint tag name
	Log("-EndpointDettach [%ls]\n",endpoint->GetName().c_str());

	//Attach
	return endpoint->Detach(media);
}

void MediaSession::onEndOfFile(Player *player,void* playerId)
{
	//Récupère l'id du player transmis à SetListener
	int id = (int)(intptr_t)playerId;

	//Récupère le contexte d'événement associé au player
	EventCtxMap::iterator it = playerEventCtx.find(id);
	if (it == playerEventCtx.end())
	{
		Error("onEndOfFile: no event context for player [%d]\n", id);
		return;
	}

	//Publie l'événement de fin de lecture (PlayerEndOfFileEvent = 1)
	PostEvent(it->second, new PlayerEndOfFileEvent(player->GetTag()));
}

int MediaSession::AudioMixerCreate(std::wstring tag)
{
        //Create ID
        int audioMixerId = maxAudioMixerId++;
	//Create player
	AudioMixerResource* audioMixer = new AudioMixerResource(tag);
	//Init it
	audioMixer->Init();
        //Append the player
        audioMixers[audioMixerId] = audioMixer;
        //Return it
        return audioMixerId;
}

int MediaSession::AudioMixerDelete(int mixerId)
{
        //Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;

        //Remove from list
        audioMixers.erase(it);

	//End it
	audioMixer->End();

        //Relete audioMixer
        delete(audioMixer);

        return 1;
}

int MediaSession::AudioMixerPortCreate(int mixerId,std::wstring tag)
{
	//Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;

	//Execute
	return audioMixer->CreatePort(tag);
}

int MediaSession::AudioMixerPortSetCodec(int mixerId,int portId,AudioCodec::Type codec)
{
	//Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;

	//Execute
	return audioMixer->SetPortCodec(portId,codec);
}

int MediaSession::AudioMixerPortDelete(int mixerId,int portId)
{
	//Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;

	//Execute
	return audioMixer->DeletePort(portId);
}


int MediaSession::AudioMixerPortAttachToEndpoint(int mixerId,int portId,int endpointId)
{
	//Get mixer
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;

	//Get endpoint
        Endpoints::iterator itEnd = endpoints.find(endpointId);

        //If not found
        if (itEnd==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");

        //Get it
        Endpoint* endpoint = itEnd->second;

	//Log endpoint tag name
	Log("-AudioMixerPortAttachToEndpoint [%ls]\n",endpoint->GetName().c_str());

	//Attach
	return audioMixer->Attach(portId,endpoint->GetJoinable(MediaFrame::Audio));
}

int MediaSession::AudioMixerPortAttachToPlayer(int mixerId,int portId,int playerId)
{
	//Get mixer
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;

	 //Get Player
        Players::iterator itPlayer = players.find(playerId);

        //If not found
        if (itPlayer==players.end())
                //Exit
                return Error("Player not found\n");

	 //Get it
        Player* player = itPlayer->second;

	//Attach
	return audioMixer->Attach(portId,player->GetJoinable(MediaFrame::Audio));
}

int MediaSession::AudioMixerPortDettach(int mixerId,int portId)
{
	//Get mixer
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        AudioMixerResource* audioMixer = it->second;
	
       //Attach
	return audioMixer->Dettach(portId);
}

int MediaSession::VideoMixerCreate(std::wstring tag)
{
        //Create ID
        int videoMixerId = maxVideoMixerId++;
	//Create player
	VideoMixerResource* videoMixer = new VideoMixerResource(tag);
	//Init
	videoMixer->Init(Mosaic::mosaic2x2,PAL);
        //Append the player
        videoMixers[videoMixerId] = videoMixer;
        //Return it
        return videoMixerId;
}

int MediaSession::VideoMixerDelete(int mixerId)
{
        //Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

        //Remove from list
        videoMixers.erase(it);

	//End it
	videoMixer->End();

        //Relete videoMixer
        delete(videoMixer);

        return 1;
}

int MediaSession::VideoMixerPortCreate(int mixerId,std::wstring tag, int mosaicId)
{
	//Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

	//Execute
	return videoMixer->CreatePort(tag,mosaicId);
}

int MediaSession::VideoMixerPortSetCodec(int mixerId,int portId,VideoCodec::Type codec,int size,int fps,int bitrate,int intraPeriod)
{
	//Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

	//Execute
	return videoMixer->SetPortCodec(portId,codec,size,fps,bitrate,intraPeriod);
}

int MediaSession::VideoMixerPortDelete(int mixerId,int portId)
{
	//Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

	//Execute
	return videoMixer->DeletePort(portId);
}


int MediaSession::VideoMixerPortAttachToEndpoint(int mixerId,int portId,int endpointId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

	//Get endpoint
        Endpoints::iterator itEnd = endpoints.find(endpointId);

        //If not found
        if (itEnd==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");

        //Get it
        Endpoint* endpoint = itEnd->second;

	//Log endpoint tag name
	Log("-VideoMixerPortAttachToEndpoint [%ls]\n",endpoint->GetName().c_str());

	//Attach
	return videoMixer->Attach(portId,endpoint->GetJoinable(MediaFrame::Video));
}

int MediaSession::VideoMixerPortAttachToPlayer(int mixerId,int portId,int playerId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

	 //Get Player
        Players::iterator itPlayer = players.find(playerId);

        //If not found
        if (itPlayer==players.end())
                //Exit
                return Error("Player not found\n");

	 //Get it
        Player* player = itPlayer->second;

	//Attach
	return videoMixer->Attach(portId,player->GetJoinable(MediaFrame::Video));
}

int MediaSession::VideoMixerPortDettach(int mixerId,int portId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->Dettach(portId);
}

int MediaSession::VideoMixerMosaicCreate(int mixerId,Mosaic::Type comp,int size)
{
		//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->CreateMosaic(comp,size);
}

int MediaSession::VideoMixerMosaicDelete(int mixerId,int mosaicId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->DeleteMosaic(mosaicId);
}

int MediaSession::VideoMixerMosaicSetSlot(int mixerId,int mosaicId,int num,int portId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->SetSlot(mosaicId,num,portId);
}

int MediaSession::VideoMixerMosaicSetCompositionType(int mixerId,int mosaicId,Mosaic::Type comp,int size)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->SetCompositionType(mosaicId,comp,size);
}

int MediaSession::VideoMixerMosaicSetOverlayPNG(int mixerId,int mosaicId,const char* overlay)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->SetOverlayPNG(mosaicId,overlay);
}

int MediaSession::VideoMixerMosaicResetSetOverlay(int mixerId,int mosaicId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->ResetOverlay(mosaicId);
}

int MediaSession::VideoMixerMosaicAddPort(int mixerId,int mosaicId,int portId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->AddMosaicParticipant(mosaicId,portId);
}

int MediaSession::VideoMixerMosaicRemovePort(int mixerId,int mosaicId,int portId)
{
	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        VideoMixerResource* videoMixer = it->second;

       //Attach
	return videoMixer->RemoveMosaicParticipant(mosaicId,portId);
}

int MediaSession::AudioTranscoderCreate(std::wstring tag)
{
	//Create ID
        int transcoderId = maxVideoTranscoderId++;
	//Create trascoder
	AudioTranscoder* transcoder = new AudioTranscoder(tag);
	//Init
	transcoder->Init(true);
        //Append the player
        audioTranscoders[transcoderId] = transcoder;
	Log("-Created audio transcoder ID %d.\n", transcoderId);
        //Return it
        return transcoderId;
}

int MediaSession::AudioTranscoderDelete(int transcoderId)
{
    //Get transcoders
    AudioTranscoders::iterator it = audioTranscoders.find(transcoderId);

    //If not found
    if (it==audioTranscoders.end())
            //Exit
            return Error("AudioTranscoder not found [%d]\n",transcoderId);
    //Get it
    AudioTranscoder* transcoder = it->second;

    //Remove from list
    audioTranscoders.erase(it);

    //End it
    if ( transcoder->End() )
    {
        delete transcoder;
        return 1;
    }
    else
    {
        return Error("Failed to stop AudioTranscoder %s.\n", transcoderId);
    }
}

AudioTranscoder * MediaSession::GetAudioTranscoder(int transcoderId)
{
    //Get transcoders
    AudioTranscoders::iterator it = audioTranscoders.find(transcoderId);

    //If not found
    if (it==audioTranscoders.end())
    {
         //Exit
        Error("AudioTranscoder not found [%d]\n",transcoderId);
        return NULL;
    }
    
    //Get it
   return it->second;
    
}


int MediaSession::VideoTranscoderCreate(std::wstring tag)
{
	//Create ID
        int videoTranscoderId = maxVideoTranscoderId++;
	//Create trascoder
	VideoTranscoder* videoTranscoder = new VideoTranscoder(tag);
	//Init
	videoTranscoder->Init(false);
        //Append the player
        videoTranscoders[videoTranscoderId] = videoTranscoder;
        //Return it
        return videoTranscoderId;
}

int MediaSession::VideoTranscoderFPU(int videoTranscoderId)
{
	//Get Player
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        VideoTranscoder* videoTranscoder = it->second;

	//Execute
	videoTranscoder->Update();

	//OK
	return 1;
}

int MediaSession::VideoTranscoderSetCodec(int videoTranscoderId,VideoCodec::Type codec,int size,int fps,int bitrate,int intraPeriod,
					  Properties & props)
{
	//Get Player
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        VideoTranscoder* videoTranscoder = it->second;

	//Execute
	return videoTranscoder->SetCodec(codec,size,fps,bitrate,intraPeriod, props);
}

int MediaSession::VideoTranscoderDelete(int videoTranscoderId)
{
	//Get Player
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        VideoTranscoder* videoTranscoder = it->second;

        //Remove from list
        videoTranscoders.erase(it);

	//End it
	videoTranscoder->End();

        //Relete videoMixer
        delete(videoTranscoder);

        return 1;
}

int MediaSession::VideoTranscoderAttachToEndpoint(int videoTranscoderId,int endpointId)
{
	//Get mixer
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        VideoTranscoder* videoTranscoder = it->second;

	//Get endpoint
        Endpoints::iterator itEnd = endpoints.find(endpointId);

        //If not found
        if (itEnd==endpoints.end())
                //Exit
                return Error("Endpoint not found [%d]\n",endpointId);

        //Get it
        Endpoint* endpoint = itEnd->second;

	//Log endpoint tag name
	Log("-VideoTranscoderAttachToEndpoint [transcoder:%ls,endpoint:%ls]\n",videoTranscoder->GetName().c_str(),endpoint->GetName().c_str());

	//Attach
	return videoTranscoder->Attach(endpoint->GetJoinable(MediaFrame::Video));
}

int MediaSession::VideoTranscoderDettach(int videoTranscoderId)
{
	//Get mixer
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        VideoTranscoder* videoTranscoder = it->second;

       //Attach
	return videoTranscoder->Dettach();
}

int MediaSession::ConfigureMediaConnection( int endpointId, MediaFrame::Type media, MediaFrame::MediaRole role, 
				      MediaFrame::MediaProtocol proto, const char * token, const char * expectedPayload )
{
	Log("-ConfigureMediaConnection: endpoint %d, for %s (%s), proto = %s.\n",
	    endpointId, MediaFrame::TypeToString(media), MediaFrame::RoleToString(role),
	    MediaFrame::ProtocolToString(proto) );
	//Get source endpoint
	if ( (token == NULL || token[0] == 0) 
	     &&
	     (proto == MediaFrame::RTMP || proto == MediaFrame::WS) )
	{
		Error("Protocol %d requires a valid token.\n", proto);
		return 0;
	}
	
	Endpoints::iterator itEndpoints = endpoints.find(endpointId);

	//If not found
	//If not found
	if (itEndpoints==endpoints.end())
	{
		//Exit
		Error("Endpoint %d not found for this session\n",endpointId);
		return -1;
	}
	
	int ret = itEndpoints->second->ConfigureMediaConnection(media, role, proto, expectedPayload);
	
	if (ret == 1)
	{
	    if (proto == MediaFrame::RTMP || proto == MediaFrame::WS)
	    {
		//Create an association
		MediaCnxToken tokenInfo;
		std::string tokenstr(token);
		
		
		tokenInfo.endpointId = endpointId;
		tokenInfo.media = media;
		tokenInfo.role = role;
		tokenInfo.proto = proto;
		
		tokens.insert(std::pair<std::string, MediaCnxToken>(tokenstr,tokenInfo) );
		Debug("-ConfigureMediaConnection: Associated token %s with endpoint %d, media %s, proto %s.\n",
		      token, endpointId, MediaFrame::TypeToString(media),  MediaFrame::ProtocolToString(proto) );
	    }
	}
	return 0;
}
				      

// url for websocket media is http://host:port/sessionId/token
// the token associates an URL with a quadruplet (endpointId, media, role, protocol)
int MediaSession::onNewMediaConnection(WebSocket *ws, const std::string & token)
{
	Tokens::iterator it = tokens.find(token);
	
	if ( it == tokens.end() )
	{
		ws->Reject(404, "No such token"); 
		return Error("-onNewMediaConnection: token %s not found.\n", 
			     token.c_str() );
	}
	
	Endpoints::iterator itEndpoints = endpoints.find(it->second.endpointId);
	
	if (itEndpoints==endpoints.end())
	{
		ws->Reject(404, "Endpoint not found");
		return Error("-onNewMediaConnection: token %s was associated with endpoint %d but"
			     "endpoint is no longer valid.\n", token.c_str(), it->second.endpointId );
	}
	
	return itEndpoints->second->onNewMediaConnection( it->second.media, 
						          it->second.role,
							  it->second.proto,
							  ws );
}
	
