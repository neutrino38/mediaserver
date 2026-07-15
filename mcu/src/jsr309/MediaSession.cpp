#include "log.h"
#include "MediaSession.h"
#include "JSR309Manager.h"
#include <errno.h>
#include <time.h>

/*****************************************************************************
 * RecorderTimer : minuterie de durée max d'un enregistrement (reason=1)
 *****************************************************************************/
RecorderTimer::RecorderTimer(std::weak_ptr<MediaSession> session,int recorderId,DWORD maxDurationMs)
{
	this->session       = session;
	this->recorderId    = recorderId;
	this->maxDurationMs  = maxDurationMs;
	stopClaimed = false;
	cancelled   = false;
}

RecorderTimer::~RecorderTimer()
{
	//Annule la minuterie et réveille le thread (flag sous mutex : pas de réveil perdu)
	{
		std::lock_guard<std::mutex> lock(mutex);
		cancelled = true;
		cond.notify_all();
	}

	if (thread.joinable()) thread.join();
}

void RecorderTimer::Start()
{
	thread = std::thread(&RecorderTimer::Run,this);
}

bool RecorderTimer::ClaimStop()
{
	bool claimed = false;
	std::lock_guard<std::mutex> lock(mutex);
	if (!stopClaimed)
	{
		stopClaimed = true;
		claimed = true;
	}
	cond.notify_all();
	return claimed;
}


void RecorderTimer::Run()
{
	//Attend l'échéance, un ClaimStop ou l'annulation. Le prédicat protège des
	//réveils intempestifs et des notifications émises avant l'entrée en attente.
	std::unique_lock<std::mutex> lock(mutex);
	bool woken = cond.wait_for(lock, std::chrono::milliseconds(maxDurationMs),
	                           [this]{ return cancelled || stopClaimed; });
	if (woken)
	{
		//Réveillé par ClaimStop ou destruction : plus rien à faire
		Log("-RecorderTimer::Run() timer interrupted before expiration\n");
		return;
	}

	//Échéance atteinte : tente de prendre l'arrêt (perd si RecorderStop est passé entre-temps)
	if (!stopClaimed)
	{
		stopClaimed = true;
		lock.unlock();
		Log("-RecorderTimer::Run() max duration of %u ms reached\n", (unsigned)maxDurationMs);
		//La session peut avoir été détruite entre-temps : le weak_ptr protège (H-2)
		std::shared_ptr<MediaSession> sess = session.lock();
		if (sess)
			sess->onRecorderMaxDuration(recorderId);
	}
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
	//No listener
	listener = NULL;
	param = NULL;
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
	//NB : suppose le mutex de session tenu par l'appelant.
	//Sans manager câblé, on ne peut rien publier : on libère l'événement pour
	//éviter une fuite mémoire.
	if (!eventMngr || sessionId <= 0)
	{
		delete ev;
		return 0;
	}

	//Résout le contexte localement (pas via GetEventContext, qui prendrait le
	//mutex déjà tenu)
	EventContexts::iterator it = eventContexts.find(eventContextId);
	if (it == eventContexts.end())
	{
		delete ev;
		return Error("event context [%d] not found\n", eventContextId);
	}

	//Remplit l'événement sous verrou puis le remet au manager : DeliverEvent ne
	//rappelle jamais la session, donc pas d'interblocage (C-3).
	ev->FillEvent(*(it->second));
	return eventMngr->DeliverEvent(sessionId, ev);
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

	//Extrait toutes les ressources sous verrou et les détruit HORS verrou : les
	//threads à joindre (minuteries, players, endpoints) peuvent être en train
	//d'attendre ce même mutex (onRecorderMaxDuration, onEndOfFile...).
	RecorderTimers	endedTimers;
	Recorders	endedRecorders;
	Endpoints	endedEndpoints;
	Players		endedPlayers;
	VideoTranscoders endedVideoTranscoders;
	AudioTranscoders endedAudioTranscoders;
	AudioMixers	endedAudioMixers;
	VideoMixers	endedVideoMixers;

	{
		std::lock_guard<std::mutex> lock(mutex);
		endedTimers.swap(recorderTimers);
		endedRecorders.swap(recorders);
		endedEndpoints.swap(endpoints);
		endedPlayers.swap(players);
		endedVideoTranscoders.swap(videoTranscoders);
		endedAudioTranscoders.swap(audioTranscoders);
		endedAudioMixers.swap(audioMixers);
		endedVideoMixers.swap(videoMixers);
		eventContexts.clear();
		playerEventCtx.clear();
		recorderEventCtx.clear();
		tokens.clear();
	}

	//Annule toutes les minuteries de recorder (annule + join des threads) AVANT de
	//libérer les recorders, pour qu'aucun thread de minuterie n'y accède encore.
	endedTimers.clear();

	//Delete all recorders
	endedRecorders.clear();

	//End all endpoints
	for (Endpoints::iterator it=endedEndpoints.begin(); it!=endedEndpoints.end(); ++it)
		//End it
		it->second->End();

	//Delete all players
	endedPlayers.clear();

	//End all video transcoders (le shared_ptr détruit ensuite)
	for (VideoTranscoders::iterator it=endedVideoTranscoders.begin(); it!=endedVideoTranscoders.end(); ++it)
		//End it
		it->second->End();
	endedVideoTranscoders.clear();

	//End all audio transcoders (le shared_ptr détruit ensuite)
	for (AudioTranscoders::iterator it=endedAudioTranscoders.begin(); it!=endedAudioTranscoders.end(); ++it)
		//End it
		it->second->End();
	endedAudioTranscoders.clear();

	//End all audio mixers (le shared_ptr détruit ensuite)
	for (AudioMixers::iterator it=endedAudioMixers.begin(); it!=endedAudioMixers.end(); ++it)
		//End it
		it->second->End();
	endedAudioMixers.clear();

	//End all video mixers (le shared_ptr détruit ensuite)
	for (VideoMixers::iterator it=endedVideoMixers.begin(); it!=endedVideoMixers.end(); ++it)
		//End it
		it->second->End();
	endedVideoMixers.clear();

	//Delete all endpoints
	endedEndpoints.clear();

	Log("<End media session\n");

	return 1;
}


