#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <log.h>
#include "statushandler.h"
#include "version.h"
#include "mcu.h"
#include "jsr309/JSR309Manager.h"
#include "jsr309/WSEndpoint.h"
#include "dtls.h"
#include "rtpsession.h"
#include "addressprofiles.h"
#include "audio.h"
#include "video.h"
#include "medkit/codecs.h"

extern "C" {
#include "libavutil/avutil.h"
}

namespace
{

//MCUDATE porte encore l'enveloppe "$Date: ... $" d'un mot-cle CVS : illisible
//pour un humain, et un JSON n'a pas a la transporter.
std::string CleanBuildDate(const char* raw)
{
	std::string date(raw ? raw : "");
	const std::string prefix = "$Date:";

	if (date.compare(0,prefix.size(),prefix) == 0)
		date.erase(0,prefix.size());
	if (!date.empty() && date[date.size()-1] == '$')
		date.erase(date.size()-1);

	//Trim
	const size_t first = date.find_first_not_of(" \t");
	if (first == std::string::npos)
		return "";
	const size_t last = date.find_last_not_of(" \t");

	return date.substr(first,last-first+1);
}

std::string Hostname()
{
	char host[256];

	if (gethostname(host,sizeof(host))!=0)
		return "";
	//gethostname peut ne pas terminer la chaine si le nom remplit le tampon.
	host[sizeof(host)-1] = '\0';

	return host;
}

std::string IsoUtc(time_t t)
{
	struct tm tm;
	char buf[32];

	if (!gmtime_r(&t,&tm))
		return "";
	strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ",&tm);

	return buf;
}

//"3 j 04:12:33" — la forme qu'on lit d'un coup d'oeil.
std::string HumanDuration(long secs)
{
	if (secs < 0)
		secs = 0;

	const long days = secs/86400;
	char buf[64];

	if (days)
		snprintf(buf,sizeof(buf),"%ld j %02ld:%02ld:%02ld",
			days,(secs%86400)/3600,(secs%3600)/60,secs%60);
	else
		snprintf(buf,sizeof(buf),"%02ld:%02ld:%02ld",
			secs/3600,(secs%3600)/60,secs%60);

	return buf;
}

/* ------------------------- Rendu JSON ------------------------- */

std::string JsonString(const std::string& in)
{
	std::string out = "\"";

	for (size_t i=0; i<in.size(); i++)
	{
		const unsigned char c = (unsigned char) in[i];

		switch (c)
		{
			case '"':	out += "\\\"";	break;
			case '\\':	out += "\\\\";	break;
			case '\b':	out += "\\b";	break;
			case '\f':	out += "\\f";	break;
			case '\n':	out += "\\n";	break;
			case '\r':	out += "\\r";	break;
			case '\t':	out += "\\t";	break;
			default:
				if (c < 0x20)
				{
					char esc[8];
					snprintf(esc,sizeof(esc),"\\u%04x",c);
					out += esc;
				}
				else
					out += (char) c;
		}
	}

	return out + "\"";
}

std::string JsonArray(const std::vector<std::string>& items)
{
	std::string out = "[";

	for (size_t i=0; i<items.size(); i++)
	{
		if (i)
			out += ",";
		out += JsonString(items[i]);
	}

	return out + "]";
}

std::string JsonBool(bool v)
{
	return v ? "true" : "false";
}

std::string JsonInt(long v)
{
	char buf[32];
	snprintf(buf,sizeof(buf),"%ld",v);

	return buf;
}

/* ------------------------- Rendu texte ------------------------ */

std::string JoinNames(const std::vector<std::string>& items)
{
	if (items.empty())
		return "aucun";

	std::string out;

	for (size_t i=0; i<items.size(); i++)
	{
		if (i)
			out += " ";
		out += items[i];
	}

	return out;
}

const char* YesNo(bool v)
{
	return v ? "oui" : "non";
}

} // namespace

/**************************************
* StatusHandler
*************************************/
StatusHandler::StatusHandler(MCU *mcu,JSR309Manager *jsr309,time_t startedAt,int eventQueueExpiresSecs) :
	mcu(mcu),
	jsr309(jsr309),
	startedAt(startedAt),
	eventQueueExpiresSecs(eventQueueExpiresSecs)
{
}

