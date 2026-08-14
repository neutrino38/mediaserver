#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/poll.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <srtp2/srtp.h>
#include <time.h>
#include "log.h"
#include "tools.h"
#include "medkit/codecs.h"
#include "rtp.h"
#include "rtpsession.h"
#include "stunmessage.h"
extern "C" {
#include <libavutil/base64.h>
}
#include <openssl/ossl_typ.h>

BYTE rtpEmpty[] = {0x80,0x14,0x00,0x00,0x00,0x00,0x00,0x00};

//P6 : amorçage NAT symétrique (comedia). Nombre de paquets RTP valides émis en
//rafale dès que la destination est connue, et intervalle (ms) entre eux. Plusieurs
//paquets espacés résistent à la perte UDP et fiabilisent le latch d'un pair
//symétrique (Asterisk nat=yes) qui n'émet qu'après avoir reçu du média.
#define NAT_PRIMING_BURST	3
#define NAT_PRIMING_INTERVAL_MS	20

//Durée (ms) pendant laquelle la map de réception PRÉCÉDENTE reste utilisable après
//une renégociation. Elle couvre le trou de l'offre/réponse (RFC 3264 §8) : le pair
//qui a offert continue d'émettre sous son ancienne numérotation jusqu'à recevoir la
//réponse — un aller-retour SIP, plus le temps que met un B2BUA à obtenir celle de
//l'autre jambe. Quelques secondes couvrent largement le cas ; au-delà, un pair qui
//n'a toujours pas basculé a un vrai problème et ses paquets redeviennent des rebuts.
#define RTP_MAP_FALLBACK_MS	5000

//Cadence maximale (ms) des traces « payload type non négocié ». Le régime normal
//qu'elles décrivent — l'offreur qui émet encore avec l'ancienne numérotation
//pendant une renégociation — dure quelques centaines de millisecondes à 20-50
//paquets/s : au-delà de la première ligne, ce qui compte est COMBIEN et jusqu'à
//QUAND, pas une ligne par paquet. Cf. RTPSession::OnUnknownPayloadType.
#define UNKNOWN_PT_LOG_PERIOD_MS	2000

//srtp library initializers
class SRTPLib
{
public:
	SRTPLib()	{ srtp_init();		}
	~SRTPLib()	{ srtp_shutdown();	}
};
SRTPLib srtp;

DWORD RTPSession::minLocalPort = 49152;
DWORD RTPSession::maxLocalPort = 65535;

bool RTPSession::SetPortRange(int minPort, int maxPort)
{
	// mitPort should be even
	if ( minPort % 2 )
		minPort++;

	//Check port range is possitive
	if (maxPort<minPort)
		//Error
		return Error("-RTPSession port range invalid [%d,%d]\n",minPort,maxPort);

	//check min range ports
	if (maxPort-minPort<50)
	{
		//Error
		Error("-RTPSession port range too short %d, should be at least 50\n",maxPort-minPort);
		//Correct
		maxPort = minPort+50;
	}

	//check min range
	if (minPort<1024)
	{
		//Error
		Error("-RTPSession min rtp port is inside privileged range, increasing it\n");
		//Correct it
		minPort = 1024;
	}

	//Check max port
	if (maxPort>65535)
	{
		//Error
		Error("-RTPSession max rtp port is too high, decreasing it\n");
		//Correc it
		maxPort = 65535;
	}
	
	//Set range
	minLocalPort = minPort;
	maxLocalPort = maxPort;

	//Log
	Log("-RTPSession configured RTP/RTCP ports range [%d,%d]\n", minLocalPort, maxLocalPort);

	//OK
	return true;
}

std::string RTPSession::announcedIp;
bool RTPSession::announcedIpResolved = false;
std::mutex RTPSession::announcedIpMutex;

//Auto-détection de l'adresse à annoncer, à défaut de --public-ip : premier IPv4
//non loopback de l'hôte. Déplacée ici depuis Endpoint::GetMediaCandidates, qui la
//refaisait — gethostbyname compris, et sans verrou — à chaque appel.
static std::string DetectAnnouncedIp()
{
	char hostname[HOST_NAME_MAX];

	//Nom de l'hôte
	if (gethostname(hostname, sizeof hostname) != 0)
	{
		//Erreur
		Error("-RTPSession cannot get hostname to detect the announced IP\n");
		//Rien
		return std::string();
	}

	//Résolution
	struct hostent *localHost = gethostbyname(hostname);

	//Comprobamos
	if (!localHost || localHost->h_addrtype != AF_INET)
	{
		//Erreur
		Error("-RTPSession cannot resolve \"%s\" to detect the announced IP\n",hostname);
		//Rien
		return std::string();
	}

	//Première adresse qui n'est pas la loopback
	for (int i=0; localHost->h_addr_list[i]!=0; i++)
	{
		struct in_addr addr;
		//Copie : une entrée de h_addr_list fait h_length octets (4 en AF_INET),
		//pas sizeof(u_long) — le déréférencement en u_long lisait 8 octets sur LP64,
		//donc 4 octets hors du buffer de la résolveuse.
		memcpy(&addr.s_addr,localHost->h_addr_list[i],sizeof(addr.s_addr));
		//En texte
		const char* host = inet_ntoa(addr);

		//Celle-là fera l'affaire
		if (host && strcmp(host,"127.0.0.1")!=0)
			return std::string(host);
	}

	//Erreur
	Error("-RTPSession no non-loopback IPv4 address found for \"%s\"\n",hostname);

	//Rien
	return std::string();
}

bool RTPSession::SetAnnouncedIp(const char* ip)
{
	//Rien de fourni : l'auto-détection de GetAnnouncedIp reste en place
	if (!ip || !*ip)
		return false;

	struct in_addr addr;

	//Une adresse annoncée fausse produit un SDP que le pair ne peut pas joindre :
	//on refuse ce qui n'est pas une IPv4 littérale plutôt que de l'annoncer.
	if (inet_pton(AF_INET,ip,&addr)!=1)
		return Error("-RTPSession announced IP \"%s\" is not a valid IPv4 address\n",ip);

	//Verrou
	std::lock_guard<std::mutex> lock(announcedIpMutex);

	//Set : une adresse explicite dispense de toute auto-détection
	announcedIp = ip;
	announcedIpResolved = true;

	//Log
	Log("-RTPSession announced IP set to \"%s\"\n",announcedIp.c_str());

	//OK
	return true;
}

const char* RTPSession::GetAnnouncedIp()
{
	//Verrou
	std::lock_guard<std::mutex> lock(announcedIpMutex);

	//Résolue une fois pour toutes — le pointeur rendu reste donc valide — et
	//l'échec est mémorisé au même titre que le succès : sans quoi un hôte qui ne
	//se résout pas relancerait un gethostbyname perdant à chaque appel.
	if (!announcedIpResolved)
	{
		announcedIp = DetectAnnouncedIp();
		announcedIpResolved = true;
	}

	//La chaîne, vide si l'hôte ne se résout pas
	return announcedIp.c_str();
}

/*************************
* RTPSession
* 	Constructro
**************************/
RTPSession::RTPSession(MediaFrame::Type media,Listener *listener,MediaFrame::MediaRole role) : dtls(*this)
{
	//Store listener
	this->listener = listener;
	//And media
	this->media = media;
	this->role	= role;
	//Init values
	sendType = -1;
	simSocket = FD_INVALID;
	simRtcpSocket = FD_INVALID;
	simPort = 0;
	simRtcpPort = 0;
	sendSeq = 0;
	sendTime = random();
	sendLastTime = sendTime;
	sendSSRC = random();
	sendSR = 0;
	recSR = 0;
	sendCycles = 0;
	
	recIP = INADDR_ANY;
	recPort = 0;
	//P7 : rattrapage NAT désactivé par défaut — le plan de contrôle l'active par la
	//propriété "natLatch" (ou en passant 0.0.0.0 à SetRemotePort), lui seul sachant
	//de quel type de jambe il s'agit. Aucun appelant historique n'est donc affecté.
	natLatch = false;
	natCorrected = false;
	natRtcpCorrected = false;
	firReqNum = 0;
	requestFPU = false;
	pendingTMBR = false;
	pendingTMBBitrate = 0;
	//Not muxing
	muxRTCP = false;
	//Default cname
	cname = strdup("default@localhost");
	//Empty types by defauilt
	rtpMapIn = NULL;
	rtpMapOut = NULL;
	//Aucune renégociation encore vue : pas de map précédente à quoi se rattraper
	rtpMapInPrev = NULL;
	setZeroTime(&rtpMapInPrevSince);
	salvagedType = 0;
	salvagedCount = 0;
	setZeroTime(&salvagedLast);
	//Aucun paquet au payload type inconnu encore reçu (cf. OnUnknownPayloadType)
	unknownPtType = 0;
	unknownPtCount = 0;
	setZeroTime(&unknownPtLast);
	//statistics
	totalSendBytes = 0;
	numSendPackets = 0;
	//Watchdog d'inactivité RTP désactivé par défaut (gap 5) : ni configuré ni armé
	setZeroTime(&lastRecv);
	rtpTimeout = 0;
	rtpTimeoutArmed = false;
	rtpTimedOut = false;
	//No reports
	setZeroTime(&lastSR);
	setZeroTime(&lastReceivedSR);
	rtt = 0;
	//No cripto
	encript = false;
	decript = false;
	//P2 : handshake DTLS client non amorcé
	dtlsClientStarted = false;
	dtlsClientFailed  = false;
	setZeroTime(&dtlsClientStart);
	sendSRTPSession = NULL;
	recvSRTPSession = NULL;
	recvSRTPSession_secondary = NULL;
	recvSRTPSessionRTX = NULL;
	recvSRTPSessionRTX_secondary = NULL;
	sendKey		= NULL;
	recvKey		= NULL;
	//No ice
	iceLocalUsername = NULL;
	iceLocalPwd = NULL;
	iceRemoteUsername = NULL;
	iceRemotePwd = NULL;
	//Aucun candidat ICE distant retenu (gap 1)
	iceRemotePriority = 0;
	//P3 : aucun check STUN sortant émis, connectivité non confirmée
	iceConnected    = false;
	iceCheckStarted = false;
	iceCheckRto     = 0;
	setZeroTime(&iceLastCheck);
	memset(iceCheckTransId,0,sizeof(iceCheckTransId));
	//P5 : événement « média établi » pas encore émis
	rtpReceivedNotified = false;
	//P6 : aucune rafale d'amorçage NAT en cours
	natPrimingLeft = 0;
	setZeroTime(&natPrimingLast);
	//NO FEC
	useFEC = false;
	useNACK = false;
	useAbsTime = false;
	isNACKEnabled = false;
	//Fill with 0
	memset(sendPacket,0,MTU+SRTP_MAX_TRAILER_LEN);
	//Preparamos las direcciones de envio
	memset(&sendAddr,       0,sizeof(struct sockaddr_in));
	memset(&sendRtcpAddr,   0,sizeof(struct sockaddr_in));
	//No thread
	running = false;
	//No stimator
	remoteRateEstimator = NULL;
	sendBitrateFeedback = false; // test

	//Set family
	sendAddr.sin_family     = AF_INET;
	sendRtcpAddr.sin_family = AF_INET;
	
	defaultStream = NULL;
	
	useOriSeqNum	=false;
	useOriTS		=false;
	useExtFIR		=false;
	useRtcpFIR		=true;
	
	lastSendSSRC		=0;
}

/*************************
* ~RTPSession
* 	Destructor
**************************/
RTPSession::~RTPSession()
{
    End();
	//Check listener
	if (remoteRateEstimator && sendBitrateFeedback)
		//Add as listener
		remoteRateEstimator->SetListener(NULL);
	if (rtpMapIn)
		delete(rtpMapIn);
	if (rtpMapInPrev)
		delete(rtpMapInPrev);
	if (rtpMapOut)
		delete(rtpMapOut);
	//Delete packets
	for(Streams::iterator it=streams.begin(); it!=streams.end(); it++)
	{
		if (it->second)
			it->second->Clear();
	}	
	//Clean mem
	if (iceLocalUsername)
		free(iceLocalUsername);
	if (iceLocalPwd)
		free(iceLocalPwd);
	if (iceRemoteUsername)
		free(iceRemoteUsername);
	if (iceRemotePwd)
		free(iceRemotePwd);
	//If secure
	if (sendSRTPSession)
		//Dealoacate
		srtp_dealloc(sendSRTPSession);
	//If secure
	if (recvSRTPSession)
		//Dealoacate
		srtp_dealloc(recvSRTPSession);
	if (recvSRTPSession_secondary)
		//Dealoacate
		srtp_dealloc(recvSRTPSession_secondary);	
	//If RTX
	if (recvSRTPSessionRTX)
		//Dealoacate
		srtp_dealloc(recvSRTPSessionRTX);
	if (recvSRTPSessionRTX_secondary)
		//Dealoacate
		srtp_dealloc(recvSRTPSessionRTX_secondary);
	if (sendKey)
		free(sendKey);
	if (recvKey)
		free(recvKey);
	if (cname)
		free(cname);
	//Delete rtx packets

}

void RTPSession::SetSendingRTPMap(RTPMap &map)
{
	//If we already have one
	if (rtpMapOut)
		//Delete it
		delete(rtpMapOut);
	//Clone it
	rtpMapOut = new RTPMap(map);
}

int RTPSession::SetLocalCryptoSDES(const char* suite, const BYTE* key,const DWORD len)
{
	srtp_err_status_t err;
	srtp_policy_t policy;

	Log("-Set local RTP SDES [key:%s,suite:%s]\n",key,suite);

	//empty policy
	memset(&policy, 0, sizeof(srtp_policy_t));

	//Get cypher
	if (strcmp(suite,"AES_CM_128_HMAC_SHA1_80")==0)
	{
		Log("RTPSession::SetLocalCryptoSDES() | suite: AES_CM_128_HMAC_SHA1_80\n");
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	}
	else if (strcmp(suite,"AES_CM_128_HMAC_SHA1_32")==0) 
	{
		Log("RTPSession::SetLocalCryptoSDES() | suite: AES_CM_128_HMAC_SHA1_32\n");
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	} 
	else if (strcmp(suite,"AES_CM_128_NULL_AUTH")==0)
	{
		Log("RTPSession::SetLocalCryptoSDES() | suite: AES_CM_128_NULL_AUTH\n");
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtcp);
	}
	else if (strcmp(suite,"NULL_CIPHER_HMAC_SHA1_80")==0) 
	{
		Log("RTPSession::SetLocalCryptoSDES() | suite: NULL_CIPHER_HMAC_SHA1_80\n");
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtcp);
	}
	else {
		return Error("Unknown cipher suite");
	}

	//Check sizes
	if (len!=policy.rtp.cipher_key_len)
		//Error
		return Error("Key size (%d) doesn't match the selected srtp profile (required %d)\n",len,policy.rtp.cipher_key_len);

	//Set polciy values
	policy.ssrc.type	= ssrc_any_outbound;
	policy.ssrc.value	= 0;
	policy.allow_repeat_tx  = 1; // supporte par libsrtp2 (necessaire pour le RTX)
    policy.key		= (BYTE*)key;
	policy.next		= NULL;
	srtp_t session;
	err = srtp_create(&session,&policy);	
	//Check error
	if (err!=srtp_err_status_ok)
		//Error
		return Error("Failed to create srtp session (%d)\n", err);

	//Set send SSRTP sesion
	sendSRTPSession = session;

	//Request an intra to start clean
	if (auto l = LockListener())
		//Request a I frame
		l->onFPURequested(this);

	//Evrything ok
	return 1;
}

int RTPSession::SetLocalCryptoSDES(const char* suite, const char* key64)
	//Log
{
	Log("-SetLocalCryptoSDES [key:%s,suite:%s]\n",key64,suite);

	//encript
	encript = true;

	//Get lenght
	WORD len64 = strlen(key64);
	//Allocate memory for the key
	BYTE sendKey[len64];
	//Decode
	WORD len = av_base64_decode(sendKey,key64,len64);

	//Set it
	return SetLocalCryptoSDES(suite,sendKey,len);
}

