/* 
 * File:   JSR309Manager.h
 * Author: Sergio
 *
 * Created on 8 de septiembre de 2011, 13:06
 */

#ifndef JSR309MANAGER_H
#define	JSR309MANAGER_H

#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "config.h"
#include "MediaSession.h"
#include "eventqueuesweeper.h"
#include "xmlstreaminghandler.h"
#include "websocketserver.h"

//Préfixe HTTP (long-poll/SSE) de la file d'événements JSR309. Source unique
//partagée entre main.cpp (enregistrement du handler) et EventQueueCreate (qui
//renvoie le chemin complet "/events/jsr309/<queueId>" au client — gap 6).
#define JSR309_EVENTS_PREFIX "/events/jsr309"

class JSR309Manager :
	public MediaSession::Listener,
	public WebSocketServer::Handler,
	//Expiration des sessions dont la file d'événements n'est plus lue
	private EventQueueSweeper
{
public:
	// NB : l'énumération des types d'événements est définie de façon unique dans
	// JSR309Event::Events (JSR309Event.h). Ne pas la dupliquer ici (contrat de fil
	// partagé avec elixip).

public:
	JSR309Manager();
	virtual ~JSR309Manager();

	//queueExpiresSecs : délai de grâce sans poller (0 = expiration désactivée,
	//comportement historique)
	int Init(XmlStreamingHandler *eventMngr,int queueExpiresSecs = XmlEventQueue::DefaultExpiresSecs);
	int End();

	int CreateEventQueue();
	int DeleteEventQueue(int id);
	int CreateMediaSession(std::wstring tag,int queueId);
	//Rend une copie de shared_ptr : la session survit tant que l'appelant la tient,
	//même si DeleteMediaSession passe entre-temps (remplace le couple
	//GetMediaSessionRef/ReleaseMediaSessionRef à refcount manuel).
	int GetMediaSessionRef(int id,std::shared_ptr<MediaSession> &sess);
	int DeleteMediaSession(int id);

	//Publication d'un événement à remplir : résout le contexte via la session (prend
	//son mutex) — réservé aux appelants qui NE tiennent PAS ce mutex (Joinable,
	//threads RTP). Prend possession de l'événement.
	int PostEvent(int sessionId,int eventContextId , JSR309Event *event);
	//Remise d'un événement déjà rempli : ne touche que l'état du manager, donc
	//appelable sous le mutex d'une session (C-3). Prend possession de l'événement.
	int DeliverEvent(int sessionId, JSR309Event *event);
	//Events
	//virtual void onPlayerEndOfFile(MediaSession *sess,Player *player,int playerId,void *param);

	// Websocket server
	virtual void onWebSocketConnection(const HTTPRequest &request, WebSocket *ws);

private:
	struct MediaSessionEntry
	{
		int id;
		int queueId;
		std::wstring tag;
		std::shared_ptr<MediaSession> sess;
	};

	typedef std::map<int,MediaSessionEntry> MediaSessions;

private:
	//EventQueueSweeper : files référencées par les sessions...
	virtual void CollectQueueIds(std::set<int>& ids);
	//... et destruction de toutes les sessions dont l'entrée pointe vers cette
	//file (extraction sous verrou, End() hors verrou comme DeleteMediaSession).
	//Rend le nombre de sessions détruites.
	virtual int DeleteQueueOwners(int queueId,const char *reason);

private:
	XmlStreamingHandler *eventMngr;
	MediaSessions	sessions;
	std::mutex	mutex;
	int maxId;
	bool inited;
};




#endif	/* JSR309MANAGER_H */

