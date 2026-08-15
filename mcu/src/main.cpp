#include "log.h"
#include "xmlrpcserver.h"
#include "xmlhandler.h"
#include "xmlstreaminghandler.h"
#include "websockets.h"
#include "statushandler.h"
#include "mcustatushandler.h"
#include "audiomixer.h"
#include "rtmpserver.h"
#include "mcu.h"
#include "broadcaster.h"
#include "mediagateway.h"
#include "jsr309/JSR309Manager.h"
#include "websocketserver.h"
#include "websockets.h"
#include "jsr309/WSEndpoint.h"
#include "addressprofiles.h"
#include "stunclient.h"
#include <signal.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "amf.h"
#include "dtls.h"
#include "video.h"
#include <openssl/crypto.h>

#ifdef MOTELI
#include "moteli/rabbitmqmcu.h"
#include "moteli/rabbitmqserver.h"
#endif

extern "C" {
#include "libavcodec/avcodec.h"
}
extern XmlHandlerCmd mcuCmdList[];
extern XmlHandlerCmd broadcasterCmdList[];
extern XmlHandlerCmd mediagatewayCmdList[];
extern XmlHandlerCmd jsr309CmdList[];

void log_ffmpeg(void* ptr, int level, const char* fmt, va_list vl)
{
	static int print_prefix = 1;
	char line[1024];


#ifndef MCUDEBUG
	if (level > AV_LOG_ERROR)
		return;
#endif

	//Format the
	av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);

	//Remove buffer errors
	if (strstr(line,"vbv buffer overflow")!=NULL)
		//exit
		return;
	//Log
	Log(line);
}

// ffmpeg >= 4 est nativement thread-safe : le gestionnaire de verrous
// (av_lockmgr_register / lock_ffmpeg) a ete supprime en ffmpeg 5.

// libmedikit route ses Log()/Debug()/Error() internes via des pointeurs de
// fonctions (SetLogFunctions, cf. libmedikit/log.c) ; sans branchement, ses
// Log() et Error() sont perdus (logfile/errfile restent NULL). On ne peut pas
// inclure <medkit/log.h> ici : ses declarations extern "C" Log/Debug/Error
// sont en conflit avec les inline de include/log.h. On redeclare donc juste
// SetLogFunctions et on branche des wrappers au format du mcu ; la sortie
// suit stdout et donc --mcu-log.
extern "C" void SetLogFunctions(int (*dbg)(const char*, va_list),
				int (*log)(const char*, va_list),
				int (*err)(const char*, va_list));

static int MedkitLogCb(const char *msg, va_list ap)
{
	char buf[80];
	printf("[0x%lx][%s][LOG]", (long)pthread_self(), LogFormatDateTime(buf, sizeof(buf)));
	vprintf(msg, ap);
	fflush(stdout);
	return 1;
}

static int MedkitDebugCb(const char *msg, va_list ap)
{
	if (Logger::IsDebugEnabled())
	{
		struct timeval tv;
		gettimeofday(&tv, NULL);
		printf("[0x%lx][%.10ld.%.3ld][DBG]", (long)pthread_self(), (long)tv.tv_sec, (long)tv.tv_usec/1000);
		vprintf(msg, ap);
	}
	return 1;
}

static int MedkitErrorCb(const char *msg, va_list ap)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	printf("[0x%lx][%.10ld.%.3ld][ERR]", (long)pthread_self(), (long)tv.tv_sec, (long)tv.tv_usec/1000);
	vprintf(msg, ap);
	return 0;
}

static XmlRpcServer * gserver = NULL;
static XmlStreamingHandler * geventHandlers[4] = { NULL, NULL, NULL, NULL };


void signing_handler(int sig)
{
    for (int i=0; i<4; i++)
    {
        if ( geventHandlers[i] != NULL) geventHandlers[i]->DestroyAllQueues();
    }
    Log("Stopping mediaserver ....\n");
    if (gserver) gserver->Stop();
}


// OpenSSL >= 1.1.0 gere son verrouillage en interne : les callbacks
// CRYPTO_set_locking_callback / CRYPTO_set_id_callback ont ete supprimes
// (macros no-op sous OpenSSL 3.x). L'ancien gestionnaire de verrous
// (init_locks / kill_locks) n'est donc plus necessaire.

