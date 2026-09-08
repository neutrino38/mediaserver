#ifndef _MCU_H_
#define _MCU_H_
#include <string>
#include <mutex>
#include <memory>
#include "multiconf.h"
#include "rtmpstream.h"
#include "rtmpapplication.h"
#include "eventqueuesweeper.h"
#include "xmlstreaminghandler.h"
#include "uploadhandler.h"
#include "websocketserver.h"



class MCU :
	public RTMPApplication,
	public MultiConf::Listener,
	public UploadHandler::Listener,
	//S5 : la porte WebSocket de l'API conférence (texte temps réel d'un
	//participant). Enregistrée par main.cpp sous le préfixe "/mcu", à côté du
	//"/jsr309" historique.
	public WebSocketServer::Handler,
	//Expiration des conférences dont la file d'événements n'est plus lue
	private EventQueueSweeper
{
public:
	//Codes partages avec TOUS les controleurs, mcuGold inclus : on AJOUTE en fin,
	//jamais on ne renumerote ni on ne reutilise. Meme discipline que
	//JSR309Event::Events.
	enum Events
	{
		ParticipantRequestFPU = 1,
		ParticipantRequestDocSharing = 2,
		//P7/S1 : le flux RTP d'un media s'est tu (transition actif -> inactif).
		ParticipantMediaTimeout = 3,
		//P7/S2 : premier paquet RTP/SRTP valide d'un cycle de reception. Pour une
		//patte securisee, cela prouve intrinsequement que la poignee de main DTLS
		//a abouti.
		ParticipantMediaConnected = 4
	};

	struct ConferenceInfo
	{
		int id;
		std::wstring name;
		int numPart;
	};

	typedef  std::map<int,ConferenceInfo> ConferencesInfo;
public:
	MCU();
	~MCU();

	//queueExpiresSecs : délai de grâce sans lecteur sur la file d'événements
	//d'une conférence avant sa destruction (0 = expiration désactivée,
	//comportement historique). MÊME politique que l'API JSR-309 : la portée du
	//nettoyage suit donc le découpage des files choisi par le contrôleur — une
	//file par conférence isole les conférences entre elles, une file partagée
	//les emporte ensemble (cf. MCU-API.md §5).
	int Init(XmlStreamingHandler *eventMngr,int queueExpiresSecs = XmlEventQueue::DefaultExpiresSecs);
	int CreateEventQueue();
	int DeleteEventQueue(int id);
	int End();

	//S5 : /mcu/<confId>/<token> — résout la conférence puis délègue au
	//MultiConf::onNewMediaConnection, le miroir du handler JSR-309.
	virtual void onWebSocketConnection(const HTTPRequest &request,WebSocket *ws);

	int CreateConference(std::wstring tag,int queueId);
	int GetConferenceRef(int id,std::shared_ptr<MultiConf> &conf);
	int GetConferenceId(const std::wstring& tag);
	int DeleteConference(int confId);
	int GetConferenceList(ConferencesInfo& lst);
	//Charge instantanée, pour /status/general. Distincte de GetConferenceList,
	//qui journalise son entrée et sa sortie : un statut interrogé en boucle par
	//une supervision remplirait /var/log/mcu.log à lui seul.
	void GetLoad(int& conferences,int& participants);

	/** Conference events*/
	virtual void onParticipantRequestFPU(MultiConf *conf,int partId);
	virtual void onParticipantRequestDocSharing(MultiConf *conf,int partId,std::wstring status);
	//P7/S1-S2
	virtual void onParticipantMediaTimeout(MultiConf *conf,int partId,MediaFrame::Type media,MediaFrame::MediaRole role);
	virtual void onParticipantMediaConnected(MultiConf *conf,int partId,MediaFrame::Type media,MediaFrame::MediaRole role);

	/** RTMP application interface*/
	virtual std::shared_ptr<RTMPNetConnection> Connect(const std::wstring& appName,RTMPNetConnection::Listener* listener);
	/** File uploader event */
	virtual int onFileUploaded(const char* url, const char *filename);
private:
	struct ConferenceEntry
	{
		int queueId;
		std::shared_ptr<MultiConf> conf;
	};

	typedef std::map<int,ConferenceEntry> Conferences;
	typedef std::map<std::wstring,int> ConferenceTags;

private:
	//EventQueueSweeper : files référencées par les conférences...
	virtual void CollectQueueIds(std::set<int>& ids);
	//... et destruction de toutes les conférences dont l'entrée pointe vers
	//cette file (extraction sous verrou, End() hors verrou comme
	//DeleteConference). Rend le nombre de conférences détruites.
	virtual int DeleteQueueOwners(int queueId,const char *reason);

private:
	XmlStreamingHandler	*eventMngr;
	Conferences		conferences;
	ConferenceTags		tags;
	int			maxId;
	std::mutex		mutex;
	int inited;
};

