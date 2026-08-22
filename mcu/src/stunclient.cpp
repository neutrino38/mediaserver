#include "stunclient.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"
#include "medkit/stunmessage.h"
#include "tools.h"

/**
 * stunclient.cpp — voir stunclient.h pour le contrat et ce que la sonde prouve.
 */

namespace
{
	//RFC 5389 §6 : le magic cookie, en tête de tout message STUN moderne.
	const BYTE kMagicCookie[4] = { 0x21, 0x12, 0xA4, 0x42 };

	//Retransmissions : un datagramme perdu ne doit pas faire échouer un démarrage.
	const int kAttempts       = 3;
	const int kTimeoutsMs[3]  = { 500, 1000, 2000 };

	//Décode MAPPED-ADDRESS / XOR-MAPPED-ADDRESS. `xored` dit lequel des deux.
	//Rend false sur tout ce qui n'est pas une adresse exploitable — un serveur
	//STUN qui répond n'importe quoi ne doit pas produire une annonce fausse.
	bool DecodeAddress(const STUNMessage::Attribute* attr, bool xored, const BYTE* transId,
	                   IPAddress& address, WORD& port)
	{
		if (!attr || attr->size < 8 || !attr->attr)
			return false;

		const BYTE family = attr->attr[1];

		BYTE raw[16];
		BYTE portBytes[2];

		memcpy(portBytes, attr->attr + 2, 2);

		if (family == 0x01)
		{
			if (attr->size < 8)
				return false;
			memcpy(raw, attr->attr + 4, 4);
		}
		else if (family == 0x02)
		{
			if (attr->size < 20)
				return false;
			memcpy(raw, attr->attr + 4, 16);
		}
		else
		{
			return false;
		}

		if (xored)
		{
			portBytes[0] ^= kMagicCookie[0];
			portBytes[1] ^= kMagicCookie[1];

			//v4 : XOR sur le cookie. v6 : cookie PUIS transaction ID (RFC 5389
			//§15.2) — s'arrêter au cookie laisserait 12 octets en clair.
			for (int i = 0; i < 4; ++i)
				raw[i] ^= kMagicCookie[i];

			if (family == 0x02)
				for (int i = 0; i < 12; ++i)
					raw[4 + i] ^= transId[i];
		}

		port = (WORD)((portBytes[0] << 8) | portBytes[1]);

		sockaddr_storage sa;
		memset(&sa, 0, sizeof(sa));

		if (family == 0x01)
		{
			sockaddr_in* in = (sockaddr_in*)&sa;
			in->sin_family = AF_INET;
			memcpy(&in->sin_addr, raw, 4);
		}
		else
		{
			sockaddr_in6* in6 = (sockaddr_in6*)&sa;
			in6->sin6_family = AF_INET6;
			memcpy(&in6->sin6_addr, raw, 16);
		}

		address = IPAddress::FromSockaddr((const sockaddr*)&sa);
		return address.IsSet();
	}
}

const char* StunClient::DefaultServer()
{
	//Serveur public, joignable partout, sans compte : le défaut doit marcher
	//sans configuration. Un déploiement qui ne veut pas dépendre d'un tiers pose
	//son propre serveur avec --stun-server — c'est le cas recommandé en
	//production, et la raison pour laquelle cette valeur est un DÉFAUT et non
	//une constante enfouie.
	return "stun.l.google.com:19302";
}

