#include <string.h>

#include "DCEndpoint.h"
#include "log.h"
#include "tools.h"

//U+FFFD REPLACEMENT CHARACTER : ce que T.140 §5.3 demande d'insérer dans le flux
//quand une perte de session est détectée — la seule trace qu'un utilisateur ait
//qu'il manque du texte. Non const : RTPPacket::SetPayload prend un BYTE* nu.
static BYTE REPLACEMENT_UTF8[] = { 0xEF, 0xBF, 0xBD };

//Un BOM seul est de la plomberie de keepalive T.140, pas de la conversation. Sur
//un canal fiable il n'a plus de rôle : on ne le fait pas traverser, dans aucun
//sens. C'est ce que fait déjà le pont conférence (ParticipantTextWS).
static const BYTE BOM_UTF8[] = { 0xEF, 0xBB, 0xBF };

static bool IsLoneBOM(const BYTE* data,DWORD size)
{
	return size == sizeof(BOM_UTF8) && memcmp(data,BOM_UTF8,sizeof(BOM_UTF8)) == 0;
}

DCEndpoint::DCEndpoint(MediaFrame::Type type) :
	RTPEndpoint(type,MediaFrame::VIDEO_MAIN,MediaFrame::SCTP),
	sctp(*this),
	t140(sctp,*this)
{
	localSCTPPort	  = DefaultSCTPPort;
	remoteSCTPPort	  = DefaultSCTPPort;
	associationWanted = false;
	useRed		  = false;
	payloadType	  = TextCodec::T140;
	pseudoSeqNum	  = 0;
	pseudoSeqCycle	  = 0;
	gettimeofday(&clock,NULL);

	//La session nous livre les données applicatives DTLS et nous prête sa cadence.
	SetDTLSApplicationListener(this);
}

DCEndpoint::~DCEndpoint()
{
	End();
}

int DCEndpoint::Init()
{
	//Le port, le socket, ICE, le DTLS et la boucle poll : tout vient de la jambe
	//RTP dont on dérive. Rien de plus à ouvrir ici — l'association SCTP attend la
	//fin du handshake DTLS.
	const int res = RTPEndpoint::Init();

	//Un data channel se demande dès l'Init : le contrôleur peut n'appeler
	//SetRemoteSCTPPort que plus tard, mais 5000 est le port de tous les
	//navigateurs et l'attente ne coûte rien.
	associationWanted = true;

	return res;
}

int DCEndpoint::End()
{
	//ORDRE. La boucle de la session bat la cadence de la pile SCTP et vide sa
	//file de sortie : démonter la pile sous ses pieds est une course. On arrête
	//donc la session D'ABORD — RTPEndpoint::End joint son thread — et on ne
	//touche à la pile qu'ensuite.
	const int res = RTPEndpoint::End();

	//Plus personne ne nous livre de données applicatives.
	SetDTLSApplicationListener(NULL);

	//Puis la pile : sa fermeture signale la perte du canal, et la jambe pontée
	//reçoit son U+FFFD.
	sctp.End();

	associationWanted = false;

	return res;
}

int DCEndpoint::StartReceiving()
{
	if (!portinited)
		return Error("-DCEndpoint::StartReceiving() | not inited\n");

	if (receiving)
		return Error("-DCEndpoint::StartReceiving() | already receiving\n");

	receiving = true;
	associationWanted = true;

	//Réveiller la boucle : elle relit la cadence et peut ouvrir l'association dès
	//que le DTLS est prêt.
	wait.Signal();

	Log("-DCEndpoint: reception ouverte [sctp local:%d,remote:%d] [%p]\n",
		localSCTPPort,remoteSCTPPort,this);
	return 1;
}

int DCEndpoint::StopReceiving()
{
	if (!receiving)
		return Error("-DCEndpoint::StopReceiving() | not receiving\n");

	receiving = false;
	return 1;
}

void DCEndpoint::SetRemoteSCTPPort(WORD port)
{
	remoteSCTPPort = port ? port : DefaultSCTPPort;
}

int DCEndpoint::GetStreamId() const
{
	return t140.IsOpen() ? (int) t140.GetStreamId() : -1;
}

int DCEndpoint::OpenDataChannel(WORD streamId)
{
	if (!sctp.IsUp())
		return Error("-DCEndpoint::OpenDataChannel() | association SCTP pas encore montee\n");

	const int res = t140.OpenChannel(streamId);

	//Le DATA_CHANNEL_OPEN attend dans la file de sortie : si on est sur le thread
	//de la session on le pousse tout de suite, sinon on la reveille.
	wait.Signal();

	return res;
}

/************************
* onDTLSApplicationData
*	Un datagramme SCTP déchiffré. On est sur le thread de la session, donc on a
*	le droit de chiffrer la réponse tout de suite : c'est ce qui garde le
*	handshake SCTP à un aller-retour.
*************************/
void DCEndpoint::onDTLSApplicationData(const BYTE* data,DWORD size)
{
	sctp.OnPacket(data,size);
	FlushSCTP();
}

void DCEndpoint::onApplicationTick(DWORD elapsedMs)
{
	//L'association attend la fin du handshake DTLS : avant, son INIT partirait
	//dans le vide et il faudrait attendre une retransmission d'une seconde.
	if (associationWanted && !sctp.IsUp() && IsDTLSHandshakeCompleted())
	{
		associationWanted = false;

		if (!sctp.Init(localSCTPPort,remoteSCTPPort))
			Error("-DCEndpoint: impossible d'ouvrir l'association SCTP [%p]\n",this);
	}

	//Les timers de la pile, et ce qu'ils ont produit.
	SCTPTransport::HandleTimers();
	FlushSCTP();
}

