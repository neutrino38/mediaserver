#ifndef _RTPSESSION_H_
#define _RTPSESSION_H_
#include <sys/socket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mutex>
#include <memory>
#include <map>
#include <string>
#include <poll.h>
#include <srtp2/srtp.h>
#include "config.h"
#include "use.h"
#include "rtp.h"
#include "rtpbuffer.h"
#include "remoteratecontrol.h"
#include "fecdecoder.h"
#include "stunmessage.h"
#include "remoterateestimator.h"
#include "dtls.h"



class RTPSession : 
	public RemoteRateEstimator::Listener,
	public DTLSConnection::Listener
{
public:
	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		virtual void onFPURequested(RTPSession *session) = 0;
		virtual void onReceiverEstimatedMaxBitrate(RTPSession *session,DWORD bitrate) = 0;
		virtual void onTempMaxMediaStreamBitrateRequest(RTPSession *session,DWORD bitrate,DWORD overhead) = 0;
		virtual void onNewStream( RTPSession *session, DWORD newSsrc, bool receiving ) ;
		//Watchdog d'inactivité RTP : appelé UNE seule fois lorsqu'aucun paquet n'a
		//été reçu depuis plus de rtpTimeout ms (gap 5 - EndpointDisconnectedEvent).
		//Non pur pour ne pas casser les Listener existants.
		virtual void onRTPTimeout( RTPSession *session ) {}
		//P5 : appelé UNE seule fois par cycle de réception, au premier paquet RTP/SRTP
		//reçu et validé (déchiffrement OK => DTLS terminé, ou pas de crypto). Ré-armé
		//via ArmRTPReceivedNotification(). Non pur (Listener existants inchangés).
		virtual void onRTPPacketReceived( RTPSession *session ) {}
	};
	
public:
	
public:
	static bool SetPortRange(int minPort, int maxPort);
	static DWORD GetMinPort() { return minLocalPort; }
	static DWORD GetMaxPort() { return maxLocalPort; }

	//Adresse annoncée dans le SDP (ligne c= et candidats ICE). Elle est globale au
	//serveur : la tenir ici, à côté de la plage de ports, évite que chaque API de
	//contrôle (JSR-309 via Endpoint::GetMediaCandidates, MCU via StartReceiving) la
	//redérive à sa façon. Derrière un NAT elle diffère de l'IP bindée, d'où le
	//réglage explicite --public-ip.
	//SetAnnouncedIp : à appeler au démarrage, avant tout GetAnnouncedIp. Rend true
	//si une adresse explicite valide a été installée, false si elle est absente ou
	//invalide (l'auto-détection reste alors en place).
	static bool SetAnnouncedIp(const char* ip);
	//Résolue à la première utilisation si aucune adresse explicite n'a été fournie :
	//premier IPv4 non loopback de l'hôte. Chaîne vide si l'hôte ne se résout pas.
	static const char* GetAnnouncedIp();

private:
	// Admissible port range
	static DWORD minLocalPort;
	static DWORD maxLocalPort;
	// Adresse annoncée dans le SDP, vide si la résolution a échoué. Le drapeau
	// distingue « pas encore résolue » de « résolue, sans résultat » : un échec
	// mémorisé évite de relancer un gethostbyname perdant à chaque appel.
	static std::string announcedIp;
	static bool announcedIpResolved;
	static std::mutex announcedIpMutex;
	
