/* 
 * File:   Endpoint.cpp
 * Author: Sergio
 * 
 * Created on 7 de septiembre de 2011, 0:59
 */
#include "log.h"
#include "Endpoint.h"
#include "RTPEndpoint.h"
#include "WSEndpoint.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

Endpoint::Endpoint(std::wstring name,bool audioSupported,bool videoSupported,bool textSupported) : eventSource(name)
{
	//Store name
	this->name = name;
	//Nullify

	//If audio
	if (audioSupported)
		//Create endpoint
		ports[MediaFrame::Audio] = std::shared_ptr<Port>(new RTPEndpoint(MediaFrame::Audio));
	//If video
	if (videoSupported)
	{
		//Create endpoint
		ports[MediaFrame::Video] = std::shared_ptr<Port>(new RTPEndpoint(MediaFrame::Video));
		ports2[MediaFrame::Video] = std::shared_ptr<Port>(new RTPEndpoint(MediaFrame::Video, MediaFrame::VIDEO_SLIDES));
	}
	//If video
	if (textSupported)
	{
		ports[MediaFrame::Text] = std::shared_ptr<Port>(new RTPEndpoint(MediaFrame::Text));
	}
	estimator.SetEventSource(&eventSource);
	estimator2.SetEventSource(&eventSource);
}

Endpoint::~Endpoint()
{
}

//Methods
int Endpoint::Init()
{
	for (int i=0; i<3; i++)
	{
	    if (ports[i]) 
	    {
			ports[i]->Init();
			if ( i == MediaFrame::Video && ports[i]->GetTransport() == MediaFrame::RTP ) 
			{
				std::shared_ptr<RTPEndpoint> rtp = std::dynamic_pointer_cast<RTPEndpoint>(ports[i]);
				rtp->SetRemoteRateEstimator(&estimator);
			}
	    }
	    
	    if (ports2[i])
	    {
			ports2[i]->Init();
			if ( i == MediaFrame::Video && ports2[i]->GetTransport() == MediaFrame::RTP) 
			{
				std::shared_ptr<RTPEndpoint> rtp = std::dynamic_pointer_cast<RTPEndpoint>(ports2[i]);
				rtp->SetRemoteRateEstimator(&estimator2);
			}
	    }
	}
	return 0;
}

int Endpoint::End()
{
	Log(">End endpoint\n");

	for (int i=0; i<3; i++)
	{
	    if (ports[i]) 
	    {
		ports[i]->End();
	    }
	    
	    if (ports2[i])
	    {
		ports2[i]->End();
	    }
	}

	Log("<End endpoint\n");
	return 0;
}

RTPEndpoint* Endpoint::GetRTPEndpoint(MediaFrame::Type media, MediaFrame::MediaRole role)
{
    std::shared_ptr<Port> p = GetPort(media, role);

	if ( p && p->GetTransport() == MediaFrame::RTP )
	{
			return (RTPEndpoint*) p.get();
	}
	else
			return NULL;
}

int Endpoint::StartSending(MediaFrame::Type media,char *sendIp,int sendPort,RTPMap& rtpMap, MediaFrame::MediaRole role)
{
	//Get enpoint
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);
	std::shared_ptr<Port> p = GetPort(media, role);
	
	//Check 
	if (!p)
		//Init it
		return Error("No media supported");

	//Stop sending for a while
	p->StopSending();

	//Set remote endpoint
	if (rtp != NULL)
	{
	    if(!rtp->SetRemotePort(sendIp,sendPort))
		//Error
		return Error("Error SetRemotePort\n");

	    //Set sending map
	    rtp->SetSendingRTPMap(rtpMap);
	}

	//And send
	return p->StartSending();
}

int Endpoint::AddICECandidate(MediaFrame::Type media,const char* candidate, MediaFrame::MediaRole role)
{
	//Récupère le flux RTP concerné (audio/vidéo/texte)
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	//Check
	if (!rtp)
		return Error("No media supported for ICE candidate\n");

	//Délègue au flux RTP (parse + éventuelle reconfiguration de la cible d'envoi)
	return rtp->AddICECandidate(candidate);
}

int Endpoint::ArmRTPTimeout(MediaFrame::Type media,DWORD timeoutMs, MediaFrame::MediaRole role)
{
	//Récupère le flux RTP concerné (audio/vidéo/texte)
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	//Check
	if (!rtp)
		return Error("No media supported for RTP timeout arming\n");

	//Arme/désarme le watchdog d'inactivité (gap 5)
	rtp->ArmRTPTimeout(timeoutMs);
	return 1;
}

