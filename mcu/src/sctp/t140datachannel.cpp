#include <string.h>

#include "t140datachannel.h"
#include "log.h"
#include "tools.h"

const char* const T140DataChannel::SubProtocol = "t140";

//Un message SCTP de longueur nulle n'existe pas : le T140block vide se dit par
//son PPID, avec un octet de bourrage que le pair ignore (RFC 8831 §6.6).
static const BYTE kEmptyPayload = 0;

T140DataChannel::T140DataChannel(SCTPTransport& sctp,Listener& listener) :
	sctp(sctp),
	listener(listener)
{
	bound	 = false;
	open	 = false;
	streamId = 0;
	gettimeofday(&clock,NULL);
}

/************************
* OpenChannel
*	DATA_CHANNEL_OPEN émis par nous. Le canal n'est déclaré ouvert qu'au
*	DATA_CHANNEL_ACK du pair : d'ici là le texte attend dans la file.
*************************/
int T140DataChannel::OpenChannel(WORD stream)
{
	DCEP::Open open;
	//Fiable et ordonné, sans paramètre de fiabilité : ce que RFC 8865 demande.
	open.channelType = DCEP::Reliable;
	open.priority	 = 0;
	open.reliability = 0;
	open.label	 = SubProtocol;
	open.protocol	 = SubProtocol;

	BYTE buffer[256];
	const DWORD length = DCEP::SerializeOpen(open,buffer,sizeof(buffer));

	if (!length)
		return Error("-T140DataChannel::OpenChannel() | sérialisation impossible\n");

	if (!sctp.Send(stream,PPIDControl,buffer,length))
		return Error("-T140DataChannel::OpenChannel() | envoi du DATA_CHANNEL_OPEN impossible\n");

	bound	 = true;
	streamId = stream;

	Log("-T140DataChannel: DATA_CHANNEL_OPEN émis sur le flux %d [%p]\n",stream,this);
	return 1;
}

void T140DataChannel::OnMessage(WORD stream,DWORD ppid,const BYTE* data,DWORD size)
{
	switch (ppid)
	{
		case PPIDControl:
		{
			DCEP::Open request;

			if (DCEP::ParseOpen(data,size,request))
			{
				//Le canal du texte est celui dont le sous-protocole dit
				//`t140`. À défaut, et si aucun n'est encore retenu, on prend
				//celui-ci : le WebSocket a enseigné qu'un client déployé ne se
				//corrige pas. Indulgent à l'entrée, exact à la sortie.
				const bool isT140 = (request.protocol == SubProtocol) ||
						    (request.label	== SubProtocol);

				if (!isT140 && bound)
				{
					Log("-T140DataChannel: canal [label:%s,protocol:%s] sur le flux %d ignoré,"
					    " le texte est déjà sur le flux %d [%p]\n",
						request.label.c_str(),request.protocol.c_str(),stream,streamId,this);
					return;
				}

				if (!isT140)
					Log("-T140DataChannel: canal [label:%s,protocol:%s] pris pour le texte"
					    " faute de sous-protocole t140 [%p]\n",
						request.label.c_str(),request.protocol.c_str(),this);

				//Un canal qui demande autre chose que fiable et ordonné est
				//accepté quand même : c'est le client qui a tort, et le
				//refuser lui coûterait sa conversation.
				if (!request.IsReliableOrdered())
					Log("-T140DataChannel: canal de type 0x%02x, RFC 8865 en demande un"
					    " fiable et ordonné [%p]\n",request.channelType,this);

				BYTE ack[1];
				const DWORD length = DCEP::SerializeAck(ack,sizeof(ack));
				sctp.Send(stream,PPIDControl,ack,length);

				Bind(stream);
				return;
			}

			if (DCEP::IsAck(data,size))
			{
				if (!bound || stream != streamId)
				{
					Log("-T140DataChannel: DATA_CHANNEL_ACK inattendu sur le flux %d [%p]\n",
						stream,this);
					return;
				}

				Bind(stream);
				return;
			}

			Log("-T140DataChannel: message DCEP inconnu de %u octets sur le flux %d [%p]\n",
				size,stream,this);
			return;
		}

		case PPIDString:
		case PPIDStringEmpty:
		{
			if (!open || stream != streamId)
			{
				Log("-T140DataChannel: texte reçu sur le flux %d, hors du canal [%p]\n",stream,this);
				return;
			}

			//Un message = un T140block (RFC 8865 §5). Livré tel quel, sans
			//découpage et sans accumulation. Le PPID « String Empty » porte un
			//T140block vide, que l'adaptateur est libre d'ignorer.
			listener.onT140Block(data,(ppid == PPIDStringEmpty) ? 0 : size);
			return;
		}

		case PPIDBinary:
		case PPIDBinaryEmpty:
		case PPIDBinaryPartial:
			Log("-T140DataChannel: message binaire de %u octets sur le flux %d, jete :"
			    " T.140 est du texte [%p]\n",size,stream,this);
			return;

		default:
			Log("-T140DataChannel: PPID %u inconnu sur le flux %d, jeté [%p]\n",ppid,stream,this);
			return;
	}
}

