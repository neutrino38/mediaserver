#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <usrsctp.h>

#include "sctptransport.h"
#include "log.h"
#include "tools.h"

namespace {

//Table des associations vivantes, indexée par leur jeton. usrsctp rend ce jeton
//au callback de sortie ; c'est le seul lien entre un datagramme et son porteur.
std::mutex				g_mutex;
std::map<void*,SCTPTransport*>		g_transports;
uintptr_t				g_nextToken = 1;
bool					g_inited    = false;

//Horloge des timers, qui sont globaux à la pile.
std::mutex	g_timerMutex;
timeval		g_lastTimers = {0,0};

//Nombre de flux négociés à l'ouverture de l'association. Un data channel WebRTC
//en utilise un par canal ; 1024 est ce que retiennent les implémentations de
//référence, et cela ne coûte que la table.
const WORD kStreams = 1024;

//usrsctp ne se termine JAMAIS. `usrsctp_finish()` alors qu'une socket vit est un
//crash connu de la bibliothèque, et il n'y a rien à gagner à le tenter : la pile
//tient dans quelques centaines de kilo-octets et le processus s'arrête de toute
//façon. Le seul état global est donc initialisé une fois.
bool EnsureStackInited(int (*output)(void*,void*,size_t,BYTE,BYTE))
{
	if (g_inited)
		return true;

	//Port 0 : pas d'encapsulation UDP par la pile. C'est nous qui portons les
	//datagrammes, dans du DTLS.
	usrsctp_init_nothreads(0,output,NULL);
	g_inited = true;

	Log("-SCTPTransport: pile usrsctp initialisée (mode sans thread).\n");
	return true;
}

} // namespace

SCTPTransport::SCTPTransport(Listener& listener) : listener(listener)
{
	sock  = NULL;
	token = NULL;
	up    = false;
}

SCTPTransport::~SCTPTransport()
{
	//Sans notification : le listener est souvent le propriétaire, en cours de
	//destruction lui aussi.
	Shutdown(false);
}

int SCTPTransport::Init(WORD localPort,WORD remotePort)
{
	if (sock)
		return Error("-SCTPTransport::Init() | déjà initialisé\n");

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		EnsureStackInited(OnOutput);

		//Jeton opaque, et inscription de l'instance AVANT toute socket : un
		//datagramme peut sortir dès le premier appel à usrsctp.
		token = (void*)(g_nextToken++);
		g_transports[token] = this;
	}

	usrsctp_register_address(token);

	sock = usrsctp_socket(AF_CONN,SOCK_STREAM,IPPROTO_SCTP,OnReceive,NULL,0,this);

	if (!sock)
	{
		Error("-SCTPTransport::Init() | usrsctp_socket a échoué [errno:%d]\n",errno);
		End();
		return 0;
	}

	//Non bloquante : usrsctp_connect rend EINPROGRESS et la montée de
	//l'association arrive par notification.
	usrsctp_set_non_blocking(sock,1);

	//Pas de Nagle : une frappe de clavier ne doit pas attendre la suivante.
	int nodelay = 1;
	usrsctp_setsockopt(sock,IPPROTO_SCTP,SCTP_NODELAY,&nodelay,sizeof(nodelay));

	//Reset de flux : c'est ainsi qu'un canal se ferme proprement (RFC 8831 §6.7).
	struct sctp_assoc_value reset;
	memset(&reset,0,sizeof(reset));
	reset.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
	usrsctp_setsockopt(sock,IPPROTO_SCTP,SCTP_ENABLE_STREAM_RESET,&reset,sizeof(reset));

	struct sctp_initmsg init;
	memset(&init,0,sizeof(init));
	init.sinit_num_ostreams  = kStreams;
	init.sinit_max_instreams = kStreams;
	usrsctp_setsockopt(sock,IPPROTO_SCTP,SCTP_INITMSG,&init,sizeof(init));

	//Les deux seuls événements qui nous intéressent : l'association monte ou
	//tombe, et un flux est remis à zéro (canal fermé par le pair).
	const WORD events[] = { SCTP_ASSOC_CHANGE, SCTP_STREAM_RESET_EVENT };

	for (size_t i = 0; i < sizeof(events)/sizeof(events[0]); i++)
	{
		struct sctp_event ev;
		memset(&ev,0,sizeof(ev));
		ev.se_assoc_id = SCTP_ALL_ASSOC;
		ev.se_type     = events[i];
		ev.se_on       = 1;
		usrsctp_setsockopt(sock,IPPROTO_SCTP,SCTP_EVENT,&ev,sizeof(ev));
	}

	struct sockaddr_conn local;
	memset(&local,0,sizeof(local));
	local.sconn_family = AF_CONN;
	local.sconn_port   = htons(localPort);
	local.sconn_addr   = token;

	if (usrsctp_bind(sock,(struct sockaddr*)&local,sizeof(local)) < 0)
	{
		Error("-SCTPTransport::Init() | usrsctp_bind(%d) a échoué [errno:%d]\n",localPort,errno);
		End();
		return 0;
	}

	//On connecte des DEUX côtés, sans se soucier du rôle DTLS : SCTP résout la
	//collision d'INIT (ouverture simultanée, RFC 4960 §5.2.4), et c'est ce que
	//font les implémentations de référence. Pas de listen/accept, donc pas de
	//seconde socket à suivre.
	struct sockaddr_conn remote;
	memset(&remote,0,sizeof(remote));
	remote.sconn_family = AF_CONN;
	remote.sconn_port   = htons(remotePort);
	remote.sconn_addr   = token;

	if (usrsctp_connect(sock,(struct sockaddr*)&remote,sizeof(remote)) < 0 && errno != EINPROGRESS)
	{
		Error("-SCTPTransport::Init() | usrsctp_connect(%d) a échoué [errno:%d]\n",remotePort,errno);
		End();
		return 0;
	}

	Log("-SCTPTransport: association ouverte [local:%d,remote:%d] [%p]\n",localPort,remotePort,this);
	return 1;
}