int Endpoint::StopSending(MediaFrame::Type media, MediaFrame::MediaRole role)
{
	//Get rtp enpoint for media
	std::shared_ptr<Port> p = GetPort(media, role);

	//Check
	if (!p)
		//Init it
		return Error("No media supported");
	
	//Stop sending for a while
	return p->StopSending();
}

int Endpoint::StartReceiving(MediaFrame::Type media,RTPMap& rtpMap, MediaFrame::MediaRole role)
{
	Log("-StartReceiving endpoint [name:%ls,media:%s]\n",name.c_str(),MediaFrame::TypeToString(media));
	
	//Get rtp enpoint for media
	std::shared_ptr<Port> p = GetPort(media, role);

	//Check (avant GetTransport : media non supporté = port NULL)
	if (!p)
		//Init it
		return Error("No media supported");

	switch(p->GetTransport())
	{
		case MediaFrame::RTP:
		{
			RTPEndpoint* rtp = GetRTPEndpoint(media, role);
			//Set map for RTP
			if ( rtp )
			{
				//Re-signalisation offer/answer : arrêter la réception AVANT de
				//changer la map (SetReceivingRTPMap fait delete/new sur rtpMapIn,
				//course avec le thread RX) ; StartReceiving redémarre ensuite
				//avec la map négociée et rend le même port local.
				if (rtp->IsReceiving())
					rtp->StopReceiving();
				//Négociation phase 4 : filtre la map proposée selon les codecs
				//réellement supportés (décision D) et mémorise le fmtp local
				//(params seuls) pour le retour XML-RPC enrichi (§5.2).
				RTPMap accepted;
				p->NegotiateReceiving(rtpMap, accepted);
				rtp->SetReceivingRTPMap(accepted);
			}
			break;
		}
			
		case MediaFrame::WS:
		{
			std::shared_ptr<WSEndpoint> wsp = std::dynamic_pointer_cast<WSEndpoint>(p);
			Log("-StartReceiving WS endpoint\n");
			for (RTPMap::iterator it = rtpMap.begin(); it!=rtpMap.end(); ++it)
			{
				//Is it our codec
				if (it->second== TextCodec::T140RED)
				{
					wsp->SetUseRed(true);
				}
				
				//Is it our codec
				if (it->second==TextCodec::T140)
				{
					//Set it
					wsp->SetPrimaryPayloadType(it->first);
					//and we are done
					continue;
				}
			}
			
			break;
		}
		
		default:
			Error(" Protocol not supported. \n");
			return -1;
	}

	//Start
	if (!p->StartReceiving())
		//Exit
		return Error("Error starting receiving media");

	//Return port
	return p->GetLocalMediaPort();
}

int Endpoint::StopReceiving(MediaFrame::Type media, MediaFrame::MediaRole role)
{
	//Get rtp enpoint for media
	std::shared_ptr<Port> p = GetPort(media, role);

	//Check
	if (!p)
		//Init it
		return Error("No media supported");
	//Start
	return p->StopReceiving();
}

//Attach
int Endpoint::Attach(MediaFrame::Type media, MediaFrame::MediaRole role, const std::shared_ptr<Joinable> & join)
{
	Log("-Endpoint attaching [media:%s]\n",MediaFrame::TypeToString(media));

	//Get rtp enpoint for media
	std::shared_ptr<Port> p = GetPort(media, role);

	//Check
	if (!p)
		//Init it
		return Error("No media supported");
	
	p->Attach(join);
	
	return 1;
}

int Endpoint::Detach(MediaFrame::Type media, MediaFrame::MediaRole role)
{
	Log("-Endpoint detaching [media:%s]\n",MediaFrame::TypeToString(media));

	std::shared_ptr<Port> p = GetPort(media, role);

	//Check
	if (! p)
		//Init it
		return Error("No media supported");

	return p->Detach();
}

std::shared_ptr<Joinable> Endpoint::GetJoinable(MediaFrame::Type media, MediaFrame::MediaRole role)
{
	Log("<Endpoint GetJoinable [media:%s]\n",MediaFrame::TypeToString(media));

	std::shared_ptr<Port> p = GetPort(media, role);

	//Check
	if (!p)
	{
		//Init it
		Error("This media or role is not supported\n");
		return nullptr;
	}

	//Le Port est déjà détenu par shared_ptr (Endpoint::ports[]) : on rend une vue
	//Joinable partageant sa propriété (C-13, lien A).
	return std::static_pointer_cast<Joinable>(p);

}

