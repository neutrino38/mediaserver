#ifndef SCTPTRANSPORT_H
#define SCTPTRANSPORT_H

#include <list>
#include <map>
#include <mutex>
#include <string>
#include "config.h"

struct socket;
union sctp_sockstore;
struct sctp_rcvinfo;
union sctp_notification;

/*
 * Une association SCTP, au-dessus de rien : la pile (usrsctp, en mode sans
 * thread) ne touche aucune socket. On lui injecte les datagrammes qu'un porteur
 * a déchiffrés — DTLS pour un data channel WebRTC — et elle en rend par la file
 * de sortie. « SCTP sur UDP » est donc ici, exactement, SCTP sur DTLS sur UDP.
 *
 * Conception : docs/conception/T140-DC/SPEC.md §5.3.
 *
 * CONTRAT DE THREAD, et c'est le point à comprendre avant d'écrire un porteur :
 * les timers d'usrsctp sont GLOBAUX au processus. Le tour de timers d'une jambe
 * fait donc avancer les associations des AUTRES jambes, et peut produire leurs
 * datagrammes sur le thread de la première. Rien ici n'écrit donc sur le fil :
 * les datagrammes sortants vont dans une file, et le porteur la vide depuis SON
 * thread (GetOutbound), réveillé par onSCTPOutboundReady. C'est ce qui garde
 * l'objet SSL du DTLS strictement mono-thread.
 */
class SCTPTransport
{
public:
	//Taille maximale d'un message, dans les deux sens. C'est le
	//`a=max-message-size` que le contrôleur publie : ce qu'on annonce et ce
	//qu'on applique sont la même valeur.
	static constexpr DWORD MaxMessageSize = 65536;

	class Listener
	{
	public:
		virtual ~Listener(){};

		//Des datagrammes attendent dans la file de sortie. Appelé depuis
		//N'IMPORTE QUEL thread : ne rien faire d'autre que réveiller le
		//porteur, qui videra la file avec GetOutbound.
		virtual void onSCTPOutboundReady() = 0;

		//Un message applicatif COMPLET (MSG_EOR reçu). Même avertissement de
		//thread : le destinataire doit être sûr en concurrence.
		virtual void onSCTPMessage(WORD streamId,DWORD ppid,const BYTE* data,DWORD size) = 0;

		//L'association est montée / perdue.
		virtual void onSCTPAssociationUp() {}
		virtual void onSCTPAssociationDown() {}
	};

	SCTPTransport(Listener& listener);
	~SCTPTransport();

	//localPort / remotePort : les deux `a=sctp-port` (5000 par défaut, RFC 8841).
	int  Init(WORD localPort,WORD remotePort);
	//Ferme l'association et SIGNALE sa perte au listener : c'est ce qui fait
	//remonter le U+FFFD de T.140 §5.3 vers la jambe qui survit. À appeler
	//explicitement — le destructeur ferme aussi, mais sans rien signaler, un
	//listener à moitié détruit n'ayant rien à apprendre.
	int  End();

	//Un datagramme déchiffré par le porteur.
	void OnPacket(const BYTE* data,DWORD size);

	//Émet un message. Sûr depuis n'importe quel thread : la pile est verrouillée
	//en interne et rien ne part sur le fil ici — le résultat atterrit dans la
	//file de sortie. Rend le nombre d'octets pris, 0 en cas d'échec.
	int  Send(WORD streamId,DWORD ppid,const BYTE* data,DWORD size);

	//Retire le plus ancien datagramme en attente. false quand la file est vide.
	//À appeler depuis le thread du porteur, celui qui a le droit de chiffrer.
	bool GetOutbound(std::string& datagram);

	bool IsUp() const { return up; }

	//Les timers de la pile, qui sont GLOBAUX à usrsctp. L'horloge est tenue ici :
	//chaque porteur appelle à sa cadence, et le temps n'avance qu'au rythme réel
	//quel que soit le nombre d'appelants.
	static void HandleTimers();

private:
	//Callbacks d'usrsctp. `addr` / `ulp_info` portent le jeton d'association.
	static int OnOutput(void* addr,void* buffer,size_t length,BYTE tos,BYTE set_df);
	static int OnReceive(struct socket* sock,union sctp_sockstore addr,void* data,
			     size_t datalen,struct sctp_rcvinfo rcv,int flags,void* ulp_info);

	int  Shutdown(bool notify);
	void Enqueue(const BYTE* data,DWORD size);
	void HandleData(WORD streamId,DWORD ppid,const BYTE* data,DWORD size,bool complete);
	void HandleNotification(const union sctp_notification* notif,size_t size);

	Listener&	listener;
	struct socket*	sock;
	//Jeton d'association : c'est lui qu'usrsctp rend au callback de sortie, et
	//c'est par lui qu'on retrouve l'instance. Un entier opaque, JAMAIS le `this` :
	//un datagramme en retard sur une destruction retrouverait alors un objet
	//libéré, là où une table rendue vide le fait simplement tomber.
	void*		token;
	bool		up;

	//File de sortie, et sa borne : un porteur qui ne vide plus est un défaut, pas
	//une raison de grossir sans fin.
	static const size_t maxOutbound = 64;
	std::mutex		outboundMutex;
	std::list<std::string>	outbound;

	//Messages partiels par flux : usrsctp livre par morceaux, la fin est
	//marquée par MSG_EOR. Un T140block est minuscule, mais rien n'oblige le
	//pair à n'envoyer que cela.
	static const size_t maxPartialStreams = 16;
	std::map<WORD,std::string> partial;
};

#endif /* SCTPTRANSPORT_H */