int SCTPTransport::End()
{
	return Shutdown(true);
}

int SCTPTransport::Shutdown(bool notify)
{
	const bool wasUp = up;

	//Retirer l'instance de la table AVANT de fermer : un datagramme produit par
	//la fermeture (SHUTDOWN, ABORT) tombe alors au lieu de revenir sur un objet
	//en train de disparaître. La jambe s'en va, son DTLS aussi.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (token)
			g_transports.erase(token);
	}

	if (sock)
	{
		usrsctp_close(sock);
		sock = NULL;
	}

	if (token)
	{
		usrsctp_deregister_address(token);
		token = NULL;
	}

	up = false;

	{
		std::lock_guard<std::mutex> lock(outboundMutex);
		outbound.clear();
	}

	partial.clear();

	if (wasUp && notify)
		listener.onSCTPAssociationDown();

	return 1;
}

void SCTPTransport::OnPacket(const BYTE* data,DWORD size)
{
	if (!token || !size)
		return;

	//La pile peut produire de la sortie et livrer des messages DANS cet appel.
	usrsctp_conninput(token,data,size,0);
}

int SCTPTransport::Send(WORD streamId,DWORD ppid,const BYTE* data,DWORD size)
{
	if (!sock)
		return Error("-SCTPTransport::Send() | pas d'association\n");

	if (size > MaxMessageSize)
		return Error("-SCTPTransport::Send() | message de %u octets, au delà de la limite annoncée (%u)\n",
				size,MaxMessageSize);

	struct sctp_sendv_spa spa;
	memset(&spa,0,sizeof(spa));
	spa.sendv_flags		   = SCTP_SEND_SNDINFO_VALID;
	spa.sendv_sndinfo.snd_sid  = streamId;
	//Le PPID voyage en ordre réseau ; usrsctp ne le convertit pas.
	spa.sendv_sndinfo.snd_ppid = htonl(ppid);

	const ssize_t sent = usrsctp_sendv(sock,data,size,NULL,0,&spa,sizeof(spa),SCTP_SENDV_SPA,0);

	if (sent < 0)
		return Error("-SCTPTransport::Send() | usrsctp_sendv a échoué [errno:%d]\n",errno);

	return (int) sent;
}

bool SCTPTransport::GetOutbound(std::string& datagram)
{
	std::lock_guard<std::mutex> lock(outboundMutex);

	if (outbound.empty())
		return false;

	datagram = outbound.front();
	outbound.pop_front();
	return true;
}

void SCTPTransport::Enqueue(const BYTE* data,DWORD size)
{
	{
		std::lock_guard<std::mutex> lock(outboundMutex);

		if (outbound.size() >= maxOutbound)
		{
			outbound.pop_front();
			Log("-SCTPTransport: file de sortie pleine, plus ancien datagramme jeté [%p]\n",this);
		}

		outbound.push_back(std::string((const char*)data,size));
	}

	//Hors du verrou : le porteur va vouloir vider la file.
	listener.onSCTPOutboundReady();
}

/************************
* HandleTimers
*	Les timers d'usrsctp sont globaux, et l'écoulement qu'on lui passe est du
*	temps réel. Le tenir ici, sous verrou, garantit que le temps avance au bon
*	rythme quel que soit le nombre de jambes qui appellent — deux jambes
*	cadencées à 10 ms le feraient sinon avancer deux fois trop vite.
*************************/
void SCTPTransport::HandleTimers()
{
	std::lock_guard<std::mutex> lock(g_timerMutex);

	if (!g_inited)
		return;

	if (isZeroTime(&g_lastTimers))
	{
		gettimeofday(&g_lastTimers,NULL);
		return;
	}

	const DWORD elapsed = (DWORD)(getDifTime(&g_lastTimers)/1000);

	if (!elapsed)
		return;

	gettimeofday(&g_lastTimers,NULL);
	usrsctp_handle_timers(elapsed);
}

