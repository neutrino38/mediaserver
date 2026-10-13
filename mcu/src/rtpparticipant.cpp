/* 
 * File:   rtpparticipant.cpp
 * Author: Sergio
 * 
 * Created on 19 de enero de 2012, 18:41
 */

#include "rtpparticipant.h"
#include "medkit/negotiator.h"

#include <stdio.h>

//Étiquette du réacteur : la trace de tour long (RtpSessionSet::LongTurnUs) ne
//nomme que le groupe, elle doit donc dire de quelle jambe il s'agit.
static std::string PollGroupName(DWORD partId,const char* suffix)
{
	char buffer[64];
	snprintf(buffer,sizeof(buffer),"part-%u-%s",(unsigned)partId,suffix);
	return buffer;
}

RTPParticipant::RTPParticipant(DWORD partId,const std::wstring &tag,bool bundle) :
	Participant(Participant::RTP,partId),
	mediaGroup(PollGroupName(partId,"media").c_str()),
	videoGroup(PollGroupName(partId,"video").c_str()),
	audio(this),
	text(NULL),
	eventSource(tag),
	nextVideoRole(MediaFrame::VIDEO_EXTRA),
	videosEnded(false),
	bundle(bundle)
{
	Log("-RTPParticipant [id:%d,tag:%ls,bundle:%d]\n",partId,tag.c_str(),bundle);
	videos[MediaFrame::VIDEO_MAIN]		= std::make_shared<VideoStream>(this,logo);
	videos[MediaFrame::VIDEO_SLIDES]	= std::make_shared<VideoStream>(this,logo,MediaFrame::VIDEO_SLIDES);
	estimator.SetEventSource(&eventSource);
}

RTPParticipant::~RTPParticipant()
{
	//Défense en profondeur (H-5, modèle ~RTMPParticipant) : arrête/joint tous
	//les threads même si l'appelant a oublié d'appeler End(). End() est
	//idempotent (DestroyParticipant/MultiConf::End l'appellent déjà).
	End();

	//Avant les membres : un VideoStream se retire de `estimator` en mourant.
	videos.clear();
}

std::shared_ptr<VideoStream> RTPParticipant::GetVideo(MediaFrame::MediaRole role)
{
	std::lock_guard<std::mutex> lock(videosMutex);
	VideoStreams::iterator it = videos.find(role);
	return it != videos.end() ? it->second : std::shared_ptr<VideoStream>();
}

bool RTPParticipant::HasVideoStream(MediaFrame::MediaRole role)
{
	return (bool)GetVideo(role);
}

static int NoVideoStream(const char* method,int partId,MediaFrame::MediaRole role)
{
	return Error("-RTPParticipant::%s: no video stream [partId:%d,role:%d]\n",method,partId,(int)role);
}

int RTPParticipant::SetVideoInput(VideoInput* input,MediaFrame::MediaRole role)
{
	std::shared_ptr<VideoStream> v = GetVideo(role);
	if (!v)
		return NoVideoStream("SetVideoInput",partId,role);
	v->SetVideoInput(input);
	return 1;
}

int RTPParticipant::SetVideoOutput(VideoOutput* output,MediaFrame::MediaRole role)
{
	std::shared_ptr<VideoStream> v = GetVideo(role);
	if (!v)
		return NoVideoStream("SetVideoOutput",partId,role);
	v->SetVideoOutput(output);
	return 1;
}

int RTPParticipant::SetVideoInput(std::shared_ptr<VideoInput> input,MediaFrame::MediaRole role)
{
	std::shared_ptr<VideoStream> v = GetVideo(role);
	if (!v)
		return NoVideoStream("SetVideoInput",partId,role);
	v->SetVideoInput(std::move(input));
	return 1;
}

int RTPParticipant::SetVideoOutput(std::shared_ptr<VideoOutput> output,MediaFrame::MediaRole role)
{
	std::shared_ptr<VideoStream> v = GetVideo(role);
	if (!v)
		return NoVideoStream("SetVideoOutput",partId,role);
	v->SetVideoOutput(std::move(output));
	return 1;
}

