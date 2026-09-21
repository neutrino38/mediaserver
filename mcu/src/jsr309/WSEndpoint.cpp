#include "WSEndpoint.h"
#include "RTPEndpoint.h"
#include <stdexcept>

static BYTE BOMUTF8[]			= {0xEF,0xBB,0xBF};
//U+FFFD REPLACEMENT CHARACTER : ce que T.140 §5.3 (et RFC 4103 §4.3 pour son
//transport) demande d'insérer dans le flux quand une perte de session est
//détectée — la seule trace qu'un utilisateur ait qu'il manque du texte.
//Non const : RTPPacket::SetPayload prend un BYTE* nu.
static BYTE REPLACEMENT_UTF8[]		= {0xEF,0xBF,0xBD};

int WSEndpoint::wsPort = 0;
char* WSEndpoint::wsHost = NULL;
bool WSEndpoint::wsSecure = false;
WebSocketServer* WSEndpoint::wsServer = NULL;

WSEndpoint::WSEndpoint(MediaFrame::Type type) : Port(type, MediaFrame::WS)
{
    msgType = WebSocket::Binary;
    joined = NULL;
	//_ws : weak_ptr vide par défaut
	RedCodec = new RedundentCodec();
	
    useRed = false;
    pseudoSeqNum = 0;
    wantConnected = false;
    retryId = 0;
    everOpened = false;
    droppedWhileDown = 0;
    //L'origine des horodatages : elle est relue par SendFrame (chemin BOM) AVANT
    //toute ouverture, et un timeval non initialise s'y lisait.
    gettimeofday(&clock,NULL);
    switch (type)
    {
        case MediaFrame::Text:
		{
			media = new TextFrame();
			break;
		}
		default:
			throw new std::logic_error("Unsupported media type\n");
    };
}

void WSEndpoint::onOpen(WebSocket *ws)
{
    std::shared_ptr<WebSocket> old = _ws.lock();
    if ( !old )
    {
	//Première association
	gettimeofday(&clock,NULL);
    }
    else
    {
	//Une connexion existait déjà : on ferme l'ancienne
	old->Close();
    }
    _ws = ws->GetWeakPtr();

    //Jambe cliente qui REVIENT : le texte de la coupure a été jeté, et T.140 §5.3
    //veut que le survivant l'annonce — sans ce caractère, l'utilisateur lirait
    //deux phrases collées sans savoir qu'il en manque une.
    //Une jambe SERVEUR garde son comportement : c'est le navigateur qui revient,
    //et lui n'a rien perdu que nous sachions.
    if (everOpened && IsClientMode())
    {
	Log("WSEndpoint: outgoing leg reconnected to %s (%u text frame(s) lost while down).\n",
	    clientUrl.c_str(), droppedWhileDown);
	droppedWhileDown = 0;
	SendReplacementChar(true);
    }

    everOpened = true;

    //Le contrôleur apprend l'ouverture par la file d'événements JSR-309 : le
    //retour de ConnectMediaConnection, lui, est antérieur (§4.8).
    if (wantConnected)
	PostEvent(new ::EndpointConnectedEvent());

    //Rejouer le texte arrivé avant que le navigateur ne soit là, dans l'ordre et
    //sans les trames trop vieilles pour être encore du dialogue (§4.5).
    if (!pending.empty())
    {
	//Date ABSOLUE, comme celle posee par SendFrame : `clock` vient d'etre remis
	//a zero par la premiere association ci-dessus, donc un age mesure depuis lui
	//declarait perimee TOUTE la file — la premiere phrase de chaque appel.
	const QWORD now = getTimeMS();
	size_t sent = 0, stale = 0;

	for (std::list<std::pair<QWORD,std::string>>::const_iterator it = pending.begin();
	     it != pending.end(); ++it)
	{
	    if (now - it->first > maxPendingAgeMs) { stale++; continue; }
	    ws->SendMessage( it->second );
	    sent++;
	}

	Log("WSEndpoint: replayed %u pending text frame(s) on connect (%u dropped as stale).\n",
	    (unsigned) sent, (unsigned) stale);
	pending.clear();
    }
}

void WSEndpoint::onError(WebSocket *ws)
{
	Error("WSEndpoint: websocket connection reported an error.\n");
}


void WSEndpoint::onMessageStart(WebSocket *ws, WebSocket::MessageType type, const DWORD length)
{
	//Reset frame
    media->SetLength(0);
    msgType = type;
    media->Alloc(length); 
   // Todo : if alloc fail, 
}

void WSEndpoint::onMessageData(WebSocket *ws,const BYTE* data, const DWORD size)
{
    media->AppendMedia( (BYTE*) data, size);
}