void T140DataChannel::OnAssociationUp()
{
	//Rien à faire : c'est DCEP qui ouvre le canal, et le cas nominal attend
	//celui du navigateur.
	Debug("-T140DataChannel: association montée, en attente du canal [%p]\n",this);
}

void T140DataChannel::OnAssociationDown()
{
	if (!bound && !open)
		return;

	Log("-T140DataChannel: association perdue, canal fermé [%p]\n",this);

	bound = false;
	open  = false;

	{
		std::lock_guard<std::mutex> lock(mutex);
		pending.clear();
	}

	listener.onT140ChannelLost();
}

void T140DataChannel::Bind(WORD stream)
{
	const bool wasOpen = open;

	bound	 = true;
	open	 = true;
	streamId = stream;

	if (!wasOpen)
		Log("-T140DataChannel: canal t140 établi sur le flux %d [%p]\n",stream,this);

	FlushPending();

	if (!wasOpen)
		listener.onT140ChannelOpen();
}

int T140DataChannel::SendText(const BYTE* data,DWORD size)
{
	if (size > SCTPTransport::MaxMessageSize)
		return Error("-T140DataChannel::SendText() | T140block de %u octets, au delà de la limite\n",size);

	if (open)
		return sctp.Send(streamId,size ? PPIDString : PPIDStringEmpty,
				 size ? data : &kEmptyPayload,size ? size : 1);

	//Pas encore de canal : on garde la trame, bornée en nombre ET en âge.
	std::lock_guard<std::mutex> lock(mutex);

	pending.push_back(std::make_pair((QWORD) getDifTime(&clock)/1000,
					 std::string((const char*)data,size)));

	if (pending.size() > maxPendingFrames)
	{
		pending.pop_front();
		Log("-T140DataChannel: file d'attente pleine, plus ancienne trame jetée [%p]\n",this);
	}

	return (int) size;
}

void T140DataChannel::FlushPending()
{
	std::list<std::pair<QWORD,std::string>> frames;

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (pending.empty())
			return;
		frames.swap(pending);
	}

	const QWORD now = getDifTime(&clock)/1000;
	size_t sent = 0, stale = 0;

	for (std::list<std::pair<QWORD,std::string>>::const_iterator it = frames.begin();
	     it != frames.end(); ++it)
	{
		//Trop vieux pour être encore du dialogue.
		if (now - it->first > maxPendingAgeMs)
		{
			stale++;
			continue;
		}

		const DWORD size = (DWORD) it->second.size();
		sctp.Send(streamId,size ? PPIDString : PPIDStringEmpty,
			  size ? (const BYTE*) it->second.data() : &kEmptyPayload,size ? size : 1);
		sent++;
	}

	Log("-T140DataChannel: %u trame(s) en attente rejouée(s) à l'ouverture (%u trop vieille(s)) [%p]\n",
		(unsigned) sent,(unsigned) stale,this);
}