VideoOutput* RTPParticipant::GetVideoOutput(MediaFrame::MediaRole role)
{
	std::shared_ptr<VideoStream> v = GetVideo(role);
	return v ? v->GetVideoOutput() : NULL;
}

int RTPParticipant::SetMediaListener(MediaFrame::Listener *listener,MediaFrame::MediaRole role)
{
	std::shared_ptr<VideoStream> v = GetVideo(role);
	return v ? v->SetMediaListener(listener) : NoVideoStream("SetMediaListener",partId,role);
}

int RTPParticipant::SetVideoCodec(VideoCodec::Type codec,int mode,int fps,int bitrate,int intraPeriod,const Properties& properties,MediaFrame::MediaRole role)
{
	//Set it
	{
		std::shared_ptr<VideoStream> v = GetVideo(role);
		return v ? v->SetVideoCodec(codec,mode,fps,bitrate,intraPeriod,properties) : NoVideoStream("SetVideoCodec",partId,role);
	}
}

int RTPParticipant::SetAudioCodec(AudioCodec::Type codec,const Properties& properties)
{
	//Set it
	return audio.SetAudioCodec(codec,properties);
}

int RTPParticipant::SetTextCodec(TextCodec::Type codec)
{
	//Set it
	return text.SetTextCodec(codec);
}

int RTPParticipant::SendVideoFPU(MediaFrame::MediaRole role)
{
	//Send it
	{
		std::shared_ptr<VideoStream> v = GetVideo(role);
		return v ? v->SendFPU() : NoVideoStream("SendFPU",partId,role);
	}
}

int RTPParticipant::SendDTMF(DTMFMessage* dtmf)
{
	//Send it
	return audio.SendDTMF(dtmf);
}

MediaStatistics RTPParticipant::GetStatistics(MediaFrame::Type type,MediaFrame::MediaRole role)
{
        //Depending on the type
        MediaStatistics stats;
        switch (type)
        {
                case MediaFrame::Audio:
                        stats =  audio.GetStatistics();
                        break;

                case MediaFrame::Video:
                {
                        std::shared_ptr<VideoStream> v = GetVideo(role);
                        if (v)
                                stats = v->GetStatistics();
                        break;
                }

                default:
                        stats = text.GetStatistics();
                        break;
        }
        Log("Stat: part %d - media %s - role %s, recv = %d, lost = %d.\n",
                partId, MediaFrame::TypeToString(type),MediaFrame::RoleToString(role),
                stats.numRecvPackets, stats.lostRecvPackets );

        return stats;
}

int RTPParticipant::End()
{
	int ret = 1;

	//En BUNDLE, la session audio porte le transport des autres : elle s'arrête
	//après elles.
	if (!bundle)
	{
		ret &= audio.End();
		ret &= text.End();
	}

	//End all streams (H-3) : SLIDES peut avoir son rtpSession aliasé sur celui de
	//MAIN (cf. onNewStream). On arrête TOUJOURS le flux "observateur" (SLIDES,
	//index le plus haut) AVANT le flux "observé" (MAIN, index 0) — jamais
	//l'inverse — sinon rtp.End() de MAIN ferme les sockets pendant que le
	//recVideoThread de SLIDES lit encore dessus (fd reuse race).
	VideoStreams streams;
	{
		std::lock_guard<std::mutex> lock(videosMutex);
		videosEnded = true;
		streams = videos;
	}
	for(VideoStreams::reverse_iterator it = streams.rbegin(); it != streams.rend(); ++it)
		ret &= it->second->End();

	if (bundle)
	{
		ret &= text.End();
		ret &= audio.End();
	}


	//aggregater results
	return ret;
}