int RTPSession::SetProperties(const Properties& properties)
{
	mutex.lock();
	//Clean txtension map
	extMap.clear();
	//For each property
	for (Properties::const_iterator it=properties.begin();it!=properties.end();++it)
	{
		Log("Setting RTP property [%s:%s] on %s %s stream %p\n",it->first.c_str(),it->second.c_str(),
		    MediaFrame::RoleToString(role), MediaFrame::TypeToString(media),this);
		
		//Check
		if (it->first.compare("rtcp-mux")==0)
		{
			//Set rtcp muxing
			muxRTCP = atoi(it->second.c_str());
		} 
		else if (it->first.compare("natLatch")==0)
		{
			//Autorise le rattrapage de la cible d'envoi vers la source réellement
			//observée quand le pair a annoncé une adresse privée (NAT symétrique).
			//Désactivé par défaut : c'est au plan de contrôle, qui seul sait de quel
			//type de jambe il s'agit, de l'activer. Voir SendPacket / NatCorrectable.
			natLatch = atoi(it->second.c_str());
			if (natLatch) Log("Activated symmetric NAT latching on %s stream %p.\n", MediaFrame::TypeToString(media), this);
		}
		else if (it->first.compare("tmmbr")==0)
		{
			sendBitrateFeedback = atoi(it->second.c_str());
			if ( sendBitrateFeedback ) Log("Activated bitrate feedback on %s stream %p.\n", MediaFrame::TypeToString(media), this);
		}
		else if (it->first.compare("ssrc")==0) {
			//Set ssrc for sending
			sendSSRC = atoi(it->second.c_str());
		} else if (it->first.compare("cname")==0) {
			//Check if already got one
			if (cname)
				//Delete it
				free(cname);
			//Clone
			cname = strdup(it->second.c_str());
		} else if (it->first.compare("useFEC")==0) {
			//Set fec decoding
			useFEC = atoi(it->second.c_str());
		} else if (it->first.compare("useNACK")==0) {
			//Set fec decoding
			useNACK = atoi(it->second.c_str());
			//Enable NACK until first RTT
			isNACKEnabled = useNACK;

		}
		else if (it->first.compare("useOriSeqNum")==0) {
			//Set numerotation of seqNum
			useOriSeqNum 	= atoi(it->second.c_str());
			useOriTS 	= atoi(it->second.c_str());
		}
		else if (it->first.compare("useExtFIR")==0) {
			//Set use of SIP INFO FIR
			useExtFIR = atoi(it->second.c_str());
			
		}
		else if (it->first.compare("useRtcpFIR")==0) {
			//Set use of RTCP FIR
			useRtcpFIR = atoi(it->second.c_str());

		}
		else if (it->first.compare("rtpTimeout")==0) {
			//Pré-configure UNIQUEMENT le seuil d'inactivité RTP en ms (gap 5).
			//N'arme PAS le watchdog : le chrono ne démarre qu'à l'armement explicite
			//(ArmRTPTimeout, appelé au SDP answer). Cela évite qu'un réglage posé au
			//setup ne déclenche des timeouts pendant la sonnerie.
			rtpTimeout = (DWORD)atoi(it->second.c_str());
			Log("Set rtpTimeout=%u ms (config, non armé) on %s stream %p\n",rtpTimeout,MediaFrame::TypeToString(media),this);
		}
		else if (it->first.compare(0, 5, "codec")==0) {
			// Ignore codec props
			//Set extension
			extMap[RTPPacket::HeaderExtension::SSRCAudioLevel] =  atoi(it->second.c_str());
		} else if (it->first.compare("urn:ietf:params:rtp-hdrext:toffset")==0) {
			//Set extension
			extMap[RTPPacket::HeaderExtension::TimeOffset] =  atoi(it->second.c_str());
		} else if (it->first.compare("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time")==0) {
			//Set extension
			extMap[RTPPacket::HeaderExtension::AbsoluteSendTime] =  atoi(it->second.c_str());
			//Use timestamsp
			useAbsTime = true;
		} else {
			Error("Unknown RTP property [%s]\n",it->first.c_str());
		}
	}
	mutex.unlock();	
	return 1;
}

int RTPSession::SetLocalSTUNCredentials(const char* username, const char* pwd)
{
	Log("-SetLocalSTUNCredentials [frag:%s,pwd:%s]\n",username,pwd);
	//Clean mem
	if (iceLocalUsername)
		free(iceLocalUsername);
	if (iceLocalPwd)
		free(iceLocalPwd);
	//Store values
	iceLocalUsername = strdup(username);
	iceLocalPwd = strdup(pwd);
	//Ok
	return 1;
}


int RTPSession::SetRemoteSTUNCredentials(const char* username, const char* pwd)
{
	Log("-SetRemoteSTUNCredentials [frag:%s,pwd:%s]\n",username,pwd);
	//Clean mem
	if (iceRemoteUsername)
		free(iceRemoteUsername);
	if (iceRemotePwd)
		free(iceRemotePwd);
	//Store values
	iceRemoteUsername = strdup(username);
	iceRemotePwd = strdup(pwd);
	//P3 : réveille le thread Run (eventfd du Wait, jamais perdu) pour (ré)évaluer
	//l'émission de checks STUN sortants dès que la destination sera connue.
	wait.Signal();
	//Ok
	return 1;
}

int RTPSession::SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint)
{
	Log("-SetRemoteCryptoDTLS [setup:%s,hash:%s,fingerpritn:%s]\n",setup,hash,fingerprint);

	//Set Suite
	if (strcasecmp(setup,"active")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_ACTIVE);
	else if (strcasecmp(setup,"passive")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_PASSIVE);
	else if (strcasecmp(setup,"actpass")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_ACTPASS);
	else if (strcasecmp(setup,"holdconn")==0)
		dtls.SetRemoteSetup(DTLSConnection::SETUP_HOLDCONN);
	else
		return Error("Unsupported setup mode [%s]\n", setup);

	//Set fingerprint
	if (strcasecmp(hash,"SHA-1")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA1,fingerprint);
	else if (strcasecmp(hash,"SHA-256")==0)
		dtls.SetRemoteFingerprint(DTLSConnection::SHA256,fingerprint);
	else
		return Error("Unsuppoted hash type [%s]. Must me SHA-1 or SHA-256.\n", hash);

	//encript & decript
	encript = true;
	decript = true;

	//Init DTLS (génère le ClientHello dans write_bio si on est en rôle client)
	int res = dtls.Init();

	//P2 : si le pair est passive, nous sommes client -> amorcer le handshake dès
	//maintenant si la destination est déjà connue (StartSending déjà appelé), sinon
	//SetRemotePort le déclenchera. Les deux ordres d'appel sont ainsi couverts.
	RequestDTLSClientHandshake();

	return res;
}

int RTPSession::SetRemoteCryptoSDES(const char* suite, const BYTE* key, const DWORD len, int keyRank)
{
	srtp_err_status_t err;
	srtp_policy_t policy;

	Log("-Set remote RTP SDES [suite:%s keyRank=%i]\n",suite,keyRank);
	
	//empty policy
	memset(&policy, 0, sizeof(srtp_policy_t));

	if (strcmp(suite,"AES_CM_128_HMAC_SHA1_80")==0)
	{
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	} else if (strcmp(suite,"AES_CM_128_HMAC_SHA1_32")==0) {
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	} else if (strcmp(suite,"AES_CM_128_NULL_AUTH")==0) {
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtp);
		srtp_crypto_policy_set_aes_cm_128_null_auth(&policy.rtcp);
	} else if (strcmp(suite,"NULL_CIPHER_HMAC_SHA1_80")==0) {
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtp);
		srtp_crypto_policy_set_null_cipher_hmac_sha1_80(&policy.rtcp);
	} else {
		return Error("Unknown cipher suite");
	}

	//Check sizes
	if (len!=policy.rtp.cipher_key_len)
		//Error
		return Error("Key size (%d) doesn't match the selected srtp profile (required %d)\n",len,policy.rtp.cipher_key_len);

			
	//Set policy values
	policy.ssrc.type	= ssrc_any_inbound;
	policy.ssrc.value	= 0;
	policy.key		= (BYTE *) key;
	policy.next		= NULL;

	if ( keyRank == 0)
		//Create new
		err = srtp_create(&recvSRTPSession,&policy);
	else
		err = srtp_create(&recvSRTPSession_secondary,&policy);
		
	//Check error
	if (err!=srtp_err_status_ok)
		//Error
		return Error("Failed set remote SDES  (%d)\n", err);

	if ( keyRank == 0)
		//Create new
		err = srtp_create(&recvSRTPSessionRTX,&policy);
	else
		err = srtp_create(&recvSRTPSessionRTX_secondary,&policy);

	//Check error
	if (err!=srtp_err_status_ok)
		//Error
		Error("------------------------------------Failed set remote RTX SDES  (%d)\n", err);

	//Everything ok
	return 1;
}

int RTPSession::SetRemoteCryptoSDES(const char* suite, const char* key64,int keyRank)
{
	const char * sep;
	char key64copy[200];
	//Log
	Log("-SetRemoteCryptoSDES [%p] [key:%s,suite:%s]\n",this,key64,suite);

	//Decript
	decript = true;

	//Get length
	WORD len64 = strlen(key64);
	
	//Allocate memory for the key
	BYTE recvKey[len64];
	//Decode
	if ( len64 > sizeof(key64copy) )
	{
		return Error("remote SDES key too long. Len = %d.\n", len64);
	}

	sep = strchr(key64, '|');
	if (sep != NULL)
	{
	    len64 = sep - key64;
	    Log("-SetRemoteCryptoSDES found additional info. key=%.*s.\n", len64, key64);
	    strncpy( key64copy, key64, len64 );
	    key64copy[len64] = 0;    
	    Log("-SetRemoteCryptoSDES found additional info. key=%s.\n", key64copy);
	}
	else
	{
	    strcpy( key64copy, key64 );
	}

	WORD len = av_base64_decode(recvKey,key64copy,len64);
	if ( len == (WORD) -1 )
	{
	    Error("-SetRemoteCryptoSDES: fail to decode base64 DES key %s len=%d.\n", key64, len64);
	    return 0;
	}

	//Set it
	return SetRemoteCryptoSDES(suite,recvKey,len,keyRank);
}

void RTPSession::SetReceivingRTPMap(RTPMap &map)
{
	//L'ancienne map n'est pas détruite : elle DEVIENT la map de repli, le temps
	//que le pair reçoive notre réponse et bascule sur la nouvelle numérotation
	//(cf. rtpMapInPrev et CodecFromPreviousMap). Celle d'avant, elle, a fait son
	//temps. Rien de concurrent ici : l'appelant arrête la réception avant de
	//changer la map (Endpoint::StartReceiving), et c'est pour cette même course
	//qu'il le fait.
	if (rtpMapInPrev)
		delete(rtpMapInPrev);
	rtpMapInPrev = rtpMapIn;
	if (rtpMapInPrev)
		getUpdDifTime(&rtpMapInPrevSince);
	//Clone it
	rtpMapIn = new RTPMap(map);

	//Nouvelle map = nouvel épisode : ce qui a été jeté ou rattrapé sous l'ancienne
	//numérotation est clos, et le premier paquet inattendu de la nouvelle doit
	//se tracer tout de suite plutôt que d'être absorbé par le résumé en cours.
	if (unknownPtCount)
		Log("-RTP [%ls,%s,local:%d] renegotiated after dropping %u packet(s) with "
		    "payload type %d absent from the receiving map\n",
		    LabelForLog(), MediaFrame::TypeToString(media), simPort,
		    unknownPtCount, unknownPtType);
	if (salvagedCount)
		Log("-RTP [%ls,%s,local:%d] renegotiated after salvaging %u packet(s) sent "
		    "under the previous payload type %d\n",
		    LabelForLog(), MediaFrame::TypeToString(media), simPort,
		    salvagedCount, salvagedType);
	unknownPtCount = 0;
	setZeroTime(&unknownPtLast);
	salvagedCount = 0;
	setZeroTime(&salvagedLast);
}

//Le pair a-t-il basculé sur la nouvelle numérotation ? Si oui, le repli n'a plus
//de raison d'être et il est fermé SUR-LE-CHAMP : le laisser vivre les cinq
//secondes de sa borne, c'est accepter de l'ancien encore longtemps après que
//l'offre/réponse a convergé — et pendant tout ce temps, un pair qui se remettrait
//à émettre l'ancien numéro pour une autre raison (recyclage du numéro par un
//équipement intermédiaire) serait servi au lieu d'être refusé.
//
//La preuve n'est pas « un paquet valide » : c'est un payload type que SEULE la
//nouvelle map porte. Un numéro commun aux deux — PCMU 0, telephone-event 101,
//tout ce que la renégociation n'a pas touché — ne prouve rien du tout, puisque le
//pair l'émettait déjà avant. Le fermer là-dessus rouvrirait le trou exact que ce
//rattrapage vient boucher.
//
//Le désarmement seul est fait ici, pas la libération : nous sommes dans le thread
//de réception, et `rtpMapInPrev` appartient au thread de contrôle, qui le détruit
//à la renégociation suivante — réception arrêtée (Endpoint::StartReceiving).
void RTPSession::RetirePreviousMap(BYTE type)
{
	//Rien d'armé, ou numéro déjà connu de l'ancienne map : aucune preuve.
	if (!rtpMapInPrev || isZeroTime(&rtpMapInPrevSince) ||
	    rtpMapInPrev->GetCodecForType(type)!=RTPMap::NotFound)
		return;

	if (salvagedCount)
		Log("-RTP [%ls,%s,local:%d] peer switched to the new numbering (payload type "
		    "%d) after %u salvaged packet(s): the previous receiving map is retired\n",
		    LabelForLog(), MediaFrame::TypeToString(media), simPort, type, salvagedCount);

	setZeroTime(&rtpMapInPrevSince);
	salvagedCount = 0;
	setZeroTime(&salvagedLast);
}

//Le payload type sous lequel la map COURANTE porte ce codec, s'il y est encore.
BYTE RTPSession::CurrentTypeForCodec(BYTE codec) const
{
	if (!rtpMapIn)
		return RTPMap::NotFound;

	for (RTPMap::const_iterator it=rtpMapIn->begin(); it!=rtpMapIn->end(); ++it)
		if (it->second==codec)
			return it->first;

	return RTPMap::NotFound;
}

//Rattrapage de renégociation : un paquet arrive avec un payload type que la map
//courante ne porte plus, mais que la PRÉCÉDENTE portait.
//
//C'est le trou de l'offre/réponse (RFC 3264 §8) : nous appliquons la nouvelle
//numérotation dès que nous répondons, l'offreur ne bascule qu'en RECEVANT cette
//réponse, et entre les deux il émet encore sous l'ancienne. Un re-INVITE de
//Linphone qui renumérote ses payload types dynamiques ouvrait ainsi une fenêtre
//de plusieurs centaines de millisecondes où tout était jeté — inaudible en audio,
//VISIBLE en vidéo : le décodeur perd tout ce qui suit la dernière intra reçue, et
//l'image reste figée jusqu'à la suivante.
//
//La garde est le codec, pas le payload type : le paquet n'est accepté que si son
//ancien codec est TOUJOURS porté par la map courante (sous un autre numéro). Ce
//qui est réparé est alors une renumérotation, rien d'autre — le flux qui traverse
//est celui que la négociation en cours autorise, et l'aval le lit par
//`packet->SetCodec()`, pas par le numéro resté dans l'en-tête. Un codec réellement
//retiré de la négociation, lui, reste un rebut : le laisser passer ferait décoder
//au pair d'en face un format qu'il vient de refuser.
BYTE RTPSession::CodecFromPreviousMap(BYTE type)
{
	//Aucune renégociation, ou repli périmé
	if (!rtpMapInPrev || isZeroTime(&rtpMapInPrevSince) ||
	    (getDifTime(&rtpMapInPrevSince)/1000) > RTP_MAP_FALLBACK_MS)
		return RTPMap::NotFound;

	BYTE codec = rtpMapInPrev->GetCodecForType(type);
	if (codec==RTPMap::NotFound)
		return RTPMap::NotFound;

	//Le codec doit toujours être négocié, sous un numéro ou un autre
	BYTE current = CurrentTypeForCodec(codec);
	if (current==RTPMap::NotFound)
		return RTPMap::NotFound;

	//Trace agrégée : le premier paquet rattrapé de l'épisode, puis un compte
	bool first = (salvagedCount==0 || type!=salvagedType || isZeroTime(&salvagedLast));

	if (!first && (getDifTime(&salvagedLast)/1000) < UNKNOWN_PT_LOG_PERIOD_MS)
	{
		salvagedCount++;
		return codec;
	}

	if (first)
	{
		salvagedType = type;
		salvagedCount = 1;

		Log("-RTP [%ls,%s,local:%d] salvaging packets still sent with the previous "
		    "payload type %d for %s (renumbered to %d by the renegotiation): the peer "
		    "has not seen our answer yet\n",
		    LabelForLog(), MediaFrame::TypeToString(media), simPort,
		    type, GetNameForCodec(media,codec), current);
	}
	else
	{
		salvagedCount++;

		Log("-RTP [%ls,%s,local:%d] still salvaging the previous payload type %d "
		    "(%s) — %u since the first one\n",
		    LabelForLog(), MediaFrame::TypeToString(media), simPort,
		    type, GetNameForCodec(media,codec), salvagedCount);
	}

	getUpdDifTime(&salvagedLast);
	return codec;
}