void WSEndpoint::onMessageEnd(WebSocket *ws)
{
   switch( media->GetType() )
    {
        case MediaFrame::Text:
	    { 
			if (useRed)
			{
				
				media->SetTimestamp(getDifTime(&clock)/1000);
				RTPRedundantPacket * packet = RedCodec->Encode(media, payloadType);
				packet->SetSeqNum(pseudoSeqNum);
				packet->SetSeqCycles(pseudoSeqCycle);
				Multiplex(*packet);
                                delete packet;
			}
			else
			{
				RTPPacket packet(MediaFrame::Text, TextCodec::T140);
				packet.SetTimestamp(getDifTime(&clock)/1000);
				packet.SetPayload(media->GetData(),media->GetLength());		
				packet.SetSeqNum(pseudoSeqNum);
				packet.SetSeqCycles(pseudoSeqCycle);
				Multiplex(packet);				
			}	
			if ( pseudoSeqNum == 0xFFFF )
				pseudoSeqCycle++;
			pseudoSeqNum++;
	    }
		
	    break;
	    
	default:
	   break;
    }
}

void WSEndpoint::onRTPPacket(RTPPacket &packet)
{
    //Pas de garde sur la présence d'un WebSocket ici : le texte est décodé
    //jusqu'à SendFrame, qui le met en attente si le navigateur n'est pas encore
    //connecté (§4.5 de jsr309_text_over_wss.md). Jeter le paquet à l'entrée,
    //comme avant, perdait la première phrase de chaque appel — celle où
    //l'appelant se présente, entre le 200 OK et le handshake WebSocket.
    {
        if ( packet.GetMedia() == media->GetType() )
		{
			switch ( packet.GetMedia() )
			{
			case MediaFrame::Text:
			{	
								
				//Check the type of data
				if (packet.GetCodec() == TextCodec::T140RED)
				{
				
					//Get redundant packet
					RTPRedundantPacket* red = (RTPRedundantPacket*) &packet;
					
					RedCodec->Decode(red, this);
							
				} 
				else if (packet.GetCodec() == TextCodec::T140)
				{
					
					//Create frame
					TextFrame frame ( packet.GetTimestamp(),packet.GetMediaData(),packet.GetMediaLength() );
					//Send it
					SendFrame(frame);
				}
				else
				{
					Error("Text codec %d: not supported.\n", packet.GetCodec() );
				}
				break;
			}
			
			default:
				Error("WSEndpoint does not support media %s.\n",
					MediaFrame::TypeToString( packet.GetMedia() ) );
				break;
			}
		}
		else
		{
			Error("WSEndpoint is associated with media %s. Cannot deliver %s packet.\n",
				   MediaFrame::TypeToString(media->GetType()),
			   MediaFrame::TypeToString(packet.GetMedia()));
		}
    }
}

void WSEndpoint::onClose(WebSocket *ws)
{
    //Ne réinitialiser que si c'est bien la connexion courante qui se ferme
    //(une nouvelle a pu la remplacer entre-temps via onOpen).
    std::shared_ptr<WebSocket> cur = _ws.lock();
    const bool wasOpen = ( cur.get() == ws );

    if ( wasOpen )
    {
        Log("WSEndpoint: connection associated with endpoint is closing.\n");
	_ws.reset();

	// Signal the interription as per
	SendReplacementChar(false);

	//Le texte en attente est PERDU à la coupure : le garder pour le rejouer
	//plus tard afficherait, après l'U+FFFD, un texte que le pair RTP croit
	//déjà perdu (arbitrage du 2026-09-21).
	if (wantConnected && !pending.empty())
	{
	    droppedWhileDown += pending.size();
	    pending.clear();
	}

	//Une jambe cliente qui TOMBE est une perte que le contrôleur doit voir.
	//Une TENTATIVE infructueuse, elle, ne publie rien : elle ne change pas
	//ce qu'il sait déjà.
	if (wantConnected)
	    PostEvent(new ::EndpointDisconnectedEvent());
    }

    //Toute connexion qui se ferme relance la reprise — établie ou jamais
    //ouverte. Sauf si une autre l'a déjà remplacée (cur non nul) : celle-là vit.
    if (wantConnected && (wasOpen || !cur))
	ScheduleReconnect();
}

/**
 * Mode client (SPEC docs/conception/WS-CLIENT §4.7)
 */
void WSEndpoint::SetServer(WebSocketServer* server)
{
	wsServer = server;
}

bool WSEndpoint::IsClientMode() const
{
	std::lock_guard<std::mutex> lock(clientMutex);
	return !clientUrl.empty();
}

std::string WSEndpoint::GetRemoteUrl() const
{
	std::lock_guard<std::mutex> lock(clientMutex);
	return clientUrl;
}