int RTPParticipant::Init()
{
	int ret = 1;
	
	//Lien par défaut de chaque flux vidéo sur SA PROPRE session (H-3) : weak_ptr
	//aliasé sur le bloc de contrôle de ce participant. shared_from_this() est
	//légal ici (Init appelé après make_shared), illégal dans le constructeur.
	auto self = shared_from_this();

	//Les réacteurs de cette jambe (§3.6), posés AVANT les Init() qui inscrivent
	//les sessions : SetPollGroup est refusé après. Démarrés ici et pas dans le
	//constructeur, pour qu'un participant jamais initialisé ne coûte aucun thread.
	mediaGroup.Start();
	videoGroup.Start();

	VideoStreams streams;
	{
		std::lock_guard<std::mutex> lock(videosMutex);
		streams = videos;
	}
	for(VideoStreams::iterator it = streams.begin(); it != streams.end(); ++it)
		ret &= StartVideoStream(*it->second,std::shared_ptr<RTPSession>(self,&it->second->GetOwnSession()));

	audio.GetOwnSession().SetPollGroup(&mediaGroup);
	ret &= audio.Init(audioInput,audioOutput);
	//M-6 : idem via le sous-objet AudioStream::Listener.
	audio.SetWeakListener(std::static_pointer_cast<AudioStream::Listener>(self));
	//text : listener volontairement NULL (cf. constructeur text(NULL)) —
	//comportement historique inchangé, pas de weak listener.
	if (bundle)
		text.GetOwnSession().JoinCarrier(&audio.GetOwnSession());
	else
		text.GetOwnSession().SetPollGroup(&mediaGroup);
	ret &= text.Init(textInput,textOutput);
	//aggregater results
	return ret;
}

int RTPParticipant::StartVideoStream(VideoStream& stream,std::shared_ptr<RTPSession> ownSession)
{
	//Set estimator for video
	stream.SetRemoteRateEstimator(&estimator);
	//Init each stream
	if (bundle)
		stream.GetOwnSession().JoinCarrier(&audio.GetOwnSession());
	else
		stream.GetOwnSession().SetPollGroup(&videoGroup);
	int ret = stream.Init(NULL,NULL);
	//Observe sa propre session par défaut
	stream.SetRTPSession(std::move(ownSession),0);
	//M-6 : arme le listener RTP en weak_ptr. static_pointer_cast obligatoire :
	//RTPParticipant hérite de RTPSession::Listener par DEUX chemins non
	//virtuels (VideoStream::Listener et AudioStream::Listener) → conversion
	//directe ambiguë. On passe par le sous-objet VideoStream::Listener.
	stream.SetWeakListener(std::static_pointer_cast<VideoStream::Listener>(shared_from_this()));
	return ret;
}

int RTPParticipant::CreateVideoStream(std::shared_ptr<VideoInput> input,std::shared_ptr<VideoOutput> output)
{
	int role;
	{
		std::lock_guard<std::mutex> lock(videosMutex);
		if (videosEnded)
			return Error("-RTPParticipant::CreateVideoStream: participant ended [partId:%d]\n",partId);
		role = nextVideoRole++;
	}

	std::shared_ptr<VideoStream> stream = std::make_shared<VideoStream>(this,logo,(MediaFrame::MediaRole)role);
	stream->SetVideoInput(std::move(input));
	stream->SetVideoOutput(std::move(output));
	//Aliasée sur le flux et non sur le participant : la session d'un flux
	//supprimé ne doit rien garder en vie.
	if (!StartVideoStream(*stream,std::shared_ptr<RTPSession>(stream,&stream->GetOwnSession())))
	{
		stream->End();
		return Error("-RTPParticipant::CreateVideoStream: init failed [partId:%d,role:%d]\n",partId,role);
	}

	{
		std::lock_guard<std::mutex> lock(videosMutex);
		if (!videosEnded)
		{
			videos[role] = stream;
			stream.reset();
		}
	}
	//End() est passé pendant l'Init : il n'a pas vu ce flux.
	if (stream)
	{
		stream->End();
		return Error("-RTPParticipant::CreateVideoStream: participant ended [partId:%d]\n",partId);
	}

	Log("-RTPParticipant::CreateVideoStream [partId:%d,role:%d]\n",partId,role);
	return role;
}

