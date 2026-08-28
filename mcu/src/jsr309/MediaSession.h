/* 
 * File:   MediaSession.h
 * Author: Sergio
 *
 * Created on 6 de septiembre de 2011, 15:55
 */

#ifndef MEDIASESSION_H
#define	MEDIASESSION_H

#include "config.h"
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include "media.h"
#include "medkit/codecs.h"
#include "video.h"
#include "Player.h"
#include "Recorder.h"
#include "Endpoint.h"
#include "AudioMixerResource.h"
#include "VideoMixerResource.h"
#include "VideoTranscoder.h"
#include "AudioTranscoder.h"
#include <string.h>
#include "JSR309Event.h"

class JSR309Manager;
class MediaSession;

/**
 * Minuterie de durée maximale d'un enregistrement.
 * À l'expiration du délai (maxDurationMs), déclenche l'arrêt automatique du
 * recorder et la publication de RecorderStoppedEvent(reason=1) via la session.
 * L'attente est annulable (pthread_cond_timedwait), et l'arrêt est protégé par
 * un « verrou d'arrêt » (ClaimStop) pour éviter le double arrêt / double
 * événement en cas de course avec un RecorderStop explicite.
 * La session est référencée par weak_ptr (H-2) : si elle est détruite avant
 * l'échéance, le lock() échoue et la minuterie ne fait rien.
 */
class RecorderTimer
{
public:
	RecorderTimer(std::weak_ptr<MediaSession> session,int recorderId,DWORD maxDurationMs);
	~RecorderTimer();

	void Start();

	//Signal that the recording has been stopped.
	bool ClaimStop();

private:
	void Run();

	std::weak_ptr<MediaSession> session;
	int             recorderId;
	DWORD           maxDurationMs;
	std::thread     thread;
	std::mutex      mutex;
	std::condition_variable cond;
	bool            stopClaimed;  //l'arrêt a déjà été pris
	bool            cancelled;    //annulation (destruction) demandée
};

class MediaSession :
	public Player::Listener,
	public std::enable_shared_from_this<MediaSession>
{
public:
	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		//virtual void onPlayerEndOfFile(MediaSession *sess, Player* player,int playerId, void *param) = 0;
		//virtual void onSipInfoFIRRequested(int EndpointId,void *param) = 0;
	};