int main(int argc,char **argv)
{
	//Brancher les logs de libmedikit sur ceux du mcu (stdout -> --mcu-log)
	SetLogFunctions(MedkitDebugCb, MedkitLogCb, MedkitErrorCb);

	//Init random
	srand(time(NULL));

	//Init open ssl lib
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

	//Set default values
	bool forking = false;
	int port = 8080;
	char* iface = NULL;
	int   wsPort = 9090;
	char* wsHost = NULL;
	int rtmpPort = 1935;
	int minPort = RTPSession::GetMinPort();
	int maxPort = RTPSession::GetMaxPort();
	//Adresse annoncée dans le SDP : NULL → auto-détectée
	const char* publicIp = NULL;
	const char* natIp        = NULL;
	const char* internalIp[2] = { NULL, NULL };   //une par famille, ordre libre
	int         internalCount = 0;
	const char* defaultProfile = NULL;
	const char* stunIp        = NULL;
	int vadPeriod = 5000;
	//Délai de grâce (s) sans long-poll sur une file d'événements avant
	//destruction de la file et des objets qui en dépendent (0 = désactivé).
	//Commun à toutes les API de contrôle (JSR309 aujourd'hui, MCU à venir).
	int eventQueueExpires = XmlEventQueue::DefaultExpiresSecs;
	const char *logfile = "mcu.log";
	const char *pidfile = "mcu.pid";
#ifdef MOTELI
	const char *queueName = "mcu1";
	const char *cnxString = NULL;
#endif
	const char *crtfile = "/etc/mediaserver/mcu.crt";
	const char *keyfile = "/etc/mediaserver/mcu.key";
	//WebSocket TLS (wss://)
	bool wsSecure = false;
	const char *wsCrtFile = NULL;	//NULL → réutilise crtfile
	const char *wsKeyFile = NULL;	//NULL → réutilise keyfile

	//Get all
	for(int i=1;i<argc;i++)
	{
		//Check options
		if (strcmp(argv[i],"-h")==0 || strcmp(argv[i],"--help")==0)
		{
			//Show usage
			printf("Mediaserver version %s %s\r\n",MCUVERSION,MCUDATE);
			printf("Usage: mcu [-h] [--help] [--mcu-log logfile] [--mcu-pid pidfile] [--http-port port] [--rtmp-port port] [--min-rtp-port port] [--max-rtp-port port] [--public-ip ip] [--vad-period ms]\r\n\r\n"
				"Options:\r\n"
				" -h,--help        Print help\r\n"
				" -f               Run as daemon in safe mode\r\n"
				" -d               Enable debug logging\r\n"
				" --mcu-log        Set mcu log file path (default: mcu.log)\r\n"
				" --mcu-pid        Set mcu pid file path (default: mcu.pid)\r\n"
				" --http-port      Set HTTP xmlrpc api port\r\n"
				" --min-rtp-port   Set min rtp port\r\n"
				" --max-rtp-port   Set max rtp port\r\n"
				" --public-ip      Set the IP address announced in the SDP (c= line and ICE candidates).\r\n"
				"                  Required behind a NAT, where it differs from the bound address;\r\n"
				"                  defaults to the first non-loopback IPv4 address of the host\r\n"
				" --nat            Public IPv4 address seen from outside, when --public-ip carries\r\n"
				"                  the locally bound address of a NATed host (IPv4 only).\r\n"
				"                  \"auto\" discovers it with a STUN server and checks that the NAT\r\n"
				"                  is 1:1 (ports preserved); requires --public-ip <RFC 1918 address>\r\n"
				" --stun-server    STUN server queried by --nat auto, host[:port]\r\n"
				"                  (default: stun.l.google.com:19302)\r\n"
				" --internal-ip    Address of the internal (service) network. Repeatable, at most\r\n"
				"                  once per family; IPv4 must be RFC 1918. Option order is not\r\n"
				"                  significant: the family is deduced from the value\r\n"
				" --default-profile\r\n"
				"                  Addressing profile used by a call that requests none:\r\n"
				"                  publicv4 (default), publicv6, internalv4, internalv6\r\n"
				" --rtmp-port      Set RTMP port\r\n"
				" --websocket-port Set WebSocket server port \r\n"
				" --websocket-secure Enable secure WebSocket (wss://)\r\n"
				" --websocket-cert Certificate file (PEM) for wss:// (default: mcu.crt; implies --websocket-secure)\r\n"
				" --websocket-key  Private key file (PEM) for wss:// (default: mcu.key; implies --websocket-secure)\r\n"
				" --vad-period     Set the VAD based conference change period in milliseconds\r\n"
				" --event-queue-expires\r\n"
				"                  Grace period, in seconds, before an event queue with no\r\n"
				"                  long-poll client is destroyed together with the media sessions\r\n"
				"                  bound to it (client liveness is proven by its long-poll).\r\n"
				"                  0 disables the cleanup (default: 60)\r\n");
#ifdef MOTELI
			printf("usage for rabbit MQ connector:\r\n"
			       "mcu --rq-queue queueName --rq-host user:passwd@host:port/vhost\n"
			       "Option:\n"
			       " --rq-queue	name of public rabbit MQ to bind to.\n"
			       " --rq-host	user:passwd user and password to connect to Rabbit MQ.\n"
			       "		host: DNS name of system running rabbit MQ brooker.\n"
			       "		vhost: virtual host to use.\n");
#endif 
				
			//Exit
			return 0;
		} else if (strcmp(argv[i],"-f")==0)
			//Fork
			forking = true;
		else if (strcmp(argv[i],"--http-port")==0 && (i+1<argc))
			//Get port
			port = atoi(argv[++i]);
		else if (strcmp(argv[i],"-d")==0)
			//Enable debug
			Logger::EnableDebug(true);
		else if (strcmp(argv[i],"--rtmp-port")==0 && (i+1<argc))
			//Get rtmp port
			rtmpPort = atoi(argv[++i]);
		else if (strcmp(argv[i],"--websocket-port")==0 && (i+1<argc))
			//Get port
			wsPort = atoi(argv[++i]);
		else if (strcmp(argv[i],"--min-rtp-port")==0 && (i+1<argc))
			//Get rtmp port
			minPort = atoi(argv[++i]);
		else if (strcmp(argv[i],"--max-rtp-port")==0 && (i+1<argc))
			//Get rtmp port
			maxPort = atoi(argv[++i]);
		else if (strcmp(argv[i],"--public-ip")==0 && (i+1<argc))
			//Get the IP to announce in the SDP
			publicIp = argv[++i];
		else if (strcmp(argv[i],"--nat")==0 && (i+1<argc))
			//Adresse publique vue de l'exterieur, quand --public-ip porte
			//l'adresse locale d'un hote natte (v4 uniquement)
			natIp = argv[++i];
		else if (strcmp(argv[i],"--internal-ip")==0 && (i+1<argc))
		{
			//Repetable, au plus une fois par famille : la famille se deduit
			//de la valeur, l'ordre des options n'est pas significatif
			if (internalCount < 2)
				internalIp[internalCount++] = argv[++i];
			else
				++i;
		}
		else if (strcmp(argv[i],"--stun-server")==0 && (i+1<argc))
			//Serveur STUN interroge par --nat auto (hote[:port])
			stunIp = argv[++i];
		else if (strcmp(argv[i],"--default-profile")==0 && (i+1<argc))
			//Profil employe par un appel qui n'en demande aucun
			defaultProfile = argv[++i];
		else if (strcmp(argv[i],"--mcu-log")==0 && (i+1<argc))
			//Get rtmp port
			logfile = argv[++i];
		else if (strcmp(argv[i],"--mcu-pid")==0 && (i+1<argc))
			//Get rtmp port
			pidfile = argv[++i];
		else if (strcmp(argv[i],"--vad-period")==0 && (i+1<=argc))
			//Get rtmp port
			vadPeriod = atoi(argv[++i]);
		else if (strcmp(argv[i],"--event-queue-expires")==0 && (i+1<argc))
			//Délai de grâce sans long-poll (0 = désactive le nettoyage)
			eventQueueExpires = atoi(argv[++i]);
		else if (strcmp(argv[i],"--websocket-host")==0 && (i+1<argc))
			//Get host
			wsHost = argv[++i];
		else if (strcmp(argv[i],"--websocket-secure")==0)
			//Enable secure WebSocket (wss://)
			wsSecure = true;
		else if (strcmp(argv[i],"--websocket-cert")==0 && (i+1<argc))
			//Certificate (PEM) for wss://
			wsCrtFile = argv[++i];
		else if (strcmp(argv[i],"--websocket-key")==0 && (i+1<argc))
			//Private key (PEM) for wss://
			wsKeyFile = argv[++i];
#ifdef MOTELI
		else if (strcmp(argv[i],"--rq-queue")==0 && (i+1<=argc))		
			queueName = argv[++i];

		else if (strcmp(argv[i],"--rq-host")==0 && (i+1<=argc))		
			cnxString = argv[++i];
#endif /* MOTELI */
	}
	
	//Loop
	while(forking)
	{
		//Create the chld
		pid_t pid = fork();
		// fork error
		if (pid<0) exit(1);
		// parent exits
		if (pid>0) exit(0);

		//Log
		printf("MCU started\r\n");
		
		//Create the safe child
		pid = fork();

		//Check pid
		if (pid==0)
		{
			//It is the child obtain a new process group
			setsid();
			//for each descriptor opened
			for (int i=getdtablesize();i>=0;--i)
				//Close it
				close(i);
			//Redirect stdout and stderr
			int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			dup(fd);
			dup2(1,2);
			close(fd);
			//And continule
			break;
		} else if (pid<0)
			//Error
			return 0;

		//Pid string
		char spid[16];
		//Print it
		sprintf(spid,"%d",pid);

		//Write pid to file
		int pfd = open(pidfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		//Write it
		write(pfd,spid,strlen(spid));
		//Close it
		close(pfd);

		int status;

		do
		{
			//Wait for child
			if (waitpid(pid, &status, WUNTRACED | WCONTINUED)<0)
				return -1;
			//If it has exited or stopped
			if (WIFEXITED(status) || WIFSTOPPED(status))
				//Exit
				return 0;
			//If we have been  killed
			if (WIFSIGNALED(status) && WTERMSIG(status)==9)
				//Exit
				return 0;
		} while (!WIFEXITED(status) && !WIFSIGNALED(status));
	}

	//Dump core on fault
	rlimit l = {RLIM_INFINITY,RLIM_INFINITY};
	//Set new limit
        setrlimit(RLIMIT_CORE, &l);

	//Set log level
	av_log_set_callback(log_ffmpeg);

	//Ignore SIGPIPE
	signal( SIGPIPE, SIG_IGN );
	signal( SIGINT, signing_handler );
	signal( SIGTERM, signing_handler );
	//Hack to allocate fd =0 and avoid bug closure
	int fdzero = socket(AF_INET, SOCK_STREAM, 0);

	//Create servers
	XmlRpcServer	server(port);
	RTMPServer	rtmpServer;
	WebSocketServer wsServer;

	//Log version
	Log("-MCU Version %s %s\r\n",MCUVERSION,MCUDATE);
        gserver = &server;

	//Accélération matérielle : sonde (et crée si possible) le device VAAPI
	//partagé une bonne fois au démarrage — le même device que les décodeurs,
	//les encodeurs et le graphe de composition des mosaïques utiliseront.
	//Le verdict est ainsi visible en tête de log plutôt que découvert au
	//premier appel.
	if (Pict::GetVAAPIDevice())
		Log("-Acceleration materielle VAAPI DISPONIBLE : decodage/encodage/composition video sur GPU actives (repli CPU automatique au cas par cas)\n");
	else
		Log("-Acceleration materielle VAAPI INDISPONIBLE : tout le traitement video se fera sur CPU\n");

	//Table des profils d'adressage (ipv6.md §14) : ce que le serveur peut lier,
	//et ce qu'il annonce. Construite ici, avant toute initialisation de serveur —
	//sans adresse annonçable aucun SDP joignable ne peut être publié, donc chaque
	//appel échouerait à la réponse. Un refus de démarrer est la panne honnête,
	//visible tout de suite et au bon endroit, plutôt qu'un serveur en apparence
	//sain qui casse appel par appel.
	{
		std::string error;

		//Le contrôleur a-t-il dit quelque chose de l'adressage ?
		const bool explicitAddressing = (publicIp && *publicIp) || internalCount > 0;

		//--public-ip : littéral v4/v6 ou nom d'hôte. SetAnnouncedIp valide,
		//résout et journalise ; la table reprend son résultat canonique.
		if (publicIp && *publicIp)
		{
			if (!RTPSession::SetAnnouncedIp(publicIp))
			{
				Error("-MCU cannot start: --public-ip \"%s\" is not a usable address.\n",publicIp);
				return -1;
			}

			if (!AddressProfiles::AddPublic(IPAddress::Parse(RTPSession::GetAnnouncedIp()),error))
			{
				Error("-MCU cannot start: --public-ip: %s\n",error.c_str());
				return -1;
			}
		}

		for (int i=0;i<internalCount;++i)
		{
			if (!AddressProfiles::AddInternal(IPAddress::Parse(internalIp[i]),error))
			{
				Error("-MCU cannot start: --internal-ip %s: %s\n",internalIp[i],error.c_str());
				return -1;
			}
		}

		//AUCUNE adresse demandée : on détecte la nôtre — la première adresse
		//annonçable du nom d'hôte, qui peut parfaitement être une RFC 1918. Elle
		//devient le profil public : « public » désigne ici le côté extérieur du
		//serveur, pas la classe de l'adresse (§14.5). Aucune détection de NAT
		//dans ce cas : rien ne dit qu'il y en a un, et deviner l'adresse vue de
		//l'extérieur sans que personne ne l'ait demandé serait une initiative
		//que l'exploitant n'a pas prise.
		if (!explicitAddressing)
		{
			const char* detected = RTPSession::GetAnnouncedIp();

			if (!detected || !*detected)
			{
				char hostname[HOST_NAME_MAX];

				//Le nom qu'on a tenté de résoudre fait partie du diagnostic
				if (gethostname(hostname, sizeof hostname)!=0)
					strcpy(hostname,"(unknown)");

				Error("-MCU cannot start: no IP address to announce in the SDP.\n"
				      "  The c= line and the ICE candidates of every call need one, and it cannot be\n"
				      "  guessed from the control channel.\n"
				      "  Host name \"%s\" does not resolve to an announceable address.\n"
				      "  Fix it with one of:\n"
				      "    - pass --public-ip <ip> (mandatory behind a NAT: the address the peers reach,\n"
				      "      which is not the one bound locally),\n"
				      "    - pass --internal-ip <ip> if this server only serves an internal network,\n"
				      "    - or make \"%s\" resolve to the host address (/etc/hosts or DNS).\n",
				      hostname,hostname);

				//On ne démarre pas
				return -1;
			}

			Log("-RTPSession announced IP auto-detected as \"%s\"\n",detected);

			if (!AddressProfiles::AddPublic(IPAddress::Parse(detected),error))
			{
				Error("-MCU cannot start: auto-detected address: %s\n",error.c_str());
				return -1;
			}
		}

		//--nat : l'adresse vue de l'extérieur. « auto » la DÉCOUVRE par STUN, et
		//vérifie au passage que le NAT est bien 1:1 — sans quoi les ports RTP
		//annoncés dans nos SDP seraient faux (voir stunclient.h).
		if (natIp && strcasecmp(natIp,"auto")==0)
		{
			const IPAddress local = AddressProfiles::BindAddress(AddressProfiles::PublicV4);

			//Réservé au cas qu'il sert : une adresse privée v4 réellement
			//attachée. Sur une adresse publique il n'y a rien à découvrir ; sans
			//--public-ip il n'y a pas de socket à sonder depuis la bonne
			//interface, et le résultat vaudrait pour n'importe quel chemin.
			if (!(publicIp && *publicIp) || !local.IsSet() || !local.IsPrivateV4())
			{
				Error("-MCU cannot start: --nat auto requires --public-ip <adresse RFC 1918 attachee a l'hote>.\n"
				      "  C'est l'adresse locale depuis laquelle le serveur STUN est interroge ;\n"
				      "  sur une adresse publique il n'y a pas de NAT a decouvrir.\n");
				return -1;
			}

			IPEndpoint stunServer;
			if (!StunClient::ParseServer(stunIp ? stunIp : StunClient::DefaultServer(),stunServer,error))
			{
				Error("-MCU cannot start: --stun-server: %s\n",error.c_str());
				return -1;
			}

			Log("-NAT auto: interrogation du serveur STUN %s depuis %s\n",
			    stunServer.ToString().c_str(),local.ToString().c_str());

			IPAddress discovered;
			bool      oneToOne = false;

			if (!StunClient::Discover(local,stunServer,discovered,oneToOne,error))
			{
				Error("-MCU cannot start: --nat auto: %s\n",error.c_str());
				return -1;
			}

			//Le NAT translate les ports : l'adresse est bonne, mais les ports
			//RTP que nous annoncerions ne seraient pas ceux que le pair doit
			//joindre. Annoncer quand même produirait des appels muets, sans un
			//mot dans le log — refuser est la seule reponse honnete.
			if (!oneToOne)
			{
				Error("-MCU cannot start: --nat auto: adresse publique %s decouverte, mais %s\n"
				      "  Un NAT qui translate les ports rend faux TOUS les ports RTP annonces.\n"
				      "  Configurer le routeur en NAT 1:1, ou passer --nat <adresse> en connaissance de cause.\n",
				      discovered.ToString().c_str(),error.c_str());
				return -1;
			}

			Log("-NAT auto: adresse publique %s, NAT 1:1 confirme (ports conserves)\n",
			    discovered.ToString().c_str());

			if (!AddressProfiles::SetNat(discovered,error))
			{
				Error("-MCU cannot start: --nat auto: %s\n",error.c_str());
				return -1;
			}
		}
		else if (natIp && !AddressProfiles::SetNat(IPAddress::Parse(natIp),error))
		{
			Error("-MCU cannot start: --nat: %s\n",error.c_str());
			return -1;
		}

		if (defaultProfile)
		{
			AddressProfiles::Id id;

			if (!AddressProfiles::ParseId(defaultProfile,id))
			{
				Error("-MCU cannot start: --default-profile: profil inconnu \"%s\"\n"
				      "  Valeurs acceptees : publicv4, publicv6, internalv4, internalv6\n",
				      defaultProfile);
				return -1;
			}

			if (!AddressProfiles::SetDefault(id,error))
			{
				Error("-MCU cannot start: --default-profile: %s\n",error.c_str());
				return -1;
			}
		}

		//Contrôles croisés : c'est ici que --nat sans --public-ip v4, ou un
		//profil par défaut indisponible, font échouer le démarrage. L'ordre des
		//options n'a donc aucune importance.
		if (!AddressProfiles::Freeze(error))
		{
			Error("-MCU cannot start: adressage: %s\n",error.c_str());
			return -1;
		}

		//Le profil par défaut décide de l'adresse annoncée par les appels qui
		//n'en demandent aucun : les deux sources restent alignées.
		const IPAddress announced = AddressProfiles::AnnouncedAddress(AddressProfiles::Default());
		if (announced.IsSet())
			RTPSession::SetAnnouncedIp(announced.ToString().c_str());

		Log("-Profils d'adressage :\n%s",AddressProfiles::Describe().c_str());
	}



	//Set DTLS certificate
	DTLSConnection::SetCertificate(crtfile,keyfile);
	//Log
	Log("-Set SSL certificate files [crt:\"%s\",key:\"%s\"]\n",crtfile,keyfile);

	//Init DTLS
	if (DTLSConnection::ClassInit()) {
	//Print hashes
		Log("-DTLS SHA1   local fingerprint \"%s\"\n",DTLSConnection::GetCertificateFingerPrint(DTLSConnection::SHA1).c_str());
		Log("-DTLS SHA256 local fingerprint \"%s\"\n",DTLSConnection::GetCertificateFingerPrint(DTLSConnection::SHA256).c_str());
	}
	// DTLS not available.
	else {
		Error("DTLS initialization failed, no DTLS available\n");
	}
	//
	//Create services
	MCU		mcu;
	Broadcaster	broadcaster;
	MediaGateway	mediaGateway;
	JSR309Manager	jsr309Manager;

#ifdef MOTELI
	McuRabbitServer rqServer(cnxString, queueName);
	McuRabbitHandler rqHandler(&mcu);
#endif

	//Create xml cmd handlers for the mcu and broadcaster
	XmlHandler xmlrpcmcu(mcuCmdList,(void*)&mcu);
	XmlHandler xmlrpcbroadcaster(broadcasterCmdList,(void*)&broadcaster);
	XmlHandler xmlrpcmediagateway(mediagatewayCmdList,(void*)&mediaGateway);
	XmlHandler xmlrpcjsr309(jsr309CmdList,(void*)&jsr309Manager);

	//Create upload handlers
	UploadHandler uploadermcu(&mcu);

	McuStatusHandler mcustatus(&mcu);

	//Create http streaming for service events
	XmlStreamingHandler xmleventjsr309;
	XmlStreamingHandler xmleventmcu;
	XmlStreamingHandler xmleventmediaGateway;

	geventHandlers[0] = &xmleventjsr309;
	geventHandlers[1] = &xmleventmcu;
	geventHandlers[2] = &xmleventmediaGateway;
	
	//And default status hanlder
	StatusHandler status;

	//Init de mcu
	mcu.Init(&xmleventmcu,eventQueueExpires);
	//Init the broadcaster
	broadcaster.Init();
	//Init the media gateway
	mediaGateway.Init(&xmleventmediaGateway);
	
	//INit the jsr309
	jsr309Manager.Init(&xmleventjsr309,eventQueueExpires);

	//Add the rtmp application from the mcu to the rtmp server
	rtmpServer.AddApplication(L"mcu/",&mcu);
	rtmpServer.AddApplication(L"mcutag/",&mcu);
	//Add the rtmp applications from the broadcaster to the rmtp server
	rtmpServer.AddApplication(L"broadcaster/publish",&broadcaster);
	rtmpServer.AddApplication(L"broadcaster",&broadcaster);
	rtmpServer.AddApplication(L"streamer/mp4",&broadcaster);
	rtmpServer.AddApplication(L"streamer/flv",&broadcaster);
	//Add the rtmp applications from the media gateway
	rtmpServer.AddApplication(L"bridge/",&mediaGateway);
	
	//Append mcu cmd handler to the http server
	server.AddHandler("/mcu",&xmlrpcmcu);
	server.AddHandler("/broadcaster",&xmlrpcbroadcaster);
	server.AddHandler("/mediagateway",&xmlrpcmediagateway);
	server.AddHandler("/jsr309",&xmlrpcjsr309);
	server.AddHandler(JSR309_EVENTS_PREFIX,&xmleventjsr309);
	server.AddHandler("/events/mcu",&xmleventmcu);
	server.AddHandler("/events/mediagateway",&xmleventmediaGateway);

	//Add uploaders
	server.AddHandler("/upload/mcu/app/",&uploadermcu);
	
	//Add websocket handlers
	wsServer.AddHandler("/jsr309", &jsr309Manager );
	//S5 : la porte texte-sur-WebSocket de l'API conférence
	//(/mcu/<confId>/<token>). Préfixes disjoints de /jsr309, le routage par
	//préfixe du WebSocketServer les départage sans ambiguïté.
	wsServer.AddHandler("/mcu", &mcu );
	//Add the html status handler
	server.AddHandler("/status/general",&status);
        server.AddHandler("/status/mcu",&mcustatus);
	//Init the rtmp server
	if (! rtmpServer.Init(rtmpPort)) goto server_init_failed;

	//Set port ramge
	if (!RTPSession::SetPortRange(minPort,maxPort))
		//Using default ones
		Log("-RTPSession using default port range [%d,%d]\n",RTPSession::GetMinPort(),RTPSession::GetMaxPort());

	//Set default video mixer vad period
	VideoMixer::SetVADDefaultChangePeriod(vadPeriod);

	//WebSocket TLS : fournir un certificat/clé active automatiquement wss:// ;
	//défauts sur les mêmes fichiers PEM que DTLS (mcu.crt/mcu.key).
	if (wsCrtFile || wsKeyFile) wsSecure = true;
	if (!wsCrtFile) wsCrtFile = crtfile;
	if (!wsKeyFile) wsKeyFile = keyfile;
	if (wsSecure)
	{
		wsServer.SetSecure(true, wsCrtFile, wsKeyFile);
		Log("-WebSocket secure (wss://) enabled [crt:\"%s\",key:\"%s\"]\n", wsCrtFile, wsKeyFile);
	} else {
		Log("-WebSocket in clear mode (ws://)\n");
	}

	//Init web socket server
	if ( ! wsServer.Init(wsPort) ) goto server_init_failed;
	
	if (wsHost)
		WSEndpoint::SetLocalHost(wsHost);

	WSEndpoint::SetLocalPort(wsPort);
	//Le schéma que GetMediaCandidates annoncera : ws:// ou wss://, sur le même port.
	WSEndpoint::SetLocalSecure(wsSecure);

#ifdef MOTELI
	if(cnxString) rqServer.Start(&rqHandler);
#endif
	//Run it
	server.Start();

server_init_failed:
	wsServer.End();
	//End the rtmp server
	rtmpServer.End();
#ifdef MOTELI
	rqServer.Stop();
#endif
	//End the mcu
	mcu.End();
	//End the broadcaster
	broadcaster.End();
	//End the media gateway
	mediaGateway.End();
	//End the jsr309
	jsr309Manager.End();
}