int RTPParticipant::DeleteVideoStream(MediaFrame::MediaRole role)
{
	if (role < MediaFrame::VIDEO_EXTRA)
		return Error("-RTPParticipant::DeleteVideoStream: role %d is not deletable [partId:%d]\n",(int)role,partId);

	std::shared_ptr<VideoStream> stream;
	{
		std::lock_guard<std::mutex> lock(videosMutex);
		VideoStreams::iterator it = videos.find(role);
		if (it == videos.end())
			return NoVideoStream("DeleteVideoStream",partId,role);
		stream = it->second;
		videos.erase(it);
	}

	Log("-RTPParticipant::DeleteVideoStream [partId:%d,role:%d]\n",partId,(int)role);
	return stream->End();
}

int RTPParticipant::SetLocalCryptoSDES(MediaFrame::Type media,const char* suite, const char* key,MediaFrame::MediaRole role )
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.SetLocalCryptoSDES(suite,key);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetLocalCryptoSDES(suite,key) : NoVideoStream("SetLocalCryptoSDES",partId,role);
			}
		case MediaFrame::Text:
			return text.SetLocalCryptoSDES(suite,key);
	}

	return 0;
}

int RTPParticipant::SetRemoteCryptoSDES(MediaFrame::Type media,const char* suite, const char* key,MediaFrame::MediaRole role,int keyRank)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.SetRemoteCryptoSDES(suite,key);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetRemoteCryptoSDES(suite,key,keyRank) : NoVideoStream("SetRemoteCryptoSDES",partId,role);
			}
			
		case MediaFrame::Text:
			return text.SetRemoteCryptoSDES(suite,key);
	}

	return 0;
}

int RTPParticipant::SetRemoteCryptoDTLS(MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint,
					MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.SetRemoteCryptoDTLS(setup,hash,fingerprint);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetRemoteCryptoDTLS(setup,hash,fingerprint) : NoVideoStream("SetRemoteCryptoDTLS",partId,role);
			}
		case MediaFrame::Text:
			return text.SetRemoteCryptoDTLS(setup,hash,fingerprint);
		default:
			return Error("Unknown media [%d]\n",media);
	}

	//OK
	return 1;
}


int RTPParticipant::SetLocalSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.SetLocalSTUNCredentials(username,pwd);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetLocalSTUNCredentials(username,pwd) : NoVideoStream("SetLocalSTUNCredentials",partId,role);
			}
		case MediaFrame::Text:
			return text.SetLocalSTUNCredentials(username,pwd);
	}

	return 0;
}
int RTPParticipant::SetRTPProperties(MediaFrame::Type media,const Properties& properties,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.SetRTPProperties(properties);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetRTPProperties(properties) : NoVideoStream("SetRTPProperties",partId,role);
			}
		case MediaFrame::Text:
			return text.SetRTPProperties(properties);
	}

	return 0;
}
int RTPParticipant::SetRemoteSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.SetRemoteSTUNCredentials(username,pwd);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetRemoteSTUNCredentials(username,pwd) : NoVideoStream("SetRemoteSTUNCredentials",partId,role);
			}
		case MediaFrame::Text:
			return text.SetRemoteSTUNCredentials(username,pwd);
	}

	return 0;
}

/***********************************
* SetAddressProfile
*	Le contrôleur dit quel côté du serveur il veut employer ; la session en tire
*	l'adresse à lier et l'adresse à annoncer. Le refus est explicite : un profil
*	indisponible qui retomberait en silence sur le défaut publierait une adresse
*	que le pair ne peut pas joindre — la panne serait vue à l'appel, pas ici.
***********************************/
int RTPParticipant::SetAddressProfile(MediaFrame::Type media,const char* profile,std::string& error,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.GetOwnSession().SetAddressProfile(profile,error) ? 1 : 0;
		case MediaFrame::Video:
		{
			std::shared_ptr<VideoStream> v = GetVideo(role);
			if (!v)
			{
				error = "role video inconnu";
				return 0;
			}
			return v->GetOwnSession().SetAddressProfile(profile,error) ? 1 : 0;
		}
		case MediaFrame::Text:
			return text.GetOwnSession().SetAddressProfile(profile,error) ? 1 : 0;
		default:
			error = "media sans session RTP";
			return 0;
	}
}