//Un paquet reçu dont le payload type n'est pas dans la map de réception.
//
//Ce n'est pas une anomalie du serveur : pendant une renégociation, l'offreur
//continue d'émettre avec l'ANCIENNE numérotation jusqu'à ce qu'il reçoive la
//réponse (RFC 3264 §8) — un re-INVITE de Linphone qui déplace OPUS de 96 à 98
//produit ainsi quelques centaines de millisecondes de paquets indécodables. Le
//paquet est jeté (nous ne savons pas quel codec il porte), et la trace dit de
//QUELLE patte, de QUEL média et de QUELLE source elle parle — la question à
//laquelle l'ancienne ligne "-RTP packet type unknown [96]", répétée deux cents
//fois sans un mot de contexte, ne répondait pas.
int RTPSession::OnUnknownPayloadType(BYTE type, DWORD ssrc, const sockaddr_in& from)
{
	//Une trace par épisode, puis un résumé compté. Un changement de payload type
	//rouvre un épisode : c'est une autre cause, pas la suite de la même.
	bool first = (unknownPtCount==0 || type!=unknownPtType || isZeroTime(&unknownPtLast));

	if (!first && (getDifTime(&unknownPtLast)/1000) < UNKNOWN_PT_LOG_PERIOD_MS)
	{
		//Même épisode, trop tôt pour reparler : compter et se taire
		unknownPtCount++;
		return 0;
	}

	if (first)
	{
		unknownPtType = type;
		unknownPtCount = 1;

		Error("-RTP received a packet whose payload type is not negotiated "
		      "[pt:%d,ssrc:%x,media:%s,role:%d,label:%ls,local port:%d,from:%s:%d] "
		      "— dropped (normal while a renegotiation is in flight)\n",
		      type, ssrc, MediaFrame::TypeToString(media), role, LabelForLog(), simPort,
		      inet_ntoa(from.sin_addr), ntohs(from.sin_port));
	}
	else
	{
		unknownPtCount++;

		Error("-RTP [%ls,%s,local:%d] still dropping packets with the unnegotiated "
		      "payload type %d [ssrc:%x] — %u since the first one\n",
		      LabelForLog(), MediaFrame::TypeToString(media), simPort,
		      type, ssrc, unknownPtCount);
	}

	getUpdDifTime(&unknownPtLast);
	return 0;
}

int RTPSession::SetLocalPort(int recvPort)
{
	//Override
	simPort = recvPort;
	return 0;
}

int RTPSession::GetLocalPort()
{
	// Return local
	return simPort;
}

int RTPSession::GetRemotePort()
{
	// Return local
	return sendAddr.sin_port;
}

bool RTPSession::SetSendingCodec(DWORD codec)
{
	//Check rtp map
	if (!rtpMapOut)
		//Error
		return Error("-SetSendingCodec error: no out RTP map\n");

	//Try to find it in the map
	for (RTPMap::iterator it = rtpMapOut->begin(); it!=rtpMapOut->end(); ++it)
	{
		//Is it ourr codec
		if (it->second==codec)
		{
			//Get type
			DWORD type = it->first;
			//Log it
			Log("-SetSendingCodec [codec:%s,type:%d]\n",GetNameForCodec(media,codec),type);
			//Set type in header
			((rtp_hdr_t *)sendPacket)->pt = type;
			//Set type
			sendType = type;
			//and we are done
			return true;
		}
	}

	//Not found
	return Error("-SetSendingCodec error: codec mapping not found [codec:%s]\n",GetNameForCodec(media,codec));
}

/***********************************
* IsRFC1918
*	Adresse non routable sur l'Internet public : le pair qui l'annonce dans son
*	SDP est derrière un NAT (ou nous ment), son média ne peut pas nous parvenir
*	de là. Seule une telle annonce ouvre droit au rattrapage (voir SendPacket) :
*	sur une adresse publique, une divergence est plus probablement du routage
*	asymétrique légitime qu'un NAT à corriger.
***********************************/
bool RTPSession::IsRFC1918(in_addr_t addr)
{
	//addr est en ordre réseau
	DWORD ip = ntohl(addr);

	//10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 (RFC 1918)
	if ((ip & 0xFF000000) == 0x0A000000) return true;
	if ((ip & 0xFFF00000) == 0xAC100000) return true;
	if ((ip & 0xFFFF0000) == 0xC0A80000) return true;
	//100.64.0.0/10 : NAT opérateur (RFC 6598), même symptôme
	if ((ip & 0xFFC00000) == 0x64400000) return true;
	//169.254.0.0/16 : link-local (RFC 3927), jamais routable jusqu'à nous
	if ((ip & 0xFFFF0000) == 0xA9FE0000) return true;

	return false;
}

/***********************************
* NatCorrectable
*	Règle commune aux rattrapages RTP et RTCP : le contrôleur l'autorise, ICE
*	n'est pas en jeu, et l'adresse annoncée est privée. La *preuve* (un paquet
*	réellement reçu d'ailleurs) est vérifiée par chaque appelant.
***********************************/
bool RTPSession::NatCorrectable(in_addr_t announced)
{
	//Le contrôleur n'a pas activé le rattrapage sur cette jambe
	if (!natLatch)
		return false;

	//ICE possède déjà la cible d'envoi (OnICEConnectivityConfirmed la pose lui-même
	//sur le pair validé) : ne pas la lui disputer.
	if (iceRemotePwd || iceLocalPwd)
		return false;

	return IsRFC1918(announced);
}

/***********************************
* SetRemotePort
*	Inicia la sesion rtp de video remota
***********************************/
int RTPSession::SetRemotePort(char *ip,int sendPort)
{
	struct in_addr remote;

	//Une adresse illisible ne doit PAS devenir une destination. inet_addr rendait
	//INADDR_NONE aussi bien sur une erreur de format que sur l'adresse de diffusion,
	//et ce retour n'était pas testé : la destination devenait 255.255.255.255 et le
	//mediaserver émettait le flux en BROADCAST sur le LAN, sans un mot dans le log.
	//inet_pton distingue les deux cas ; tous les appelants traitent déjà 0 en erreur.
	if (!ip || inet_pton(AF_INET,ip,&remote)!=1)
		return Error("-SetRemotePort: adresse invalide [%s]\n",ip?ip:"(null)");

	DWORD ipAddr = remote.s_addr;

	//Un contrôleur qui passe 0.0.0.0 demande explicitement le latch : il ne connaît
	//pas la vraie adresse du pair et s'en remet à la source réellement observée.
	//Vaut autorisation, au même titre que la propriété RTP "natLatch".
	if (ipAddr==INADDR_ANY)
		natLatch = true;

	//If we already have one and it is a NATed
	if (recIP!=INADDR_ANY && ipAddr==INADDR_ANY)
	{
		//Exit
		Log("-SetRemotePort NAT already bound to [%s:%d]\n",inet_ntoa(sendAddr.sin_addr),recPort);

		return 1;
	}
	//Ok, let's et it
	Log("-SetRemotePort [%s:%d]\n",ip,sendPort);

	//Nouvelle cible posée par le plan de contrôle (re-INVITE, UPDATE…) : une
	//correction précédente porte sur l'ancienne, elle est caduque. On rouvre le droit
	//au rattrapage, sinon un pair qui change de mapping resterait coincé sur l'ancien.
	natCorrected = false;
	natRtcpCorrected = false;

	//Ip y puerto de destino
	sendAddr.sin_addr.s_addr 	= ipAddr;
	sendRtcpAddr.sin_addr.s_addr 	= ipAddr;
	sendAddr.sin_port 		= htons(sendPort);
	
	//Check if doing rtcp muxing
	if (muxRTCP)
		//Same than rtp
		sendRtcpAddr.sin_port 	= htons(sendPort);
	else
		//One more than rtp
		sendRtcpAddr.sin_port 	= htons(sendPort+1);

	//Amorçage du chemin d'envoi (ouverture NAT). En chiffré (DTLS/SRTP) on garde
	//l'ouverture minimale historique : le handshake DTLS et les checks STUN prennent
	//le relais. En clair (P6), on émet une rafale de paquets RTP valides pour faire
	//latcher un pair symétrique (comedia) — le simple rtpEmpty de 8 octets ne suffit
	//pas à un stack RTP strict.
	if (encript)
		sendto(simSocket,rtpEmpty,sizeof(rtpEmpty),0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));
	else
		ArmNATPriming();

	//P2 : la destination d'envoi est désormais connue -> si on est client DTLS
	//(SetRemoteCryptoDTLS déjà appelé avec setup=passive), émettre le ClientHello.
	RequestDTLSClientHandshake();

	//Y abrimos los sockets
	return 1;
}

void RTPSession::ArmRTPTimeout(DWORD timeoutMs)
{
	//Armement du watchdog d'inactivité RTP (gap 5), piloté par le contrôleur au
	//moment du SDP answer. Le chrono part de MAINTENANT : la sonnerie (avant answer)
	//n'est jamais surveillée, et « répondu mais aucun média reçu » est détecté après
	//timeoutMs sans paquet.
	if (timeoutMs > 0)
	{
		mutex.lock();
		rtpTimeout      = timeoutMs;
		rtpTimeoutArmed = true;
		rtpTimedOut     = false;
		gettimeofday(&lastRecv,NULL);   //démarre le chrono
		mutex.unlock();
		Log("-ArmRTPTimeout: watchdog armé à %u ms [%p]\n",timeoutMs,this);

		//Si le thread dort dans poll(-1) (watchdog jusqu'ici désarmé), on le réveille
		//via l'eventfd pour qu'il reprenne l'attente bornée sans attendre un paquet.
		wait.Signal();
	}
	else
	{
		//timeoutMs == 0 : désarme (p.ex. mise en attente / sendonly légitime)
		mutex.lock();
		rtpTimeoutArmed = false;
		mutex.unlock();
		Log("-ArmRTPTimeout: watchdog désarmé [%p]\n",this);
	}
}

//Borne globale de sécurité du handshake DTLS client (ms) : au-delà, on bascule
//sur le chemin d'erreur transport (onRTPTimeout -> EndpointDisconnectedEvent).
//OpenSSL abandonne en général avant (HandleTimeout renvoie -1), c'est un filet.
#define DTLS_CLIENT_HANDSHAKE_TIMEOUT 30000

void RTPSession::FlushDTLS()
{
	//Rien à émettre tant que la destination n'est pas connue (StartSending /
	//candidat ICE / latch STUN n'ont pas encore fixé sendAddr).
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		return;

	BYTE buffer[MTU];
	int len;
	//Vide toutes les données DTLS en attente dans write_bio (ClientHello puis
	//flights suivants) vers la cible d'envoi, indépendamment du STUN entrant.
	while ((len = dtls.Read(buffer,MTU)) > 0)
		sendto(simSocket,buffer,len,0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));
}

void RTPSession::RequestDTLSClientHandshake()
{
	//No-op si on n'est pas en rôle client DTLS (rôle serveur/passive, SDES, ou DTLS
	//pas encore initialisé) : aucune régression pour setup=active / navigateurs.
	if (!dtls.IsInited() || !dtls.IsClientRole())
		return;

	//Réveille le thread Run (eventfd) pour qu'il pilote le handshake sans attendre
	//un paquet entrant (le pair ICE-lite/passive n'en enverra pas).
	wait.Signal();
}

void RTPSession::DriveDTLSClientHandshake()
{
	//Rôle client uniquement, DTLS prêt, handshake pas terminé, destination connue.
	if (!dtls.IsInited() || !dtls.IsClientRole() || dtls.IsHandshakeCompleted())
		return;
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		return;

	//Première émission : le ClientHello généré par dtls.Init() attend dans write_bio.
	if (!dtlsClientStarted)
	{
		Log("-RTPSession DTLS client: émission du ClientHello vers [%s:%d] [%p]\n",
			inet_ntoa(sendAddr.sin_addr),ntohs(sendAddr.sin_port),this);
		dtlsClientStarted = true;
		dtlsClientFailed  = false;
		gettimeofday(&dtlsClientStart,NULL);
		FlushDTLS();
		return;
	}

	//Retransmissions pilotées par OpenSSL (backoff exponentiel DTLS).
	int r = dtls.HandleTimeout();
	if (r>0)
		//Un flight a été re-mis en file d'attente : on le pousse sur le fil.
		FlushDTLS();
	else if (r<0 && !dtlsClientFailed)
	{
		Error("-RTPSession DTLS client: échec du handshake (trop de retransmissions) [%p]\n",this);
		dtlsClientFailed = true;
		if (auto l = LockListener())
			l->onRTPTimeout(this);
		return;
	}

	//Filet de sécurité : borne globale même si OpenSSL ne rend pas -1.
	if (!dtlsClientFailed && (getDifTime(&dtlsClientStart)/1000) > DTLS_CLIENT_HANDSHAKE_TIMEOUT)
	{
		Error("-RTPSession DTLS client: timeout global du handshake (%d ms) [%p]\n",
			DTLS_CLIENT_HANDSHAKE_TIMEOUT,this);
		dtlsClientFailed = true;
		if (auto l = LockListener())
			l->onRTPTimeout(this);
	}
}

//P3 : bornes de retransmission des binding requests STUN sortants (backoff)
#define ICE_CHECK_MIN_RTO 250   //ms : premier intervalle de retransmission
#define ICE_CHECK_MAX_RTO 1500  //ms : intervalle maximal (doublement borné)

void RTPSession::SendICEBindingRequest()
{
	//Il faut les credentials (locales+distantes) et une destination connue.
	if (!iceLocalUsername || !iceRemoteUsername || !iceRemotePwd)
		return;
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		return;

	//Transaction id (0 | timestamp), mémorisé pour corréler la réponse.
	set4(iceCheckTransId,0,0);
	set8(iceCheckTransId,4,getTime());

	//USERNAME = "remoteUfrag:localUfrag" : AddUsernameAttribute(local,remote) produit
	//bien "remote:local". MESSAGE-INTEGRITY clé = mot de passe DISTANT. Rôle
	//CONTROLLING + USE-CANDIDATE : nomination agressive, acceptable pour un pair lite.
	STUNMessage *request = new STUNMessage(STUNMessage::Request,STUNMessage::Binding,iceCheckTransId);
	request->AddUsernameAttribute(iceLocalUsername,iceRemoteUsername);
	request->AddAttribute(STUNMessage::Attribute::IceControlling,(QWORD)-1);
	request->AddAttribute(STUNMessage::Attribute::UseCandidate);
	request->AddAttribute(STUNMessage::Attribute::Priority,(DWORD)33554431);

	DWORD size = request->GetSize();
	BYTE* aux = (BYTE*)malloc(size);
	DWORD len = request->AuthenticatedFingerPrint(aux,size,iceRemotePwd);
	if (len)
	{
		Debug("ICE: envoi Binding Request sortant (user=%s) vers %s:%d [%p]\n",
			iceRemoteUsername, inet_ntoa(sendAddr.sin_addr), ntohs(sendAddr.sin_port), this);
		sendto(simSocket,aux,len,0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));
	}
	free(aux);
	delete(request);
}

void RTPSession::DriveICEChecks()
{
	//Actif uniquement si : creds locales+distantes connues, destination connue,
	//connectivité pas encore confirmée. Constant face à un pair ICE-lite (qui n'initie
	//jamais) ; inoffensif face à un pair full (il répond -> iceConnected -> on s'arrête).
	if (iceConnected)
		return;
	if (!iceLocalUsername || !iceRemoteUsername || !iceRemotePwd)
		return;
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		return;

	//Première émission immédiate.
	if (!iceCheckStarted)
	{
		iceCheckStarted = true;
		iceCheckRto     = ICE_CHECK_MIN_RTO;
		SendICEBindingRequest();
		gettimeofday(&iceLastCheck,NULL);
		return;
	}

	//Retransmission avec backoff exponentiel borné jusqu'à réception d'une réponse.
	if ((getDifTime(&iceLastCheck)/1000) >= iceCheckRto)
	{
		SendICEBindingRequest();
		gettimeofday(&iceLastCheck,NULL);
		if (iceCheckRto < ICE_CHECK_MAX_RTO)
			iceCheckRto *= 2;
	}
}