public:
	RTPSession(MediaFrame::Type media,Listener *listener, MediaFrame::MediaRole role = MediaFrame::VIDEO_MAIN);
	~RTPSession();
	//M-6 : enregistrement différé d'un listener géré par shared_ptr
	//(RTPParticipant). Une fois appelé, prend le pas sur le pointeur brut du
	//constructeur (utilisé tel quel par MediaBridgeSession, non converti).
	void SetWeakListener(std::weak_ptr<Listener> l) { weakListener = std::move(l); hasWeakListener = true; }
	int Init();
	void SetRemoteRateEstimator(RemoteRateEstimator* estimator);
	int SetLocalPort(int recvPort);
	int GetLocalPort();
	int SetRemotePort(char *ip,int sendPort);
	int 					GetRemotePort();
	//Configure UNIQUEMENT le seuil d'inactivité RTP en ms (n'arme pas le watchdog).
	//Le chrono ne démarre qu'à l'armement explicite (ArmRTPTimeout), tenu par le
	//contrôleur au moment du SDP answer (gap 5).
	void SetRTPTimeout(DWORD timeout) { rtpTimeout = timeout; }
	//Arme/désarme le watchdog d'inactivité RTP. À appeler par le contrôleur (elixip)
	//juste après l'envoi du SDP answer : le chrono part de cet instant, ce qui évite
	//les timeouts intempestifs pendant la sonnerie ET détecte « répondu mais aucun
	//média reçu ». timeoutMs>0 : (re)configure le seuil + arme ; timeoutMs==0 : désarme.
	void ArmRTPTimeout(DWORD timeoutMs);
	//P5 : (ré)arme la notification « premier paquet RTP/SRTP reçu » (onRTPPacketReceived).
	//Appelé à chaque (re)démarrage de la réception pour ré-émettre l'événement de connexion.
	void ArmRTPReceivedNotification() { rtpReceivedNotified = false; }
	//P6 : amorce le NAT traversal symétrique (comedia). Émet une courte rafale de
	//paquets RTP valides vers la destination pour ouvrir le pinhole et faire latcher
	//un pair symétrique (Asterisk nat=yes) qui n'émet qu'après avoir reçu du média.
	//Réservé au RTP en clair (le WebRTC amorce via STUN/DTLS). Appelé quand la
	//destination d'envoi devient connue (SetRemotePort) ; le thread Run cadence les
	//paquets suivants (~20 ms). No-op si chiffré ou destination inconnue.
	void ArmNATPriming();
	//Trickle ICE Niveau 1 (gap 1) : parse une ligne SDP "candidate:..." et, pour un
	//candidat host/srflx de priorité supérieure au candidat courant, reconfigure la
	//cible d'envoi RTP/RTCP. Retourne 1 si accepté/ignoré proprement, 0 sur erreur.
	int AddICECandidate(const char* candidate);
	
	
	RemoteRateEstimator* 	GetRemoteRateEstimator() 	{	return remoteRateEstimator; };
	bool 			SendBitrateFeedback() 		{	return sendBitrateFeedback; };
	bool 			IsNACKEnabled() 		{	return isNACKEnabled; }
	bool 			IsRequestFPU() 			{	return requestFPU; };
	bool 			UseFEC()			{	return useFEC; };
	bool 			UseExtFIR()			{	return useExtFIR; };
	bool 			UseRtcpFIR()			{	return useRtcpFIR; };
	int End();

	/**
	 *  Create a new RTP stream. If the stream already exists, it does nothing
	 *  
	 *  @param receiving: if this is a receiving or sending stream (only receiving is supported at the moment)
	 *  @param ssrc: ssrc of this new stream.
	 */
	bool AddStream( bool receiving, DWORD ssrc );
	
	/**
	 *  Get the default RTP stream SSRC of this session. It is the stream that is automatically created.
	 */
	DWORD GetDefaultStream(bool receiving) { return (defaultStream != NULL) ? defaultStream->GetRecSSRC() : 0 ; }


        bool DeleteStreams();
	/**
	 * Set the stream designated by SSRC as the defaut stream, if the stream does not exist create it
	 *
	 * @param: receiving whether it is receving or sending default stream
	 * @param: ssrc: ssrc of the stream to be set as default
	 *
	 **/
	bool SetDefaultStream(bool receiving, DWORD ssrc );
	

	/**
	 *  Change the SSRC of an existing stream.
	 */
	bool ChangeStream( DWORD oldssrc, DWORD newssrc );

	void SetSendingRTPMap(RTPMap &map);
	void SetReceivingRTPMap(RTPMap &map);
	bool SetSendingCodec(DWORD codec);

	int ForwardPacket( RTPPacket &packet, DWORD recssrc );
	
	int SendEmptyPacket();
	int SendPacket(RTPPacket &packet,DWORD timestamp);
	int SendPacket(RTPPacket &packet);
	
	
	void CancelGetPacket();
	
	// Multi stream
	RTPPacket* GetPacket();
	RTPPacket* GetPacket(DWORD & ssrc);
	void CancelGetPacket(DWORD & ssrc);
	
	void ResetPacket(bool clear) { if (defaultStream != NULL) defaultStream->Reset(clear) ;};
	void ResetPacket(DWORD & ssrc, bool clear);
	
        /**
         * Obtain the statistcs for a given stream or all the streams
         * @param ssrc SSRC of the receiving stream, 0 to sum up all the streams
         * @param stats statistic structure to populate
         * @return  true if the stats coulf be gathered, false in case of error
         */
        bool GetStatistics( DWORD ssrc, MediaStatistics & stats);


	DWORD 	GetSendSSRC()			const { return sendSSRC;	}
	const RTPMap* GetRtpMapIn()			const  { return rtpMapIn;	}
	timeval GetLastSR()				const  { return lastSR;	}
	timeval GetLastReceivedSR()		const  { return lastReceivedSR;	}
	//DWORD 	GetSendLastTime()		const  { return sendLastTime;	}
	DWORD 	GetRecSR()				const  { return recSR;	}
	//DWORD 	GetSendSR()				const  { return sendSR;	}
	
	//bool	GetPendingTMBR()			const  { return pendingTMBR;	}
	//DWORD	GetPendingTMBBitrate()	const  { return pendingTMBBitrate;	}
	
	//char*	GetCname()				const  { return cname;	}
	
	void 	SetSendSR(DWORD sendsr)				{ sendSR=sendsr;	}
	
	MediaFrame::Type 		GetMediaType()	const { return media;		}
	MediaFrame::MediaRole 	GetMediaRole()	const { return role;		}

	int SetLocalCryptoSDES(const char* suite, const char* key64);
	int SetRemoteCryptoSDES(const char* suite, const char* key64,int keyRank=0);
	int SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint);
	int SetLocalSTUNCredentials(const char* username, const char* pwd);
	int SetRemoteSTUNCredentials(const char* username, const char* pwd);
	int SetProperties(const Properties& properties);
	int RequestFPU();
	int RequestFPU(DWORD & ssrc);
	
	int SendTempMaxMediaStreamBitrateNotification(DWORD bitrate,DWORD overhead);

	virtual void onTargetBitrateRequested(DWORD bitrate);
	virtual void onDTLSSetup(DTLSConnection::Suite suite,BYTE* localMasterKey,DWORD localMasterKeySize,BYTE* remoteMasterKey,DWORD remoteMasterKeySize);