IPAddress RTPParticipant::GetAnnouncedAddress(MediaFrame::Type media,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.GetOwnSession().GetAnnouncedAddress();
		case MediaFrame::Video:
		{
			std::shared_ptr<VideoStream> v = GetVideo(role);
			return v ? v->GetOwnSession().GetAnnouncedAddress() : IPAddress();
		}
		case MediaFrame::Text:
			return text.GetOwnSession().GetAnnouncedAddress();
		default:
			return IPAddress();
	}
}

int RTPParticipant::StartSending(MediaFrame::Type media,char *ip, int port,RTPMap& rtpMap,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.StartSending(ip,port,rtpMap);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->StartSending(ip,port,rtpMap) : NoVideoStream("StartSending",partId,role);
			}
		case MediaFrame::Text:
			return text.StartSending(ip,port,rtpMap);
	}

	return 0;
}

int RTPParticipant::StartSending(MediaFrame::Type media,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return 0;
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->StartSending() : NoVideoStream("StartSending",partId,role);
			}
		case MediaFrame::Text:
			return 0;
	}

	return 0;
}

int RTPParticipant::StopSending(MediaFrame::Type media,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.StopSending();
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->StopSending() : NoVideoStream("StopSending",partId,role);
			}
		case MediaFrame::Text:
			return text.StopSending();
	}

	return 0;

}

int RTPParticipant::StartReceiving(MediaFrame::Type media,RTPMap& rtpMap,MediaFrame::MediaRole role)
{
		switch (media)
	{
		case MediaFrame::Audio:
			return audio.StartReceiving(rtpMap);
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->StartReceiving(rtpMap) : NoVideoStream("StartReceiving",partId,role);
			}
		case MediaFrame::Text:
			return text.StartReceiving(rtpMap);
	}

	return 0;
}