/**************************************
* Collect
*	L'unique lecture de l'etat. Les deux rendus en derivent, jamais l'inverse.
*************************************/
void StatusHandler::Collect(Info& info,time_t now)
{
	info.product	= "mediaserver";
	info.version	= MCUVERSION;
	info.buildDate	= CleanBuildDate(MCUDATE);
	info.hostname	= Hostname();
	info.pid	= getpid();
	info.startedAt	= startedAt;
	info.uptimeSecs	= (long) (now - startedAt);
	info.ffmpegVersion = av_version_info() ? av_version_info() : "";

	//Capacites codec : AudioCodecFactory/VideoCodecFactory sont la SEULE autorite
	//(ce que la libmedikit/ffmpeg compilee sait vraiment faire). Les deux sens
	//sont publies separement parce qu'ils ne coincident pas : un contrant qui
	//lit une seule liste finit par demander au serveur d'emettre un codec qu'il
	//ne sait que recevoir.
	info.audioDecode.clear();
	for (AudioCodec::Type t : AudioCodecFactory::GetSupportedCodecs())
		info.audioDecode.push_back(AudioCodec::GetNameFor(t));

	info.audioEncode.clear();
	for (AudioCodec::Type t : AudioCodecFactory::GetSupportedEncoderCodecs())
		info.audioEncode.push_back(AudioCodec::GetNameFor(t));

	info.videoDecode.clear();
	for (VideoCodec::Type t : VideoCodecFactory::GetSupportedCodecs())
		info.videoDecode.push_back(VideoCodec::GetNameFor(t));

	info.videoEncode.clear();
	for (VideoCodec::Type t : VideoCodecFactory::GetSupportedEncoderCodecs())
		info.videoEncode.push_back(VideoCodec::GetNameFor(t));

	//Le meme device VAAPI partage que decodeurs, encodeurs et graphes de
	//composition : la sonde a deja eu lieu au demarrage, on ne fait que la relire.
	info.vaapi = Pict::GetVAAPIDevice() != NULL;

	//Texte temps reel, par transport.
	info.textRtp		= TextCodec::IsSupported(TextCodec::T140);
	info.textRtpRedundancy	= TextCodec::IsSupported(TextCodec::T140RED);
	//RFC 8865 : le canal est du SCTP dans des records DTLS. Sans DTLS utilisable,
	//il n'y a pas de data channel — la dependance est structurelle, pas une option.
	info.textDataChannel	= DTLSConnection::IsAvailable();
	//Le WebSocket ne chiffre ni ne negocie rien du cote texte : la porte est
	//enregistree par main() dans les deux API, donc toujours ouverte.
	info.textWebSocket	= true;

	info.bfcp = AppCodec::IsSupported(AppCodec::BFCP);

	//Chiffrement du media. "none" est toujours la : une patte en clair reste
	//acceptee (SIP sans SDES).
	info.encryptionModes.clear();
	info.encryptionModes.push_back("none");
	info.encryptionModes.push_back("sdes-srtp");
	if (DTLSConnection::IsAvailable())
		info.encryptionModes.push_back("dtls-srtp");

	//Les suites que RTPSession::SetLocalCryptoSDES accepte reellement.
	info.sdesSuites.clear();
	info.sdesSuites.push_back("AES_CM_128_HMAC_SHA1_80");
	info.sdesSuites.push_back("AES_CM_128_HMAC_SHA1_32");
	info.sdesSuites.push_back("AES_CM_128_NULL_AUTH");
	info.sdesSuites.push_back("NULL_CIPHER_HMAC_SHA1_80");

	//Le DTLS annonce du GCM dans use_srtp, mais l'export de cles est fixe a la
	//longueur AES_CM_128 : c'est bien la seule suite qu'une patte DTLS obtient.
	info.dtlsSrtpSuite = DTLSConnection::IsAvailable() ? "AES_CM_128_HMAC_SHA1_80" : "";
	info.dtlsFingerprintSha256 = DTLSConnection::IsAvailable()
		? DTLSConnection::GetCertificateFingerPrint(DTLSConnection::SHA256)
		: "";

	//Profils d'adressage : la SEULE source de l'adresse annoncee dans le SDP.
	//Les deux adresses de chaque profil sont publiees separement — les confondre
	//rend un deploiement derriere NAT indescriptible. Meme table que celle lue
	//par GetNetworkProfiles et par les jambes RTP, pas une copie.
	info.profiles.clear();
	for (int i=0; i<AddressProfiles::Count; i++)
	{
		const AddressProfiles::Id id = (AddressProfiles::Id) i;
		Info::Profile profile;

		profile.name		= AddressProfiles::NameOf(id);
		profile.available	= AddressProfiles::IsAvailable(id);
		profile.isDefault	= (AddressProfiles::Default() == id);
		profile.bindAddress	= profile.available
			? AddressProfiles::BindAddress(id).ToString() : "";
		profile.announcedAddress = profile.available
			? AddressProfiles::AnnouncedAddress(id).ToString() : "";

		info.profiles.push_back(profile);
	}
	info.defaultProfile = AddressProfiles::NameOf(AddressProfiles::Default());

	info.rtpMinPort	= RTPSession::GetMinPort();
	info.rtpMaxPort	= RTPSession::GetMaxPort();

	{
		//MEME resolution d'hote que MultiConf::ConfigureParticipantMediaConnection,
		//qui signe les URL reellement remises aux participants : l'adresse
		//annoncee par defaut, que --websocket-host surcharge. En rederiver une
		//autre ici publierait une URL que le serveur ne delivre pas.
		const char* host = RTPSession::GetAnnouncedIp();
		if (WSEndpoint::GetLocalHost() && *WSEndpoint::GetLocalHost())
			host = WSEndpoint::GetLocalHost();

		if (!host || !*host)
			//Aucune adresse annoncable : il n'y a pas d'URL, et le dire vide est
			//plus honnete qu'un "ws://:9090" qui a l'air d'en etre une.
			info.webSocketUrl = "";
		else
		{
			char url[512];
			snprintf(url,sizeof(url),"%s://%s:%d",
				WSEndpoint::IsLocalSecure() ? "wss" : "ws",
				host,WSEndpoint::GetLocalPort());
			info.webSocketUrl = url;
		}
	}

#ifdef MOTELI
	info.moteli = true;
#else
	info.moteli = false;
#endif

	//Le delai de grace du long-poll : c'est le contrat de vitalite que le
	//controleur doit respecter pour que ses sessions survivent.
	info.eventQueueExpiresSecs = eventQueueExpiresSecs;

	info.conferences	= 0;
	info.participants	= 0;
	if (mcu)
		mcu->GetLoad(info.conferences,info.participants);

	info.mediaSessions = jsr309 ? jsr309->GetMediaSessionCount() : 0;
}