int Endpoint::GetNegotiatedFmtp(MediaFrame::Type media, MediaFrame::MediaRole role, std::map<int,std::string>& out)
{
	std::shared_ptr<Port> p = GetPort(media, role);

	//Check
	if (!p)
		return Error("No media supported");

	//Copie le fmtp mémorisé lors du dernier StartReceiving (phase 4).
	out = p->GetNegotiatedFmtp();
	return 1;
}


int Endpoint::RequestUpdate(MediaFrame::Type media, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	//Check
	if (rtp) return rtp->RequestUpdate();

	return Error("Unknown media [%d]\n",media);
}

int Endpoint::SetLocalCryptoSDES(MediaFrame::Type media,const char* suite, const char* key, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	//Check
	if (rtp) return rtp->SetLocalCryptoSDES(suite,key);
	
	return Error("Unknown media [%d]\n",media);
}

int Endpoint::SetRemoteCryptoSDES(MediaFrame::Type media,const char* suite, const char* key, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	if (rtp) return rtp->SetRemoteCryptoSDES(suite,key);
	
	return Error("Unknown media [%d]\n",media);
}

int Endpoint::SetRemoteCryptoDTLS(MediaFrame::Type media,const char *setup,const char *hash,const char *fingerprint, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	if (rtp) return rtp->SetRemoteCryptoDTLS(setup,hash,fingerprint);
	
	return Error("Unknown media [%d]\n",media);
	//OK
	return 1;
}

int Endpoint::SetLocalSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	if (rtp) return rtp->SetLocalSTUNCredentials(username,pwd);
	
	return Error("Unknown media [%d]\n",media);
}

int Endpoint::SetRTPProperties(MediaFrame::Type media,const Properties& properties, MediaFrame::MediaRole role)
{
	std::shared_ptr<Port> p = GetPort(media, role);
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	//Route les clés codec.* vers le stockage du négociateur (phase 4, §4.3).
	//RTPSession les ignore ; on les capte ici pour dériver le fmtp local.
	if (p)
		p->StoreCodecProperties(properties);

	//Les clés transport continuent vers RTPSession (qui ignore codec.*).
	if (rtp) return rtp->SetProperties(properties);

	return Error("Unknown media [%d]\n",media);
}

int Endpoint::SetRTPTsTransparency(MediaFrame::Type media, bool transparency, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	if (rtp)
	{
		rtp->SetTsTransparency(transparency);
		return 1;
	}
	
	return Error("Unknown media [%d] - cannot set transparency\n",media);
}

int Endpoint::SetRemoteSTUNCredentials(MediaFrame::Type media,const char* username, const char* pwd, MediaFrame::MediaRole role)
{
	RTPEndpoint* rtp = GetRTPEndpoint(media, role);

	if (rtp) return rtp->SetRemoteSTUNCredentials(username,pwd);
	
	return Error("Unknown media [%d]\n",media);
}

int Endpoint::onNewMediaConnection(MediaFrame::Type media, MediaFrame::MediaRole role,
	                           MediaFrame::MediaProtocol transp, WebSocket * ws )
{
    std::shared_ptr<Port> p;
    
	
    if ( role == MediaFrame::VIDEO_MAIN )
    {
        p = ports[media];
    }
    else if (media == MediaFrame::Video && role != MediaFrame::VIDEO_MAIN)
    {
        p = ports2[media];
    }
    else
    {
        return Error("Invalid media %d / role %d parameters\n", media, role );
    }
    
    if ( p && p->GetTransport() != MediaFrame::WS )
    {
		// Previous port was not a web socket
		Log("Endpoint: replacing port transport=%d for %s by WebSocket\n",
			p->GetTransport(), MediaFrame::TypeToString(media) );

		p->End();
		
		std::shared_ptr<Port> wsp = std::shared_ptr<Port>(new WSEndpoint(media));
		if ( role == MediaFrame::VIDEO_MAIN )
		{
			ports[media] = wsp;
		}
		else if (media == MediaFrame::Video && role != MediaFrame::VIDEO_MAIN)
		{
			ports2[media] = wsp;
		}

		ws->Accept( std::weak_ptr<WSEndpoint>(std::dynamic_pointer_cast<WSEndpoint>(wsp)) );
		return 1;
    }
    else
    {
        // Previous port WAs already a using websocket
		Log("Endpoint: Accepting WebSocket connection for media %s\n",
			 MediaFrame::TypeToString(media) );
		std::shared_ptr<WSEndpoint> wsp = std::dynamic_pointer_cast<WSEndpoint>(p);
		ws->Accept( std::weak_ptr<WSEndpoint>(wsp) );
		return 1;
    }
}