public:
	MediaSession(std::wstring tag);
	~MediaSession();

	void SetListener(MediaSession::Listener *listener,void* param);

	int Init();
	int End();

	//Player management
	int PlayerCreate(std::wstring tag);
	int PlayerDelete(int playerId);
	//Player functionality
	int PlayerOpen(int playerId,const char* filename);
	int PlayerPlay(int playerId);
	int PlayerSeek(int playerId,QWORD time);
	int PlayerStop(int playerId);
	int PlayerClose(int playerId);

	//Recorder management
	int RecorderCreate(std::wstring tag);
	int RecorderDelete(int recorderId);
	//Recorder functionality
	//maxDuration : durée max d'enregistrement en ms (0 = illimité). À expiration,
	//arrêt auto + RecorderStoppedEvent(reason=1).
	int RecorderRecord(int recorderId,const char* filename,DWORD maxDuration=0,bool waitVideo=true,bool echoVideo=false);
	int RecorderStop(int recorderId);

	//Callback interne appelé par RecorderTimer à l'expiration de la durée max.
	void onRecorderMaxDuration(int recorderId);

	//Join other objects
	int RecorderAttachToEndpoint(int recorderId,int endpointId,MediaFrame::Type media);
	int RecorderAttachToAudioMixerPort(int recorderId,int mixerId,int portId);
	int RecorderAttachToVideoMixerPort(int recorderId,int mixerId,int portId);
	int RecorderDettach(int recorderId,MediaFrame::Type media);
	

	//Endpoint management
	int EndpointCreate(std::wstring name,bool audioSupported,bool videoSupported,bool textSupported);
	int EndpointDelete(int endpointId);
	int EndpointSetLocalCryptoSDES(int id,MediaFrame::Type media,const char *suite,const char* key);
	int EndpointSetRemoteCryptoSDES(int id,MediaFrame::Type media,const char *suite,const char* key);
	int EndpointSetRemoteCryptoDTLS(int id,MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint);
	int EndpointSetLocalSTUNCredentials(int id,MediaFrame::Type media,const char *username,const char* pwd);
	int EndpointSetRemoteSTUNCredentials(int id,MediaFrame::Type media,const char *username,const char* pwd);
	int EndpointSetRTPProperties(int id,MediaFrame::Type media,const Properties& properties);
	//Endpoint Video functionality
	int EndpointStartSending(int endpointId,MediaFrame::Type media,char *sendVideoIp,int sendVideoPort,RTPMap& rtpMap);
	//Trickle ICE Niveau 1 (gap 1) : ajoute un candidat distant à un flux d'endpoint.
	int EndpointAddICECandidate(int endpointId,MediaFrame::Type media,const char* candidate);
	//Watchdog d'inactivité RTP (gap 5) : arme/désarme le flux d'un endpoint.
	int EndpointStartRTPTimeout(int endpointId,MediaFrame::Type media,DWORD timeoutMs);
	int EndpointStopSending(int endpointId,MediaFrame::Type media);
	//P8a : `offerFmtp` (optionnel) = le fmtp de l'offre par payload type, relayé au
	//négociateur ; NULL = pas d'entrée distante (contrôleur pré-P8a).
	//Profil d'adressage demandé par le contrôleur pour une jambe (voir
	//NETWORK-CONFIGURATION.md). À poser AVANT EndpointStartReceiving/StartSending.
	bool EndpointSetAddressProfile(int endpointId,MediaFrame::Type media,const char* profile,std::string& error);
	int EndpointStartReceiving(int endpointId,MediaFrame::Type media,RTPMap& rtpMap,std::map<int,std::string>& fmtpOut,
	                           const std::map<int,std::string>* offerFmtp = NULL);
	int EndpointStopReceiving(int endpointId,MediaFrame::Type media);

	int EndpointRequestUpdate(int endpointId,MediaFrame::Type media);
	//Attach intput to
	int EndpointAttachToPlayer(int endpointId,int playerId,MediaFrame::Type media);
	int EndpointAttachToEndpoint(int endpointId,int sourceId,MediaFrame::Type media);
	int EndpointAttachToAudioMixerPort(int endpointId,int mixerId,int portId);
	int EndpointDettach(int endpointId,MediaFrame::Type media);
	int EndpointAttachToVideoMixerPort(int endpointId,int mixerId,int portId);
	int EndpointAttachToVideoTranscoder(int endpointId,int transcoderId);

	//AudioMixer management
	int AudioMixerCreate(std::wstring tag);
	int AudioMixerDelete(int mixerId);
	//AudioMixer port management
	int AudioMixerPortCreate(int mixerId,std::wstring tag);
	int AudioMixerPortSetCodec(int mixerId,int portId,AudioCodec::Type codec);
	int AudioMixerPortDelete(int mixerId,int portId);
	//Filters type: 0=pre 1=post
	//int AudioMixerPortAddFilter(int mixerId,int portId,int type,...);
	//int AudioMixerPortUpdatedFilter(int mixerId,int portId,...);
	//int AudioMixerPortDeleteFilter(int mixerId,int portId,int filterId);
	//int AudioMixerPortClearFilters(int mixerId,int portId);
	//Port Attach  to
	int AudioMixerPortAttachToEndpoint(int mixerId,int portId,int endpointId);
	int AudioMixerPortAttachToPlayer(int mixerId,int portId,int playerId);
	int AudioMixerPortDettach(int mixerId,int portId);

        int AudioTranscoderCreate(std::wstring tag);
        int AudioTranscoderDelete(int transcoderId);
        std::shared_ptr<AudioTranscoder> GetAudioTranscoder(int transcoderId);
        
	//Video Mixer management
	int VideoMixerCreate(std::wstring tag);
	int VideoMixerDelete(int mixerId);
	//Video mixer port management
	int VideoMixerPortCreate(int mixerId,std::wstring tag,int mosiacId);
	int VideoMixerPortSetCodec(int mixerId,int portId,VideoCodec::Type codec,int size,int fps,int bitrate,int intraPeriod);
	int VideoMixerPortDelete(int mixerId,int porId);
	int VideoMixerPortAttachToEndpoint(int mixerId,int portId,int endpointId);
	int VideoMixerPortAttachToPlayer(int mixerId,int portId,int playerId);
	int VideoMixerPortDettach(int mixerId,int portId);
	//Video mixer mosaic management
	int VideoMixerMosaicCreate(int mixerId,Mosaic::Type comp,int size);
	int VideoMixerMosaicDelete(int mixerId,int portId);
	int VideoMixerMosaicSetSlot(int mixerId,int mosaicId,int num,int portId);
	int VideoMixerMosaicSetCompositionType(int mixerId,int mosaicId,Mosaic::Type comp,int size);
	int VideoMixerMosaicSetOverlayPNG(int mixerId,int mosaicId,const char* overlay);
	int VideoMixerMosaicResetSetOverlay(int mixerId,int mosaicId);
	int VideoMixerMosaicAddPort(int mixerId,int mosaicId,int portId);
	int VideoMixerMosaicRemovePort(int mixerId,int mosaicId,int portId);

	int VideoTranscoderCreate(std::wstring tag);
	int VideoTranscoderDelete(int transcoderId);
	int VideoTranscoderSetCodec(int transcoderId,VideoCodec::Type codec,int size,int fps,
				    int bitrate,int intraPeriod, Properties & props);					
	int VideoTranscoderFPU(int transcoderId);
	int VideoTranscoderAttachToEndpoint(int transcoderId,int endpointId);
	int VideoTranscoderDettach(int transcoderId);

	//Events
	virtual void onEndOfFile(Player *player,void* param);

	//Publication d'événements : la session connaît son propre id et le manager,
	//ce qui permet à toute ressource (player, recorder, endpoint) de publier sans
	//dépendre du câblage Joinable.
	void SetEventHandler(int sessionId, JSR309Manager* mngr);

	//Getters — copies de shared_ptr : l'appelant garde l'objet vivant même si un
	//Delete concurrent le retire des maps (C-2).
	std::wstring& GetTag() { return tag;	}

	std::shared_ptr<Endpoint>			GetEndpoint(int endpointId) ;
	std::shared_ptr<Player>				GetPlayer(int playerId) ;
	std::shared_ptr<JSR309EventContext>	GetEventContext(int EventContextId) ;
	/**
	 *  Callback that associate an actual media connection (here a web socket)
	 *  with an endpoint. The endpoint needs to be properly prepared by calling
	 *  ConfigureMediaConnection() an specify Web socket as transport protocol.
	 *  It shall also associate a "token" that will serve as security but also
	 *  as key to identify the concerned endpoint and the concerned media and
	 *  role
	 */
	int onNewMediaConnection(WebSocket *ws, const std::string & token);
	
	/**
	 *  Configure / change the protocol of a media for a given EndPoint.
	 *  By default, RTP media is used as transport but using this function
	 *  one can replace RTP by another media transport
	 *  @param endpointId:  id of enpoint to configure
	 *  @param media: Audio, Video, Text or Application. If selected protocol is RTMP
	 *  all media are automatically reconfigured
	 *  @param role: only for Video media, can select main video or slides
	 *  @param proto: media transport protocol expected for this media and role.
	 *  @param token: token to identify the triplet (endpoint, media, role)
	 *  This parameter is only useful for WebSocket and RTMP. It is ignored in
	 *  case of other protocol
	 *  @param expectedPayload: string that describe what kind of media is transported.
	 *  Unused for now.
	 *  @return
	 *  1: the endoint has been successfully configured
	 *  0:the endpoint could not be configured: unexpected error or bad state.
	 *  -1: no such endpoint.
	 *  -2: protocol not yet supported
	 *  -3: media or role not supported for this protocol	 
	 */
	int ConfigureMediaConnection( int endpointId, MediaFrame::Type media, MediaFrame::MediaRole role, 
				      MediaFrame::MediaProtocol proto, const char * token, const char * expectedPayload );