/**************************************
* RenderJSON
*************************************/
std::string StatusHandler::RenderJSON(const Info& info)
{
	std::string out;

	out += "{\"server\":{";
	out += "\"product\":"	+ JsonString(info.product);
	out += ",\"version\":"	+ JsonString(info.version);
	out += ",\"buildDate\":"+ JsonString(info.buildDate);
	out += ",\"hostname\":"	+ JsonString(info.hostname);
	out += ",\"pid\":"	+ JsonInt(info.pid);
	out += ",\"startedAt\":"+ JsonString(IsoUtc(info.startedAt));
	out += ",\"uptimeSecs\":"+ JsonInt(info.uptimeSecs);
	out += ",\"ffmpeg\":"	+ JsonString(info.ffmpegVersion);
	out += "}";

	out += ",\"capabilities\":{";
	out += "\"audio\":{\"decode\":" + JsonArray(info.audioDecode);
	out += ",\"encode\":"		+ JsonArray(info.audioEncode) + "}";
	out += ",\"video\":{\"decode\":" + JsonArray(info.videoDecode);
	out += ",\"encode\":"		+ JsonArray(info.videoEncode) + "}";
	out += ",\"text\":{";
	out += "\"rfc4103\":"		+ JsonBool(info.textRtp);
	out += ",\"rfc4103Redundancy\":"+ JsonBool(info.textRtpRedundancy);
	out += ",\"rfc8865\":"		+ JsonBool(info.textDataChannel);
	out += ",\"websocket\":"	+ JsonBool(info.textWebSocket);
	out += "}";
	out += ",\"hardware\":{\"vaapi\":" + JsonBool(info.vaapi) + "}";
	out += ",\"bfcp\":"		+ JsonBool(info.bfcp);
	out += "}";

	out += ",\"security\":{";
	out += "\"modes\":"		+ JsonArray(info.encryptionModes);
	out += ",\"sdesSuites\":"	+ JsonArray(info.sdesSuites);
	out += ",\"dtls\":{\"available\":" + JsonBool(DTLSConnection::IsAvailable());
	out += ",\"srtpSuite\":"	+ JsonString(info.dtlsSrtpSuite);
	out += ",\"fingerprintSha256\":" + JsonString(info.dtlsFingerprintSha256);
	out += "}";
	out += "}";

	out += ",\"network\":{";
	out += "\"defaultProfile\":" + JsonString(info.defaultProfile);
	out += ",\"profiles\":[";
	for (size_t i=0; i<info.profiles.size(); i++)
	{
		const Info::Profile& p = info.profiles[i];

		if (i)
			out += ",";
		out += "{\"name\":"		+ JsonString(p.name);
		out += ",\"available\":"	+ JsonBool(p.available);
		out += ",\"default\":"	+ JsonBool(p.isDefault);
		out += ",\"bindAddress\":"	+ JsonString(p.bindAddress);
		out += ",\"announcedAddress\":" + JsonString(p.announcedAddress);
		out += "}";
	}
	out += "]";
	out += ",\"rtpPortRange\":{\"min\":" + JsonInt(info.rtpMinPort);
	out += ",\"max\":"		+ JsonInt(info.rtpMaxPort) + "}";
	out += ",\"websocketUrl\":"	+ JsonString(info.webSocketUrl);
	out += ",\"moteli\":"		+ JsonBool(info.moteli);
	out += ",\"eventQueueExpiresSecs\":" + JsonInt(info.eventQueueExpiresSecs);
	out += "}";

	out += ",\"load\":{";
	out += "\"conferences\":"	+ JsonInt(info.conferences);
	out += ",\"participants\":"	+ JsonInt(info.participants);
	out += ",\"mediaSessions\":"	+ JsonInt(info.mediaSessions);
	out += "}}\n";

	return out;
}