int Endpoint::Port::Attach(const std::shared_ptr<Joinable> & join)
{
    //Déjà attaché à cette même source ?
    if (join == joined.lock())
    {
	return 1;
    }

    //Store new one (lien retour non possédant)
    joined = join;
     //If it is not null
    if (join)
	//Join to the new one
	    join->AddListener((RTPEndpoint*) this);

    //OK
    return 1;
}

int Endpoint::Port::Detach()
{
        //Detach if joined — lock() : ne déréférence pas si la source a disparu
	if (std::shared_ptr<Joinable> j = joined.lock())
		//Remove ourself as listeners
		j->RemoveListener( (RTPEndpoint*) this);
	//Not joined anymore
	joined.reset();
	return 1;
}

void Endpoint::Port::StoreCodecProperties(const Properties& properties)
{
	//Même convention que le MCU (VideoStream::SetRTPProperties) : on ne garde que
	//les clés « codec.<codec>.<param> » et on retire le préfixe « codec. » pour
	//obtenir « <codec>.<param> » (ex. h264.profile-level-id), forme attendue par
	//les générateurs de fmtp de la phase 2 (*Encoder::GetFmtpParams).
	for (Properties::const_iterator it=properties.begin(); it!=properties.end(); ++it)
	{
		if (it->first.compare(0, 6, "codec.")==0)
		{
			std::string key = it->first.substr(6);
			codecProperties[key] = it->second;
		}
	}
}

void Endpoint::Port::NegotiateReceiving(const RTPMap& proposed, RTPMap& acceptedOut)
{
	//Repart d'un état propre à chaque (re)négociation.
	acceptedOut.clear();
	negotiatedFmtp.clear();
	negotiatedProps.clear();

	//RTPMap (BYTE,BYTE) -> map<int,int> attendue par le négociateur (agnostique
	//du RTPMap MCU, cf. phase 3).
	std::map<int,int> in;
	for (RTPMap::const_iterator it=proposed.begin(); it!=proposed.end(); ++it)
		in[it->first] = it->second;

	NegotiationResult result;
	//remoteFmtp ignoré en phase 4 (ingestion = phase 5).
	if (!CodecNegotiator::Negotiate(type, in, codecProperties, NULL, result))
	{
		//Média non négociable (ne devrait pas arriver pour audio/vidéo/texte) :
		//on retombe sur la map proposée telle quelle (comportement historique).
		acceptedOut = proposed;
		return;
	}

	//Reconstruit la RTPMap acceptée (sous-ensemble filtré, décision D).
	for (std::map<int,int>::const_iterator it=result.acceptedMap.begin(); it!=result.acceptedMap.end(); ++it)
		acceptedOut[(BYTE)it->first] = (BYTE)it->second;

	//Mémorise le fmtp par PT et les effectiveProps (réservées à l'encodeur
	//d'émission, phase 5). Contrat XML-RPC (§5.2) : TOUT PT accepté est présent,
	//y compris les codecs SANS fmtp (valeur = chaîne vide "" — PCMU, T140,
	//telephone-event sans plage…). La présence de la clé signale « accepté » ;
	//l'absence signale « filtré/non supporté » (result.codecs ne contient que
	//les codecs retenus). C'est le contrôleur SIP qui déduit les PT acceptés de
	//cette struct, la distinction accepté-sans-fmtp / rejeté doit donc être nette.
	for (std::vector<NegotiatedCodec>::const_iterator it=result.codecs.begin(); it!=result.codecs.end(); ++it)
	{
		negotiatedFmtp[it->payloadType] = it->fmtp;
		negotiatedProps[it->payloadType] = it->effectiveProps;
	}
}

int Endpoint::Port::GetLocalMediaPort()
{
	switch(proto)
	{
		case MediaFrame::RTP:
		{
			RTPEndpoint* rtp = ( RTPEndpoint * ) (this);
			return rtp->GetLocalPort();
		}
			
		case MediaFrame::WS:
		{
			WSEndpoint * wsp = ( WSEndpoint * ) (this);
			return wsp->GetLocalPort();
		}
		
		default:
			Error(" Protocol not supported. \n");
			return -1;
	}
}

char* Endpoint::Port::GetLocalMediaHost()
{
	switch(proto)
	{
		case MediaFrame::WS:
		{
			WSEndpoint * wsp = ( WSEndpoint * ) (this);
			return wsp->GetLocalHost();
		}
		
		default:
			Error(" Protocol %i  not supported. \n",proto);
			return NULL;
	}
}


