#ifndef _STATUSHANDLER_H_
#define _STATUSHANDLER_H_
#include <ctime>
#include <string>
#include <vector>
#include "xmlrpcserver.h"

class MCU;
class JSR309Manager;
class SpeechService;

/**
 * /status/general — ce que le serveur sait de lui-même.
 *
 * Deux rendus du MÊME état : JSON pour un contrôleur, texte aligné pour un
 * humain. L'état est collecté UNE fois dans un StatusInfo, puis rendu deux
 * fois : deux collectes indépendantes finiraient par se contredire, et un
 * statut qui se contredit ne sert plus à décider.
 */
class StatusHandler :
	public Handler
{
public:
	struct Info
	{
		// Identité du processus
		std::string	product;
		std::string	version;
		std::string	buildDate;
		std::string	hostname;
		int		pid;
		time_t		startedAt;
		long		uptimeSecs;
		std::string	ffmpegVersion;

		// Capacités média. Les DEUX sens, parce qu'ils ne coïncident pas :
		// ffmpeg décode des codecs qu'il n'encode pas (VP6).
		std::vector<std::string> audioDecode;
		std::vector<std::string> audioEncode;
		std::vector<std::string> videoDecode;
		std::vector<std::string> videoEncode;
		// Le device VAAPI partagé existe-t-il ? C'est une CAPACITÉ, pas un
		// usage : les compteurs ci-dessous disent ce qui s'en sert vraiment.
		bool		vaapi;
		int		videoEncoders;		// encodeurs vidéo ouverts
		int		videoEncodersHw;	// ... dont matériels
		int		videoDecoders;		// décodeurs vidéo ouverts
		int		videoDecodersHw;	// ... dont matériels
		int		hwFallbacks;		// replis GPU -> CPU depuis le démarrage

		// Texte temps réel, par transport
		bool		textRtp;	// T.140 sur RTP (RFC 4103)
		bool		textRtpRedundancy;	// redondance RED du RFC 4103
		bool		textDataChannel;	// T.140 sur data channel (RFC 8865)
		bool		textWebSocket;

		bool		bfcp;		// partage de document/écran

		// Chiffrement du média
		std::vector<std::string> encryptionModes;
		std::vector<std::string> sdesSuites;
		std::string	dtlsSrtpSuite;
		std::string	dtlsFingerprintSha256;

		// Profils d'adressage (addressprofiles.h). Un profil porte DEUX
		// adresses : celle qu'on lie et celle qu'on annonce. C'est ce qui rend
		// un déploiement natté descriptible.
		struct Profile
		{
			std::string	name;
			bool		available;
			bool		isDefault;
			std::string	bindAddress;
			std::string	announcedAddress;
		};
		std::vector<Profile>	profiles;
		std::string		defaultProfile;

		// Réseau et transports
		int		rtpMinPort;
		int		rtpMaxPort;
		std::string	webSocketUrl;
		bool		moteli;
		int		eventQueueExpiresSecs;

		// Charge
		int		conferences;
		int		participants;
		int		mediaSessions;

		// Client du speech-server (SPEC SPEECH-PROCESSOR §5). Sans URL, seuls
		// les relais d'un SpeechProcessor fonctionnent : c'est ce que le
		// contrôleur doit pouvoir lire avant de demander une transcription.
		struct Speech
		{
			bool		configured = false;
			std::string	controlUrl;
			std::string	streamingUrl;
			std::string	account;
			int		asrStreams = 0;	// flux prêts maintenant
			int		ttsStreams = 0;
			long		reopenings = 0;	// flux revenus après une panne
			long		rotations = 0;
			long		failures = 0;
			time_t		lastFailureAt = 0;	// 0 : aucun échec
			std::string	lastFailureFunction;
			std::string	lastFailureReason;
		} speech;
	};

public:
	// Les deux services sont OBSERVÉS, jamais possédés : ils vivent dans main().
	// startedAt : instant de démarrage du processus, seule source de l'uptime.
	StatusHandler(MCU *mcu,JSR309Manager *jsr309,time_t startedAt,int eventQueueExpiresSecs,
		      SpeechService *speech = NULL);

	virtual int ProcessRequest(TRequestInfo *req,TSession * const ses);

	// `now` est injecté : sans cela l'uptime rendrait tout test irreproductible.
	void Collect(Info& info,time_t now);

	static std::string RenderJSON(const Info& info);
	static std::string RenderText(const Info& info);

private:
	MCU		*mcu;
	JSR309Manager	*jsr309;
	SpeechService	*speech;
	time_t		startedAt;
	int		eventQueueExpiresSecs;
};


#endif