int MediaSession::PlayerCreate(std::wstring tag)
{
	//Create player
	std::shared_ptr<Player> player = std::make_shared<Player>(tag);

	std::lock_guard<std::mutex> lock(mutex);

	//Create ID
	int playerId = maxPlayersId++;
	//Set event listener
	player->SetListener(this,(void*)(intptr_t)playerId);
	//Append the player
	players[playerId] = player;

	int eventContextId = maxEventContextId++;
	eventContexts[eventContextId] = std::make_shared<JSR309EventContext>( playerId, MediaFrame::Video, MediaFrame::VIDEO_MAIN);
	player->SetEventContextId(MediaFrame::Video,eventContextId);
	//Mémorise le contexte pour publier les événements de cycle de vie du player
	playerEventCtx[playerId] = eventContextId;

	//Return it
	return playerId;

}

int MediaSession::PlayerOpen(int playerId,const char* filename)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second.get();

        return player->Open(filename);
}

int MediaSession::PlayerPlay(int playerId)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second.get();

        //Choisit, parmi les codecs du fichier, une alternative acceptée par les
        //endpoints déjà attachés (le chemin Player→endpoint est en passthrough).
        //Sans effet si rien n'est attaché : la sélection par défaut d'Open reste.
        player->NegotiateCodecs();

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
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second.get();

        return player->Seek(time);
}

int MediaSession::PlayerStop(int playerId)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second.get();

        return player->Stop();
}

int MediaSession::PlayerClose(int playerId)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Players::iterator it = players.find(playerId);

        //If not found
        if (it==players.end())
                //Exit
                return Error("Player not found\n");
        //Get it
        Player* player = it->second.get();

        return player->Close();
}

int MediaSession::PlayerDelete(int playerId)
{
        //Détruit hors verrou : la destruction du player joint son thread de
        //lecture, qui peut être bloqué sur le mutex dans onEndOfFile.
        std::shared_ptr<Player> player;

        {
                std::lock_guard<std::mutex> lock(mutex);

                //Get Player
                Players::iterator it = players.find(playerId);

                //If not found
                if (it==players.end())
                        //Exit
                        return Error("Player not found\n");
                //Get it
                player = std::move(it->second);

                //Remove from list
                players.erase(it);

                //Libère le contexte d'événement associé
                EventCtxMap::iterator ctx = playerEventCtx.find(playerId);
                if (ctx != playerEventCtx.end())
                {
                        eventContexts.erase(ctx->second);
                        playerEventCtx.erase(ctx);
                }
        }

        return 1;
}

int MediaSession::RecorderCreate(std::wstring tag)
{
	//Create recorder
	std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>(tag);

	std::lock_guard<std::mutex> lock(mutex);

	//Create ID
	int recorderId = maxRecordersId++;
	//Append the recorder
	recorders[recorderId] = recorder;

	//Contexte d'événement du recorder, symétrique à celui des players
	int eventContextId = maxEventContextId++;
	eventContexts[eventContextId] = std::make_shared<JSR309EventContext>( recorderId, MediaFrame::Video, MediaFrame::VIDEO_MAIN);
	recorderEventCtx[recorderId] = eventContextId;

	//Return it
	return recorderId;
}