void DCEndpoint::FlushSCTP()
{
	std::string datagram;

	//Seul endroit qui chiffre, et il est sur le thread de la session : c'est le
	//contrat de SCTPTransport, et celui de SendDTLSApplicationData.
	while (sctp.GetOutbound(datagram))
		SendDTLSApplicationData((const BYTE*) datagram.data(),(DWORD) datagram.size());
}

void DCEndpoint::onSCTPOutboundReady()
{
	//Appelable depuis n'importe quel thread : on ne fait que réveiller la boucle,
	//qui videra la file au tour suivant.
	wait.Signal();
}

void DCEndpoint::onSCTPMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size)
{
	t140.OnMessage(streamId,ppid,data,size);
}

void DCEndpoint::onSCTPAssociationUp()
{
	t140.OnAssociationUp();
}

void DCEndpoint::onSCTPAssociationDown()
{
	t140.OnAssociationDown();
}

void DCEndpoint::onT140ChannelOpen()
{
	Log("-DCEndpoint: canal t140 ouvert sur le flux %d [%p]\n",GetStreamId(),this);
}

void DCEndpoint::onT140ChannelLost()
{
	//T.140 §5.3 : le côté qui SURVIT apprend la perte par un U+FFFD dans le flux.
	//Ici c'est la jambe pontée.
	Log("-DCEndpoint: canal t140 perdu, U+FFFD vers la jambe pontee [%p]\n",this);

	TextFrame lost(getDifTime(&clock)/1000,REPLACEMENT_UTF8,sizeof(REPLACEMENT_UTF8));
	onT140Block(lost.GetData(),lost.GetLength());
}

/************************
* onT140Block
*	Un T140block reçu du pair, à remettre à la jambe pontée. Le paquet est
*	fabriqué dans le dialecte qu'elle a négocié : T140RED si elle a demandé la
*	redondance de RFC 4103, T140 sinon. Sur le canal, lui, il n'y a jamais de
*	redondance — SCTP est fiable.
*************************/
void DCEndpoint::onT140Block(const BYTE* data,DWORD size)
{
	if (!size)
		//Un T140block vide n'a rien à dire à la jambe pontée.
		return;

	if (IsLoneBOM(data,size))
	{
		Debug("-DCEndpoint: BOM seul, non transmis [%p]\n",this);
		return;
	}

	if (useRed)
	{
		TextFrame frame(getDifTime(&clock)/1000,data,size);

		RTPRedundantPacket* packet = redCodec.Encode(&frame,payloadType);

		if (!packet)
		{
			Error("-DCEndpoint: encodage de la redondance impossible [%p]\n",this);
			return;
		}

		packet->SetSeqNum(pseudoSeqNum);
		packet->SetSeqCycles(pseudoSeqCycle);
		Multiplex(*packet);
		delete packet;
	}
	else
	{
		RTPPacket packet(MediaFrame::Text,TextCodec::T140);
		packet.SetTimestamp(getDifTime(&clock)/1000);
		packet.SetPayload((BYTE*) data,size);
		packet.SetSeqNum(pseudoSeqNum);
		packet.SetSeqCycles(pseudoSeqCycle);
		Multiplex(packet);
	}

	if (pseudoSeqNum == 0xFFFF)
		pseudoSeqCycle++;
	pseudoSeqNum++;
}

/************************
* onRTPPacket
*	La jambe pontée parle. Le texte en arrive en T140 ou en T140RED : la
*	redondance est terminée ICI, le canal ne porte jamais que du texte
*	dé-redondé (RFC 8865 : le canal est fiable, la redondance n'y a pas de sens).
*************************/
void DCEndpoint::onRTPPacket(RTPPacket& packet)
{
	if (packet.GetMedia() != MediaFrame::Text)
	{
		Error("-DCEndpoint: media %s non supporte sur un canal T.140 [%p]\n",
			MediaFrame::TypeToString(packet.GetMedia()),this);
		return;
	}

	switch (packet.GetCodec())
	{
		case TextCodec::T140RED:
		{
			RTPRedundantPacket* red = (RTPRedundantPacket*) &packet;
			//Le décodeur rend les blocs primaires par SendFrame.
			redCodec.Decode(red,this);
			break;
		}

		case TextCodec::T140:
		{
			TextFrame frame(packet.GetTimestamp(),packet.GetMediaData(),packet.GetMediaLength());
			SendFrame(frame);
			break;
		}

		default:
			Error("-DCEndpoint: codec texte %d non supporte [%p]\n",packet.GetCodec(),this);
			break;
	}
}

int DCEndpoint::SendFrame(TextFrame& frame)
{
	if (!frame.GetLength())
		return 1;

	if (IsLoneBOM(frame.GetData(),frame.GetLength()))
	{
		Debug("-DCEndpoint: BOM seul de la jambe pontee, non transmis [%p]\n",this);
		return 1;
	}

	//Sûr depuis ce thread, qui est celui de la jambe pontée : rien n'est chiffré
	//ici, le datagramme finit dans la file de sortie du transport.
	t140.SendText(frame.GetData(),frame.GetLength());
	return 1;
}

void DCEndpoint::onResetStream()
{
	//La source de la jambe pontée a disparu : le côté qui survit est le canal.
	t140.SendText(REPLACEMENT_UTF8,sizeof(REPLACEMENT_UTF8));
}

void DCEndpoint::onEndStream()
{
	t140.SendText(REPLACEMENT_UTF8,sizeof(REPLACEMENT_UTF8));
}