int SCTPTransport::OnOutput(void* addr,void* buffer,size_t length,BYTE tos,BYTE set_df)
{
	SCTPTransport* transport = NULL;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::map<void*,SCTPTransport*>::const_iterator it = g_transports.find(addr);
		if (it != g_transports.end())
			transport = it->second;
	}

	//Jeton inconnu : l'association a ete detruite entre-temps. Le datagramme
	//tombe, ce qui est exactement ce qu'on veut.
	if (!transport || !buffer || !length)
		return 0;

	transport->Enqueue((const BYTE*)buffer,(DWORD)length);
	return 0;
}

int SCTPTransport::OnReceive(struct socket* sock,union sctp_sockstore addr,void* data,
			     size_t datalen,struct sctp_rcvinfo rcv,int flags,void* ulp_info)
{
	SCTPTransport* transport = (SCTPTransport*) ulp_info;

	//data NULL : la pile signale la fin de l'association.
	if (!transport || !data)
	{
		if (transport && transport->up)
		{
			transport->up = false;
			transport->listener.onSCTPAssociationDown();
		}
		return 1;
	}

	if (flags & MSG_NOTIFICATION)
		transport->HandleNotification((const union sctp_notification*)data,datalen);
	else
		transport->HandleData(rcv.rcv_sid,ntohl(rcv.rcv_ppid),
				      (const BYTE*)data,(DWORD)datalen,(flags & MSG_EOR) != 0);

	//Le tampon appartient au callback.
	free(data);
	return 1;
}

void SCTPTransport::HandleData(WORD streamId,DWORD ppid,const BYTE* data,DWORD size,bool complete)
{
	//Cas courant, et le seul qu'un T140block emprunte : un message entier d'un
	//coup, rien à recoller.
	if (complete && partial.find(streamId) == partial.end())
	{
		if (size > MaxMessageSize)
		{
			Log("-SCTPTransport: message de %u octets sur le flux %d, jeté [%p]\n",size,streamId,this);
			return;
		}

		listener.onSCTPMessage(streamId,ppid,data,size);
		return;
	}

	if (partial.size() >= maxPartialStreams && partial.find(streamId) == partial.end())
	{
		Log("-SCTPTransport: trop de messages partiels en cours, flux %d ignore [%p]\n",streamId,this);
		return;
	}

	std::string& buffer = partial[streamId];

	//Borner AVANT de recoller : un pair qui n'envoie jamais MSG_EOR ne doit pas
	//pouvoir faire grossir ce tampon sans fin.
	if (buffer.size() + size > MaxMessageSize)
	{
		Log("-SCTPTransport: message partiel du flux %d au delà de la limite, abandonné [%p]\n",
			streamId,this);
		partial.erase(streamId);
		return;
	}

	buffer.append((const char*)data,size);

	if (!complete)
		return;

	const std::string message = buffer;
	partial.erase(streamId);

	listener.onSCTPMessage(streamId,ppid,(const BYTE*)message.data(),(DWORD)message.size());
}

void SCTPTransport::HandleNotification(const union sctp_notification* notif,size_t size)
{
	if (size < sizeof(notif->sn_header))
		return;

	switch (notif->sn_header.sn_type)
	{
		case SCTP_ASSOC_CHANGE:
		{
			if (size < sizeof(struct sctp_assoc_change))
				return;

			switch (notif->sn_assoc_change.sac_state)
			{
				case SCTP_COMM_UP:
				case SCTP_RESTART:
					if (!up)
					{
						up = true;
						Log("-SCTPTransport: association établie [%p]\n",this);
						listener.onSCTPAssociationUp();
					}
					break;

				case SCTP_COMM_LOST:
				case SCTP_SHUTDOWN_COMP:
				case SCTP_CANT_STR_ASSOC:
					if (up)
					{
						up = false;
						Log("-SCTPTransport: association perdue [etat:%d] [%p]\n",
							notif->sn_assoc_change.sac_state,this);
						listener.onSCTPAssociationDown();
					}
					break;
			}
			break;
		}

		case SCTP_STREAM_RESET_EVENT:
			//Le pair a ferme un canal. La couche qui connait les canaux le
			//deduit de la perte des messages ; rien a faire ici pour l'instant.
			Debug("-SCTPTransport: reset de flux reçu [%p]\n",this);
			break;

		default:
			Debug("-SCTPTransport: notification %d ignorée [%p]\n",
				notif->sn_header.sn_type,this);
			break;
	}
}