int MediaSession::RecorderRecord(int recorderId,const char* filename,DWORD maxDuration,bool waitVideo,bool echoVideo)
{
	//L'éventuelle minuterie précédente est détruite HORS verrou (join du thread)
	std::unique_ptr<RecorderTimer> oldTimer;
	int res = 0;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get recorder
		Recorders::iterator it = recorders.find(recorderId);
		//If not found
		if (it==recorders.end())
			//Exit
			return Error("Recorder not found\n");
		//Get it
		Recorder* recorder = it->second.get();
		//waitVideo doit être posé avant Create (qui instancie le mp4writer)
		recorder->SetWaitVideo(waitVideo);
		//Écho vidéo vers l'appelant pendant l'enregistrement
		recorder->SetEchoVideo(echoVideo);
		//create recording
		if (!recorder->Create(filename))
			//Error
			return Error("-Could not create file");
		//Start recording
		res = recorder->Record();

		//Sur succès : publie RecorderStartedEvent et arme la minuterie de durée max
		if (res)
		{
			EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
			if (ctx != recorderEventCtx.end())
				PostEvent(ctx->second, new RecorderStartedEvent(recorder->GetTag()));

			//Durée max demandée : (ré)arme la minuterie d'arrêt automatique
			if (maxDuration > 0)
			{
				//Détache une éventuelle minuterie précédente
				RecorderTimers::iterator t = recorderTimers.find(recorderId);
				if (t != recorderTimers.end())
				{
					oldTimer = std::move(t->second);
					recorderTimers.erase(t);
				}
				//Arme la nouvelle minuterie (weak_ptr : la session peut disparaître avant l'échéance)
				std::unique_ptr<RecorderTimer> timer(new RecorderTimer(weak_from_this(),recorderId,maxDuration));
				timer->Start();
				recorderTimers[recorderId] = std::move(timer);
			}
		}
	}

	//oldTimer détruite ici, hors verrou
	return res;
}

int MediaSession::RecorderStop(int recorderId)
{
	//La minuterie est détruite HORS verrou : son thread peut être bloqué sur ce
	//même mutex dans onRecorderMaxDuration, et le destructeur le joint.
	std::unique_ptr<RecorderTimer> timer;
	int res = 1;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get recorder
		Recorders::iterator it = recorders.find(recorderId);
		//If not found
		if (it==recorders.end())
			//Exit
			return Error("Recorder not found\n");
		//Get it
		Recorder* recorder = it->second.get();

		//Récupère et détache l'éventuelle minuterie de durée max
		RecorderTimers::iterator t = recorderTimers.find(recorderId);
		if (t != recorderTimers.end())
		{
			timer = std::move(t->second);
			recorderTimers.erase(t);
		}

		//Détermine qui pilote l'arrêt : si la minuterie a déjà déclenché (reason=1),
		//elle a déjà fermé le fichier et publié l'événement.
		bool claimed = timer ? timer->ClaimStop() : true;

		if (claimed)
		{
			//Fin de l'écho avec l'enregistrement
			recorder->SetEchoVideo(false);
			//Arrêt explicite
			res = recorder->Close();

			//Publie RecorderStoppedEvent (reason=0, explicite)
			EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
			if (ctx != recorderEventCtx.end())
				PostEvent(ctx->second, new RecorderStoppedEvent(recorder->GetTag(), RecorderStoppedEvent::Explicit));
		}
		//sinon : la durée max a déjà arrêté l'enregistrement, plus rien à faire
	}

	//timer détruite ici, hors verrou (join)
	return res;
}

int MediaSession::RecorderDelete(int recorderId)
{
	//Minuterie et recorder détruits hors verrou (join des threads)
	std::unique_ptr<RecorderTimer> timer;
	std::shared_ptr<Recorder> recorder;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get recorder
		Recorders::iterator it = recorders.find(recorderId);

		//If not found
		if (it==recorders.end())
			//Exit
			return Error("Recorder not found\n");

		//Détache l'éventuelle minuterie AVANT de libérer le recorder
		//(le join garantit que le thread de minuterie n'utilise plus le recorder).
		RecorderTimers::iterator t = recorderTimers.find(recorderId);
		if (t != recorderTimers.end())
		{
			timer = std::move(t->second);
			recorderTimers.erase(t);
		}

		//Get it
		recorder = std::move(it->second);

		//Remove from list
		recorders.erase(it);

		//Libère le contexte d'événement associé
		EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
		if (ctx != recorderEventCtx.end())
		{
			eventContexts.erase(ctx->second);
			recorderEventCtx.erase(ctx);
		}
	}

	//timer (join) puis recorder détruits ici, hors verrou
	timer.reset();

	return 1;
}

void MediaSession::onRecorderMaxDuration(int recorderId)
{
	//Appelé depuis le thread RecorderTimer à l'expiration de la durée max.
	//NB : ne touche PAS la map recorderTimers (nettoyée par RecorderStop/Delete).
	std::lock_guard<std::mutex> lock(mutex);

	Recorders::iterator it = recorders.find(recorderId);
	if (it == recorders.end())
		return;
	Recorder* recorder = it->second.get();

	//Arrêt automatique de l'enregistrement
	recorder->Close();

	//Publie RecorderStoppedEvent (reason=1, durée max atteinte)
	EventCtxMap::iterator ctx = recorderEventCtx.find(recorderId);
	if (ctx != recorderEventCtx.end())
		PostEvent(ctx->second, new RecorderStoppedEvent(recorder->GetTag(), RecorderStoppedEvent::MaxDuration));
}