bool StunClient::ParseServer(const char* text, IPEndpoint& out, std::string& error)
{
	if (!text || !*text)
	{
		error = "serveur STUN vide";
		return false;
	}

	std::string host(text);
	WORD        port = 3478;   //RFC 5389 §9

	//Littéral IPv6 entre crochets : « [2001:db8::1]:3478 ». Les crochets sont
	//une syntaxe d'URL, imposée ici par le « : » du port — sans eux la chaîne
	//serait indécoupable.
	if (host[0] == '[')
	{
		const size_t close = host.find(']');
		if (close == std::string::npos)
		{
			error = "serveur STUN : crochet fermant manquant";
			return false;
		}

		std::string portPart = host.substr(close + 1);
		host = host.substr(1, close - 1);

		if (!portPart.empty())
		{
			if (portPart[0] != ':')
			{
				error = "serveur STUN : port attendu apres ]";
				return false;
			}
			port = (WORD)atoi(portPart.c_str() + 1);
		}
	}
	else
	{
		//Un seul « : » = hôte:port. Plusieurs = littéral IPv6 nu, qu'on refuse
		//ici : il faudrait des crochets pour distinguer l'adresse du port.
		const size_t colon = host.find(':');
		if (colon != std::string::npos && host.find(':', colon + 1) == std::string::npos)
		{
			port = (WORD)atoi(host.c_str() + colon + 1);
			host = host.substr(0, colon);
		}
	}

	if (!port)
	{
		error = "serveur STUN : port invalide";
		return false;
	}

	//Résolution en IPv4 : --nat auto ne sert que le NAT IPv4, il n'y a pas de
	//NAT IPv6 dans ce produit (NETWORK-CONFIGURATION.md).
	int                  err = 0;
	std::list<IPAddress> addrs = IPAddress::Resolve(host.c_str(), err, AF_INET);

	IPAddress server;
	for (std::list<IPAddress>::const_iterator it = addrs.begin(); it != addrs.end(); ++it)
	{
		if (it->IsV4())
		{
			server = *it;
			break;
		}
	}

	if (!server.IsSet())
	{
		error = "serveur STUN \"" + host + "\" sans adresse IPv4 (err " + std::to_string(err) + ")";
		return false;
	}

	out = server.To(port);
	return true;
}

namespace
{
	//Ouvre une socket UDP liée à `localBind` sur un port ÉPHÉMÈRE, hors de la
	//plage RTP : la découverte ne doit pas consommer un port que le média
	//utilisera. Rend -1 et remplit `error` en cas d'échec.
	int OpenProbeSocket(const IPAddress& localBind, int family, WORD& localPort, std::string& error)
	{
		const int fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0)
		{
			error = std::string("socket: ") + strerror(errno);
			return -1;
		}

		if (localBind.IsSet())
		{
			const IPEndpoint bindTo = localBind.To(0);
			if (bind(fd, bindTo, bindTo.Len()) != 0)
			{
				error = "bind sur " + localBind.ToString() + " : " + strerror(errno);
				close(fd);
				return -1;
			}
		}

		//Port local réellement obtenu : c'est LUI qu'on comparera au port public.
		IPEndpoint local;
		if (getsockname(fd, local.Data(), local.LenPtr()) != 0)
		{
			error = std::string("getsockname: ") + strerror(errno);
			close(fd);
			return -1;
		}

		localPort = local.Port();
		return fd;
	}
}

//Sonde sur une socket DÉJÀ ouverte : c'est ce que Discover appelle, après avoir
//ouvert ses deux sockets — voir là-bas pourquoi elles doivent coexister.
static bool ProbeOn(int fd, WORD localPort, const IPEndpoint& server,
                    StunClient::Mapping& out, std::string& error)
{
	out.localPort = localPort;

	//Transaction ID : 96 bits qui doivent être imprévisibles (RFC 5389 §6). Le
	//temps courant et le descripteur suffisent ici — nous ne sommes pas exposés,
	//c'est nous qui initions, et la réponse est appariée sur ce même ID.
	BYTE transId[12];
	set4(transId, 0, (DWORD)localPort);
	set8(transId, 4, getTime());

	STUNMessage request(STUNMessage::Request, STUNMessage::Binding, transId);

	BYTE  buffer[MTU];
	DWORD len = request.NonAuthenticatedFingerPrint(buffer, sizeof(buffer));

	bool answered = false;

	for (int attempt = 0; attempt < kAttempts && !answered; ++attempt)
	{
		if (sendto(fd, buffer, len, 0, server, server.Len()) != (ssize_t)len)
		{
			error = std::string("sendto: ") + strerror(errno);
			return false;
		}

		pollfd pfd;
		pfd.fd      = fd;
		pfd.events  = POLLIN;
		pfd.revents = 0;

		const int ret = poll(&pfd, 1, kTimeoutsMs[attempt]);

		//Interruption : ce n'est pas une erreur dure, on retente.
		if (ret < 0 && errno == EINTR)
			continue;

		if (ret < 0)
		{
			error = std::string("poll: ") + strerror(errno);
			return false;
		}

		if (ret == 0)
			continue;   //silence : retransmission

		BYTE       response[MTU];
		IPEndpoint from;
		const ssize_t size = recvfrom(fd, response, sizeof(response), 0, from.Data(), from.LenPtr());

		if (size <= 0)
			continue;

		STUNMessage* msg = STUNMessage::Parse(response, size);
		if (!msg)
			continue;   //ce n'est pas du STUN : on ignore et on retente

		if (msg->GetType() != STUNMessage::Response || msg->GetMethod() != STUNMessage::Binding)
		{
			delete msg;
			continue;
		}

		//XOR-MAPPED d'abord (RFC 5389), MAPPED en repli (RFC 3489) : les vieux
		//serveurs ne connaissent que le second.
		bool decoded = DecodeAddress(msg->GetAttribute(STUNMessage::Attribute::XorMappedAddress),
		                             true, transId, out.address, out.port);

		if (!decoded)
			decoded = DecodeAddress(msg->GetAttribute(STUNMessage::Attribute::MappedAddress),
			                        false, transId, out.address, out.port);

		delete msg;

		if (!decoded)
		{
			error = "reponse STUN sans adresse exploitable";
			return false;
		}

		answered = true;
	}

	if (!answered)
	{
		error = "aucune reponse du serveur STUN " + server.ToString();
		return false;
	}

	return true;
}