private:
	int SetLocalCryptoSDES(const char* suite, const BYTE* key, const DWORD len);
	int SetRemoteCryptoSDES(const char* suite, const BYTE* key, const DWORD len);
	void SetRTT(DWORD rtt);
	void Start();
	void Stop();
	int  ReadRTP();
	int  ReadRTCP();
	void ProcessRTCPPacket(RTCPCompoundPacket *packet, const char * fromAddr);
	void ReSendPacket(int seq);

	int SetRemoteCryptoSDES(const char* suite, const BYTE* key, const DWORD len, int keyRank=0);
	int Run();

	//P2 (offreur WebRTC) : pilotage du handshake DTLS en rôle CLIENT
	void FlushDTLS();                    //vide write_bio DTLS vers sendAddr
	void RequestDTLSClientHandshake();   //depuis les setters : réveille le thread Run
	void DriveDTLSClientHandshake();     //depuis Run : émet le ClientHello + retransmet

	//P3 (offreur WebRTC) : binding requests STUN sortants vers un pair ICE-lite
	void SendICEBindingRequest();               //émet un Binding Request vers sendAddr
	void DriveICEChecks();                       //depuis Run : émission + retransmission
	void OnICEConnectivityConfirmed(sockaddr_in* from); //réponse valide reçue -> débloque

private:
	static  void* run(void *par);