int MediaSession::RecorderAttachToAudioMixerPort(int recorderId,int mixerId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second.get();

	 //Get Player
        AudioMixers::iterator itMixer = audioMixers.find(mixerId);

        //If not found
        if (itMixer==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");

	 //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = itMixer->second;

	//Attach
	return recorder->Attach(MediaFrame::Audio,audioMixer->GetJoinable(portId));
}

int MediaSession::RecorderAttachToVideoMixerPort(int recorderId,int mixerId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second.get();

	 //Get Player
        VideoMixers::iterator itMixer = videoMixers.find(mixerId);

        //If not found
        if (itMixer==videoMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");

	 //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = itMixer->second;

	//And attach
	return recorder->Attach(MediaFrame::Video,videoMixer->GetJoinable(portId));
}

int MediaSession::RecorderAttachToEndpoint(int recorderId,int endpointId,MediaFrame::Type media)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second.get();

	//Get source endpoint
        Endpoints::iterator itEndpoints = endpoints.find(endpointId);

        //If not found
        if (itEndpoints==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* source = itEndpoints->second.get();

	//Attach
	return recorder->Attach(media,source->GetJoinable(media));
}

int MediaSession::RecorderDettach(int recorderId,MediaFrame::Type media)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        Recorders::iterator it = recorders.find(recorderId);

        //If not found
        if (it==recorders.end())
                //Exit
                return Error("Recorder not found\n");
        //Get it
        Recorder* recorder = it->second.get();

	//Attach
	return recorder->Dettach(media);
}


std::shared_ptr<Endpoint> MediaSession::GetEndpoint(int endpointId)
{
	std::lock_guard<std::mutex> lock(mutex);

	Endpoints::iterator it = endpoints.find(endpointId);

	//If not found
	if (it == endpoints.end())
	{
		//Exit
		Error("Endpoint [%d] not found\n", endpointId);
		return NULL;
	}

	//Get it
	return it->second;

}

std::shared_ptr<Player> MediaSession::GetPlayer(int playerId)
{
	std::lock_guard<std::mutex> lock(mutex);

	Players::iterator it = players.find(playerId);

	//If not found
	if (it == players.end())
	{
		//Exit
		Error("player [%d] not found\n", playerId);
		return NULL;
	}

	//Get it
	return it->second;

}

std::shared_ptr<JSR309EventContext> MediaSession::GetEventContext(int EventContextId)
{
	std::lock_guard<std::mutex> lock(mutex);

	EventContexts::iterator it = eventContexts.find(EventContextId);

	//If not found
	if (it == eventContexts.end())
	{
		//Exit
		Error("event context [%d] not found\n", EventContextId);
		return NULL;
	}

	//Get it
	return it->second;

}

int MediaSession::EndpointCreate(std::wstring name,bool audioSupported,bool videoSupported,bool textSupport)
{
	//Create endpoint
	std::shared_ptr<Endpoint> endpoint = std::make_shared<Endpoint>(name,audioSupported,videoSupported,textSupport);

	//Init it
	endpoint->Init();

	std::lock_guard<std::mutex> lock(mutex);

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
		eventContexts[eventContextId] = std::make_shared<JSR309EventContext>( endpointId,MediaFrame::Audio, MediaFrame::VIDEO_MAIN);
		endpoint->SetEventContextId(MediaFrame::Audio, MediaFrame::VIDEO_MAIN, eventContextId);
	}
	if (videoSupported)
	{
		eventContextId = maxEventContextId++;
		eventContexts[eventContextId] = std::make_shared<JSR309EventContext>( endpointId,MediaFrame::Video, MediaFrame::VIDEO_MAIN);
		endpoint->SetEventContextId(MediaFrame::Video, MediaFrame::VIDEO_MAIN, eventContextId);
	}
	if(textSupport)
	{
		eventContextId = maxEventContextId++;
		eventContexts[eventContextId] = std::make_shared<JSR309EventContext>( endpointId,MediaFrame::Text, MediaFrame::VIDEO_MAIN);
		endpoint->SetEventContextId(MediaFrame::Text, MediaFrame::VIDEO_MAIN, eventContextId);
	}

	//Return it
	return endpointId;

}
int MediaSession::EndpointDelete(int endpointId)
{
	//End + destruction hors verrou : End() joint les threads RTP, qui peuvent
	//être en train de publier un événement (Joinable::PostEvent → GetEventContext).
	std::shared_ptr<Endpoint> endpoint;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get Player
		Endpoints::iterator it = endpoints.find(endpointId);

		//If not found
		if (it==endpoints.end())
				//Exit
				return Error("Endpoint not found\n");
		//Get it
		endpoint = std::move(it->second);

		//Log endpoint tag name
		Log("-EndpointDelete [%ls]\n",endpoint->GetName().c_str());

		//Remove from list
		endpoints.erase(it);
	}

	//End it
	endpoint->End();

	return 1;
}

int MediaSession::EndpointSetLocalCryptoSDES(int endpointId,MediaFrame::Type media,const char *suite,const char* key)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Call it
	return endpoint->SetLocalCryptoSDES(media,suite,key);
}