private:
	struct MediaCnxToken
	{
	    int endpointId;
	    MediaFrame::Type media;
	    MediaFrame::MediaRole role;
	    MediaFrame::MediaProtocol proto;
	};
	


	//Les ressources qui s'échappent de la session (getters, minuteries, threads)
	//sont détenues par shared_ptr ; les autres restent des pointeurs bruts détruits
	//sous le mutex de la session.
	typedef std::map<int,std::shared_ptr<Endpoint>> Endpoints;
	typedef std::map<int,std::shared_ptr<Recorder>> Recorders;
	typedef std::map<int,std::shared_ptr<Player>> Players;
	typedef std::map<int,std::shared_ptr<AudioMixerResource>> AudioMixers;
	typedef std::map<int,std::shared_ptr<VideoMixerResource>> VideoMixers;
	typedef std::map<int,std::shared_ptr<AudioTranscoder>> AudioTranscoders;
	typedef std::map<int,std::shared_ptr<VideoTranscoder>> VideoTranscoders;
	typedef std::map<std::string, MediaCnxToken> Tokens;
	typedef std::map<int, std::shared_ptr<JSR309EventContext>> EventContexts;
	typedef std::map<int, int> EventCtxMap;
private:
	//Publication d'événements interne. Appelable avec ou sans le mutex de session
	//tenu : le contexte est résolu sous eventContextsMutex. Remplit l'événement
	//à partir du contexte puis le remet au manager (DeliverEvent), qui ne
	//rappelle jamais la session — pas d'inversion de verrous (C-3).
	int PostEvent(int eventContextId, JSR309Event* ev);