/**********************
* StartReceiving (variante negociee) — P8a
*	Delegue la selection des codecs au media serveur : on filtre la map proposee
*	via CodecNegotiator, on installe la map ACCEPTEE au lieu de la proposee, et on
*	remonte au controleur le fmtp par PT accepte.
*
*	Decalque de Endpoint::Port::NegotiateReceiving (chemin JSR-309), a une
*	difference pres : la struct du fil est indexee par PAYLOAD TYPE alors que le
*	negociateur attend le fmtp distant par NOM de codec. La conversion se fait ici
*	parce que c'est le seul endroit ou la rtpMap et le fmtp de l'offre sont tous
*	deux en main.
***********************/
int RTPParticipant::StartReceiving(MediaFrame::Type media,RTPMap& rtpMap,
                                   const std::map<int,std::string>& offerFmtp,
                                   std::map<int,std::string>& negotiatedFmtpOut,
                                   MediaFrame::MediaRole role)
{
	negotiatedFmtpOut.clear();

	//RTPMap (BYTE,BYTE) -> map<int,int> attendue par le negociateur (qui reste
	//agnostique du RTPMap du MCU).
	std::map<int,int> proposed;
	for (RTPMap::const_iterator it=rtpMap.begin(); it!=rtpMap.end(); ++it)
		proposed[it->first] = it->second;

	//fmtp distant : PT -> "pt.<pt>.fmtp", la convention du negociateur (§5.3
	//nego_fmtp.md). Un fmtp portant un PT que l'offre ne propose pas est ignore.
	//
	//La cle est par PAYLOAD TYPE et non par nom de codec, et ce n'est pas un detail :
	//une offre navigateur enumere le meme H.264 sous six ou sept PT pour decrire
	//autant de couples (profil, packetization-mode). Ce code ecrivait
	//"h264.fmtp" dans une boucle sur les PT — le dernier itere gagnait, et les sept
	//PT acceptes repartaient tous avec SON profil. Six d'entre eux decrivaient donc
	//un codec que l'appelant n'avait pas offert (RFC 6184 §8.2.2) : Chrome refuse la
	//reponse entiere et raccroche juste apres l'ACK (capture du 2026-08-06).
	Properties remoteFmtp;
	for (std::map<int,std::string>::const_iterator it=offerFmtp.begin(); it!=offerFmtp.end(); ++it)
	{
		if (proposed.find(it->first) == proposed.end())
			continue;

		char key[32];
		snprintf(key,sizeof(key),"pt.%d.fmtp",it->first);
		remoteFmtp[key] = it->second;
	}

	//Props locales de CE media : ce que SetRTPProperties a retenu, d'ou le
	//negociateur derive le fmtp que nous annoncons. Le texte n'en a pas (le fmtp du
	//T140RED se derive de la seule map proposee), d'ou la map vide.
	static const Properties noProps;
	const Properties* localProps = &noProps;
	std::shared_ptr<VideoStream> v;
	switch (media)
	{
		case MediaFrame::Audio:
			localProps = &audio.GetCodecProperties();
			break;
		case MediaFrame::Video:
			v = GetVideo(role);
			if (!v)
				return Error("StartReceiving: no video stream for role %d\n",(int)role);
			localProps = &v->GetCodecProperties();
			break;
		default:
			break;
	}

	NegotiationResult result;
	if (CodecNegotiator::Negotiate(media,proposed,*localProps,&remoteFmtp,result))
	{
		//Map ACCEPTEE (decision D) : un PT propose non supporte DISPARAIT.
		RTPMap accepted;
		for (std::map<int,int>::const_iterator it=result.acceptedMap.begin(); it!=result.acceptedMap.end(); ++it)
			accepted[(BYTE)it->first] = (BYTE)it->second;

		//Contrat du retour : TOUT PT accepte est une cle, y compris les codecs sans
		//fmtp (valeur vide). La presence de la cle EST le signal d'acceptation.
		for (std::vector<NegotiatedCodec>::const_iterator it=result.codecs.begin(); it!=result.codecs.end(); ++it)
			negotiatedFmtpOut[it->payloadType] = it->fmtp;

		Log("-StartReceiving negotiated %s [partId:%d,proposed:%u,accepted:%u]\n",
		    MediaFrame::TypeToString(media),GetPartId(),
		    (unsigned)proposed.size(),(unsigned)accepted.size());

		//C'est la map filtree qui est installee, et c'est elle que l'appelant doit
		//voir : le controleur construit son SDP depuis le retour, pas depuis ce
		//qu'il avait propose.
		rtpMap = accepted;
	}
	else
	{
		//Media non negociable : repli sur la map proposee telle quelle
		//(comportement historique), et aucun fmtp remonte.
		Log("-StartReceiving %s not negotiable, keeping the proposed map [partId:%d]\n",
		    MediaFrame::TypeToString(media),GetPartId());
	}

	return StartReceiving(media,rtpMap,role);
}

int RTPParticipant::StartReceiving(MediaFrame::Type media,MediaFrame::MediaRole role)
{
		switch (media)
	{
		case MediaFrame::Audio:
			return 0;
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->StartReceiving() : NoVideoStream("StartReceiving",partId,role);
			}
		case MediaFrame::Text:
			return 0;
	}

	return 0;
}

int RTPParticipant::StopReceiving(MediaFrame::Type media,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			return audio.StopReceiving();
		case MediaFrame::Video:
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->StopReceiving() : NoVideoStream("StopReceiving",partId,role);
			}
		case MediaFrame::Text:
			return text.StopReceiving();
	}
	return 0;
}