int WSEndpoint::Connect(const std::string& url)
{
	if (!wsServer)
		return Error("WSEndpoint: no websocket server to open an outgoing leg.\n");

	//Ce qui est PERMANENT se dit tout de suite : une URL illisible ou un schéma
	//inconnu ne se répareront pas en attendant 5 s. Ce qui est TRANSITOIRE (le
	//pair n'écoute pas encore) arme la reprise et rend 1.
	WebSocketServer::Target target;
	if (!WebSocketServer::ParseWsUrl(url,target))
		return Error("WSEndpoint: cannot use url \"%s\" for an outgoing leg.\n",url.c_str());

	{
		std::lock_guard<std::mutex> lock(clientMutex);
		if (!clientUrl.empty() && clientUrl!=url)
			//Rearmer une jambe vivante sur une AUTRE cible, c'est deux
			//conversations dans un seul port : il faut la reconfigurer.
			return Error("WSEndpoint: leg is already armed on %s.\n",clientUrl.c_str());

		clientUrl     = url;
		wantConnected = true;
	}

	Log("WSEndpoint: outgoing text leg armed on %s\n",url.c_str());

	TryConnect();

	return 1;
}

void WSEndpoint::TryConnect()
{
	std::string url;
	{
		std::lock_guard<std::mutex> lock(clientMutex);
		if (!wantConnected)
			return;
		url = clientUrl;
	}

	if (!wsServer)
		return;

	//weak_ptr, et non shared_ptr : c'est l'expiration de cette référence qui dit
	//au serveur que la jambe est morte et que la reprise doit s'arrêter.
	std::weak_ptr<WebSocket::Listener> self = weak_from_this();
	if (self.expired())
	{
		//Un WSEndpoint hors shared_ptr ne peut pas se donner en listener. Le
		//dire vaut mieux que d'ouvrir une connexion que personne ne pilotera.
		Error("WSEndpoint: endpoint is not held by a shared_ptr, cannot connect.\n");
		return;
	}

	//HORS du verrou : Connect() résout un nom, et un DNS lent tiendrait alors
	//tout le reste de la jambe.
	if (!wsServer->Connect(url,self))
		//Échec SYNCHRONE : personne n'a été notifié — il n'y a pas de jambe —,
		//donc c'est ici que la reprise se replanifie.
		ScheduleReconnect();
}

void WSEndpoint::ScheduleReconnect()
{
	std::weak_ptr<WebSocket::Listener> self = weak_from_this();
	if (self.expired())
		return;

	std::string url;
	uint64_t previous = 0;
	{
		std::lock_guard<std::mutex> lock(clientMutex);
		if (!wantConnected)
			return;
		url = clientUrl;
		//Une seule demande en vol à la fois : deux demandes feraient deux
		//connexions concurrentes pour une seule jambe.
		previous = retryId;
		retryId  = 0;
	}

	if (!wsServer)
		return;

	if (previous)
		wsServer->CancelConnectLater(previous);

	const uint64_t id = wsServer->ConnectLater(url,self,retryMs);

	std::lock_guard<std::mutex> lock(clientMutex);
	//End() a pu passer pendant l'appel : ne pas ressusciter la reprise.
	if (wantConnected)
		retryId = id;
	else if (id)
		wsServer->CancelConnectLater(id);
}

//T.140 §5.3 : une perte de session s'annonce par un U+FFFD dans le flux, du côté
//qui SURVIT — l'utilisateur voit alors qu'il manque du texte, au lieu de lire
//deux phrases collées. `toWsSide` dit de quel côté envoyer : vrai quand la perte
//vient du RTP (onResetStream/onEndStream), faux quand c'est le WebSocket qui est
//tombé (onClose) et que le pair RTP doit l'apprendre.
void WSEndpoint::SendReplacementChar(bool toWsSide)
{
    if (toWsSide)
    {
        //Vers le navigateur : un message WebSocket d'un seul caractère.
        if (std::shared_ptr<WebSocket> ws = _ws.lock())
        {
            std::string msg((const char*) REPLACEMENT_UTF8, sizeof(REPLACEMENT_UTF8));
            ws->SendMessage( msg );
            Debug("WSEndpoint: sent U+FFFD to the websocket side.\n");
        }
        return;
    }

    //Vers le côté RTP : par le même chemin que le texte ordinaire, redondance
    //comprise — un U+FFFD perdu en route serait une perte annoncée que personne
    //ne reçoit.
    TextFrame lost(getDifTime(&clock)/1000, REPLACEMENT_UTF8, sizeof(REPLACEMENT_UTF8));

    if (useRed)
    {
        RTPRedundantPacket *packet = RedCodec->Encode( &lost, payloadType);
        if (packet)
        {
            packet->SetSeqNum(pseudoSeqNum++);
            packet->SetSeqCycles(pseudoSeqCycle);
            if (pseudoSeqNum == 0) pseudoSeqCycle++;
            Multiplex(*packet);
            delete packet;
        }
    }
    else
    {
        RTPPacket packet(MediaFrame::Text, TextCodec::T140);
        packet.SetTimestamp(getDifTime(&clock)/1000);
        packet.SetPayload(REPLACEMENT_UTF8, sizeof(REPLACEMENT_UTF8));
        packet.SetSeqNum(pseudoSeqNum++);
        packet.SetSeqCycles(pseudoSeqCycle);
        if (pseudoSeqNum == 0) pseudoSeqCycle++;
        Multiplex(packet);
    }

    Debug("WSEndpoint: sent U+FFFD to the RTP side.\n");
}