void RTPSession::OnICEConnectivityConfirmed(sockaddr_in* from)
{
	//Latch symétrique de l'adresse distante si pas encore fixée.
	if (recIP == INADDR_ANY)
	{
		recIP   = from->sin_addr.s_addr;
		recPort = ntohs(from->sin_port);
	}
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
	{
		sendAddr.sin_addr.s_addr = from->sin_addr.s_addr;
		sendAddr.sin_port        = from->sin_port;
	}

	if (!iceConnected)
	{
		Log("-RTPSession ICE: connectivité confirmée avec [%s:%d] [%p]\n",
			inet_ntoa(from->sin_addr), ntohs(from->sin_port), this);
		//Connectivité validée : on cesse d'émettre des checks sortants. Le handshake
		//DTLS client (P2) et le média ne sont pas gatés par l'ICE dans cette
		//implémentation (cf. audit P1) : ils sont pilotés indépendamment par la boucle
		//Run. Marquer iceConnected débloque donc simplement l'arrêt des checks.
		iceConnected = true;
	}
}

int RTPSession::AddICECandidate(const char* candidate)
{
	//Trickle ICE Niveau 1 pragmatique (gap 1) : on parse la ligne SDP "candidate:"
	//et, pour un candidat host/srflx dont la priorité dépasse la meilleure connue,
	//on bascule la cible d'envoi RTP/RTCP. Combiné à l'apprentissage d'adresse par
	//STUN entrant déjà présent, cela couvre le cas « un candidat gagnant arrive
	//après le SDP initial » sans agent ICE complet (Niveau 2 hors périmètre).
	if (!candidate)
		return Error("-AddICECandidate: candidat nul\n");

	//Saute le préfixe optionnel "candidate:"
	const char* p = candidate;
	if (strncmp(p,"candidate:",10)==0)
		p += 10;

	//candidate:<fnd> <cmp> <transport> <prio> <addr> <port> typ <type> ...
	char foundation[64], transport[16], address[128], typ[8], candType[32];
	unsigned int priority = 0;
	int component = 0, port = 0;
	int n = sscanf(p,"%63s %d %15s %u %127s %d %7s %31s",
		foundation,&component,transport,&priority,address,&port,typ,candType);
	if (n < 8)
		return Error("-AddICECandidate: format non reconnu [%s]\n",candidate);

	//On ne pilote la cible d'envoi que sur la composante RTP (1) en UDP
	if (component != 1 || strcasecmp(transport,"UDP")!=0)
	{
		Log("-AddICECandidate: ignoré (component=%d transport=%s)\n",component,transport);
		return 1;
	}

	//Niveau 1 : seuls les candidats host/srflx sont considérés
	if (strcasecmp(candType,"host")!=0 && strcasecmp(candType,"srflx")!=0)
	{
		Log("-AddICECandidate: type [%s] ignoré (Niveau 1)\n",candType);
		return 1;
	}

	//Ne reconfigure que si la priorité dépasse la meilleure déjà retenue
	if (iceRemotePriority != 0 && priority <= iceRemotePriority)
	{
		Log("-AddICECandidate: priorité %u <= courante %u, ignoré\n",priority,iceRemotePriority);
		return 1;
	}

	//Résout l'adresse
	DWORD ipAddr = inet_addr(address);
	if (ipAddr == INADDR_NONE)
		return Error("-AddICECandidate: adresse invalide [%s]\n",address);

	Log("-AddICECandidate: bascule cible d'envoi vers [%s:%d] typ %s prio %u\n",address,port,candType,priority);

	//Reconfigure la cible d'envoi RTP/RTCP
	mutex.lock();
	sendAddr.sin_addr.s_addr     = ipAddr;
	sendRtcpAddr.sin_addr.s_addr = ipAddr;
	sendAddr.sin_port            = htons(port);
	sendRtcpAddr.sin_port        = htons(muxRTCP ? port : port+1);
	iceRemotePriority            = priority;
	mutex.unlock();

	//Amorce le chemin (ouverture NAT ; le STUN entrant confirmera la connectivité)
	sendto(simSocket,rtpEmpty,sizeof(rtpEmpty),0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));

	//P2 : un candidat gagnant fournit une destination -> amorcer le ClientHello si
	//on est client DTLS et que StartSending n'a pas encore fixé l'adresse.
	RequestDTLSClientHandshake();

	return 1;
}

int RTPSession::SendEmptyPacket()
{
	//Check if we have sendinf ip address
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		//Exit
		return 0;

	//Open rtp and rtcp ports
	sendto(simSocket,rtpEmpty,sizeof(rtpEmpty),0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));

	//ok
	return 1;
}

//P6 : construit et émet UN paquet RTP valide (en-tête 12 octets, payload vide) vers
//la destination, à des fins d'amorçage NAT symétrique. Un en-tête RTPv2 complet (avec
//SSRC) suffit à faire latcher un pair comedia, contrairement au rtpEmpty tronqué.
//Décrémente le compteur de rafale. À n'utiliser qu'en clair (voir ArmNATPriming).
int RTPSession::SendNATPrimingPacket()
{
	//Destination inconnue : rien à émettre
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		return 0;

	//En-tête RTP minimal mais valide (V=2, P=0, X=0, CC=0, M=0). PT = type d'envoi
	//négocié si connu, sinon PCMU(0) : le latch comedia ne dépend pas du PT.
	BYTE packet[12];
	int pt = (sendType >= 0) ? sendType : 0;
	packet[0] = 0x80;
	packet[1] = (BYTE)(pt & 0x7f);
	set2(packet,2,sendSeq++);
	set4(packet,4,sendTime);
	set4(packet,8,sendSSRC);

	sendto(simSocket,packet,sizeof(packet),0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));

	if (natPrimingLeft > 0)
		natPrimingLeft--;

	return 1;
}

//P6 : (ré)arme une rafale d'amorçage NAT. Émet immédiatement le premier paquet pour
//ouvrir le pinhole au plus tôt ; le thread Run cadence les suivants (~20 ms). No-op
//en chiffré (le WebRTC amorce via STUN/DTLS) ou tant que la destination est inconnue.
void RTPSession::ArmNATPriming()
{
	if (encript)
		return;
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
		return;

	natPrimingLeft = NAT_PRIMING_BURST;
	//Premier paquet tout de suite (SendNATPrimingPacket décrémente le compteur)
	SendNATPrimingPacket();
	gettimeofday(&natPrimingLast,NULL);

	//Réveille le thread Run (s'il tourne) pour qu'il cadence la suite de la rafale
	wait.Signal();
}

void RTPSession::SetRemoteRateEstimator(RemoteRateEstimator* estimator)
{
	Log("-SetRemoteRateEstimator\n");

	//Store it
	remoteRateEstimator = estimator;

	//Add as listener
	remoteRateEstimator->SetListener(this);
}

/********************************
* Init
*	Inicia el control rtcp
********************************/
int RTPSession::Init()
{
	int retries = 0;
	simPort=0;	
	Log(">Init RTPSession %p for media %s and role %s\n", this, MediaFrame::TypeToString(media), MediaFrame::RoleToString(role));

	sockaddr_in recAddr;

	//Clear addr
	memset(&recAddr,0,sizeof(struct sockaddr_in));

	//Set family
	recAddr.sin_family     	= AF_INET;
	
	//Get two consecutive ramdom ports
	while (retries++<100)
	{
		//If we have a rtp socket
		if (simSocket!=FD_INVALID)
		{
			// Close first socket
			close(simSocket);
			//No socket
			simSocket = FD_INVALID;
		}
		//If we have a rtcp socket
		if (simRtcpSocket!=FD_INVALID)
		{
			///Close it
			close(simRtcpSocket);
			//No socket
			simRtcpSocket = FD_INVALID;
		}

		//Create new sockets
		simSocket = socket(PF_INET,SOCK_DGRAM,0);
		//If not forced to any port
		if (!simPort)
		{
			//Get random
			simPort = (int) (RTPSession::GetMinPort()+(RTPSession::GetMaxPort()-RTPSession::GetMinPort())*double(rand()/double(RAND_MAX)));
			//Make even
			simPort &= 0xFFFFFFFE;
		}
		//Try to bind to port
		recAddr.sin_port = htons(simPort);
		//Bind the rtcp socket
		if(bind(simSocket,(struct sockaddr *)&recAddr,sizeof(struct sockaddr_in))!=0)
		{
			Log("Failed to open RTP port %d : %s.\n", simPort, strerror(errno) );
			simPort=0;
			//Try again
			continue;
		}
		//Create new sockets
		simRtcpSocket = socket(PF_INET,SOCK_DGRAM,0);
		//Next port
		simRtcpPort = simPort+1;
		//Try to bind to port
		recAddr.sin_port = htons(simRtcpPort);
		//Bind the rtcp socket
		if(bind(simRtcpSocket,(struct sockaddr *)&recAddr,sizeof(struct sockaddr_in))!=0)
		{
			//Use random
			simPort = 0;
			//Try again
			Log("Failed to open RTCP port %d : %s.\n", simRtcpPort, strerror(errno) );
			continue;
		}
		//Set COS
		int cos = 5;
		setsockopt(simSocket,     SOL_SOCKET, SO_PRIORITY, &cos, sizeof(cos));
		setsockopt(simRtcpSocket, SOL_SOCKET, SO_PRIORITY, &cos, sizeof(cos));
		//Set TOS
		int tos = 0x2E;
		setsockopt(simSocket,     IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
		setsockopt(simRtcpSocket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
		//Everything ok
		Log("-Got ports [%d,%d]\n",simPort,simRtcpPort);
		//Start receiving
		Start();
		//Done
		Log("<Init RTPSession\n");
		//Opened
		return 1;
	}
	//Error
	Error("RTPSession too many failed attemps opening sockets");
	
	//Failed
	return 0;
}

/*********************************
* End
*	Termina la todo
*********************************/
int RTPSession::End()
{
	//Check if not running
	if (!running)
		//Nothing
		return 0;
		
	//Stop just in case
	Stop();

	//Not running;
	running = false;
	//If got socket
	if (simSocket!=FD_INVALID)
	{
		//Will cause poll to return
		close(simSocket);
		//No sockets
		simSocket = FD_INVALID;
	}
	if (simRtcpSocket!=FD_INVALID)
	{
		//Will cause poll to return
		close(simRtcpSocket);
		//No sockets
		simRtcpSocket = FD_INVALID;
	}
	
	return 1;
}

int RTPSession::SendPacket(RTCPCompoundPacket &rtcp)
{
	BYTE data[MTU+SRTP_MAX_TRAILER_LEN] ZEROALIGNEDTO32;
	DWORD size = RTPPAYLOADSIZE;
	int ret = 0;

	//Check if we have sendinf ip address
	if (sendRtcpAddr.sin_addr.s_addr == INADDR_ANY && !muxRTCP)
	{
		//Debug
		Debug("-Error sending rtcp packet, no remote IP yet\n");
		//Exit

		return 0;
	}
	//Serialize
	int len = rtcp.Serialize(data,size);
	//Check result
	if (!len)
		//Error
		return Error("Error serializing RTCP packet\n");

	//If encripted
	if (encript)
	{
		//Check  session
		if (!sendSRTPSession)
			return Error("-no sendSRTPSession\n");
		//Protect
		srtp_err_status_t err = srtp_protect_rtcp(sendSRTPSession,data,&len);
		//Check error
		if (err!=srtp_err_status_ok)
			//Nothing
			return Error("Error protecting RTCP packet [%d]\n",err);
	}

	//If muxin
	if (muxRTCP)
		//Send using RTP port
		ret = sendto(simSocket,data,len,0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));
	else
		//Send using RCTP port
		ret = sendto(simRtcpSocket,data,len,0,(sockaddr *)&sendRtcpAddr,sizeof(struct sockaddr_in));

	//Check error
	if (ret<0)
	{
		int err = errno;

		Error("-Error sending RTP packet [%d]\n",err);
		//Return
		return 0;
	}

	//Exit
	return 1;
}

int RTPSession::SendPacket(RTPPacket &packet)
{
	return SendPacket(packet,packet.GetTimestamp());
}

int RTPSession::SendPacket(RTPPacket &packet,DWORD timestamp)
{
	//If session is not running drop the packet silently
	if (!running) return 0;
	
	//Check if we have sendinf ip address
	if (sendAddr.sin_addr.s_addr == INADDR_ANY)
	{
		//Do we have rec ip?
		if (recIP!=INADDR_ANY )
		{
			//Do NAT
			sendAddr.sin_addr.s_addr = recIP;
			//Set port
			sendAddr.sin_port = htons(recPort);
			//La cible est posée : plus de rattrapage ultérieur (voir plus bas)
			natCorrected = true;
			//Log
			Log("-RTPSession NAT: Now sending %s to [%s:%d].\n", MediaFrame::TypeToString(media),inet_ntoa(sendAddr.sin_addr), recPort);
			//Check if using ice
		}
		else {
			//Exit
			Debug("-No remote address for [%s]\n",MediaFrame::TypeToString(media));
			//Exit
			return 0;
		}
	}
	else if ( recIP != INADDR_ANY && recIP != sendAddr.sin_addr.s_addr )
        {
		//Le pair a annoncé une adresse privée dans son SDP mais son RTP nous arrive
		//d'ailleurs : c'est le mapping d'un NAT symétrique, qui a réécrit l'adresse ET
		//le port. Émettre vers l'annonce ne mènerait nulle part — on ré-aiguille sur la
		//source réellement observée, seule preuve dont nous disposons.
		//One-shot : recIP est recalé sur *chaque* paquet de source différente (voir
		//ReadRTP), sans ce garde la cible battrait au gré du moindre paquet égaré.
		if (!natCorrected && NatCorrectable(sendAddr.sin_addr.s_addr))
		{
			sendAddr.sin_addr.s_addr = recIP;
			sendAddr.sin_port = htons(recPort);
			natCorrected = true;
			Log("-RTPSession NAT: %s now sending to [%s:%d] (annonce privee corrigee sur la source reelle).\n",
			    MediaFrame::TypeToString(media),inet_ntoa(sendAddr.sin_addr),recPort);
		}
		else
			Log("-RTPSession NAT: WARNING Trying to send packet from different ip address than receiving one.\n");
	}
	
	//Check if we need to send SR
	if (isZeroTime(&lastSR) || getDifTime(&lastSR)>4000000)
		//Send it
		SendSenderReport();
	
	//Modificamos las cabeceras del packete
	rtp_hdr_t *headers = (rtp_hdr_t *)sendPacket;
	
	// if the codec of the packet we want to send is not the same than defined, we changed it

	if (rtpMapOut->find(sendType) == rtpMapOut->end() || (*rtpMapOut)[sendType] != packet.GetCodec())
	{
		this->SetSendingCodec( packet.GetCodec());
	} 

	//Init send packet
	headers->version = RTP_VERSION;
	
	//if we detect a change of ssrc in packet, we change the ssrc
	if (lastSendSSRC != 0 && lastSendSSRC != packet.GetSSRC())
	{
		Debug("Changing sending SSRC - lastRecSSRC=%x, packet ssrc=%x \n",lastSendSSRC,packet.GetSSRC());
		sendSSRC = random();	
	}
	
	headers->ssrc = htonl(sendSSRC);
	lastSendSSRC = packet.GetSSRC();
	
/* 
	//Simulate SSRC change - for test purporse only
    if ( (sendSeq%50) == 0
	   || (sendSeq%50) == 1
	   || (sendSeq%50) == 4 )
	{
		headers->ssrc = random();
	}
*/
	// in case of bridging, we don't change the timestamp of the packet.
	if ( useOriTS && this->media != MediaFrame::Text)
	{ 
		sendLastTime = packet.GetTimestamp();
	}
	else
	{
		//Calculate last timestamp
		sendLastTime = sendTime + timestamp;

	}
	headers->ts = htonl( sendLastTime );

	//Incrementamos el numero de secuencia
	// in case of bridging , we don't change the seq num of the packet.
	if ( useOriSeqNum && this->media != MediaFrame::Text)
	{
		sendSeq = packet.GetSeqNum();
		headers->seq = htons( sendSeq );
	}
	else
	{
		headers->seq = htons( sendSeq );
		sendSeq++;
	}

	//Check seq wrap
	if( sendSeq == 0 )
	{
		//Inc cycles
		sendCycles++;
	}

	//La marca de fin de frame
	headers->m=packet.GetMark();

	//Calculamos el inicio
	int ini = sizeof(rtp_hdr_t);
	
	//If we have are using any sending extensions
	if (useAbsTime)
	{
		//Get header
		rtp_hdr_ext_t* ext = (rtp_hdr_ext_t*)(sendPacket + ini);
		//Set extension header
		headers->x = 1;
		//Set magic cookie
		ext->ext_type = htons(0xBEDE);
		//Set total length in 32bits words
		ext->len = htons(1);
		//Increase ini
		ini += sizeof(rtp_hdr_ext_t);
		//Calculate absolute send time field (convert ms to 24-bit unsigned with 18 bit fractional part.
		// Encoding: Timestamp is in seconds, 24 bit 6.18 fixed point, yielding 64s wraparound and 3.8us resolution (one increment for each 477 bytes going out on a 1Gbps interface).
		DWORD abs = ((getTimeMS() << 18) / 1000) & 0x00ffffff;
		//Set header
		sendPacket[ini] = extMap.GetCodecForType(RTPPacket::HeaderExtension::AbsoluteSendTime) << 4 | 0x02;
		//Set data
		set3(sendPacket,ini+1,abs);
		//Increase ini
		ini+=4;
	}

	//Comprobamos que quepan
	if (ini+packet.GetMediaLength()>MTU)
		return Error("SendPacket Overflow [size:%d,max:%d]\n",ini+packet.GetMediaLength(),MTU);

	//Copiamos los datos
        memcpy(sendPacket+ini,packet.GetMediaData(),packet.GetMediaLength());

	//Set pateckt length
	int len = packet.GetMediaLength()+ini;

	//Check if we ar encripted
	if (encript)
	{
		if (!sendSRTPSession)
		{
			Debug("-RTPSession: encryption is not yet setup.\n");
			return 0;
		}
		srtp_err_status_t err;
		
		//Encript
		err = srtp_protect(sendSRTPSession,sendPacket,&len);
		//Check error
		if (err!=srtp_err_status_ok)
		{
			//Nothing
			Error("Error protecting RTP packet for %s with recSSRC=%x  and for session=%p : [%d]\n",MediaFrame::TypeToString(media),packet.GetSSRC(),this, err);
		
			return -1;
		}
	}
	
	//Add it rtx queue
	if (useNACK)
	{
			//Create new pacekt
			RTPTimedPacket *rtx = new RTPTimedPacket(media,sendPacket,len);

			rtxUse.WaitUnusedAndLock();

			//Set cycles
			rtx->SetSeqCycles(sendCycles);
			//Add it to que
			rtxs[rtx->GetExtSeqNum()] = rtx;

			RTPOrderedPackets::iterator it = rtxs.begin();
			if ( it != rtxs.end() )
			{
					RTPTimedPacket *pkt = it->second;
					if ( rtxs.size() > 200)
					{
							rtxs.erase(it++);
							delete pkt;
					}
			}
			//Unlock
			rtxUse.Unlock();
	}

	//SIMULATING PACKET LOST 10%	
    /*
	int ret =0;
    if (this->media == MediaFrame::Video  && (sendSeq%10) == 0)
    {
		ret =0;
	}
	else
	{
		//Send packet
		ret = sendto(simSocket,sendPacket,len,0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));
	}
	*/	
	
	//Send packet
	int ret = sendto(simSocket,sendPacket,len,0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));

    if (ret <= 0)
	{
		Log("Failed to send RTP packet seqnum %d, errno=%d.\n", sendSeq, errno);
	}
        else
	{
		//Inc stats
		numSendPackets++;
		totalSendBytes += packet.GetMediaLength();
		
	}

	//Exit
	return (ret>0);
}
int RTPSession::ReadRTCP()
{
	BYTE buffer[MTU];
	sockaddr_in from_addr;
	DWORD from_len = sizeof(from_addr);

	//Receive from everywhere
	memset(&from_addr, 0, from_len);

	//Read rtcp socket
	int size = recvfrom(simRtcpSocket,buffer,MTU,MSG_DONTWAIT,(sockaddr*)&from_addr, &from_len);

	
	//Check if it is an STUN request
	STUNMessage *stun = STUNMessage::Parse(buffer,size);
	
	//If it was
	if (stun)
	{
		STUNMessage::Type type = stun->GetType();
		STUNMessage::Method method = stun->GetMethod();
		
		//If it is a request
		if (type==STUNMessage::Request && method==STUNMessage::Binding && iceLocalPwd)
		{
			DWORD len = 0;
			//Create response
			STUNMessage* resp = stun->CreateResponse();
			//Add received xor mapped addres
			resp->AddXorAddressAttribute(&from_addr);
			//TODO: Check incoming request username attribute value starts with iceLocalUsername+":"
			//Create  response
			DWORD size = resp->GetSize();
			BYTE *aux = (BYTE*)malloc(size);

			//Check if we have local passworkd
			if (iceLocalPwd)
				//Serialize and autenticate
				len = resp->AuthenticatedFingerPrint(aux,size,iceLocalPwd);
			else
				//Do nto authenticate
				len = resp->NonAuthenticatedFingerPrint(aux,size);
			
			//Send it
			sendto(simRtcpSocket,aux,len,0,(sockaddr *)&from_addr,sizeof(struct sockaddr_in));

			//Clean memory
			free(aux);
			//Clean response
			delete(resp);

			//Do NAT
			sendRtcpAddr.sin_addr.s_addr = from_addr.sin_addr.s_addr;
			//Set port : c'est bien sin_port qu'il faut recopier. La ligne prenait
			//sin_addr.s_addr, donc le port RTCP de destination valait deux octets de
			//l'ADRESSE du pair — le RTCP partait dans le vide dès qu'un binding STUN
			//arrivait sur la socket RTCP (ICE sans rtcp-mux).
			sendRtcpAddr.sin_port = from_addr.sin_port;
		}

		//Delete message
		delete(stun);
		//Exit
		return 1;
	}

	//Check if it is RTCP
	if (!RTCPCompoundPacket::IsRTCP(buffer,size))
		//Exit
		return 0;

	//Rattrapage NAT du RTCP, pendant de celui du RTP dans SendPacket. Le mapping du
	//port RTCP est indépendant de celui du RTP : on le prend sur *ce* paquet plutôt
	//que de le deviner à recPort+1. Sans objet en rtcp-mux, où le RTCP part sur
	//sendAddr (déjà corrigé côté RTP).
	if (sendRtcpAddr.sin_addr.s_addr != INADDR_ANY
	    && sendRtcpAddr.sin_addr.s_addr != from_addr.sin_addr.s_addr
	    && !natRtcpCorrected
	    && NatCorrectable(sendRtcpAddr.sin_addr.s_addr))
	{
		sendRtcpAddr.sin_addr.s_addr = from_addr.sin_addr.s_addr;
		sendRtcpAddr.sin_port        = from_addr.sin_port;
		natRtcpCorrected             = true;
		Log("-RTPSession NAT: RTCP now sending to %s:%d (annonce privee corrigee sur la source reelle).\n",
		    inet_ntoa(sendRtcpAddr.sin_addr),ntohs(sendRtcpAddr.sin_port));
	}

	//Check if we have sendinf ip address
	if (sendRtcpAddr.sin_addr.s_addr == INADDR_ANY)
	{
		//Do NAT
		sendRtcpAddr.sin_addr.s_addr = from_addr.sin_addr.s_addr;
		//Set port
		sendRtcpAddr.sin_port = from_addr.sin_port;
		//Cible RTCP posée : plus de rattrapage ultérieur
		natRtcpCorrected = true;
		//Log it
		Log("-Got first RTCP packet, sending to %s:%d with rtcp-muxing %s\n",
		    inet_ntoa(sendRtcpAddr.sin_addr),ntohs(sendRtcpAddr.sin_port),
		    muxRTCP ? "on": "off");
	}
	
	//Decript
	if (decript)
	{
		if (!recvSRTPSession)
			return Error("-No recvSRTPSession yet (RTCP)\n");
		//unprotect
		srtp_err_status_t err = srtp_unprotect_rtcp(recvSRTPSession,buffer,&size);
		//Check error
		if (err!=srtp_err_status_ok)
			return Error("Error unprotecting rtcp packet [%d]\n",err);
	}
	//RTCP mux disabled
	muxRTCP = false;
	//Parse it
	RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(buffer,size);
	//Check packet
	if (rtcp)
		//Handle incomming rtcp packets
		ProcessRTCPPacket(rtcp, inet_ntoa(from_addr.sin_addr));
	
	//OK
	return 1;
}