int MediaSession::EndpointSetRemoteCryptoSDES(int endpointId,MediaFrame::Type media,const char *suite,const char* key)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Call it
	return endpoint->SetRemoteCryptoSDES(media,suite,key);
}

int MediaSession::EndpointSetRemoteCryptoDTLS(int endpointId,MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Call it
	Log("-EndpointSetRemoteCryptoDTLS: endpoint %ls and media %s.\n", endpoint->GetName().c_str(), MediaFrame::TypeToString(media));
	return endpoint->SetRemoteCryptoDTLS(media,setup,hash,fingerprint);
}


int MediaSession::EndpointSetLocalSTUNCredentials(int endpointId,MediaFrame::Type media,const char *username,const char* pwd)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	Log("-EndpointSetLocalSTUNCredentials: endpoint %ls and media %s.\n", endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Call it
	return endpoint->SetLocalSTUNCredentials(media,username,pwd);
}

int MediaSession::EndpointSetRemoteSTUNCredentials(int endpointId,MediaFrame::Type media,const char *username,const char* pwd)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();
	Log("-EndpointSetRemoteSTUNCredentials: endpoint %ls and media %s.\n", endpoint->GetName().c_str(), MediaFrame::TypeToString(media));
	//Call it
	return endpoint->SetRemoteSTUNCredentials(media,username,pwd);
}

int MediaSession::EndpointSetRTPProperties(int endpointId,MediaFrame::Type media,const Properties& properties)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();
	//Call it
	return endpoint->SetRTPProperties(media,properties);
}

//Endpoint Video functionality
int MediaSession::EndpointStartSending(int endpointId,MediaFrame::Type media,char *sendVideoIp,int sendVideoPort,RTPMap& rtpMap)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointStartSending [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	return endpoint->StartSending(media,sendVideoIp, sendVideoPort, rtpMap);
}

int MediaSession::EndpointAddICECandidate(int endpointId,MediaFrame::Type media,const char* candidate)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Délègue au endpoint (trickle ICE Niveau 1)
	return endpoint->AddICECandidate(media,candidate);
}

int MediaSession::EndpointStartRTPTimeout(int endpointId,MediaFrame::Type media,DWORD timeoutMs)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Délègue au endpoint (watchdog d'inactivité RTP - gap 5)
	return endpoint->ArmRTPTimeout(media,timeoutMs);
}

int MediaSession::EndpointStopSending(int endpointId,MediaFrame::Type media)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointStopSending [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	return endpoint->StopSending(media);
}

int MediaSession::EndpointStartReceiving(int endpointId,MediaFrame::Type media,RTPMap& rtpMap,std::map<int,std::string>& fmtpOut)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointStartReceiving [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	int port = endpoint->StartReceiving(media,rtpMap);

	//Récupère le fmtp négocié (phase 4) pour le retour enrichi XML-RPC (§5.2).
	//Sous le même verrou : négociation et lecture du résultat sont atomiques.
	if (port > 0)
		endpoint->GetNegotiatedFmtp(media, MediaFrame::VIDEO_MAIN, fmtpOut);

	return port;
}
int MediaSession::EndpointStopReceiving(int endpointId,MediaFrame::Type media)
{
        std::lock_guard<std::mutex> lock(mutex);

        //Get Player
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointStopReceiving [%ls,media:%s]\n",endpoint->GetName().c_str(), MediaFrame::TypeToString(media));

	//Execute
	return endpoint->StopReceiving(media);
}

int MediaSession::EndpointRequestUpdate(int endpointId,MediaFrame::Type media)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointRequestUpdate [%ls]\n",endpoint->GetName().c_str());

	//Execute
	return endpoint->RequestUpdate(media);
}
int MediaSession::EndpointAttachToPlayer(int endpointId,int playerId,MediaFrame::Type media)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointAttachToPlayer [%ls]\n",endpoint->GetName().c_str());

	 //Get Player
        Players::iterator itPlayer = players.find(playerId);

        //If not found
        if (itPlayer==players.end())
                //Exit
                return Error("Player not found\n");

	 //Get it
        Player* player = itPlayer->second.get();

	//Attach
	return endpoint->Attach(media,MediaFrame::VIDEO_MAIN,player->GetJoinable(media));
}

int MediaSession::EndpointAttachToAudioMixerPort(int endpointId,int mixerId,int portId)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointAttachToAudioMixerPort [%ls]\n",endpoint->GetName().c_str());

	 //Get Player
        AudioMixers::iterator itMixer = audioMixers.find(mixerId);

        //If not found
        if (itMixer==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");

	 //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = itMixer->second;

	//Attach
	return endpoint->Attach(MediaFrame::Audio,MediaFrame::VIDEO_MAIN,audioMixer->GetJoinable(portId));
}