protected:
	

	class RTPStream : public RTPBuffer
	{
	public:
		RTPStream(RTPSession *s,DWORD recSSRC)
		{
			this->s = s;
			recExtSeq = 0;
			this->recSSRC = recSSRC;
			recCycles = 0;
			recTimestamp = 0;
			setZeroTime(&recTimeval);
			
			//Empty stats
			numRecvPackets = 0;
			totalRecvBytes = 0;
			lostRecvPackets = 0;
			totalRecvPacketsSinceLastSR = 0;
			totalRecvBytesSinceLastSR = 0;
			minRecvExtSeqNumSinceLastSR = RTPPacket::MaxExtSeqNum;
			jitter = 0;
			disabled = false;		
		}
		bool	Add(RTPTimedPacket *rtp, DWORD size);
		DWORD 	GetRecSSRC(){return recSSRC;};
		void	SetRecSSRC(DWORD ssrc) {recSSRC = ssrc;}; 
		
		int 				SendReceiverReport();
		RTCPReport* 		CreateReceiverReport();
		
		
		DWORD 	GetNumRecvPackets()					const { return numRecvPackets;	}
		DWORD 	GetTotalRecvBytes()					const { return totalRecvBytes;	}
		DWORD 	GetLostRecvPackets()				const { return lostRecvPackets;	}
		DWORD	GetRecExtSeq() 						const { return recExtSeq;	}	
		DWORD	GetTotalRecvPacketsSinceLastSR() 	const { return totalRecvPacketsSinceLastSR;	}
		DWORD	GetMinRecvExtSeqNumSinceLastSR() 	const { return minRecvExtSeqNumSinceLastSR;	}
		DWORD	GetRecCycles() 						const { return recCycles;	}
                DWORD   GetRecCodec() const { return recCodec; }
	
		bool disabled;
	private:
		DWORD	recExtSeq;
		DWORD	recSSRC;
		DWORD	recTimestamp;
		timeval recTimeval;
		//DWORD	recSR;
		DWORD   recCycles;
		
		//Statistics
		DWORD	numRecvPackets;
		DWORD	totalRecvBytes;
		DWORD	lostRecvPackets;
		
		DWORD	totalRecvPacketsSinceLastSR;
		DWORD	totalRecvBytesSinceLastSR;
		DWORD   minRecvExtSeqNumSinceLastSR;
		DWORD	jitter;

                DWORD   recCodec;
		
		FECDecoder		fec;
		
		RTPSession *s;
	};

	
	
	//Envio y recepcion de rtcp
	int RecvRtcp();
	int SendPacket(RTCPCompoundPacket &rtcp);
	int SendSenderReport();
	int SendFIR(DWORD & ssrc);
	RTCPCompoundPacket* CreateSenderReport();
        /**
	 *  Find the stream associated to the SSRC .
	 */
	RTPStream* getStream(DWORD ssrc);

private:
	typedef std::map<DWORD,RTPTimedPacket*> RTPOrderedPackets;
	typedef std::map<DWORD, RTPStream*> Streams;
protected:
	RemoteRateEstimator*	remoteRateEstimator;