/*********************************
* GetTextPacket
*	Lee el siguiente paquete de video
*********************************/
int RTPSession::ReadRTP()
{
	BYTE data[MTU];
	BYTE *buffer = data;
	sockaddr_in from_addr;
	bool isRTX = false;
	DWORD from_len = sizeof(from_addr);

	//Receive from everywhere
	memset(&from_addr, 0, from_len);

	//Leemos del socket
	int size = recvfrom(simSocket,buffer,MTU,MSG_DONTWAIT,(sockaddr*)&from_addr, &from_len);

	if (size <= 0)
	{
		Error("ReadRTP on fd %d error: errno=%d.\n", simSocket, errno);
		if (errno == ENOTCONN)
		{
			Error("-fd is not valid. Stoppong RTP session %p.\n", this);
			running = false;
		}
		return 0;
	}

	//Check if it is an STUN request
	STUNMessage *stun = STUNMessage::Parse(buffer,size);
	
	//If it was
	if (stun)
	{
		STUNMessage::Type type = stun->GetType();
		STUNMessage::Method method = stun->GetMethod();
		
		//If it is a request
		if (type==STUNMessage::Request && method==STUNMessage::Binding)
		{
			DWORD len = 0;
			//Create response
			STUNMessage* resp = stun->CreateResponse();
			//Add received xor mapped addres
			resp->AddXorAddressAttribute(&from_addr);
			//TODO: Check incoming request username attribute value starts with iceLocalUsername+":"
			//Create  response
			DWORD size = resp->GetSize();
			BYTE *aux = (BYTE*)malloc(size);

			//Check if we have local passworkd
			Debug("ICE: receiving Binding Request from %s localPwd=%s\n", inet_ntoa(from_addr.sin_addr), 
			    (iceLocalPwd != NULL) ? iceLocalPwd : "no password");
			if (iceRemotePwd)
			{
				if (iceLocalPwd)
					//Serialize and autenticate
					len = resp->AuthenticatedFingerPrint(aux,size,iceLocalPwd);
				else
					//Do nto authenticate
					len = resp->NonAuthenticatedFingerPrint(aux,size);
				if (!len)
				{
					Debug("ICE: packet empty no need to send it\n");
					return 0;	
				}
						
				//Send it
				sendto(simSocket,aux,len,0,(sockaddr *)&from_addr,sizeof(struct sockaddr_in));
			}
			else
			{
				Debug("ICE: No iceRemotePwd defined yet. Dropping request...\n");
			}

			//Clean memory
			free(aux);
			//Clean response
			delete(resp);

			if ( iceRemoteIP == INADDR_ANY )
			{
				iceRemoteIP = from_addr.sin_addr.s_addr;
			}

			//P3 : un check entrant valide prouve la connectivité (rôle serveur /
			//navigateur) -> on cesse nos éventuels checks sortants (pas de régression).
			if (!iceConnected && iceRemotePwd)
			{
				Log("-RTPSession ICE: connectivité confirmée (check entrant) [%p]\n",this);
				iceConnected = true;
			}

			//If set
			if (stun->HasAttribute(STUNMessage::Attribute::IceControlled)
				|| stun->HasAttribute(STUNMessage::Attribute::UseCandidate)
				|| iceRemoteIP == from_addr.sin_addr.s_addr)
			{
				// We should check that username matches
				if (iceRemoteUsername)
				{
					// ICE is enabled
					if ( recIP == INADDR_ANY )
					{
						// set recIP if not set
						recIP = from_addr.sin_addr.s_addr;
						recPort = ntohs(from_addr.sin_port);
					}
					
					
					if ( sendAddr.sin_addr.s_addr != recIP 
					     || 
					     sendAddr.sin_port != htons(recPort) )
					{
						// Do symetric RTP 
						sendAddr.sin_addr.s_addr = recIP;
						//recPort est en ordre HÔTE, sin_port se stocke en ordre RÉSEAU :
						//htons, comme dans le test juste au-dessus (même résultat sur les
						//deux architectures, mais la conversion était écrite à l'envers).
						sendAddr.sin_port = htons(recPort);
					}
				}

				DWORD len = 0;
				//Create trans id
				BYTE transId[12];
				//Set first to 0
				set4(transId,0,0);
				//Set timestamp as trans id
				set8(transId,4,getTime());
				//Create binding request to send back
				STUNMessage *request = new STUNMessage(STUNMessage::Request,STUNMessage::Binding,transId);
				//Check usernames
				if (iceLocalUsername && iceRemoteUsername)
					//Add username
					request->AddUsernameAttribute(iceLocalUsername,iceRemoteUsername);
					//Add other attributes
				if ( stun->HasAttribute(STUNMessage::Attribute::IceControlled ) )
				{
					request->AddAttribute(STUNMessage::Attribute::IceControlling,(QWORD)-1);
					request->AddAttribute(STUNMessage::Attribute::UseCandidate);
				}
				else
					request->AddAttribute(STUNMessage::Attribute::IceControlled,(QWORD)-1);

				request->AddAttribute(STUNMessage::Attribute::Priority,(DWORD)33554431);
				//Create  request
				DWORD size = request->GetSize();
				BYTE* aux = (BYTE*)malloc(size);

				//Check remote pwd
				if (iceRemotePwd)
				{
					Debug("ICE: sending bind request with remote user=[%s], remote password=[%s] to %s:%d.\n",
				      (iceRemoteUsername != NULL) ? iceRemoteUsername : "no user",
				      (iceRemotePwd != NULL ) ? iceRemotePwd : "no pwd",
					 inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
					if (iceRemotePwd)
					//Serialize and autenticate
						len = request->AuthenticatedFingerPrint(aux,size,iceRemotePwd);
					else
					//Do nto authenticate
						len = request->NonAuthenticatedFingerPrint(aux,size);

					//Send it
					if (!len)
					{
						Debug("ICE: packet empty no need to send it\n");
						return 0;	
					}
					
					sendto(simSocket,aux,len,0,(sockaddr *)&from_addr,sizeof(struct sockaddr_in));
				}

				//Clean memory
				free(aux);
				//Clean response
				delete(request);

				// Needed for DTLS in client mode (otherwise the DTLS "Client Hello" is not sent over the wire)
				len = dtls.Read(buffer,MTU);
				//Send back
				if (!len)
				{
					Debug("DTLS: packet empty no need to send it\n");
					return 0;	
				}
				sendto(simSocket,buffer,len,0,(sockaddr *)&from_addr,sizeof(struct sockaddr_in));
			}
		}
		//P3 : réponse à un binding request que NOUS avons émis (pair ICE-lite ou full).
		//Le handler historique n'acceptait que les Request : les Response étaient
		//ignorées, ce qui empêchait toute validation de connectivité côté offreur.
		else if (type==STUNMessage::Response && method==STUNMessage::Binding)
		{
			Log("ICE: réception Binding Response de %s:%d [%p]\n",
				inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port), this);
			//Validation pragmatique (parité avec le niveau ICE existant : pas de
			//vérif MESSAGE-INTEGRITY sur l'entrant) : une Binding Response provenant du
			//pair attendu confirme la connectivité.
			if (iceRemoteIP == INADDR_ANY || iceRemoteIP == from_addr.sin_addr.s_addr)
				OnICEConnectivityConfirmed(&from_addr);
			else
				Debug("ICE: Binding Response d'une source inattendue, ignorée [%p]\n",this);
		}

		//Delete message
		delete(stun);
		//Exit
		return 1;
	}

	//Check if it is RTCP
	if (RTCPCompoundPacket::IsRTCP(buffer,size))
	{
		//Decript
		if (decript)
		{
			if (!recvSRTPSession)
				return Error("-No recvSRTPSession yet (RTCP)\n");
			//unprotect
			srtp_err_status_t err = srtp_unprotect_rtcp(recvSRTPSession,buffer,&size);
			//Check error
			if (err!=srtp_err_status_ok)
				return Error("Error unprotecting rtcp packet [%d]\n",err);
		}
		//RTCP mux enabled
		muxRTCP = true;
		//Parse it
		RTCPCompoundPacket* rtcp = RTCPCompoundPacket::Parse(buffer,size);
		//Check packet
		if (rtcp)
			//Handle incomming rtcp packets
			ProcessRTCPPacket(rtcp,inet_ntoa(from_addr.sin_addr));
		
		//Skip
		return 1;
	}

	//Check if it a DTLS packet
	if (DTLSConnection::IsDTLS(buffer,size))
	{
		Log("-RTPSession DTLS: received packet from [%s:%d]\n", inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
		//Feed it
		if (!dtls.Write(buffer,size))
		{
			Debug("-RTPSession DTLS: nothing to send back.\n");
			//Exit
			return 0;
		}
		//Read
		int len = dtls.Read(buffer,MTU);
		//Send it back
		if (!len)
		{
			Debug("-RTPSession DTLS: packet empty no need to send it\n");
			return 1;	
		}

		sendto(simSocket,buffer,len,0,(sockaddr *)&from_addr,sizeof(struct sockaddr_in));
		return 1;
	}

	//Double check it is an RTP packet
	if (!RTPPacket::IsRTP(buffer,size))
	{
		//Debug
		Debug("-Not RTP data recevied\n");
		//Exit
		return 1;
	}
	
	//If we don't have originating IP
	if (recIP != from_addr.sin_addr.s_addr)
	{
		//Bind it to first received packet ip
		recIP = from_addr.sin_addr.s_addr;
		//Get also port
		recPort = ntohs(from_addr.sin_port);
		//Log
		Log("-RTPSession NAT: received packet from [%s:%d] for media %s.\n",
                   inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port),
                   MediaFrame::TypeToString(media));
		//Check if got listener
		if (auto l = LockListener())
			//Request a I frame
			l->onFPURequested(this);
	}

	//Check minimum size for rtp packet
	if (size<12)
		return Error("Invalid RTP packet read from fd %d. Packet too small of %d bytes.\n", simSocket, size);

	DWORD defaultSSRC = GetDefaultStream(true);
	
	//This should be improbed
	/*if (useNACK && defaultSSRC && defaultSSRC!=RTPPacket::GetSSRC(buffer))
	{
		Debug("-----nacked %x %x\n",defaultSSRC,RTPPacket::GetSSRC(buffer));
		//It is a retransmited packet
		isRTX = true;
	}*/
	
	//Check if it is encripted
	if (decript )
	{
		srtp_err_status_t err;
		//Check if it is a retransmited packet
	
		//Check session
		if (!recvSRTPSession)
			return Error("-No recvSRTPSession yet\n");

		if (!isRTX)
		{
			//unprotect
			if (defaultSSRC == 0 ||  defaultSSRC == RTPPacket::GetSSRC(buffer) )
				err = srtp_unprotect(recvSRTPSession,buffer,&size);
			else
			{
				if (recvSRTPSession_secondary)
					err = srtp_unprotect(recvSRTPSession_secondary,buffer,&size);
			}
		}
		else
		{
			//unprotect RTX
			if (defaultSSRC == 0 || defaultSSRC == RTPPacket::GetSSRC(buffer) )
				err = srtp_unprotect(recvSRTPSessionRTX,buffer,&size);
			else
			{
				if (recvSRTPSessionRTX_secondary)
					err = srtp_unprotect(recvSRTPSessionRTX_secondary,buffer,&size);
			}
		}
		
		//Check status
		if (err!=srtp_err_status_ok)
			//Error
			return Error("Error unprotecting rtp packet [%d] for RTPSession=%p, ssrc=%x defaultSSRC=%x\n",err,this, RTPPacket::GetSSRC(buffer),defaultSSRC);
	}

	//P5 : premier paquet RTP/SRTP reçu ET validé (déchiffrement réussi si SRTP => le
	//handshake DTLS est terminé ; sinon pas de crypto). On notifie le listener UNE
	//seule fois par cycle de réception (ré-armé par ArmRTPReceivedNotification).
	if (!rtpReceivedNotified)
	{
		rtpReceivedNotified = true;
		if (auto l = LockListener())
			l->onRTPPacketReceived(this);
	}

	//If it is a retransmission
	if (isRTX)
	{
		//Get original sequence number
		WORD osn = get2(buffer,sizeof(rtp_hdr_t));
		//Move origin
		for (int i=sizeof(rtp_hdr_t)-1;i>=0;--i)
			//Move
			buffer[i+2] = buffer[i];
		//Move init
		buffer+=2;
		//Set original seq num
		set2(buffer,2,osn);
	}

	//Get type
	BYTE type = RTPPacket::GetType(buffer);

	//Check rtp map
	if (!rtpMapIn)
		//Error
		return Error("-RTP map not set\n");
	
	//Set initial codec
	BYTE codec = rtpMapIn->GetCodecForType(type);

	//Renumérotation en cours de renégociation : la map précédente répond encore
	//pour le temps que le pair mette à recevoir notre réponse (§ CodecFromPreviousMap),
	//et pas une seconde de plus qu'il n'en faut — le premier numéro que seule la
	//nouvelle map porte prouve qu'il a basculé et referme le repli.
	if (codec!=RTPMap::NotFound)
		RetirePreviousMap(type);
	else
		codec = CodecFromPreviousMap(type);

	//Check codec
	if (codec==RTPMap::NotFound)
		//Exit : le paquet est indécodable, on le jette. La trace est agrégée —
		//c'est un régime normal pendant une renégociation (cf. unknownPtCount).
		return OnUnknownPayloadType(type, RTPPacket::GetSSRC(buffer), from_addr);

	//Create rtp packet
	RTPTimedPacket *packet = NULL;

	//Peek type
	if (codec==TextCodec::T140RED || codec==VideoCodec::RED)
	{
		//Create redundant type
		RTPRedundantPacket *red = new RTPRedundantPacket(media,buffer,size);
		//Get primary type
		BYTE t = red->GetPrimaryType();
		//Map primary data codec
		BYTE c = rtpMapIn->GetCodecForType(t);
		//Le bloc primaire porte son propre payload type : il est renuméroté par la
		//renégociation comme les autres, et le même rattrapage lui est dû.
		if (c==RTPMap::NotFound)
			c = CodecFromPreviousMap(t);
		//Check codec
		if (c==RTPMap::NotFound)
		{
			//Delete red packet
			delete(red);
			//Exit
			return Error("-RTP packet type unknown for primary type of redundant data [%d]\n",t);
		}
		//Set it
		red->SetPrimaryCodec(c);
		//For each redundant packet
		for (int i=0; i<red->GetRedundantCount();++i)
		{
			//Get redundant type
			BYTE t = red->GetRedundantType(i);
			//Map redundant data codec
			BYTE c = rtpMapIn->GetCodecForType(t);
			//Idem pour chaque bloc redondant (RFC 4103) : ils portent le même
			//payload type que le primaire d'un paquet antérieur.
			if (c==RTPMap::NotFound)
				c = CodecFromPreviousMap(t);
			//Check codec
			if (c==RTPMap::NotFound)
			{
				//Delete red packet
				delete(red);
				//Exit
				return Error("-RTP packet type unknown for primary type of secundary data [%d,%d]\n",i,t);
			}
			//Set it
			red->SetRedundantCodec(i,c);
		}
		//Set packet
		packet = red;
	} else {
		//Create normal packet
		packet = new RTPTimedPacket(media,buffer,size);
	}
		//Set codec
	packet->SetCodec(codec);
	//Get ssrc
	DWORD ssrc = packet->GetSSRC();

        streamUse.IncUse();

        //if ( defaultSSRC == 0 && defaultStream != NULL) defaultStream->Cancel();

        RTPStream* stream = getStream(ssrc);

        if (!isRTX)
        {
	    if ( stream == NULL )
	    {
                //Send SR to old one
                SendSenderReport();
                streamUse.DecUse();
				if ( defaultStream == NULL && ssrc > 0)
				{
					Log("-Creating default stream SSRC [new:%x] for RTPSession=%p \n",ssrc,this);
					SetDefaultStream(true, ssrc);
				}
				else if (auto l = LockListener()) //call listener
				{
					l->onNewStream(this, ssrc, true);
				}
				else
				{
					Log("-No Listener. Adding new SSRC [new:%x]\n",ssrc);
					if (defaultStream == NULL) 
						SetDefaultStream(true, ssrc);
					else
						ChangeStream( defaultSSRC, ssrc );
				}

                streamUse.IncUse();
                stream = getStream(ssrc);
            }

            if ( stream != NULL )
            {
                stream->Add(packet, size);
            }
        }
        else /* RTP retransmission enabled. We do not handle multi stream in that case */
        {
            if (defaultSSRC > 0)
            {
                //Set SSRC of the original stream for retransmitted packets
                if ( ssrc != defaultSSRC ) packet->SetSSRC(defaultSSRC);
            }
            else
            {
                streamUse.DecUse();
                SetDefaultStream(true, ssrc);
                streamUse.IncUse();
            }
            defaultStream->Add(packet,size);
        }
        streamUse.DecUse();
	//OK
	return 1;
}

