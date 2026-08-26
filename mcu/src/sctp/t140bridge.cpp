#include "t140bridge.h"
#include "log.h"

T140Bridge::T140Bridge(RTPSession& session,T140DataChannel::Listener& listener) :
	session(session),
	sctp(*this),
	t140(sctp,listener)
{
	localSCTPPort  = DefaultSCTPPort;
	remoteSCTPPort = DefaultSCTPPort;
	wanted	       = false;
}

T140Bridge::~T140Bridge()
{
	//Sans notification : le propriétaire est le plus souvent en cours de
	//destruction lui aussi. Stop() explicite est le chemin normal.
	session.SetDTLSApplicationListener(NULL);
	wanted = false;
}

int T140Bridge::Start()
{
	wanted = true;

	//La session nous livre les données applicatives DTLS et nous prête sa cadence.
	session.SetDTLSApplicationListener(this);

	Log("-T140Bridge: demarre [sctp local:%d,remote:%d] [%p]\n",
		localSCTPPort,remoteSCTPPort,this);
	return 1;
}

int T140Bridge::Stop()
{
	//Se débrancher d'abord : plus rien ne doit nous être livré pendant qu'on
	//démonte la pile.
	session.SetDTLSApplicationListener(NULL);

	wanted = false;

	//Puis la pile : sa fermeture signale la perte du canal, et le consommateur
	//pose son U+FFFD du côté qui survit.
	sctp.End();

	return 1;
}

void T140Bridge::SetRemoteSCTPPort(WORD port)
{
	remoteSCTPPort = port ? port : DefaultSCTPPort;
}

int T140Bridge::GetStreamId() const
{
	return t140.IsOpen() ? (int) t140.GetStreamId() : -1;
}

int T140Bridge::SendText(const BYTE* data,DWORD size)
{
	return t140.SendText(data,size);
}

int T140Bridge::OpenChannel(WORD streamId)
{
	if (!sctp.IsUp())
		return Error("-T140Bridge::OpenChannel() | association SCTP pas encore montee\n");

	const int res = t140.OpenChannel(streamId);

	//Le DATA_CHANNEL_OPEN attend dans la file de sortie : réveiller la boucle,
	//qui seule a le droit de le chiffrer.
	session.WakeUp();

	return res;
}

/************************
* onDTLSApplicationData
*	Un datagramme SCTP déchiffré. On est sur le thread de la session, donc on a
*	le droit de chiffrer la réponse tout de suite : c'est ce qui garde le
*	handshake SCTP à un aller-retour.
*************************/
void T140Bridge::onDTLSApplicationData(const BYTE* data,DWORD size)
{
	sctp.OnPacket(data,size);
	Flush();
}

void T140Bridge::onApplicationTick(DWORD elapsedMs)
{
	if (wanted && !sctp.IsUp() && session.IsDTLSHandshakeCompleted())
	{
		wanted = false;

		if (!sctp.Init(localSCTPPort,remoteSCTPPort))
			Error("-T140Bridge: impossible d'ouvrir l'association SCTP [%p]\n",this);
	}

	//Les timers de la pile, et ce qu'ils ont produit.
	SCTPTransport::HandleTimers();
	Flush();
}

void T140Bridge::Flush()
{
	std::string datagram;

	//Le SEUL endroit qui chiffre, et il est sur le thread de la session : c'est le
	//contrat de SCTPTransport, et celui de SendDTLSApplicationData.
	while (sctp.GetOutbound(datagram))
		session.SendDTLSApplicationData((const BYTE*) datagram.data(),(DWORD) datagram.size());
}

void T140Bridge::onSCTPOutboundReady()
{
	//Appelable depuis n'importe quel thread : on ne fait que réveiller la boucle,
	//qui videra la file au tour suivant.
	session.WakeUp();
}

void T140Bridge::onSCTPMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size)
{
	t140.OnMessage(streamId,ppid,data,size);
}

void T140Bridge::onSCTPAssociationUp()
{
	t140.OnAssociationUp();
}

void T140Bridge::onSCTPAssociationDown()
{
	t140.OnAssociationDown();
}