void RTPParticipant::onFPURequested(RTPSession *session)
{
	//Request it
	if (std::shared_ptr<VideoStream> v = GetVideo(session->GetMediaRole()))
		v->SendFPU();
}


void RTPParticipant::onReceiverEstimatedMaxBitrate(RTPSession *session,DWORD estimation)
{
	//Limit video taking into count max audio
	if (std::shared_ptr<VideoStream> v = GetVideo(session->GetMediaRole()))
		v->SetTemporalBitrateLimit(estimation);
}

void RTPParticipant::onTempMaxMediaStreamBitrateRequest(RTPSession *session,DWORD estimation,DWORD overhead)
{
	//Check which session is
	if (session->GetMediaType()==MediaFrame::Video)
		//Limit video
		if (std::shared_ptr<VideoStream> v = GetVideo(session->GetMediaRole()))
			v->SetTemporalBitrateLimit(estimation);
}

void RTPParticipant::onSenderEstimatedBitrate(RTPSession *session,DWORD estimation)
{
	if (session->GetMediaType()==MediaFrame::Video)
		if (std::shared_ptr<VideoStream> v = GetVideo(session->GetMediaRole()))
			v->SetSenderEstimatedBitrate(estimation);
}

void RTPParticipant::onRequestFPU()
{
	//Check
	if (listener)
		//Call listener
		listener->onRequestFPU(this);
}

void RTPParticipant::onDTMF( DTMFMessage* dtmf)
{
	//Check
	if (listener)
		//Call listener
		listener->onDTMF(this, dtmf);
}

/**********************
* onRTPTimeout / onRTPPacketReceived
*	P7/S1-S2. Le flux RTP d'un media s'est tu, ou son premier paquet vient
*	d'arriver. On se contente de relayer : la politique (BYE, liberation du
*	quota, jonction de la mosaique) est au controleur SIP.
*
*	Ces deux callbacks sont partagees par les trois piles, contrairement a
*	onRequestFPU (VideoStream::Listener) ou onDTMF (AudioStream::Listener) qui
*	sont portees par des interfaces distinctes. C'est la session qui dit de quel
*	media il s'agit.
***********************/
void RTPParticipant::onRTPTimeout( RTPSession *session )
{
	if (!session)
		return;

	Log("-RTPParticipant onRTPTimeout [partId:%d,media:%s,role:%d]\n",
	    GetPartId(),MediaFrame::TypeToString(session->GetMediaType()),(int)session->GetMediaRole());

	//Check
	if (listener)
		//Call listener
		listener->onParticipantMediaTimeout(this,session->GetMediaType(),session->GetMediaRole());
}

void RTPParticipant::onRTPPacketReceived( RTPSession *session )
{
	if (!session)
		return;

	Log("-RTPParticipant onRTPPacketReceived [partId:%d,media:%s,role:%d]\n",
	    GetPartId(),MediaFrame::TypeToString(session->GetMediaType()),(int)session->GetMediaRole());

	//Check
	if (listener)
		//Call listener
		listener->onParticipantMediaConnected(this,session->GetMediaType(),session->GetMediaRole());
}

/**********************
* StartRTPTimeout
*	P7/S1. Arme (timeoutMs > 0) ou desarme (0) le chien de garde d'un media.
*	Le controleur l'arme APRES avoir envoye la reponse SDP, ce qui rend
*	detectable le cas « repondu mais aucun media n'est jamais arrive » sans
*	jamais surveiller la phase de sonnerie.
***********************/
int RTPParticipant::StartRTPTimeout(MediaFrame::Type media,DWORD timeoutMs,MediaFrame::MediaRole role)
{
	switch (media)
	{
		case MediaFrame::Audio:
			audio.ArmRTPTimeout(timeoutMs);
			return 1;
		case MediaFrame::Video:
		{
			std::shared_ptr<VideoStream> v = GetVideo(role);
			if (!v)
				return Error("StartRTPTimeout: no video stream for role %d\n",(int)role);
			v->ArmRTPTimeout(timeoutMs);
			return 1;
		}
		case MediaFrame::Text:
			text.ArmRTPTimeout(timeoutMs);
			return 1;
		default:
			return Error("StartRTPTimeout: unsupported media %d\n",(int)media);
	}
}