class PlayerRequestDocSharingEvent: public XmlEvent
{
public:
	PlayerRequestDocSharingEvent(int confId,std::wstring &tag, int partId, std::wstring &status)
	{
		//ACTIVE,WAITING_ACCEPT,NONE,FAILED
		//Get status
		UTF8Parser statusParser(status);
		
		//Get session tag
		UTF8Parser tagParser(tag);
		
		//Serialize
		DWORD len = statusParser.Serialize(this->status,64);
		
		//Serialize
		DWORD lenTag = tagParser.Serialize(this->tag,1024);

		//Set end
		this->status[len] = 0;
		this->tag[lenTag] = 0;
		
		//Store other values
		this->confId = confId;
		this->partId = partId;
	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env)
	{
		return xmlrpc_build_value(env,"(iisis)",(int)MCU::ParticipantRequestDocSharing,confId,tag,partId,status);
	}
private:
	int confId;
	BYTE tag[1024];
	BYTE status[64];
	int partId;
};

class PlayerRequestFPUEvent: public XmlEvent
{
public:
	PlayerRequestFPUEvent(int confId,std::wstring &tag,int partId)
	{
		//Get session tag
		UTF8Parser tagParser(tag);
		
		//Serialize
		DWORD len = tagParser.Serialize(this->tag,1024);

		//Set end
		this->tag[len] = 0;
		//Store other values
		this->confId = confId;
		this->partId = partId;
	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env)
	{
		return xmlrpc_build_value(env,"(iisi)",(int)MCU::ParticipantRequestFPU,confId,tag,partId);
	}
private:
	int confId;
	BYTE tag[1024];
	int partId;
};

/**
 * P7/S1-S2 : evenements media par participant.
 *
 * Meme tuple pour les deux, seul le type change :
 *   (i type, i confId, s tag, i partId, i media, i role)
 *
 * `media` est un MediaFrame::Type et `role` un MediaFrame::MediaRole : le
 * controleur a besoin des deux pour savoir QUELLE ligne m= s'est tue, un
 * participant pouvant porter audio + video principale + video presentation.
 */
class ParticipantMediaEvent: public XmlEvent
{
public:
	ParticipantMediaEvent(MCU::Events type,int confId,std::wstring &tag,int partId,int media,int role)
	{
		//Get session tag
		UTF8Parser tagParser(tag);

		//Serialize
		DWORD len = tagParser.Serialize(this->tag,1024);

		//Set end
		this->tag[len] = 0;
		//Store other values
		this->type   = (int)type;
		this->confId = confId;
		this->partId = partId;
		this->media  = media;
		this->role   = role;
	}

	virtual xmlrpc_value* GetXmlValue(xmlrpc_env *env)
	{
		return xmlrpc_build_value(env,"(iisiii)",type,confId,tag,partId,media,role);
	}
private:
	int type;
	int confId;
	BYTE tag[1024];
	int partId;
	int media;
	int role;
};

#endif