void RTPSession::Start()
{
	//We are running
	running = true;

	//Create thread
	StartThread();
}

void RTPSession::Stop()
{
	//Check running
	if (running)
	{
		//Not running
		running = false;

		//Réveille le poll (eventfd du Wait hérité) et joint le thread
		StopThread();
                DeleteStreams();
	}

        rtxUse.WaitUnusedAndLock();
        for (RTPOrderedPackets::iterator it = rtxs.begin(); it!=rtxs.end();++it)
	{
		//Get pacekt
		RTPTimedPacket *pkt = it->second;
		//Delete object
		delete(pkt);
	}
        rtxUse.Unlock();
}

/***********************
* run
*       Helper thread function
************************/
/***************************
 * Run
 * 	Server running thread
 ***************************/
int RTPSession::Run()
{
	Log(">Run RTPSession [%p]\n",this);

	//Set values for polling
	ufds[0].fd = simSocket;
	ufds[0].events = POLLIN | POLLERR | POLLHUP;
	ufds[1].fd = simRtcpSocket;
	ufds[1].events = POLLIN | POLLERR | POLLHUP;

	//Set non blocking so we can get an error when we are closed by end
	int fsflags = fcntl(simSocket,F_GETFL,0);
	fsflags |= O_NONBLOCK;
	fcntl(simSocket,F_SETFL,fsflags);

	fsflags = fcntl(simRtcpSocket,F_GETFL,0);
	fsflags |= O_NONBLOCK;
	fcntl(simRtcpSocket,F_SETFL,fsflags);

	//Réveil inter-thread : eventfd du Wait hérité (remplace le SIGIO historique)
	ufds[2].fd = wait.GetPollFd();
	ufds[2].events = POLLIN;

	//Le chrono d'inactivité ne court que lorsqu'il est armé (ArmRTPTimeout, au SDP
	//answer) : rien à amorcer ici.

	//Attente bornée seulement lorsque le watchdog est armé, pour vérifier
	//périodiquement l'inactivité ; sinon on conserve l'attente infinie d'origine
	//(aucun réveil superflu pour les sessions n'utilisant pas le watchdog).
	const int pollTimeout = 1000; //ms
	//P2 : lorsqu'on pilote un handshake DTLS client, on borne l'attente plus court
	//pour cadencer les retransmissions (le backoff réel est décidé par OpenSSL).
	const int dtlsPollTimeout = 250; //ms

	//Run until ended
	while(running)
	{
		//P2 : pilotons-nous un handshake DTLS en rôle client ? (client, DTLS prêt,
		//non terminé, destination connue). Le cas serveur/passive reste inchangé.
		bool dtlsDriving = dtls.IsInited() && dtls.IsClientRole()
				&& !dtls.IsHandshakeCompleted()
				&& sendAddr.sin_addr.s_addr != INADDR_ANY;

		//P3 : émettons-nous des checks STUN sortants ? (creds connues, destination
		//connue, connectivité pas encore confirmée). Face à un pair ICE-lite qui
		//n'initie jamais ; s'arrête dès la 1re réponse/check entrant.
		bool iceDriving = !iceConnected && iceLocalUsername && iceRemoteUsername
				&& iceRemotePwd && sendAddr.sin_addr.s_addr != INADDR_ANY;

		//P6 : reste-t-il des paquets d'amorçage NAT à émettre ? (rafale armée, clair,
		//destination connue). Cadencés ~20 ms par le poll borné ci-dessous.
		bool natPriming = natPrimingLeft > 0 && !encript
				&& sendAddr.sin_addr.s_addr != INADDR_ANY;

		//Attente : infinie par défaut, bornée si watchdog armé et/ou handshake DTLS
		//client / checks ICE en cours (on prend le plus court des seuils).
		int waitMs = rtpTimeoutArmed ? pollTimeout : -1;
		if (dtlsDriving || iceDriving)
			waitMs = (waitMs < 0) ? dtlsPollTimeout
					      : (waitMs < dtlsPollTimeout ? waitMs : dtlsPollTimeout);
		if (natPriming)
			waitMs = (waitMs < 0) ? NAT_PRIMING_INTERVAL_MS
					      : (waitMs < NAT_PRIMING_INTERVAL_MS ? waitMs : NAT_PRIMING_INTERVAL_MS);

		//Wait for events
		int nready = poll(ufds,3,waitMs);
		if(nready<0)
		{
			//EINTR/EAGAIN : interruption par signal, on retente sans rien signaler
			if (errno==EINTR || errno==EAGAIN)
				continue;
			//Erreur dure (EBADF, EINVAL, ENOMEM...) : inutile de boucler à vide,
			//on log et on sort proprement (msleep de garde anti busy-spin)
			Error("-RTPSession poll error, arret de la boucle: errno=%d (%s) [%p]\n",
					errno,strerror(errno),this);
			msleep(10);
			break;
		}

		if (ufds[0].revents & POLLIN)
		{
			//Any inbound traffic (RTP/STUN/DTLS) prouve que le pair est vivant :
			//on mémorise l'instant et on réarme l'anti-rebond.
			gettimeofday(&lastRecv,NULL);
			rtpTimedOut = false;
			//Read rtp data
			ReadRTP();
		}
		if (ufds[1].revents & POLLIN)
			//Read rtcp data
			ReadRTCP();

		//P6 : cadence la rafale d'amorçage NAT (~20 ms entre paquets) tant qu'il en
		//reste. Placé APRÈS ReadRTP : si le pair a déjà latché et répondu, la rafale
		//continue quand même jusqu'au bout (inoffensif) — elle est courte et bornée.
		if (natPriming && (getDifTime(&natPrimingLast)/1000) >= NAT_PRIMING_INTERVAL_MS)
		{
			SendNATPrimingPacket();
			gettimeofday(&natPrimingLast,NULL);
		}

		//P3 : émettre/retransmettre les binding requests STUN sortants tant que la
		//connectivité n'est pas confirmée (placé APRÈS ReadRTP : un check/réponse
		//entrant peut avoir mis iceConnected à true et court-circuité l'émission).
		if (iceDriving)
			DriveICEChecks();

		//P2 : piloter le handshake DTLS client (émission initiale du ClientHello puis
		//retransmissions). Placé APRÈS ReadRTP pour que les flights entrants (traités
		//par la branche DTLS de ReadRTP) fassent d'abord progresser le handshake.
		if (dtlsDriving)
			DriveDTLSClientHandshake();

		//Watchdog d'inactivité (gap 5) : armé et aucun paquet depuis > rtpTimeout
		//(mesuré depuis l'armement ou le dernier paquet) => émettre UNE seule fois
		//onRTPTimeout (anti-rebond via rtpTimedOut).
		if (rtpTimeoutArmed && rtpTimeout>0 && !rtpTimedOut
				&& (getDifTime(&lastRecv)/1000) > rtpTimeout)
		{
			//Marque la transition actif -> inactif
			rtpTimedOut = true;
			Log("-RTPSession inactivité > %u ms, notification onRTPTimeout [%p]\n",rtpTimeout,this);
			//Notifie le listener (RTPEndpoint publiera EndpointDisconnectedEvent)
			if (auto l = LockListener())
				l->onRTPTimeout(this);
		}

		//Erreur/fermeture sur l'un des deux sockets : POLLHUP (pair parti),
		//POLLERR (erreur socket) ou POLLNVAL (fd fermé, ex. via End()).
		//NB: on teste bien ufds[1] pour le socket RTCP (bug historique corrige).
		if ((ufds[0].revents & (POLLHUP|POLLERR|POLLNVAL)) ||
		    (ufds[1].revents & (POLLHUP|POLLERR|POLLNVAL)))
		{
			//Error : on sort proprement de la boucle
			Log("-RTPSession sortie sur evenement socket RTP=0x%x RTCP=0x%x [%p]\n",
					ufds[0].revents,ufds[1].revents,this);
			//Exit
			break;
		}
	}

	Log("<RTPSession run\n");
	return 0;
}

RTPPacket* RTPSession::GetPacket()
{
    streamUse.IncUse();
    RTPPacket* rtp = (defaultStream != NULL && !defaultStream->disabled) ? defaultStream->Wait() : NULL;
    streamUse.DecUse();
    if (rtp == NULL) msleep(100);
    return rtp;
}

RTPPacket* RTPSession::GetPacket(DWORD & ssrc)
{
    streamUse.IncUse();
    RTPStream * s = (ssrc != 0) ? getStream(ssrc) : defaultStream;
    RTPPacket* rtp = (s != NULL && !s->disabled) ? s->Wait() : NULL;
	streamUse.DecUse();
    if (rtp == NULL) msleep(100);
    return rtp;
}

void RTPSession::CancelGetPacket()
{
	//cancel
    streamUse.IncUse();
	if (defaultStream != NULL) defaultStream->Cancel();
    streamUse.DecUse();
}

