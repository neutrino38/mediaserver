#ifndef _MULTICONF_H_
#define _MULTICONF_H_

#include "videomixer.h"

#include "audiomixer.h"
#include "textmixer.h"
#include "videomixer.h"
#include "participant.h"
#include "rtpparticipant.h"
#include "rtmpparticipant.h"
#include "FLVEncoder.h"
#include "broadcastsession.h"
#include "mp4player.h"
#include "mp4recorder.h"
#include "audioencoder.h"
#include "textencoder.h"
#include "rtmpnetconnection.h"
#include "participanttextws.h"
#include "appmixer.h"
#include "shareddocmixer.h"
#include "dtmfmessage.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

class RTMPParticipant;

class MultiConf :
	public RTMPNetConnection,
	public Participant::Listener,
	public RTMPClientConnection::Listener
{
public:
	static const int AppMixerId = 1;
	static const int SharedDocMixerId = 2;
	static const int RecorderId = 10;
	
public:
	typedef std::map<std::string,MediaStatistics> ParticipantStatistics;
public:
	class NetStream : public RTMPNetStream
	{
	public:
		NetStream(DWORD streamId,MultiConf *conf,RTMPNetStream::Listener *listener);
		virtual ~NetStream();
		virtual void doPlay(std::wstring& url,RTMPMediaStream::Listener *listener);
		virtual void doPublish(std::wstring& url);
		virtual void doSeek(DWORD time);
		virtual void doClose(RTMPMediaStream::Listener *listener);
                virtual void doPause();
                virtual void doResume();
                virtual void doCommand(RTMPCommandMessage *cmd);
	protected:
		void Close();
	private:
		MultiConf *conf;
                std::weak_ptr<RTMPParticipant> part;
		bool opened;
	};

	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
		virtual void onParticipantRequestFPU(MultiConf *conf,int partId) = 0;
		virtual void onParticipantRequestDocSharing(MultiConf *conf,int partId,std::wstring status) = 0;

		//P7/S1-S2 : le flux RTP d'un media d'un participant s'est tu, ou son
		//premier paquet vient d'arriver. Non pures (cf. Participant::Listener).
		virtual void onParticipantMediaTimeout(MultiConf *conf,int partId,MediaFrame::Type media,MediaFrame::MediaRole role) {}
		virtual void onParticipantMediaConnected(MultiConf *conf,int partId,MediaFrame::Type media,MediaFrame::MediaRole role) {}
	};
	