private:
	std::wstring tag;
	//Protège toutes les maps et compteurs d'ids de la session (C-1)
	std::mutex mutex;

	MediaSession::Listener *listener;
	void* param;

	Endpoints endpoints;
	int maxEndpointId;

	Players players;
	int maxPlayersId;

	Recorders recorders;
	int maxRecordersId;

	AudioMixers	audioMixers;
	int maxAudioMixerId;

	VideoMixers	videoMixers;
	int maxVideoMixerId;

        AudioTranscoders audioTranscoders;
	VideoTranscoders videoTranscoders;
	int maxVideoTranscoderId;
	
	Tokens		tokens;
	
	//Les contextes d'événement ont un verrou À EUX, plus fin que celui de la
	//session : GetEventContext est appelé depuis le CHEMIN DES PAQUETS
	//(Joinable::Update → JSR309Manager::PostEvent), sous le mutex du port
	//source, pendant que le thread XML-RPC tient déjà le mutex de session et
	//attend ce même mutex de port. Prendre le mutex de session ici fermerait le
	//cycle. Ordre de verrouillage : mutex → eventContextsMutex, jamais
	//l'inverse ; eventContextsMutex ne protège que des lectures/écritures de
	//map, on n'appelle rien sous lui.
	std::mutex eventContextsMutex;
	EventContexts eventContexts;
	int maxEventContextId;

	//Correspondance playerId/recorderId -> id de contexte d'événement, pour publier
	//les événements de cycle de vie (fin de lecture, démarrage/arrêt d'enregistrement).
	EventCtxMap playerEventCtx;
	EventCtxMap recorderEventCtx;

	//Minuteries de durée max par recorder (reason=1). unique_ptr : la destruction
	//(qui joint le thread) doit toujours se faire HORS du mutex de session, car le
	//thread de minuterie peut être bloqué dessus dans onRecorderMaxDuration.
	typedef std::map<int, std::unique_ptr<RecorderTimer>> RecorderTimers;
	RecorderTimers recorderTimers;

	//Publication d'événements
	JSR309Manager* eventMngr;
	int            sessionId;

};

