#include "WSEndpoint.h"
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

WSEndpoint::WSEndpoint(MediaFrame::Type type) : Port(type, MediaFrame::WS)
{
    msgType = WebSocket::Binary;
    joined = NULL;
	//_ws : weak_ptr vide par défaut
	RedCodec = new RedundentCodec();
	
    useRed = false;
    pseudoSeqNum = 0;
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

    //Rejouer le texte arrivé avant que le navigateur ne soit là, dans l'ordre et
    //sans les trames trop vieilles pour être encore du dialogue (§4.5).
    if (!pending.empty())
    {
	const QWORD now = getDifTime(&clock)/1000;
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
    if ( cur.get() == ws )
    {
        Log("WSEndpoint: connection associated with endpoint is closing.\n");
	_ws.reset();

	// Signal the interription as per
	SendReplacementChar(false);
    }
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
		if (useRed)
		{
			RTPRedundantPacket *packet = RedCodec->Encode( &bom, payloadType);
			Multiplex(*packet);
			delete packet;
		}
		else
		{
			RTPPacket packet(MediaFrame::Text, TextCodec::T140);
			packet.SetTimestamp(getDifTime(&clock)/1000);
			packet.SetPayload(BOMUTF8,3);
			Multiplex(packet);			
		}
		Debug("BOM ping pong.\n");
	}
	
    //Verrouiller la référence le temps de l'envoi (thread-safe vs destruction)
    if (std::shared_ptr<WebSocket> ws = _ws.lock())
    {
        ws->SendMessage( msg );
    }
    else
    {
        //Pas encore de navigateur : garder la trame (§4.5 de
        //jsr309_text_over_wss.md). Bornée en nombre ET en âge.
        pending.push_back(std::make_pair((QWORD) getDifTime(&clock)/1000, msg));

        if (pending.size() > maxPendingFrames)
        {
            pending.pop_front();
            Log("WSEndpoint: pending text queue full, oldest frame dropped.\n");
        }
    }

	return 0;
}