/**************************************
* RenderText
*	Le meme etat, pour un humain devant un terminal ou un navigateur.
*************************************/
std::string StatusHandler::RenderText(const Info& info)
{
	char buf[1024];
	std::string out;

	snprintf(buf,sizeof(buf),"%s %s (%s)\n",
		info.product.c_str(),info.version.c_str(),info.buildDate.c_str());
	out += buf;

	snprintf(buf,sizeof(buf),"  hote           %s, pid %d\n",
		info.hostname.c_str(),info.pid);
	out += buf;
	snprintf(buf,sizeof(buf),"  demarre        %s\n",IsoUtc(info.startedAt).c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  uptime         %s (%ld s)\n",
		HumanDuration(info.uptimeSecs).c_str(),info.uptimeSecs);
	out += buf;
	snprintf(buf,sizeof(buf),"  ffmpeg         %s\n",info.ffmpegVersion.c_str());
	out += buf;

	out += "\nCapacites\n";
	snprintf(buf,sizeof(buf),"  audio decode   %s\n",JoinNames(info.audioDecode).c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  audio encode   %s\n",JoinNames(info.audioEncode).c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  video decode   %s\n",JoinNames(info.videoDecode).c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  video encode   %s\n",JoinNames(info.videoEncode).c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  acceleration   VAAPI %s%s\n",
		YesNo(info.vaapi),
		info.vaapi ? "" : " (tout le traitement video sur CPU)");
	out += buf;
	snprintf(buf,sizeof(buf),"  texte          RTP RFC 4103 %s (redondance %s), "
		"data channel RFC 8865 %s, WebSocket %s\n",
		YesNo(info.textRtp),YesNo(info.textRtpRedundancy),
		YesNo(info.textDataChannel),YesNo(info.textWebSocket));
	out += buf;
	snprintf(buf,sizeof(buf),"  BFCP           %s\n",YesNo(info.bfcp));
	out += buf;

	out += "\nChiffrement du media\n";
	snprintf(buf,sizeof(buf),"  modes          %s\n",JoinNames(info.encryptionModes).c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  suites SDES    %s\n",JoinNames(info.sdesSuites).c_str());
	out += buf;
	if (info.dtlsSrtpSuite.empty())
		out += "  DTLS-SRTP      indisponible : certificat ou cle illisible au demarrage\n";
	else
	{
		snprintf(buf,sizeof(buf),"  DTLS-SRTP      disponible, suite %s\n",
			info.dtlsSrtpSuite.c_str());
		out += buf;
		snprintf(buf,sizeof(buf),"  empreinte      SHA-256 %s\n",
			info.dtlsFingerprintSha256.c_str());
		out += buf;
	}

	out += "\nReseau\n";
	snprintf(buf,sizeof(buf),"  ports RTP      %d-%d\n",info.rtpMinPort,info.rtpMaxPort);
	out += buf;
	snprintf(buf,sizeof(buf),"  WebSocket      %s\n",info.webSocketUrl.c_str());
	out += buf;

	//Colonnes explicites : « lie » et « annonce » ne sont egales que hors NAT,
	//et c'est justement l'ecart qu'un exploitant vient verifier ici. Un bind
	//vide n'est pas une panne : c'est l'ecoute sur toutes les interfaces, le cas
	//nominal d'une adresse annoncee qui vit sur le routeur (--nat). Affiche en
	//blanc, il se lirait comme une valeur manquante, d'ou le "*".
	out += "  profils        nom         lie (*=toutes)       annonce\n";
	for (size_t i=0; i<info.profiles.size(); i++)
	{
		const Info::Profile& p = info.profiles[i];

		if (!p.available)
		{
			snprintf(buf,sizeof(buf),"                 %-11s indisponible\n",p.name.c_str());
			out += buf;
			continue;
		}

		snprintf(buf,sizeof(buf),"                 %-11s %-20s %-20s%s\n",
			p.name.c_str(),
			p.bindAddress.empty() ? "*" : p.bindAddress.c_str(),
			p.announcedAddress.c_str(),
			p.isDefault ? " (defaut)" : "");
		out += buf;
	}

	const std::string expires = info.eventQueueExpiresSecs > 0
		? HumanDuration(info.eventQueueExpiresSecs) + " sans long-poll = destruction"
		: std::string("expiration desarmee");
	snprintf(buf,sizeof(buf),"  files evts     %s\n",expires.c_str());
	out += buf;
	snprintf(buf,sizeof(buf),"  MOTELI         %s\n",
		info.moteli ? "compile" : "non compile");
	out += buf;

	out += "\nCharge\n";
	snprintf(buf,sizeof(buf),"  conferences    %d (%d participants)\n",
		info.conferences,info.participants);
	out += buf;
	snprintf(buf,sizeof(buf),"  sessions JSR309 %d\n",info.mediaSessions);
	out += buf;

	return out;
}

/**************************************
* ProcessRequest
*	Procesa una peticion
*************************************/
int StatusHandler::ProcessRequest(TRequestInfo *req,TSession * const ses)
{

	//Debug et non Log : ce point est fait pour etre interroge en boucle par une
	//supervision, deux lignes par appel rempliraient /var/log/mcu.log a elles
	//seules.
	Debug(">ProcessRequest [%s]\n",req->uri);

	int inputLen = 0;
	char * buffer = NULL;

	//Obtenemos el content length
	const char * content_length = RequestHeaderValue(ses, (char*)"content-length");

	//Si no hay 
	if (content_length != NULL)
	{
		//Obtenemos el entero
		inputLen = atoi(content_length);

		//Creamos un buffer para el body
		buffer = (char *) malloc(inputLen);
	}

	//Check lenght
	if (inputLen)
	{
		if (!XmlRpcServer::GetBody(ses,buffer,inputLen))
		{
			//LIberamos el buffer
			free(buffer);

			//Y salimos sin devolver nada
			return Error("Error getting request body\n");
		}
	}

	//Liberamos el buffer
	if (buffer != NULL)
		//Free buffer
		free(buffer);

	//Quel rendu ? "?format=text" et "?format=json" tranchent explicitement.
	//Sans parametre, l'en-tete Accept decide : un navigateur demande text/html
	//et recoit la version lisible, tout le reste (curl, un controleur) recoit
	//du JSON.
	bool wantText = false;

	if (req->query != NULL && strstr(req->query,"format=text") != NULL)
		wantText = true;
	else if (req->query != NULL && strstr(req->query,"format=json") != NULL)
		wantText = false;
	else
	{
		const char* accept = RequestHeaderValue(ses, (char*)"accept");

		if (accept != NULL
		    && strstr(accept,"application/json") == NULL
		    && (strstr(accept,"text/html") != NULL || strstr(accept,"text/plain") != NULL))
			wantText = true;
	}

	Info info;
	Collect(info,time(NULL));

	const std::string body = wantText ? RenderText(info) : RenderJSON(info);

	ResponseContentType(ses, wantText
		? (char*)"text/plain; charset=utf-8"
		: (char*)"application/json; charset=utf-8");

	XmlRpcServer::SendResponse(ses,200,body.c_str(),body.length());

	Debug("<ProccessRequest\n");

	return TRUE;
}