int MediaSession::EndpointAttachToVideoMixerPort(int endpointId,int mixerId,int portId)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointAttachToVideoMixerPort [%ls]\n",endpoint->GetName().c_str());

	 //Get Player
        VideoMixers::iterator itMixer = videoMixers.find(mixerId);

        //If not found
        if (itMixer==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found\n");

	 //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = itMixer->second;

	//And attach
	return endpoint->Attach(MediaFrame::Video,MediaFrame::VIDEO_MAIN,videoMixer->GetJoinable(portId));
}

int MediaSession::EndpointAttachToVideoTranscoder(int endpointId,int videoTranscoderId)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint (recherche directe : GetEndpoint prendrait le mutex déjà tenu)
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	 //Get Video transcoder
        VideoTranscoders::iterator itTranscoder = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (itTranscoder==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found[%d]\n",videoTranscoderId);

	 //Get it
        std::shared_ptr<VideoTranscoder> videoTranscoder = itTranscoder->second;

	//Log endpoint tag name
	Log("-EndpointAttachToVideoTranscoder [endpoint:%ls,transcoder:%ls]\n",endpoint->GetName().c_str(),videoTranscoder->GetName().c_str());

	//And attach
	return endpoint->Attach(MediaFrame::Video,MediaFrame::VIDEO_MAIN,videoTranscoder);
}

int MediaSession::EndpointAttachToEndpoint(int endpointId,int sourceId,MediaFrame::Type media)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointAttachToEndpoint [%ls] for media\n",endpoint->GetName().c_str());

	//Get source endpoint
        it = endpoints.find(sourceId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");
        //Get it
        Endpoint* source = it->second.get();

	Log("-EndpointAttachToEndpoint: activating TS transparency.\n");
	endpoint->SetRTPTsTransparency(media, true, MediaFrame::VIDEO_MAIN);
	source->SetRTPTsTransparency(media, true, MediaFrame::VIDEO_MAIN);
	//Attach
	return endpoint->Attach(media,MediaFrame::VIDEO_MAIN,source->GetJoinable(media));
}

int MediaSession::EndpointDettach(int endpointId,MediaFrame::Type media)
{
        std::lock_guard<std::mutex> lock(mutex);

	//Get endpoint
        Endpoints::iterator it = endpoints.find(endpointId);

        //If not found
        if (it==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");

	//Get it
        Endpoint* endpoint = it->second.get();

	//Log endpoint tag name
	Log("-EndpointDettach [%ls]\n",endpoint->GetName().c_str());

	//Attach
	return endpoint->Detach(media);
}

void MediaSession::onEndOfFile(Player *player,void* playerId)
{
	//Récupère l'id du player transmis à SetListener
	int id = (int)(intptr_t)playerId;

	//Appelé depuis le thread de lecture du player : le mutex protège les maps ;
	//le player reste vivant car sa destruction (PlayerDelete/End) joint ce thread
	//hors verrou.
	std::lock_guard<std::mutex> lock(mutex);

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
	//Create player
	std::shared_ptr<AudioMixerResource> audioMixer = std::make_shared<AudioMixerResource>(tag);
	//Init it
	audioMixer->Init();

	std::lock_guard<std::mutex> lock(mutex);

	//Create ID
	int audioMixerId = maxAudioMixerId++;
	//Append the player
	audioMixers[audioMixerId] = audioMixer;
	//Return it
	return audioMixerId;
}

int MediaSession::AudioMixerDelete(int mixerId)
{
	//End hors verrou (End joint les threads du mixer) ; le shared_ptr local
	//détruit le mixer en sortie de portée.
	std::shared_ptr<AudioMixerResource> audioMixer;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get Player
		AudioMixers::iterator it = audioMixers.find(mixerId);

		//If not found
		if (it==audioMixers.end())
			//Exit
			return Error("AudioMixerResource not found\n");
		//Get it
		audioMixer = it->second;

		//Remove from list
		audioMixers.erase(it);
	}

	//End it
	audioMixer->End();

	return 1;
}

int MediaSession::AudioMixerPortCreate(int mixerId,std::wstring tag)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = it->second;

	//Execute
	return audioMixer->CreatePort(tag);
}

int MediaSession::AudioMixerPortSetCodec(int mixerId,int portId,AudioCodec::Type codec)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = it->second;

	//Execute
	return audioMixer->SetPortCodec(portId,codec);
}

int MediaSession::AudioMixerPortDelete(int mixerId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = it->second;

	//Execute
	return audioMixer->DeletePort(portId);
}