/**
 * Base commune des événements de cycle de vie d'un Player.
 * Sérialisation XML-RPC : (iss) = {type, sessionTag, playerTag}.
 * Le type effectif est passé au constructeur pour éviter la duplication de code
 * entre PlayerEndOfFileEvent et PlayerStartedEvent.
 */
class PlayerEvent: public JSR309Event
{
public:
	PlayerEvent(int type,std::wstring &playerTag) : type(type)
	{
		//Serialize player tag
		UTF8Parser playerTagParser(playerTag);
		DWORD playerLen  = playerTagParser.Serialize(this->playerTag,1024);
		this->playerTag[playerLen] = 0;
	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env)
	{
		BYTE sessTag[1024];
		UTF8Parser sessTagParser(sessionTag);
		DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
		sessTag[sessLen] = 0;

		return xmlrpc_build_value(env,"(iss)",type,sessTag,playerTag);
	}
protected:
	int  type;
	BYTE playerTag[1024];
};

class PlayerEndOfFileEvent: public PlayerEvent
{
public:
	PlayerEndOfFileEvent(std::wstring &playerTag)
		: PlayerEvent(JSR309Event::PlayerEndOfFileEvent,playerTag) {}
};

class PlayerStartedEvent: public PlayerEvent
{
public:
	PlayerStartedEvent(std::wstring &playerTag)
		: PlayerEvent(JSR309Event::PlayerStartedEvent,playerTag) {}
};

/**
 * Base commune des événements de cycle de vie d'un Recorder.
 * Sérialisation XML-RPC : (iss) = {type, sessionTag, recorderTag}.
 */
class RecorderEvent: public JSR309Event
{
public:
	RecorderEvent(int type,std::wstring &recorderTag) : type(type)
	{
		//Serialize recorder tag
		UTF8Parser recorderTagParser(recorderTag);
		DWORD recorderLen  = recorderTagParser.Serialize(this->recorderTag,1024);
		this->recorderTag[recorderLen] = 0;
	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env)
	{
		BYTE sessTag[1024];
		UTF8Parser sessTagParser(sessionTag);
		DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
		sessTag[sessLen] = 0;

		return xmlrpc_build_value(env,"(iss)",type,sessTag,recorderTag);
	}
protected:
	int  type;
	BYTE recorderTag[1024];
};

class RecorderStartedEvent: public RecorderEvent
{
public:
	RecorderStartedEvent(std::wstring &recorderTag)
		: RecorderEvent(JSR309Event::RecorderStartedEvent,recorderTag) {}
};

/**
 * Arrêt d'un enregistrement. Sérialisation XML-RPC :
 * (issi) = {type, sessionTag, recorderTag, reason}.
 * Motifs (alignés sur elixip) : 0=explicite/appelant, 1=durée max,
 * 2=silence (Phase 5), 3=DTMF (Phase 5).
 */
class RecorderStoppedEvent: public RecorderEvent
{
public:
	enum Reason
	{
		Explicit	= 0,
		MaxDuration	= 1,
		Silence		= 2,
		DTMF		= 3
	};

	RecorderStoppedEvent(std::wstring &recorderTag,int reason)
		: RecorderEvent(JSR309Event::RecorderStoppedEvent,recorderTag), reason(reason) {}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env)
	{
		BYTE sessTag[1024];
		UTF8Parser sessTagParser(sessionTag);
		DWORD sessLen = sessTagParser.Serialize(sessTag,1024);
		sessTag[sessLen] = 0;

		return xmlrpc_build_value(env,"(issi)",type,sessTag,recorderTag,reason);
	}
private:
	int reason;
};


#endif	/* MEDIASESSION_H */