private:
	MediaFrame::Type media;	
	MediaFrame::MediaRole role;
	
	Listener* listener;
	//M-6 : listener géré par shared_ptr (prioritaire si hasWeakListener).
	std::weak_ptr<Listener> weakListener;
	bool hasWeakListener = false;
	//Résout le listener courant : weak_ptr.lock() si armé, sinon wrapper
	//non-possédant du pointeur brut (comportement historique inchangé).
	std::shared_ptr<Listener> LockListener()
	{
		if (hasWeakListener)
			return weakListener.lock();
		return listener ? std::shared_ptr<Listener>(listener, [](Listener*){}) : nullptr;
	}

	Streams streams;
	RTPStream * defaultStream;
	
	bool muxRTCP;
	//Sockets
	int 	simSocket;
	int 	simRtcpSocket;
	int 	simPort;
	int	simRtcpPort;
	pollfd	ufds[2];
	bool	inited;
	bool	running;

	//Watchdog d'inactivité (gap 5) : horodatage de la dernière activité (paquet reçu
	//OU instant d'armement), seuil d'inactivité en ms (0 = désactivé), drapeau
	//d'armement (le chrono ne court que lorsqu'il est armé — armé au SDP answer) et
	//drapeau anti-rebond (transition unique actif -> inactif).
	timeval	lastRecv;
	DWORD	rtpTimeout;
	bool	rtpTimeoutArmed;
	bool	rtpTimedOut;

	DTLSConnection dtls;
	//P2 : état du handshake DTLS piloté en rôle client (offreur WebRTC).
	//dtlsClientStarted = ClientHello déjà émis (on pilote alors les retransmissions) ;
	//dtlsClientStart   = horodatage de la 1re émission (borne globale) ;
	//dtlsClientFailed  = échec déjà notifié (anti-rebond sur le chemin d'erreur transport).
	bool	dtlsClientStarted;
	timeval	dtlsClientStart;
	bool	dtlsClientFailed;
	bool	encript;
	bool	decript;
	srtp_t	sendSRTPSession;
	BYTE*	sendKey;
	srtp_t	recvSRTPSession;
	srtp_t	recvSRTPSessionRTX;
	srtp_t	recvSRTPSession_secondary;
	srtp_t	recvSRTPSessionRTX_secondary;
	BYTE*	recvKey;

	char*	cname;
	char*	iceRemoteUsername;
	char*	iceRemotePwd;
	char*	iceLocalUsername;
	char*	iceLocalPwd;
	//Meilleure priorité de candidat distant retenue (trickle ICE Niveau 1, gap 1)
	DWORD	iceRemotePriority;
	//P3 : émission de binding requests STUN sortants (pair ICE-lite qui n'initie
	//jamais). iceConnected = connectivité confirmée (réponse reçue OU check entrant) ;
	//iceCheckStarted = 1re émission faite ; iceCheckRto = intervalle de retransmission
	//courant (backoff) ; iceLastCheck = horodatage de la dernière émission ;
	//iceCheckTransId = transaction id du dernier check émis (corrélation de la réponse).
	bool	iceConnected;
	bool	iceCheckStarted;
	DWORD	iceCheckRto;
	timeval	iceLastCheck;
	BYTE	iceCheckTransId[12];
	//P5 : anti-rebond one-shot de l'événement « média établi » (premier paquet RTP/SRTP
	//reçu). Remis à false par ArmRTPReceivedNotification() à chaque StartReceiving.
	bool	rtpReceivedNotified;
	//P6 : amorçage NAT symétrique (comedia). natPrimingLeft = paquets restants dans
	//la rafale courante (0 = inactif) ; natPrimingLast = horodatage du dernier envoi,
	//utilisé par le thread Run pour cadencer ~20 ms. Émet un paquet et décrémente.
	int	natPrimingLeft;
	timeval	natPrimingLast;
	int	SendNATPrimingPacket();
	pthread_t thread;
	std::mutex mutex;	

	//Tipos
	int 	sendType;

	//Transmision
	sockaddr_in sendAddr;
	sockaddr_in sendRtcpAddr;
	//srtp_protect() chiffre en place ET ajoute le trailer SRTP (jusqu'a
	//SRTP_MAX_TRAILER_LEN octets au-dela de la charge utile) : le tampon doit
	//donc reserver MTU + trailer, sinon debordement a l'emission (le memset du
	//constructeur supposait deja cette taille).
	BYTE 	sendPacket[MTU+SRTP_MAX_TRAILER_LEN];
	WORD    sendSeq;
        DWORD   sendExtSeq;
        DWORD   sendCycles;
	DWORD   sendTime;
	DWORD	sendLastTime;
	DWORD	sendSSRC;
	DWORD	sendSR;
	DWORD	recSR;
	//Recepcion
	BYTE	recBuffer[MTU];
	in_addr_t recIP;
	in_addr_t iceRemoteIP;
	DWORD	  recPort;

	//RTP Map types
	RTPMap* rtpMapIn;
	RTPMap* rtpMapOut;
	RTPMap	extMap;

	DWORD	numSendPackets;
	DWORD	totalSendBytes;	
	BYTE	firReqNum;

	DWORD	rtt;
	timeval lastSR;
	timeval lastReceivedSR;
	bool	requestFPU;
	bool	pendingTMBR;
	DWORD	pendingTMBBitrate;

	//FECDecoder		fec;
	bool			useFEC;
	bool			useNACK;
	bool			isNACKEnabled;
	bool			sendBitrateFeedback;
	bool			useAbsTime;
	bool 			useOriSeqNum;
	bool 			useOriTS;
	bool 			useExtFIR;
	bool 			useRtcpFIR;

	RTPOrderedPackets	rtxs;
	Use				rtxUse;
	Use				streamUse;
    bool        	resetRequested;
	
	DWORD			lastSendSSRC;

};

#endif