int MediaSession::AudioMixerPortAttachToEndpoint(int mixerId,int portId,int endpointId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = it->second;

	//Get endpoint
        Endpoints::iterator itEnd = endpoints.find(endpointId);

        //If not found
        if (itEnd==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");

        //Get it
        Endpoint* endpoint = itEnd->second.get();

	//Log endpoint tag name
	Log("-AudioMixerPortAttachToEndpoint [%ls]\n",endpoint->GetName().c_str());

	//Attach
	return audioMixer->Attach(portId,endpoint->GetJoinable(MediaFrame::Audio));
}

int MediaSession::AudioMixerPortAttachToPlayer(int mixerId,int portId,int playerId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = it->second;

	 //Get Player
        Players::iterator itPlayer = players.find(playerId);

        //If not found
        if (itPlayer==players.end())
                //Exit
                return Error("Player not found\n");

	 //Get it
        Player* player = itPlayer->second.get();

	//Attach
	return audioMixer->Attach(portId,player->GetJoinable(MediaFrame::Audio));
}

int MediaSession::AudioMixerPortDettach(int mixerId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        AudioMixers::iterator it = audioMixers.find(mixerId);

        //If not found
        if (it==audioMixers.end())
                //Exit
                return Error("AudioMixerResource not found\n");
        //Get it
        std::shared_ptr<AudioMixerResource> audioMixer = it->second;

       //Attach
	return audioMixer->Dettach(portId);
}

int MediaSession::VideoMixerCreate(std::wstring tag)
{
	//Create player
	std::shared_ptr<VideoMixerResource> videoMixer = std::make_shared<VideoMixerResource>(tag);
	//Init
	videoMixer->Init(Mosaic::mosaic2x2,PAL);

	std::lock_guard<std::mutex> lock(mutex);

	//Create ID
	int videoMixerId = maxVideoMixerId++;
	//Append the player
	videoMixers[videoMixerId] = videoMixer;
	//Return it
	return videoMixerId;
}

int MediaSession::VideoMixerDelete(int mixerId)
{
	//End hors verrou (End joint les threads du mixer) ; le shared_ptr local
	//détruit le mixer en sortie de portée.
	std::shared_ptr<VideoMixerResource> videoMixer;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get Player
		VideoMixers::iterator it = videoMixers.find(mixerId);

		//If not found
		if (it==videoMixers.end())
			//Exit
			return Error("VideoMixerResource not found [%d]\n",mixerId);
		//Get it
		videoMixer = it->second;

		//Remove from list
		videoMixers.erase(it);
	}

	//End it
	videoMixer->End();

	return 1;
}

int MediaSession::VideoMixerPortCreate(int mixerId,std::wstring tag, int mosaicId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

	//Execute
	return videoMixer->CreatePort(tag,mosaicId);
}

int MediaSession::VideoMixerPortSetCodec(int mixerId,int portId,VideoCodec::Type codec,int size,int fps,int bitrate,int intraPeriod)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

	//Execute
	return videoMixer->SetPortCodec(portId,codec,size,fps,bitrate,intraPeriod);
}

int MediaSession::VideoMixerPortDelete(int mixerId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

	//Execute
	return videoMixer->DeletePort(portId);
}


int MediaSession::VideoMixerPortAttachToEndpoint(int mixerId,int portId,int endpointId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

	//Get endpoint
        Endpoints::iterator itEnd = endpoints.find(endpointId);

        //If not found
        if (itEnd==endpoints.end())
                //Exit
                return Error("Endpoint not found\n");

        //Get it
        Endpoint* endpoint = itEnd->second.get();

	//Log endpoint tag name
	Log("-VideoMixerPortAttachToEndpoint [%ls]\n",endpoint->GetName().c_str());

	//Attach
	return videoMixer->Attach(portId,endpoint->GetJoinable(MediaFrame::Video));
}

int MediaSession::VideoMixerPortAttachToPlayer(int mixerId,int portId,int playerId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

	 //Get Player
        Players::iterator itPlayer = players.find(playerId);

        //If not found
        if (itPlayer==players.end())
                //Exit
                return Error("Player not found\n");

	 //Get it
        Player* player = itPlayer->second.get();

	//Attach
	return videoMixer->Attach(portId,player->GetJoinable(MediaFrame::Video));
}

int MediaSession::VideoMixerPortDettach(int mixerId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->Dettach(portId);
}

int MediaSession::VideoMixerMosaicCreate(int mixerId,Mosaic::Type comp,int size)
{
	std::lock_guard<std::mutex> lock(mutex);

		//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->CreateMosaic(comp,size);
}

int MediaSession::VideoMixerMosaicDelete(int mixerId,int mosaicId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->DeleteMosaic(mosaicId);
}

int MediaSession::VideoMixerMosaicSetSlot(int mixerId,int mosaicId,int num,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->SetSlot(mosaicId,num,portId);
}

int MediaSession::VideoMixerMosaicSetCompositionType(int mixerId,int mosaicId,Mosaic::Type comp,int size)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->SetCompositionType(mosaicId,comp,size);
}

int MediaSession::VideoMixerMosaicSetOverlayPNG(int mixerId,int mosaicId,const char* overlay)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->SetOverlayPNG(mosaicId,overlay);
}

int MediaSession::VideoMixerMosaicResetSetOverlay(int mixerId,int mosaicId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->ResetOverlay(mosaicId);
}

int MediaSession::VideoMixerMosaicAddPort(int mixerId,int mosaicId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->AddMosaicParticipant(mosaicId,portId);
}