int Endpoint::ConfigureMediaConnection( MediaFrame::Type media, MediaFrame::MediaRole role, 
				        MediaFrame::MediaProtocol proto, const char * expectedPayload )
{
	std::shared_ptr<Port> p = GetPort(media, role);
	
	if (p != NULL)
	{
	    if ( p->GetTransport() != proto )
	    {
			std::shared_ptr<Port> p2;
			
			switch(proto)
			{
				case MediaFrame::WS:
					Log("Recreating WSEndpoint for media %s\n", MediaFrame::TypeToString(media) );
					p2 = std::shared_ptr<Port>(new WSEndpoint(media));
				    break;
				
				case MediaFrame::RTP:
					Log("Recreating RTPEndpoint for media %s\n", MediaFrame::TypeToString(media) );
					p2 = std::shared_ptr<Port>(new RTPEndpoint(media));
					break;
				
				default:
					return Error("Transport not supported.\n");
			}
			
			if ( role == MediaFrame::VIDEO_MAIN )
			{
				p2->SwitchJoin(p);
				ports[media] = p2;
			}
			else if ( media == MediaFrame::Video && role != MediaFrame::VIDEO_MAIN)
			{
				p2->SwitchJoin(p);
				ports2[MediaFrame::Video] = p2;
			}
			else
			{
				return Error("Invalid media=%d, role=%d .\n", media, role);
			}
			
			return 1;
	    }
	    else
	    {
	        // Already using the right protool
			Log("Already using the right protocol %s\n",MediaFrame::ProtocolToString(proto) );
			return 1;
	    }
	}
	else
	{
		return Error("Invalid media / role.\n");
	}
}



char* Endpoint::GetMediaCandidates( MediaFrame::MediaProtocol protocol , MediaFrame::Type media ) 
{
	
    char url[50];
	//Réglage global du serveur (--public-ip, sinon auto-détectée) : l'adresse
	//annoncée n'est plus redérivée ici à chaque appel, et elle est désormais
	//corrigeable derrière un NAT.
	const char* host = RTPSession::GetAnnouncedIp();
	bool addrfound = (host && *host);

	if (addrfound)
	{
		int port = 0;
		char* wshost = NULL;
		std::shared_ptr<Port> p =  GetPort(media);
		
		if ( p == NULL)
		{
			Error("No such media %s", MediaFrame::TypeToString(media));
			return NULL;
		}
		
		if ( p->GetTransport() != protocol )
		{
			Error ("Media is configured with protocol %s. Cannot get media candidate for protocol %s.\n",MediaFrame::ProtocolToString( p->GetTransport()), MediaFrame::ProtocolToString( protocol));
			return NULL;
		}
		
		port 	= p->GetLocalMediaPort();
		wshost	= p->GetLocalMediaHost();
		
		if ( port == -1)
			return NULL;
		
		if (wshost)
			host=wshost;
			
		if (port > 0)
		{
			sprintf(url, "%s://%s:%d", MediaFrame::ProtocolToString( protocol), host, port );
		}
		else
		{
			sprintf(url, "%s://%s", MediaFrame::ProtocolToString( protocol), host );
		}
		Log("urL = %s\n", url);
		return strdup(url);
	}
	else	
	{
		Error("No address found. \n");
		return NULL;
	}
}


int Endpoint::Port::SwitchJoin(std::shared_ptr<Port> oldPort)
{
    if (oldPort)
	{
	    if (std::shared_ptr<Joinable> oldJoined = oldPort->joined.lock())
	    {
		oldPort->Detach();
		Attach(oldJoined);
	    }
	}
	return 0;
}


Endpoint::Port::~Port()
{
	Detach();
}


int Endpoint::SetEventContextId( MediaFrame::Type media, MediaFrame::MediaRole role, int ctxId )
{
	std::shared_ptr<Port> p = GetPort(media, role);
	if (p)
		p->SetEventContextId(ctxId);
	return 0;
}

 int  Endpoint::SetEventHandler( MediaFrame::Type media, MediaFrame::MediaRole role, int sessionId,	JSR309Manager* jsrManager)
 {
	std::shared_ptr<Port> p = GetPort(media, role);
	if (p)
		p->SetEventHandler(sessionId,jsrManager);
	return 0;
 }


const Endpoint::Statistics * Endpoint::GetStatistics()
{
    stats.clear();

    for (int i=0; i<3; i++)
    {
        MediaFrame::Type m = (MediaFrame::Type) i;
        
		RTPEndpoint * rtp = GetRTPEndpoint(m, MediaFrame::VIDEO_MAIN);
        if ( rtp )
        {
            MediaStatistics statsport;

            rtp->GetStatistics(0, statsport);
            stats[ MediaFrame::TypeToString(m) ] = statsport;
        }
    }
    return &stats;
}