bool StunClient::Probe(const IPAddress& localBind, const IPEndpoint& server,
                       Mapping& out, std::string& error)
{
	if (!server.IsSet())
	{
		error = "serveur STUN inconnu";
		return false;
	}

	WORD      localPort = 0;
	const int fd = OpenProbeSocket(localBind, server.Address().Family(), localPort, error);

	if (fd < 0)
		return false;

	const bool ok = ProbeOn(fd, localPort, server, out, error);
	close(fd);
	return ok;
}

bool StunClient::Discover(const IPAddress& localBind, const IPEndpoint& server,
                          IPAddress& publicAddress, bool& oneToOne, std::string& error)
{
	oneToOne = false;

	if (!server.IsSet())
	{
		error = "serveur STUN inconnu";
		return false;
	}

	//LES DEUX SOCKETS SONT OUVERTES D'ABORD, ET TENUES OUVERTES pendant les deux
	//sondes. Sonder l'une puis l'autre en fermant entre les deux laisserait le
	//noyau réattribuer le MÊME port éphémère à la seconde — et le verdict
	//deviendrait impossible à rendre, de façon intermittente et seulement sous
	//charge. Deux sockets vivantes ne peuvent pas partager un port.
	WORD      firstPort = 0, secondPort = 0;
	const int firstFd  = OpenProbeSocket(localBind, server.Address().Family(), firstPort, error);

	if (firstFd < 0)
		return false;

	const int secondFd = OpenProbeSocket(localBind, server.Address().Family(), secondPort, error);

	if (secondFd < 0)
	{
		close(firstFd);
		return false;
	}

	Mapping first, second;
	const bool ok = ProbeOn(firstFd, firstPort, server, first, error)
	             && ProbeOn(secondFd, secondPort, server, second, error);

	close(firstFd);
	close(secondFd);

	if (!ok)
		return false;

	//Garde-fou : deux sockets ouvertes ne peuvent pas porter le même port, mais
	//si cela arrivait le verdict n'aurait aucune valeur.
	if (first.localPort == second.localPort)
	{
		error = "les deux sondes ont obtenu le meme port local, verdict impossible";
		return false;
	}

	if (!(first.address == second.address))
	{
		error = "adresse publique instable : " + first.address.ToString() +
		        " puis " + second.address.ToString() +
		        " (NAT a mapping dependant de la destination, ou repartition de charge)";
		return false;
	}

	publicAddress = first.address;

	//Le verdict : les DEUX ports sont conservés. C'est la signature d'un mapping
	//1:1 — l'adresse change, les ports non, donc le port RTP annoncé dans nos
	//SDP est celui que le pair doit joindre.
	oneToOne = (first.port == first.localPort) && (second.port == second.localPort);

	if (!oneToOne)
		error = "le NAT translate les ports (local " + std::to_string(first.localPort) +
		        " -> public " + std::to_string(first.port) + ", local " +
		        std::to_string(second.localPort) + " -> public " + std::to_string(second.port) + ")";

	return true;
}