public:
	MultiConf(const std::wstring& tag);
	~MultiConf();

	int Init(int vad,DWORD rate);
	int End();

	void SetListener(Listener *listener);

	int CreateMosaic(Mosaic::Type comp,int size);
	int SetMosaicOverlayImage(int mosaicId,const char* filename);
	int ResetMosaicOverlay(int mosaicId);
	int DeleteMosaic(int mosaicId);
	int CreateSidebar();
	int DeleteSidebar(int sidebarId);
	int CreateParticipant(int mosaicId,int sidebarId,std::wstring name,Participant::Type type);
	int StartRecordingParticipant(int partId,const char* filename);
	int StopRecordingParticipant(int partId);
	int SendFPU(int partId);
	int SetMute(int partId,MediaFrame::Type media,bool isMuted);
	void SetVADMode(int mode);
	ParticipantStatistics* GetParticipantStatistic(int partId);
	int SetParticipantMosaic(int partId,int mosaicId);
	int SetParticipantSidebar(int partId,int sidebarId);
	int SetParticipantDisplayName(int mosaicId, int partId, const char *name, int scriptCode);
	int DeleteParticipant(int partId);

	int CreatePlayer(int privateId,std::wstring name);
	int StartPlaying(int playerId,const char* filename,bool loop);
	int StopPlaying(int playerId);
	int DeletePlayer(int playerId);

	int AppMixerDisplayImage(const char* filename);
	// int AppMixerWebsocketConnectRequest(int partId,WebSocket *ws,bool isPresenter);
	
	int SetCompositionType(int mosaicId,Mosaic::Type comp,int size);
	int SetMosaicSlot(int mosaicId,int num,int id);
	int AddMosaicParticipant(int mosaicId,int partId);
	int RemoveMosaicParticipant(int mosaicId,int partId);
	int AddSidebarParticipant(int sidebar,int partId);
	int RemoveSidebarParticipant(int sidebar,int partId);

	int GetMosaicPositions(int mosaicId,std::list<int> &positions);
	
	//Profil d'adressage demandé par le contrôleur pour une jambe (voir
	//NETWORK-CONFIGURATION.md). À appeler AVANT StartSending/StartReceiving, qui
	//publient le port.
	int SetAddressProfile(int partId,MediaFrame::Type media,const char* profile,std::string& error,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//Adresse à publier dans le SDP pour cette jambe.
	IPAddress GetAnnouncedAddress(int partId,MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StartSending(int partId,MediaFrame::Type media,char *sendIp,int sendPort,RTPMap& rtpMap,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int StopSending(int partId,MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//P8a : `offerFmtp` (fmtp de l'offre par PT) et `negotiatedFmtpOut` (fmtp par PT
	//accepte) sont optionnels — NULL redonne exactement le comportement d'avant la
	//delegation, ce qui est ce qu'un controleur anterieur obtient.
	int StartReceiving(int partId,MediaFrame::Type media,RTPMap& rtpMap,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN,int confID= 0, MediaFrame::MediaProtocol proto = MediaFrame::TCP,
	                   const std::map<int,std::string>* offerFmtp = NULL,
	                   std::map<int,std::string>* negotiatedFmtpOut = NULL);
	int StopReceiving(int partId,MediaFrame::Type media,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	//P7/S1 : arme (timeoutMs > 0) ou desarme (0) le chien de garde d'inactivite
	//RTP d'un media d'un participant.
	int StartRTPTimeout(int partId,MediaFrame::Type media,DWORD timeoutMs,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetLocalCryptoSDES(int id,MediaFrame::Type media,const char *suite,const char* key, MediaFrame::MediaRole role);
	int SetRemoteCryptoSDES(int id,MediaFrame::Type media,const char *suite,const char* key, MediaFrame::MediaRole role,int keyRank=0);
	int SetLocalSTUNCredentials(int id,MediaFrame::Type media,const char *username,const char* pwd, MediaFrame::MediaRole role);
	int SetRemoteSTUNCredentials(int id,MediaFrame::Type media,const char *username,const char* pwd, MediaFrame::MediaRole role);
	int SetRemoteCryptoDTLS(int id,MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint);	
	int SetRTPProperties(int id,MediaFrame::Type media,const Properties& properties,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetParticipantBackground(int id, const char * filename);
	int SetParticipantOverlay(int mosaicId, int id, const char * filename);
	
	
	int AcceptDocSharingRequest(int confId,int partId);
	int RefuseDocSharingRequest(int confId,int partId);
	int StopDocSharing(int confId,int partId);
	int SetDocSharingMosaic(int mosaicId, int partId=0);
	
	int SetVideoCodec(int partId,int codec,int mode,int fps,int bitrate,int intraPeriod,const Properties &properties,MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	int SetAudioCodec(int partId,int codec,const Properties& properties);
	int SetTextCodec(int partId,int codec);
	int SetAppCodec(int confId, int partId,int codec);

	int  StartBroadcaster(int mosaicId, int sidebarId);
	int  StartRecordingBroadcaster(const char* filename,int mosaicId, int sidebarId);
	int  StopRecordingBroadcaster();
	int  StartPublishing(const char* server,int port, const char* app,const char* name);
	int  StopPublishing(int id);
	int  StopBroadcaster();

	bool AddParticipantInputToken(int partId,const std::wstring &token);
	bool AddParticipantOutputToken(int partId,const std::wstring &token);
	bool AddBroadcastToken(const std::wstring &token);

	//S5 : texte temps réel sur WebSocket pour un participant de conférence —
	//le miroir, sur cette API, du ConfigureMediaConnection JSR-309. Bascule le
	//plan texte du participant du RTP vers un pont ParticipantTextWS (couture
	//du mixeur), enregistre `token` pour la résolution de l'URL, et rend la
	//base `ws://host:port` ou `wss://host:port` (schéma décidé par le serveur,
	//jamais par le contrôleur). Chaîne vide en cas d'échec. Seul
	//media=Text/proto=WS est accepté. Une re-configuration remplace le token
	//du participant (l'ancien cesse de résoudre) et conserve le pont.
	std::string ConfigureParticipantMediaConnection(int partId,MediaFrame::Type media,MediaFrame::MediaProtocol proto,const std::string &token);
	//S5 : une connexion /mcu/<confId>/<token> acceptée par le handler du MCU.
	//Résout le token vers le pont du participant, ou Reject(404).
	void onNewMediaConnection(WebSocket *ws,const std::string &token);
	//S5 : le plan texte de ce participant est-il sur WebSocket ? Garde
	//StartSending/StartReceiving(TEXT), qui ouvriraient sinon un flux RTP
	//muet en silence (le proto y est ignoré pour les médias non-BFCP).
	bool TextOnWebSocket(int partId);

	std::weak_ptr<RTMPParticipant> ConsumeParticipantOutputToken(const std::wstring &token);
	RTMPMediaStream::Listener* ConsumeParticipantInputToken(const std::wstring &token);
	RTMPMediaStream* ConsumeBroadcastToken(const std::wstring &token);

	int GetNumParticipants() { return participants.size(); }
	std::wstring& GetTag() { return tag;	}

	/** Participants event */
	void onRequestFPU(Participant * part);
	void onRequestDocSharing(int partId, std::wstring status);
	void onDTMF(Participant * part , DTMFMessage* dtmf);
	//P7/S1-S2
	void onParticipantMediaTimeout(Participant *part,MediaFrame::Type media,MediaFrame::MediaRole role);
	void onParticipantMediaConnected(Participant *part,MediaFrame::Type media,MediaFrame::MediaRole role);

	/** RTMPNetConnection */
	//virtual void Connect(RTMPNetConnection::Listener* listener); -> Not needed to be overriden yet
	virtual RTMPNetStream* CreateStream(DWORD streamId,DWORD audioCaps,DWORD videoCaps,RTMPNetStream::Listener* listener);
	virtual void DeleteStream(RTMPNetStream *stream);
	//virtual void Disconnect(RTMPNetConnection::Listener* listener);  -> Not needed to be overriden yet

	/** RTMPClientConnection for pubblishers*/
	virtual void onConnected(RTMPClientConnection* conn);
	virtual void onNetStreamCreated(RTMPClientConnection* conn,RTMPClientConnection::NetStream *stream);
	virtual void onCommandResponse(RTMPClientConnection* conn,DWORD id,bool isError,AMFData* param);
	virtual void onDisconnected(RTMPClientConnection* conn);

        /**
         * Dump participant info as printable string
         * @param partId
         * @param info info to printout
         * @return HTTP error code
         */
        int DumpParticipantInfo(int partId, std::string & info);
        int DumpMixerInfo(int id, MediaFrame::Type media, std::string & info);
        int DumpInfo(std::string & info); 
private:
	ParticipantPtr GetParticipant(int partId);
	ParticipantPtr GetParticipant(int partId,Participant::Type type);
	RTPParticipantPtr GetRTPParticipant(int partId);

	int DestroyParticipant(int partId,ParticipantPtr part);


private:
	struct PublisherInfo
	{
		DWORD			id;
		std::wstring		name;
		std::unique_ptr<RTMPClientConnection>			conn;
		std::unique_ptr<RTMPClientConnection::NetStream>	stream;
	};
private:
	typedef std::map<int, ParticipantPtr> Participants;
	typedef std::set<std::wstring> BroadcastTokens;
	typedef std::map<std::wstring,DWORD> ParticipantTokens;
	typedef std::map<int, std::unique_ptr<MP4Player>> Players;
	typedef std::map<int, PublisherInfo> Publishers;

private:
	ParticipantTokens	inputTokens;
	ParticipantTokens	outputTokens;
	BroadcastTokens		tokens;

	//S5 : ponts texte-sur-WebSocket, par participant, et leurs tokens d'URL.
	//Contrairement aux tokens JSR-309 (fuite connue), ceux-ci meurent avec le
	//participant (DestroyParticipant). Un seul verrou pour les deux maps.
	typedef std::map<std::string,DWORD> TextWSTokens;
	typedef std::map<DWORD,std::shared_ptr<ParticipantTextWS>> TextWSBridges;
	TextWSTokens		textWSTokens;
	TextWSBridges		textWSBridges;
	std::mutex		textWSMutex;
	//Atributos
	int		inited;
	int		maxId;
	std::wstring	tag;

	Listener *listener;

	//Los mixers
	VideoMixer videoMixer;
	AudioMixer audioMixer;
	TextMixer  textMixer;
	AppMixer   appMixer;
	SharedDocMixer sharedDocMixer;
	
	//Lists
	Participants		participants;
	Players			players;

	int			watcherId;
	int			broadcastId;
	FLVEncoder		flvEncoder;
	FLVEncoder		recEncoder;
	AudioEncoderWorker	audioEncoder;
	TextEncoder		textEncoder;
	BroadcastSession	broadcast;
	std::unique_ptr<RecorderControl>	recorder;
	Publishers		publishers;
	int			maxPublisherId;

	Use			participantsLock;
	Use			playersLock;
	Use			publishersLock;
};

#endif