int RTPParticipant::SetMute(MediaFrame::Type media, bool isMuted,MediaFrame::MediaRole role)
{
	//Depending on the type
	switch (media)
	{
		case MediaFrame::Audio:
			// audio
			return audio.SetMute(isMuted);
		case MediaFrame::Video:
			//Attach audio
			{
				std::shared_ptr<VideoStream> v = GetVideo(role);
				return v ? v->SetMute(isMuted) : NoVideoStream("SetMute",partId,role);
			}
		case MediaFrame::Text:
			//text
			return text.SetMute(isMuted);
	}
	return 0;
}

// Default behavior
void RTPParticipant::onNewStream( RTPSession *session, DWORD newSsrc, bool receiving )
{
	if ( ! receiving) return;

	Debug("RTPParticipant::onNewStream ssrc=%x\n",newSsrc);
	session->AddStream(receiving,newSsrc);
	
	//Seule une jambe vidéo MAIN ou SLIDES porte un contenu BFCP. L'audio, et un
	//flux de rôle >= 2 qui a sa propre section, suivent la règle ordinaire.
	//Une session jointe ne reçoit que ce que le démultiplexage de sa porteuse
	//lui attribue : le contenu BFCP n'y arrive jamais par un SSRC inconnu.
	if ((GetDocSharingMode() == Participant::BFCP_TCP || GetDocSharingMode() == Participant::BFCP_UDP)
	    && !session->IsJoined()
	    && session->GetMediaType() == MediaFrame::Video
	    && session->GetMediaRole() < MediaFrame::VIDEO_EXTRA)
	{
		session->AddStream(receiving,newSsrc);
		//SLIDES observe la session de MAIN (H-3) : weak_ptr aliasé sur le bloc de
		//contrôle de ce participant (session appartient à ce participant), pour
		//que lock() garde MAIN vivant pendant que SLIDES lit.
		if (std::shared_ptr<VideoStream> slides = GetVideo(MediaFrame::VIDEO_SLIDES))
			slides->SetRTPSession(std::shared_ptr<RTPSession>(shared_from_this(),session),newSsrc);
	}
	else
	{
		DWORD oldssrc = session->GetDefaultStream(true);
		if (oldssrc == 0 ) 
			session->SetDefaultStream(true, newSsrc);
		else
			session->ChangeStream( oldssrc , newSsrc);
	}
	
		
}

int RTPParticipant::DumpInfo(std::string& info)
{
    char partInfo[200];
    MediaStatistics s = GetStatistics(MediaFrame::Audio, MediaFrame::VIDEO_MAIN);

    sprintf(partInfo,
            "  Type=%s, DocSharing=%s.\n"
            "  Audio: nb packets rcved %d, nb packets sent %d, lost packets %d, is_receiving=%d, is_sending=%d\n",
            type == RTP ? "RTP" : "RTMP",
            (docSharingMode == BFCP_TCP || docSharingMode == BFCP_UDP )? "BFCP" : "NONE",
            s.numRecvPackets, s.numSendPackets, s.lostRecvPackets,
            audio.IsReceiving(), audio.IsSending()	);

    info += partInfo;

    s = GetStatistics(MediaFrame::Video, MediaFrame::VIDEO_MAIN);

    sprintf(partInfo,
            "  Video: nb packets rcved %d, nb packets sent %d, lost packets %d, is_receiving=%d, is_sending=%d\n",
            s.numRecvPackets, s.numSendPackets, s.lostRecvPackets,
            GetVideo(MediaFrame::VIDEO_MAIN)->IsReceiving(), GetVideo(MediaFrame::VIDEO_MAIN)->IsSending());

    info += partInfo;

    return 200;
}