void RTPSession::CancelGetPacket(DWORD & ssrc)
{
    streamUse.IncUse();
    RTPStream * s = (ssrc != 0) ? getStream(ssrc) : defaultStream;
    if (s) s->Cancel();
    streamUse.DecUse();
}

void RTPSession::ResetPacket(DWORD & ssrc, bool clear) 
{ 
    streamUse.IncUse();
    RTPStream * s = (ssrc != 0) ? getStream(ssrc) : defaultStream;
    if (s) s->Reset(clear);
    streamUse.DecUse();
}
void RTPSession::ProcessRTCPPacket(RTCPCompoundPacket *rtcp, const char * fromAddr)
{
	//For each packet
	for (int i = 0; i<rtcp->GetPacketCount();i++)
	{
		//Get pacekt
		RTCPPacket* packet = rtcp->GetPacket(i);
		//Check packet type
		switch (packet->GetType())
		{
			case RTCPPacket::SenderReport:
			{
				RTCPSenderReport* sr = (RTCPSenderReport*)packet;
				//Get Timestamp, the middle 32 bits out of 64 in the NTP timestamp (as explained in Section 4) received as part of the most recent RTCP sender report (SR) packet from source SSRC_n. If no SR has been received yet, the field is set to zero.
				recSR = sr->GetTimestamp() >> 16;
				//Uptade last received SR
				getUpdDifTime(&lastReceivedSR);
				//Check recievd report
				for (int j=0;j<sr->GetCount();j++)
				{
					//Get report
					RTCPReport *report = sr->GetReport(j);
					//Check ssrc
					if (report->GetSSRC()==sendSSRC)
					{
						//Calculate RTT
						if (!isZeroTime(&lastSR) && report->GetLastSR() == sendSR)
						{
							//Calculate new rtt
							DWORD rtt = getDifTime(&lastSR)/1000-report->GetDelaySinceLastSRMilis();
							//Set it
							SetRTT(rtt);
						}
					}
				}
				break;
			}
			case RTCPPacket::ReceiverReport:
			{
				RTCPReceiverReport* rr = (RTCPReceiverReport*)packet;
				//Check recievd report
				for (int j=0;j<rr->GetCount();j++)
				{
					//Get report
					RTCPReport *report = rr->GetReport(j);
					//Check ssrc
					if (report->GetSSRC()==sendSSRC)
					{
						//Calculate RTT
						if (!isZeroTime(&lastSR) && report->GetLastSR() == sendSR)
						{
							//Calculate new rtt
							DWORD rtt = getDifTime(&lastSR)/1000-report->GetDelaySinceLastSRMilis();
							//Set it
							SetRTT(rtt);
						}
					}
				}
				break;
			}
				break;
			case RTCPPacket::SDES:
				break;
			case RTCPPacket::Bye:
				break;
			case RTCPPacket::App:
				break;
			case RTCPPacket::RTPFeedback:
			{
				//Get feedback packet
				RTCPRTPFeedback *fb = (RTCPRTPFeedback*) packet;
				//Check feedback type
				switch(fb->GetFeedbackType())
				{
					case RTCPRTPFeedback::NACK:
						//Debug("-Nack received\n");
						for (BYTE i=0;i<fb->GetFieldCount();i++)
						{
							//Get field
							RTCPRTPFeedback::NACKField *field = (RTCPRTPFeedback::NACKField*) fb->GetField(i);
							//Resent it
							ReSendPacket(field->pid);
							//Check each bit of the mask
							for (int i=0;i<16;i++)
								//Check it bit is present to rtx the packets
								if ((field->blp >> (15-i)) & 1)
									//Resent it
									ReSendPacket(field->pid+i+1);
						}
						break;
					case RTCPRTPFeedback::TempMaxMediaStreamBitrateRequest:
						Log("-TempMaxMediaStreamBitrateRequest received from [%s] on %s stream\n", fromAddr, MediaFrame::TypeToString(media));
						for (BYTE i=0;i<fb->GetFieldCount();i++)
						{
							//Get field
							RTCPRTPFeedback::TempMaxMediaStreamBitrateField *field = (RTCPRTPFeedback::TempMaxMediaStreamBitrateField*) fb->GetField(i);
							//Check if it is for us
							if (auto l = LockListener(); l && field->GetSSRC()==sendSSRC)
								//call listener
								l->onTempMaxMediaStreamBitrateRequest(this,field->GetBitrate(),field->GetOverhead());
						}
						break;
					case RTCPRTPFeedback::TempMaxMediaStreamBitrateNotification:
						Debug("-TempMaxMediaStreamBitrateNotification received from [%s] on %s stream\n", fromAddr, MediaFrame::TypeToString(media));
						pendingTMBR = false;
						if (requestFPU)
						{
							requestFPU = false;
							DWORD ssrc=defaultStream->GetRecSSRC();
							SendFIR(ssrc);
						}
						for (BYTE i=0;i<fb->GetFieldCount();i++)
						{
							//Get field
							RTCPRTPFeedback::TempMaxMediaStreamBitrateField *field = (RTCPRTPFeedback::TempMaxMediaStreamBitrateField*) fb->GetField(i);
							Debug("-TempMaxMediaStreamBitrateNotification: maxBitrate = %d, overhead=%d\n", field->GetBitrate(), field->GetOverhead() );


						}
		
						break;
				}
				break;
			}
			case RTCPPacket::PayloadFeedback:
			{
				//Get feedback packet
				RTCPPayloadFeedback *fb = (RTCPPayloadFeedback*) packet;
				//Check feedback type
				switch(fb->GetFeedbackType())
				{
					case RTCPPayloadFeedback::PictureLossIndication:
					case RTCPPayloadFeedback::FullIntraRequest:
						//Chec listener
						if (auto l = LockListener())
							//Send intra refresh
							l->onFPURequested(this);
						break;
					case RTCPPayloadFeedback::SliceLossIndication:
						Log("-RTCP SliceLossIndication received\n");
						break;
					case RTCPPayloadFeedback::ReferencePictureSelectionIndication:
						Log("-RTCP ReferencePictureSelectionIndication  received\n");
						break;
					case RTCPPayloadFeedback::TemporalSpatialTradeOffRequest:
						Log("-RTCP TemporalSpatialTradeOffRequest  received\n");
						break;
					case RTCPPayloadFeedback::TemporalSpatialTradeOffNotification:
						Log("-RTCP TemporalSpatialTradeOffNotification\n");
						break;
					case RTCPPayloadFeedback::VideoBackChannelMessage:
						Log("-RTCP VideoBackChannelMessage\n");
						break;
					case RTCPPayloadFeedback::ApplicationLayerFeeedbackMessage:
						for (BYTE i=0;i<fb->GetFieldCount();i++)
						{
							//Get feedback
							RTCPPayloadFeedback::ApplicationLayerFeeedbackField* msg = (RTCPPayloadFeedback::ApplicationLayerFeeedbackField*)fb->GetField(i);
							//Get size and payload
							DWORD len	= msg->GetLength();
							BYTE* payload	= msg->GetPayload();
							//Check if it is a REMB
							if (len>8 && payload[0]=='R' && payload[1]=='E' && payload[2]=='M' && payload[3]=='B')
							{
								//GEt exponent
								BYTE exp = payload[5] >> 2;
								DWORD mantisa = payload[5] & 0x03;
								mantisa = mantisa << 8 | payload[6];
								mantisa = mantisa << 8 | payload[7];
								//Get bitrate
								DWORD bitrate = mantisa << exp;
								//Check if it is for us
								if (auto l = LockListener())
									//call listener
									l->onReceiverEstimatedMaxBitrate(this,bitrate);
							}
						}
						break;
				}
				break;
			}
			case RTCPPacket::FullIntraRequest:
				//This is message deprecated and just for H261, but just in case
				//Check listener
				if (auto l = LockListener())
					//Send intra refresh
					l->onFPURequested(this);
				break;
			case RTCPPacket::NACK:
				break;
		}
	}
	//Delete pacekt
	delete(rtcp);
}

int RTPSession::SendFIR(DWORD & ssrc)
{

	Log("-SendFIR\n");

	//Create rtcp sender retpor
	RTCPCompoundPacket* rtcp = CreateSenderReport();
	
	DWORD recSSRC = ssrc;
	if (getStream(ssrc) == NULL && recSSRC == 0 && defaultStream != NULL)
		recSSRC = defaultStream->GetRecSSRC();
		
	//Create fir request
	RTCPPayloadFeedback *fir = RTCPPayloadFeedback::Create(RTCPPayloadFeedback::FullIntraRequest,sendSSRC,recSSRC);
	//ADD field
	fir->AddField(new RTCPPayloadFeedback::FullIntraRequestField(recSSRC,firReqNum++));
	//Add to rtcp
	rtcp->AddRTCPacket(fir);

	//Add PLI
	RTCPPayloadFeedback *pli = RTCPPayloadFeedback::Create(RTCPPayloadFeedback::PictureLossIndication,sendSSRC,recSSRC);
	//Add to rtcp
	rtcp->AddRTCPacket(pli);

	//Send packet
	int ret = SendPacket(*rtcp);

	//Delete it
	delete(rtcp);

	return ret;
}

int RTPSession::RequestFPU()
{
	 if (defaultStream == NULL)
		return 0;

	DWORD ssrc=defaultStream->GetRecSSRC();
	return RequestFPU(ssrc);
}

int RTPSession::RequestFPU(DWORD & ssrc)
{
	//Send all the packets inmediatelly to the decoderso I frame can be handled as soon as possoble
	RTPStream* stream = getStream(ssrc);
	if (stream == NULL  )
		stream = defaultStream;
	
	
	if (stream != NULL) stream->HurryUp();
	ssrc=stream->GetRecSSRC();	
	//request FIR
	SendFIR(ssrc);

	//packets.Reset();
	/*if (!pendingTMBR)
	{
		//request FIR
		SendFIR();
	} else {
		//Wait for TMBN response to no overflow
		requestFPU = true;
	}*/
	return 0;
}

void RTPSession::SetRTT(DWORD rtt)
{
	//Set it
	this->rtt = rtt;
	DWORD recSSRC =0;
	if (defaultStream != NULL)
		recSSRC = defaultStream->GetRecSSRC();
	else
		return;
	
	//if got estimator
	if (remoteRateEstimator)
	{
		//Update estimator
		remoteRateEstimator->UpdateRTT(recSSRC,rtt,getTimeMS());
	}

	//Check RTT to enable NACK
	if (useNACK && rtt < 240)
	{
		//Enable NACK only if RTT is small
		isNACKEnabled = true;
		//Update jitter buffer size in ms in [60+rtt,300]
		defaultStream->SetMaxWaitTime(fmin(60+rtt,300));
	} else {
		//Disable NACK
		isNACKEnabled = false;
		//Reduce jitter buffer as we don't use NACK
		defaultStream->SetMaxWaitTime(60);
	}		
}

void RTPSession::onTargetBitrateRequested(DWORD bitrate)
{
    bool fb;

    // Memory barrier
    mutex.lock();
    fb = sendBitrateFeedback;
    mutex.unlock();
    Debug("-RTPSession::onTargetBitrateRequested() %i, bitrate [%d] for %s stream %p.\n", fb, bitrate, MediaFrame::TypeToString(media), this);
    if (fb)
    {
	Debug("-RTPSession::onTargetBitrateRequested() sending TMMBR\n",bitrate);
	//Create rtcp sender retpor
	RTCPCompoundPacket* rtcp = CreateSenderReport();
	
	DWORD recSSRC =0;
	if (defaultStream != NULL)
		recSSRC = defaultStream->GetRecSSRC();
	
	//Create TMMBR
	RTCPRTPFeedback *rfb = RTCPRTPFeedback::Create(RTCPRTPFeedback::TempMaxMediaStreamBitrateRequest,sendSSRC,recSSRC);
	//Limit incoming bitrate
	rfb->AddField( new RTCPRTPFeedback::TempMaxMediaStreamBitrateField(recSSRC,bitrate,0));
	//Add to packet
	rtcp->AddRTCPacket(rfb);

	//We have a pending request
	pendingTMBR = true;
	//Store values
	pendingTMBBitrate = bitrate;

	//Send packet
	SendPacket(*rtcp);

	//Delete it
	delete(rtcp);
    }
}

void RTPSession::ReSendPacket(int seq)
{
	//Lock
	if (!useNACK) 
	{
		Debug("Asked to resend RTP seq %d but NACK not enabled.\n", seq);
		return;
	}

	rtxUse.IncUse();
	DWORD recCycles =0;
	if (defaultStream != NULL)
		recCycles = defaultStream->GetRecCycles();
	
	//Calculate ext seq number
	DWORD ext = ((DWORD)recCycles)<<16 | seq;

	//Find packet to retransmit
	RTPOrderedPackets::iterator it = rtxs.find(ext);

	//If we still have it
	if (it!=rtxs.end())
	{
		//Get packet
                DWORD size = MTU;
                BYTE data[MTU+SRTP_MAX_TRAILER_LEN];

                
		RTPTimedPacket* packet = it->second;
                int len = packet->GetSize();

                if (len>size)
		{
                    //Error
                    Error("-RTPSession::ReSendPacket() | not enougth size for copying packet [len:%d]\n",len);
		    return;
		}

               //Copy
                memcpy(data,packet->GetData(),packet->GetSize());
#if 0		// Does not work - recencryption fails
                //If using abs-time
                if (useAbsTime)
                {
                        //Calculate absolute send time field (convert ms to 24-bit unsigned with 18 bit fractional part.
                        // Encoding: Timestamp is in seconds, 24 bit 6.18 fixed point, yielding 64s wraparound and 3.8us resolution (one increment for each 477 bytes going out on a 1Gbps interface).
                        DWORD abs = ((getTimeMS() << 18) / 1000) & 0x00ffffff;
                        //Overwrite it
                        set3(data,sizeof(rtp_hdr_t)+sizeof(rtp_hdr_ext_t)+1,abs);
                }

                //Check if we ar encripted
                if (encript)
                {
                        //Check  session
                        if (!sendSRTPSession)
			{
                                //Error
                                Error("-RTPSession::ReSendPacket() | no sendSRTPSession\n");
				return;
			}
                        //Encript
                        srtp_err_status_t err = srtp_protect(sendSRTPSession,data,&len);
                        //Check error
                        if (err!=srtp_err_status_ok)
                        {
                                //Check if got listener
                                if (auto l = LockListener())
                                        //Request a I frame
                                        l->onFPURequested(this);
                                //Nothing
                                Error("-RTPSession::ReSendPacket() | Error protecting RTP packet [%d] sending intra instead\n",err);
				return;
                        }
                }
                //Check len
#endif
                if (len)
                {
                        Debug("-resent nacked packet ext %d seq %d rtpsess=%p.\n", ext, packet->GetSeqNum(), this);
                        //Send packet
                        sendto(simSocket,data,len,0,(sockaddr *)&sendAddr,sizeof(struct sockaddr_in));
                }
	}
	else
	{
		it = rtxs.begin();
		int first = (it == rtxs.end())? 0 : it->first;

		Debug("-could not resent necket packet seq %d: not in buffer anymore. first seq = %d, cout = %d, rtpsess=%p useNacl=%d\n", 
			ext, first, rtxs.size(), this, useNACK);
                                //Check if got listener
                if (auto l = LockListener())
                        //Request a I frame
                        l->onFPURequested(this);

	}

	//Unlock
	rtxUse.DecUse();
}
int RTPSession::SendTempMaxMediaStreamBitrateNotification(DWORD bitrate,DWORD overhead)
{
	Log("-SendTempMaxMediaStreamBitrateNotification [%d,%d]\n",bitrate,overhead);

	//Create rtcp sender retpor
	RTCPCompoundPacket* rtcp = CreateSenderReport();
	
	DWORD recSSRC =0;
	if (defaultStream != NULL)
		recSSRC = defaultStream->GetRecSSRC();
	
	//Create TMMBR
	RTCPRTPFeedback *rfb = RTCPRTPFeedback::Create(RTCPRTPFeedback::TempMaxMediaStreamBitrateNotification,sendSSRC,recSSRC);
	//Limit incoming bitrate
	rfb->AddField( new RTCPRTPFeedback::TempMaxMediaStreamBitrateField(sendSSRC,bitrate,0));
	//Add to packet
	rtcp->AddRTCPacket(rfb);

	//Send packet
	int ret = SendPacket(*rtcp);

	//Delete it
	delete(rtcp);

	//Exit
	return ret;
}


/**
 *  Create a new RTP stream. If the stream already exists, it does nothing
 *  
 *  @param receiving: if this is a receiving or sending stream (only receiving is supported at the moment)
 *  @param ssrc: ssrc of this new stream.
 */