void WSEndpoint::SetLocalPort(int port)
{
	//return 1;
	wsPort = port;
}

void WSEndpoint::SetLocalHost(char* host)
{
	//return 1;
	wsHost = host;
}

void WSEndpoint::SetLocalSecure(bool secure)
{
	wsSecure = secure;
}

bool WSEndpoint::IsLocalSecure()
{
	return wsSecure;
}

int WSEndpoint::GetLocalPort()
{
	//return 1;
	return wsPort;
}

char* WSEndpoint::GetLocalHost()
{
	//return 1;
	return wsHost;
}

WSEndpoint::~WSEndpoint()
{
    End();
}


int WSEndpoint::End()
{
    //Arrêter la reprise AVANT de fermer : sinon la fermeture qu'on déclenche
    //rouvrirait la jambe. C'est le seul point d'arrêt volontaire de la boucle —
    //autrement elle ne s'arrête que si la jambe est détruite.
    uint64_t pendingRetry = 0;
    {
        std::lock_guard<std::mutex> lock(clientMutex);
        wantConnected = false;
        pendingRetry  = retryId;
        retryId       = 0;
    }
    if (pendingRetry && wsServer)
        wsServer->CancelConnectLater(pendingRetry);

    if ( std::shared_ptr<WebSocket> ws = _ws.lock() )
    {
        ws->Close();
    }
    _ws.reset();

	RedCodec = NULL;
	return 0;
}


int WSEndpoint::SendFrame(TextFrame &frame)
{
	std::string msg( (const char*) frame.GetData(), frame.GetLength());
	if ( frame.GetLength() == 3 && memcmp(frame.GetData(), BOMUTF8, 3) == 0)
	{
		TextFrame bom(getDifTime(&clock)/1000, BOMUTF8,3);
		//Avant le Multiplex : place apres, cette trace ne dit rien quand le
		//Multiplex ne rend pas la main, ce qui est justement le cas a voir.
		Debug("BOM ping pong.\n");
		if (useRed)
		{
			RTPRedundantPacket *packet = RedCodec->Encode( &bom, payloadType);
			if (packet)
			{
				packet->SetSeqNum(pseudoSeqNum++);
				packet->SetSeqCycles(pseudoSeqCycle);
				if (pseudoSeqNum == 0) pseudoSeqCycle++;
				Multiplex(*packet);
				delete packet;
			}
		}
		else
		{
			RTPPacket packet(MediaFrame::Text, TextCodec::T140);
			packet.SetTimestamp(getDifTime(&clock)/1000);
			packet.SetPayload(BOMUTF8,3);
			packet.SetSeqNum(pseudoSeqNum++);
			packet.SetSeqCycles(pseudoSeqCycle);
			if (pseudoSeqNum == 0) pseudoSeqCycle++;
			Multiplex(packet);
		}
	}
	
    //Verrouiller la référence le temps de l'envoi (thread-safe vs destruction)
    if (std::shared_ptr<WebSocket> ws = _ws.lock())
    {
        ws->SendMessage( msg );
    }
    else if (everOpened && IsClientMode())
    {
        //Jambe cliente COUPÉE : le texte est perdu, et la perte s'annonce par un
        //U+FFFD à la reconnexion (arbitrage du 2026-09-21). Le garder pour le
        //rejouer afficherait, après ce caractère, un texte déjà déclaré perdu.
        droppedWhileDown++;
    }
    else
    {
        //Pas encore de navigateur : garder la trame (§4.5 de
        //jsr309_text_over_wss.md). Bornée en nombre ET en âge.
        pending.push_back(std::make_pair(getTimeMS(), msg));

        if (pending.size() > maxPendingFrames)
        {
            pending.pop_front();
            Log("WSEndpoint: pending text queue full, oldest frame dropped.\n");
        }
    }

	return 0;
}