int MediaSession::VideoMixerMosaicRemovePort(int mixerId,int mosaicId,int portId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoMixers::iterator it = videoMixers.find(mixerId);

        //If not found
        if (it==videoMixers.end())
                //Exit
                return Error("VideoMixerResource not found [%d]\n",mixerId);
        //Get it
        std::shared_ptr<VideoMixerResource> videoMixer = it->second;

       //Attach
	return videoMixer->RemoveMosaicParticipant(mosaicId,portId);
}

int MediaSession::AudioTranscoderCreate(std::wstring tag)
{
	//Create trascoder
	std::shared_ptr<AudioTranscoder> transcoder = std::make_shared<AudioTranscoder>(tag);
	//Init
	transcoder->Init(true);

	std::lock_guard<std::mutex> lock(mutex);

	//Create ID
	int transcoderId = maxVideoTranscoderId++;
	//Append the player
	audioTranscoders[transcoderId] = transcoder;
	Log("-Created audio transcoder ID %d.\n", transcoderId);
	//Return it
	return transcoderId;
}

int MediaSession::AudioTranscoderDelete(int transcoderId)
{
    //End + destruction hors verrou (End arrête les workers du transcodeur)
    std::shared_ptr<AudioTranscoder> transcoder;

    {
        std::lock_guard<std::mutex> lock(mutex);

        //Get transcoders
        AudioTranscoders::iterator it = audioTranscoders.find(transcoderId);

        //If not found
        if (it==audioTranscoders.end())
                //Exit
                return Error("AudioTranscoder not found [%d]\n",transcoderId);
        //Get it
        transcoder = std::move(it->second);

        //Remove from list
        audioTranscoders.erase(it);
    }

    //End it (le dernier shared_ptr détruit l'objet)
    transcoder->End();

    return 1;
}

std::shared_ptr<AudioTranscoder> MediaSession::GetAudioTranscoder(int transcoderId)
{
    std::lock_guard<std::mutex> lock(mutex);

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
	//Create trascoder
	std::shared_ptr<VideoTranscoder> videoTranscoder = std::make_shared<VideoTranscoder>(tag);
	//Init
	videoTranscoder->Init(false);

	std::lock_guard<std::mutex> lock(mutex);

	//Create ID
	int videoTranscoderId = maxVideoTranscoderId++;
	//Append the player
	videoTranscoders[videoTranscoderId] = videoTranscoder;
	//Return it
	return videoTranscoderId;
}

int MediaSession::VideoTranscoderFPU(int videoTranscoderId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        std::shared_ptr<VideoTranscoder> videoTranscoder = it->second;

	//Execute
	videoTranscoder->Update();

	//OK
	return 1;
}

int MediaSession::VideoTranscoderSetCodec(int videoTranscoderId,VideoCodec::Type codec,int size,int fps,int bitrate,int intraPeriod,
					  Properties & props)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get Player
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        std::shared_ptr<VideoTranscoder> videoTranscoder = it->second;

	//Execute
	return videoTranscoder->SetCodec(codec,size,fps,bitrate,intraPeriod, props);
}

int MediaSession::VideoTranscoderDelete(int videoTranscoderId)
{
	//End hors verrou (End arrête les workers du transcodeur) ; le shared_ptr
	//local détruit le transcodeur en sortie de portée.
	std::shared_ptr<VideoTranscoder> videoTranscoder;

	{
		std::lock_guard<std::mutex> lock(mutex);

		//Get Player
		VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

		//If not found
		if (it==videoTranscoders.end())
			//Exit
			return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
		//Get it
		videoTranscoder = it->second;

		//Remove from list
		videoTranscoders.erase(it);
	}

	//End it
	videoTranscoder->End();

	return 1;
}

int MediaSession::VideoTranscoderAttachToEndpoint(int videoTranscoderId,int endpointId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        std::shared_ptr<VideoTranscoder> videoTranscoder = it->second;

	//Get endpoint
        Endpoints::iterator itEnd = endpoints.find(endpointId);

        //If not found
        if (itEnd==endpoints.end())
                //Exit
                return Error("Endpoint not found [%d]\n",endpointId);

        //Get it
        Endpoint* endpoint = itEnd->second.get();

	//Log endpoint tag name
	Log("-VideoTranscoderAttachToEndpoint [transcoder:%ls,endpoint:%ls]\n",videoTranscoder->GetName().c_str(),endpoint->GetName().c_str());

	//Attach
	return videoTranscoder->Attach(endpoint->GetJoinable(MediaFrame::Video));
}

int MediaSession::VideoTranscoderDettach(int videoTranscoderId)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Get mixer
        VideoTranscoders::iterator it = videoTranscoders.find(videoTranscoderId);

        //If not found
        if (it==videoTranscoders.end())
                //Exit
                return Error("VideoTranscoder not found [%d]\n",videoTranscoderId);
        //Get it
        std::shared_ptr<VideoTranscoder> videoTranscoder = it->second;

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

	std::lock_guard<std::mutex> lock(mutex);

	Endpoints::iterator itEndpoints = endpoints.find(endpointId);

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
	std::lock_guard<std::mutex> lock(mutex);

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