bool RTPSession::AddStream( bool receiving, DWORD ssrc )
{
    if ( streamUse.WaitUnusedAndLock(500) )
    {
	RTPStream* stream = getStream(ssrc);
	if ( stream == NULL )
	{
		stream = new RTPStream(this,ssrc);
		
		streams[ssrc]=stream;
		//If remote estimator
		if (remoteRateEstimator)
			//Add stream
			remoteRateEstimator->AddStream(ssrc);
	}
        streamUse.Unlock();
	return true;
    }
    else
    {
        return false;
    }
}

bool RTPSession::DeleteStreams()
{
    streamUse.IncUse();
    for (Streams::iterator it = streams.begin(); it != streams.end(); it++)
    {
	it->second->disabled = true;
        it->second->Cancel();
    }
    streamUse.DecUse();

    streamUse.WaitUnusedAndLock();

    for (Streams::iterator it = streams.begin(); it != streams.end(); it++)
    {
        if (remoteRateEstimator)
	//Add stream
            remoteRateEstimator->RemoveStream(it->first);
        delete it->second;
    }

    streams.clear();

    defaultStream = NULL;
    streamUse.Unlock();
    return true;
}
/**
 * Set the stream designated by SSRC as the defaut stream, if the stream does not exist create it
 *
 * @param: receiving whether it is receving or sending default stream
 * @param: ssrc: ssrc of the stream to be set as default
 *
 **/
 
bool RTPSession::SetDefaultStream(bool receiving, DWORD ssrc )
{
	AddStream(receiving,ssrc);
	defaultStream = getStream(ssrc);

	return true;
}



/**
 *  get the stream associated to the SSRC .
 */
RTPSession::RTPStream* RTPSession::getStream(DWORD ssrc)
{
	//Find stream
	Streams::iterator it = streams.find(ssrc);

	//If not found
	if (it == streams.end())
		//Error
		return NULL;

	//Get the stream
	return it->second;
}

/**
 *  Change the SSRC of an existing stream.
 */
bool RTPSession::ChangeStream( DWORD oldssrc, DWORD newssrc )
{
	RTPStream* stream = getStream(oldssrc);
	streams.erase(oldssrc);
	stream->SetRecSSRC(newssrc);
	streams[newssrc] =stream;

	return true;
}

RTCPCompoundPacket* RTPSession::CreateSenderReport()
{
	timeval tv;

	//Create packet
	RTCPCompoundPacket* rtcp = new RTCPCompoundPacket();

	//Get now
	gettimeofday(&tv, NULL);

	//Create Sender report
	RTCPSenderReport *sr = new RTCPSenderReport();
	
	//Append data
	sr->SetSSRC(sendSSRC );
	sr->SetTimestamp(&tv);
	sr->SetRtpTimestamp(sendLastTime );
	sr->SetOctectsSent(totalSendBytes );
	sr->SetPacketsSent(numSendPackets );

	//Update time of latest sr
	DWORD sinceLastSR = getUpdDifTime(&lastSR);
	
	
	for(Streams::iterator it=streams.begin(); it!=streams.end(); it++)
	{
		RTCPReport *report = NULL;
		
		if (it->second )
		{
			//Create report
			report = it->second->CreateReceiverReport();
		}
		
		if (report != NULL)
		{
			//Append it
			sr->AddReport(report);
		}
		
	}
	

	//Append SR to rtcp
	rtcp->AddRTCPacket(sr);

	//Store last send SR 32 middle bits
	SetSendSR(sr->GetNTPSec() << 16 | sr->GetNTPFrac() >> 16);

	//Create SDES
	RTCPSDES* sdes = new RTCPSDES();

	//Create description
	RTCPSDES::Description *desc = new RTCPSDES::Description();
	//Set ssrc
	desc->SetSSRC(sendSSRC);
	//Add item
	desc->AddItem( new RTCPSDES::Item(RTCPSDES::Item::CName,cname ));

	//Add it
	sdes->AddDescription(desc);

	//Add to rtcp
	rtcp->AddRTCPacket(sdes);

	//Return it
	return rtcp;
}

int RTPSession::SendSenderReport()
{
	//Create rtcp sender retpor
	RTCPCompoundPacket* rtcp = CreateSenderReport();
	DWORD recSSRC =0;
	if (defaultStream != NULL)
		recSSRC = defaultStream->GetRecSSRC();
		
	//If we have not got a notification from latest TMBR
	if (pendingTMBR )
	{
		//Resend TMMBR
		RTCPRTPFeedback *rfb = RTCPRTPFeedback::Create(RTCPRTPFeedback::TempMaxMediaStreamBitrateRequest,sendSSRC ,recSSRC);
		//Limit incoming bitrate
		rfb->AddField( new RTCPRTPFeedback::TempMaxMediaStreamBitrateField(recSSRC,pendingTMBBitrate,0));
		//Add to packet
		rtcp->AddRTCPacket(rfb);
	} else if (remoteRateEstimator && sendBitrateFeedback )	{
		//Get lastest estimation and convert to kbps
		DWORD estimation = remoteRateEstimator->GetEstimatedBitrate();
		//If it was ok
		if (estimation)
		{
			//Resend TMMBR
			RTCPRTPFeedback *rfb = RTCPRTPFeedback::Create(RTCPRTPFeedback::TempMaxMediaStreamBitrateRequest,sendSSRC,recSSRC);
			//Limit incoming bitrate
			rfb->AddField( new RTCPRTPFeedback::TempMaxMediaStreamBitrateField(recSSRC,estimation,0));
			//Add to packet
			rtcp->AddRTCPacket(rfb);
			std::list<DWORD> ssrcs;
			//Get ssrcs
			remoteRateEstimator->GetSSRCs(ssrcs);
			//Create feedback
			// SSRC of media source (32 bits):  Always 0; this is the same convention as in [RFC5104] section 4.2.2.2 (TMMBN).
			RTCPPayloadFeedback *remb = RTCPPayloadFeedback::Create(RTCPPayloadFeedback::ApplicationLayerFeeedbackMessage,sendSSRC,0);
			//Send estimation
			remb->AddField(RTCPPayloadFeedback::ApplicationLayerFeeedbackField::CreateReceiverEstimatedMaxBitrate(ssrcs,estimation));
			//Add to packet
			rtcp->AddRTCPacket(remb);
			Debug("SR: reporting estimated bandwidth of %d to %s", estimation,  inet_ntoa(sendRtcpAddr.sin_addr));
		}
	}
	
	//Send packet
	int ret = SendPacket(*rtcp);

	//Delete it
	delete(rtcp);

	//Exit
	return ret;
}


// Default behavior
void RTPSession::Listener::onNewStream( RTPSession *session, DWORD newSsrc, bool receiving )
{
	if ( ! receiving) return;
	
	
	DWORD oldssrc = session->GetDefaultStream(true);
	
	if ( oldssrc )
	{
		session->ChangeStream( oldssrc, newSsrc );
	}
	else
	{
		session->SetDefaultStream( true, newSsrc );
	}
}

bool RTPSession::RTPStream::Add(RTPTimedPacket *packet, DWORD size)
{

	//Get sec number
	WORD seq = packet->GetSeqNum();
	
	//Check if we have a sequence wrap
	if (seq == 0)
	{
		Log("-RTPSession: we should have a seq wrap. lastSeq=0x%0x, current cycle = %d\n", recExtSeq, recCycles);
	}
	
	if ( seq<0x00FF && (recExtSeq & 0xFFFF) > 0xFF00U)
	{
		//Increase cycles
		recCycles++;
		Log("-RTPSession: seqno wrap. lastSeq=0x%0x, NEW cycle = %d\n", recExtSeq, recCycles);
	}
	//Set cycles
	packet->SetSeqCycles(recCycles);

	//If remote estimator
	if ( s->GetRemoteRateEstimator() )
		//Update rate control
		s->GetRemoteRateEstimator()->Update(recSSRC,packet,getTimeMS());

	//Increase stats
	numRecvPackets++;
	totalRecvPacketsSinceLastSR++;
	totalRecvBytes += size;
	totalRecvBytesSinceLastSR += size;
        recCodec = packet->GetCodec();

	//Get ext seq
	DWORD extSeq = packet->GetExtSeqNum();

	//Check if it is the min for this SR
	if (extSeq<minRecvExtSeqNumSinceLastSR)
		//Store minimum
		minRecvExtSeqNumSinceLastSR = extSeq;

	//If we have a not out of order pacekt
	if (extSeq>recExtSeq)
	{
		//Check possible lost pacekts
		if (recExtSeq && extSeq>(recExtSeq+1))
		{
			//Get number of lost packets
			WORD lost = extSeq-recExtSeq-1;
			//Base packet missing
			WORD base = recExtSeq+1;

			//If remote estimator
			if ( s->GetRemoteRateEstimator() )
				//Update estimator
				s->GetRemoteRateEstimator()->UpdateLost(recSSRC,lost, size);

			//If nack is enable t waiting for a PLI/FIR response (to not oeverflow)
			if (s->IsNACKEnabled() && !s->IsRequestFPU())
			{
				//Double check
				if (lost<0x0FFF)
				{				
				Log("-Nacking %d lost %d\n",base,lost);

				//Generate new RTCP NACK report
				RTCPCompoundPacket* rtcp = new RTCPCompoundPacket();

				//Send them
				while (lost>0)
				{
					//Skip base
					lost--;
					//Get number of lost in the 16 mask
					WORD n = lost;
					//Check nex 16 packets
					if (n>16)
						//Clip
						n = 16;
					//Create mask
					WORD mask = 0xFFFF << (16-n);
					//Create NACK
					RTCPRTPFeedback *nack = RTCPRTPFeedback::Create(RTCPRTPFeedback::NACK,s->GetSendSSRC(),recSSRC);
					//Limit incoming bitrate
					nack->AddField( new RTCPRTPFeedback::NACKField(base,mask));
					//Add to packet
					rtcp->AddRTCPacket(nack);
					//Reduce lost
					lost -= n;
					//Increase base
					base += 16;
				}

				//Send packet
				s->SendPacket(*rtcp);

				//Delete it
				delete(rtcp);
			} else {
					Error("-Weird lost count [lost:%d,base:%d,recExtSeq:%d,recCycles:%d,extSeq:%d,seq:%d]\n",lost,base,recExtSeq,recCycles,extSeq,packet->GetSeqNum());
				}
			}
		}
		
		//Update seq num
		recExtSeq = extSeq;

		//Get diff from previous
		DWORD diff = getUpdDifTime(&recTimeval)/1000;

		//If it is not first one and not from the same frame
		if (recTimestamp && recTimestamp<packet->GetClockTimestamp())
		{
			//Get difference of arravail times
			int d = (packet->GetClockTimestamp()-recTimestamp)-diff;
			//Check negative
			if (d<0)
				//Calc abs
				d = -d;
			//Calculate variance
			int v = d-jitter;
			//Calculate jitter
			jitter += v/16;
		}

		//Update rtp timestamp
		recTimestamp = packet->GetClockTimestamp();
	}

	//Check if we are using fec
	if (s->UseFEC())
	{
		//Append to the FEC decoder
		if (fec.AddPacket(packet))
			//Append only packets with media data
			RTPBuffer::Add(packet);
		//Try to recover
		RTPTimedPacket* recovered = fec.Recover();
		//If we have recovered a pacekt
		while(recovered)
		{
			//Log
			Log("packet recovered!!!!\n");
			//Overwrite time with the time of the original 
			recovered->SetTime(packet->GetTime());
			//Get pacekte type
			BYTE t = recovered->GetType();
			//Map receovered data codec
			BYTE c = s->GetRtpMapIn()->GetCodecForType(t);
			//Check codec
			if (c!=RTPMap::NotFound)
				//Set codec
				recovered->SetCodec(c);
			else
				//Set type for passtrhought
				recovered->SetCodec(t);
			//add recovered packet
			RTPBuffer::Add(recovered);
			//Try to recover another one (yuhu!)
			recovered = fec.Recover();
		}
	} 
	else 
	{
		//Add packet
		RTPBuffer::Add(packet);
	}
	timeval lastSR = s->GetLastSR();
	
	//Check if we need to send SR
	if (isZeroTime(&lastSR) || getDifTime(&lastSR)>2000000)
		//Send it
		s->SendSenderReport();

	return true;
}

RTCPReport* RTPSession::RTPStream::CreateReceiverReport()
{

	//Create report
	RTCPReport *report = NULL;
	//If we have received somthing
	timeval lastReceivedSR = s->GetLastReceivedSR();
	
	if (totalRecvPacketsSinceLastSR && recExtSeq>=minRecvExtSeqNumSinceLastSR)
	{

		//Get number of total packtes
		DWORD total = recExtSeq - minRecvExtSeqNumSinceLastSR + 1;
		//Calculate lost
		DWORD lostRecvPacketsSinceLastSR = total - totalRecvPacketsSinceLastSR;
		//Add to total lost count
		lostRecvPackets += lostRecvPacketsSinceLastSR;
		//Calculate fraction lost
		DWORD frac = (lostRecvPacketsSinceLastSR*256)/total;

		//Create report
		report = new RTCPReport();

		//Set SSRC of incoming rtp stream
		report->SetSSRC(recSSRC);

		//Check last report time
		if (!isZeroTime(&lastReceivedSR))
			//Get time and update it
			report->SetDelaySinceLastSRMilis(getDifTime(&lastReceivedSR)/1000);
		else
			//No previous SR
			report->SetDelaySinceLastSRMilis(0);
		//Set data
		report->SetLastSR(s->GetRecSR() );
		report->SetFractionLost(frac);
		report->SetLastJitter(jitter);
		report->SetLostCount(lostRecvPackets);
		report->SetLastSeqNum(recExtSeq);

		//Reset data
		totalRecvPacketsSinceLastSR = 0;
		totalRecvBytesSinceLastSR = 0;
		minRecvExtSeqNumSinceLastSR = RTPPacket::MaxExtSeqNum;

		
	}
	return report;
}

void RTPSession::onDTLSSetup(DTLSConnection::Suite suite,BYTE* localMasterKey,DWORD localMasterKeySize,BYTE* remoteMasterKey,DWORD remoteMasterKeySize)
{
	Log("-onDTLSSetup for [%s]\n",MediaFrame::TypeToString(media));

	switch (suite)
	{
		case DTLSConnection::AES_CM_128_HMAC_SHA1_80:
			//Set keys
			SetLocalCryptoSDES("AES_CM_128_HMAC_SHA1_80",localMasterKey,localMasterKeySize);
			SetRemoteCryptoSDES("AES_CM_128_HMAC_SHA1_80",remoteMasterKey,remoteMasterKeySize, 0);
			break;
		case DTLSConnection::AES_CM_128_HMAC_SHA1_32:
			//Set keys
			SetLocalCryptoSDES("AES_CM_128_HMAC_SHA1_32",localMasterKey,localMasterKeySize);
			SetRemoteCryptoSDES("AES_CM_128_HMAC_SHA1_32",remoteMasterKey,remoteMasterKeySize, 0);
			break;
		case DTLSConnection::F8_128_HMAC_SHA1_80:
			//Set keys
			SetLocalCryptoSDES("NULL_CIPHER_HMAC_SHA1_80",localMasterKey,localMasterKeySize);
			SetRemoteCryptoSDES("NULL_CIPHER_HMAC_SHA1_80",remoteMasterKey,remoteMasterKeySize, 0);
			break;
	}
}

bool RTPSession::GetStatistics( DWORD ssrc, MediaStatistics & stats)
{
     memset(&stats, 0, sizeof(stats));
     
    if (! running || rtpMapOut == NULL)
    {
        return false;
    }
		
    stats.numSendPackets = numSendPackets;
    stats.totalSendBytes = totalSendBytes;
	
	if (rtpMapOut->find(sendType) != rtpMapOut->end() )
		stats.sendingCodec = (*rtpMapOut)[sendType];
	else
		stats.sendingCodec = -1;
		
    RTPStream * s =(ssrc > 0) ? getStream(ssrc) : NULL;
    if (s != NULL)
    {
        stats.numRecvPackets = s->GetNumRecvPackets();
        stats.totalRecvBytes = s->GetTotalRecvBytes();
        stats.bwIn = 0;
        stats.lostRecvPackets = s->GetLostRecvPackets();
        stats.receivingCodec = s->GetRecCodec();
    }
    else
    {
        for (Streams::iterator it = streams.begin(); it != streams.end(); it++)
        {
            s = it->second;
			if (s != NULL)	
            {
				stats.numRecvPackets += s->GetNumRecvPackets();
				stats.totalRecvBytes += s->GetTotalRecvBytes();
				stats.bwIn = 0;
				stats.lostRecvPackets += s->GetLostRecvPackets();
			}
        }
        if (defaultStream != NULL)
            stats.receivingCodec = defaultStream->GetRecCodec();
    }
    return true;


}

