#include "ipaddress.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * ipaddress.cpp — voir ipaddress.h pour le contrat et les trois invariants.
 *
 * Une seule règle d'écriture ici : TOUTE classification passe par la forme
 * dé-mappée (invariant 3). Les prédicats appellent donc `Unmapped()` en
 * ouverture, sans exception — c'est ce qui garantit qu'une même adresse ne
 * change pas de politique selon la famille de la socket qui l'a apportée.
 */

namespace
{
	// Octets bruts d'une in6_addr, pour les tests de préfixe.
	inline const BYTE* Bytes(const in6_addr& a)
	{
		return (const BYTE*)&a;
	}

	// Forme « IPv4-compatible » ::a.b.c.d (RFC 4291 §2.5.5.1), DÉPRÉCIÉE.
	// `::` et `::1` en font partie syntaxiquement : ce sont les deux seules
	// exceptions à conserver, elles ont un sens propre (non spécifiée, loopback).
	bool IsDeprecatedV4Compatible(const in6_addr& a)
	{
		const BYTE* b = Bytes(a);
		for (int i = 0; i < 12; ++i)
			if (b[i]) return false;
		//  ::         -> non spécifiée
		//  ::1        -> loopback
		if (!b[12] && !b[13] && !b[14] && (b[15] == 0 || b[15] == 1))
			return false;
		return true;
	}

	// « fe80::1%eth0 » -> adresse + index d'interface. Rend false si la zone est
	// présente mais inexploitable (nom d'interface inconnu et pas un nombre) :
	// mieux vaut refuser que d'émettre par une interface arbitraire.
	bool SplitZone(const char* text, std::string& addr, DWORD& scope)
	{
		const char* pct = strchr(text, '%');
		if (!pct)
		{
			addr  = text;
			scope = 0;
			return true;
		}

		addr = std::string(text, pct - text);

		const char* zone = pct + 1;
		if (!*zone)
			return false;

		//Nom d'interface (le cas courant : ce qu'écrit un navigateur)
		const unsigned int index = if_nametoindex(zone);
		if (index)
		{
			scope = index;
			return true;
		}

		//À défaut, index numérique — ce que rend inet_ntop sur une interface
		//disparue depuis, et ce que porte parfois un candidat ICE recopié.
		char* end = NULL;
		const unsigned long parsed = strtoul(zone, &end, 10);
		if (!end || *end || !parsed || parsed > 0xFFFFFFFFul)
			return false;

		scope = (DWORD)parsed;
		return true;
	}
}

/***********************************
* Parse
*	Littéral strict, sans DNS. Voir la liste des formes acceptées et refusées
*	dans ipaddress.h.
***********************************/
IPAddress IPAddress::Parse(const char* text, int& err)
{
	IPAddress out;

	err = EINVAL;

	if (!text || !*text)
		return out;

	//Les crochets appartiennent aux URL (RFC 3986 §3.2.2), pas aux adresses :
	//un appelant qui recopie l'hôte d'une URL sans le déparenthéser doit se
	//faire jeter, pas produire une destination silencieuse.
	if (strchr(text, '[') || strchr(text, ']'))
		return out;

	//Espaces de tête ou de queue : inet_pton les refuse déjà, mais le dire ici
	//évite de dépendre de ce détail d'implémentation.
	const size_t len = strlen(text);
	if (isspace((unsigned char)text[0]) || isspace((unsigned char)text[len - 1]))
		return out;

	//Pas de ':' -> c'est une v4 (ou rien). inet_pton est STRICT : il refuse les
	//formes courtes et octales qu'acceptait inet_addr (192.168.1, 0300.0250.0.1).
	if (!strchr(text, ':'))
	{
		in_addr v4;
		if (inet_pton(AF_INET, text, &v4) != 1)
			return out;

		out.family  = AF_INET;
		out.addr.v4 = v4;
		err = 0;
		return out;
	}

	std::string literal;
	DWORD       scope = 0;
	if (!SplitZone(text, literal, scope))
		return out;

	in6_addr v6;
	if (inet_pton(AF_INET6, literal.c_str(), &v6) != 1)
		return out;

	if (IsDeprecatedV4Compatible(v6))
		return out;

	//Dé-mappage à l'entrée (invariant 3) : le reste du programme ne voit jamais
	//de ::ffff:a.b.c.d, donc aucune comparaison ne peut se tromper de forme.
	if (IN6_IS_ADDR_V4MAPPED(&v6))
	{
		out.family = AF_INET;
		memcpy(&out.addr.v4, Bytes(v6) + 12, sizeof(out.addr.v4));
		err = 0;
		return out;
	}

	out.family  = AF_INET6;
	out.addr.v6 = v6;
	out.scope   = scope;
	err = 0;
	return out;
}

IPAddress IPAddress::Parse(const std::string& text, int& err)
{
	return Parse(text.c_str(), err);
}

IPAddress IPAddress::Parse(const char* text)
{
	int err = 0;
	return Parse(text, err);
}

IPAddress IPAddress::Parse(const std::string& text)
{
	int err = 0;
	return Parse(text.c_str(), err);
}

/***********************************
* Resolve
*	Littéral OU nom d'hôte. L'ordre du résultat est le NÔTRE, pas celui du
*	résolveur : cette adresse finit dans une ligne c= de SDP, elle ne peut pas
*	dépendre de /etc/gai.conf ni de l'humeur du serveur DNS.
***********************************/
std::list<IPAddress> IPAddress::Resolve(const char* host, int& err, int prefer)
{
	std::list<IPAddress> out;

	if (!host || !*host)
	{
		err = EINVAL;
		return out;
	}

	//Un littéral se passe de résolveur — et surtout, il ne doit pas pouvoir
	//devenir autre chose que lui-même.
	int       parseErr = 0;
	IPAddress literal  = Parse(host, parseErr);
	if (literal.IsSet())
	{
		err = 0;
		out.push_back(literal);
		return out;
	}

	addrinfo hints = {};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	addrinfo* res = NULL;
	const int rc = getaddrinfo(host, NULL, &hints, &res);
	if (rc)
	{
		//Codes errno, jamais des EAI_* : ceux-ci sont négatifs en glibc et
		//aucun appelant ne les reconnaîtrait (cf. ipaddress.h).
		err = (rc == EAI_AGAIN) ? EAGAIN : ENOENT;
		if (res)
			freeaddrinfo(res);
		return out;
	}

	//Deux passes, famille préférée d'abord : l'ordre du résolveur n'est
	//conservé qu'À L'INTÉRIEUR d'une famille.
	std::list<IPAddress> preferred;
	std::list<IPAddress> other;

	for (addrinfo* p = res; p; p = p->ai_next)
	{
		const IPAddress addr = FromSockaddr(p->ai_addr);
		if (!addr.IsAnnounceable())
			continue;

		//Doublons : un nom porte souvent la même adresse pour SOCK_DGRAM et
		//SOCK_STREAM selon la configuration du résolveur.
		bool seen = false;
		for (std::list<IPAddress>::const_iterator it = preferred.begin(); it != preferred.end() && !seen; ++it)
			seen = (*it == addr);
		for (std::list<IPAddress>::const_iterator it = other.begin(); it != other.end() && !seen; ++it)
			seen = (*it == addr);
		if (seen)
			continue;

		if (prefer != AF_UNSPEC && addr.Family() == prefer)
			preferred.push_back(addr);
		else
			other.push_back(addr);
	}

	freeaddrinfo(res);

	out.splice(out.end(), preferred);
	out.splice(out.end(), other);

	//Le nom se résout, mais rien de publiable (que de la loopback, par exemple).
	err = out.empty() ? ENOENT : 0;
	return out;
}

std::list<IPAddress> IPAddress::Resolve(const std::string& host, int& err, int prefer)
{
	return Resolve(host.c_str(), err, prefer);
}

/***********************************
* FromSockaddr
*	Ce que rend le noyau. Dé-mappe (invariant 3) et récupère la zone.
***********************************/
IPAddress IPAddress::FromSockaddr(const sockaddr* addr)
{
	IPAddress out;

	if (!addr)
		return out;

	if (addr->sa_family == AF_INET)
	{
		const sockaddr_in* in = (const sockaddr_in*)addr;
		out.family  = AF_INET;
		out.addr.v4 = in->sin_addr;
		return out;
	}

	if (addr->sa_family == AF_INET6)
	{
		const sockaddr_in6* in6 = (const sockaddr_in6*)addr;

		if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr))
		{
			out.family = AF_INET;
			memcpy(&out.addr.v4, Bytes(in6->sin6_addr) + 12, sizeof(out.addr.v4));
			return out;
		}

		out.family  = AF_INET6;
		out.addr.v6 = in6->sin6_addr;
		out.scope   = in6->sin6_scope_id;
		return out;
	}

	//Famille inconnue (AF_UNIX, AF_PACKET…) : adresse vide, pas d'exception.
	return out;
}

/***********************************
* Any
*	Adresse d'écoute « toutes interfaces ».
***********************************/
IPAddress IPAddress::Any(int family)
{
	IPAddress out;

	if (family == AF_INET)
	{
		out.family = AF_INET;
		out.addr.v4.s_addr = htonl(INADDR_ANY);
	}
	else if (family == AF_INET6)
	{
		out.family  = AF_INET6;
		out.addr.v6 = in6addr_any;
	}

	return out;
}

/***********************************
* Mapping v4 <-> v6
***********************************/
bool IPAddress::IsV4Mapped() const
{
	return family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&addr.v6);
}

IPAddress IPAddress::Unmapped() const
{
	if (!IsV4Mapped())
		return *this;

	IPAddress out;
	out.family = AF_INET;
	memcpy(&out.addr.v4, Bytes(addr.v6) + 12, sizeof(out.addr.v4));
	//Une adresse mappée ne porte pas de zone : rien à recopier.
	return out;
}

IPAddress IPAddress::MappedToV6() const
{
	if (family != AF_INET)
		return *this;

	IPAddress out;
	out.family = AF_INET6;

	BYTE* b = (BYTE*)&out.addr.v6;
	memset(b, 0, 10);
	b[10] = 0xFF;
	b[11] = 0xFF;
	memcpy(b + 12, &addr.v4, sizeof(addr.v4));

	return out;
}

/***********************************
* Classification — toujours sur la forme dé-mappée
***********************************/
bool IPAddress::IsUnspecified() const
{
	const IPAddress a = Unmapped();

	if (a.family == AF_INET)
		return a.addr.v4.s_addr == 0;
	if (a.family == AF_INET6)
		return IN6_IS_ADDR_UNSPECIFIED(&a.addr.v6);

	//Une adresse vide n'est pas « l'adresse non spécifiée » : c'est l'absence
	//d'adresse. La distinction est tout l'intérêt de l'invariant 1.
	return false;
}

bool IPAddress::IsLoopback() const
{
	const IPAddress a = Unmapped();

	if (a.family == AF_INET)
		return (ntohl(a.addr.v4.s_addr) & 0xFF000000) == 0x7F000000;   //127.0.0.0/8
	if (a.family == AF_INET6)
		return IN6_IS_ADDR_LOOPBACK(&a.addr.v6);

	return false;
}

bool IPAddress::IsMulticast() const
{
	const IPAddress a = Unmapped();

	if (a.family == AF_INET)
		return (ntohl(a.addr.v4.s_addr) & 0xF0000000) == 0xE0000000;   //224.0.0.0/4
	if (a.family == AF_INET6)
		return IN6_IS_ADDR_MULTICAST(&a.addr.v6);

	return false;
}

bool IPAddress::IsLinkLocal() const
{
	const IPAddress a = Unmapped();

	if (a.family == AF_INET)
		return (ntohl(a.addr.v4.s_addr) & 0xFFFF0000) == 0xA9FE0000;   //169.254.0.0/16
	if (a.family == AF_INET6)
		return IN6_IS_ADDR_LINKLOCAL(&a.addr.v6);                      //fe80::/10

	return false;
}

bool IPAddress::IsPrivateV4() const
{
	const IPAddress a = Unmapped();

	if (a.family != AF_INET)
		return false;

	//Transposition à l'identique de l'historique RTPSession::IsRFC1918.
	const DWORD ip = ntohl(a.addr.v4.s_addr);

	if ((ip & 0xFF000000) == 0x0A000000) return true;   //10.0.0.0/8
	if ((ip & 0xFFF00000) == 0xAC100000) return true;   //172.16.0.0/12
	if ((ip & 0xFFFF0000) == 0xC0A80000) return true;   //192.168.0.0/16
	if ((ip & 0xFFC00000) == 0x64400000) return true;   //100.64.0.0/10, NAT opérateur (RFC 6598)
	if ((ip & 0xFFFF0000) == 0xA9FE0000) return true;   //169.254.0.0/16 (RFC 3927)

	return false;
}

bool IPAddress::IsUniqueLocalV6() const
{
	//fc00::/7 (RFC 4193). Une ULA n'est jamais mappée : pas de Unmapped() ici.
	return family == AF_INET6 && (Bytes(addr.v6)[0] & 0xFE) == 0xFC;
}

bool IPAddress::IsPrivate() const
{
	const IPAddress a = Unmapped();

	if (a.family == AF_INET)
	{
		//Registre IANA « IPv4 Special-Purpose Address » (RFC 6890). Les plages
		//PRIVÉES au sens propre sont dans IsPrivateV4 ; s'y ajoutent ici celles
		//qui ne sont pas routables sans être privées pour autant.
		const DWORD ip = ntohl(a.addr.v4.s_addr);

		if (IsPrivateV4())                   return true;
		if ((ip & 0xFF000000) == 0x00000000) return true;   //0.0.0.0/8      « this network »
		if ((ip & 0xFF000000) == 0x7F000000) return true;   //127.0.0.0/8    loopback
		if ((ip & 0xFFFFFF00) == 0xC0000000) return true;   //192.0.0.0/24   IETF protocol assignments
		if ((ip & 0xFFFFFF00) == 0xC0000200) return true;   //192.0.2.0/24   documentation (RFC 5737)
		if ((ip & 0xFFFE0000) == 0xC6120000) return true;   //198.18.0.0/15  benchmarking (RFC 2544)
		if ((ip & 0xFFFFFF00) == 0xC6336400) return true;   //198.51.100.0/24 documentation
		if ((ip & 0xFFFFFF00) == 0xCB007100) return true;   //203.0.113.0/24 documentation
		if ((ip & 0xF0000000) == 0xF0000000) return true;   //240.0.0.0/4    réservé + 255.255.255.255

		//Le multicast (224/4) est EXCLU : routable, simplement pas unicast.
		return false;
	}

	if (a.family == AF_INET6)
	{
		const BYTE* b = Bytes(a.addr.v6);

		if (IN6_IS_ADDR_UNSPECIFIED(&a.addr.v6)) return true;   //::/128
		if (IN6_IS_ADDR_LOOPBACK(&a.addr.v6))    return true;   //::1/128
		if (a.IsUniqueLocalV6())                 return true;   //fc00::/7  ULA (RFC 4193)
		if (IN6_IS_ADDR_LINKLOCAL(&a.addr.v6))   return true;   //fe80::/10
		if (IN6_IS_ADDR_SITELOCAL(&a.addr.v6))   return true;   //fec0::/10 déprécié (RFC 3879)

		//100::/64 — trou noir de mise au rebut (RFC 6666)
		if (b[0] == 0x01 && b[1] == 0x00 && !b[2] && !b[3] && !b[4] && !b[5] && !b[6] && !b[7])
			return true;

		//2001:db8::/32 (RFC 3849) et 3fff::/20 (RFC 9637) — documentation
		if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8)
			return true;
		if (b[0] == 0x3F && b[1] == 0xFF && (b[2] & 0xF0) == 0x00)
			return true;

		//Teredo et 6to4 sont GLOBALES : voir le commentaire dans ipaddress.h.
		//Le multicast (ff00::/8) est exclu, comme en v4.
		return false;
	}

	return false;
}

bool IPAddress::IsTeredo() const
{
	if (family != AF_INET6)
		return false;

	const BYTE* b = Bytes(addr.v6);
	return b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00;   //2001::/32
}

bool IPAddress::Is6to4() const
{
	if (family != AF_INET6)
		return false;

	const BYTE* b = Bytes(addr.v6);
	return b[0] == 0x20 && b[1] == 0x02;                                   //2002::/16
}

bool IPAddress::NeedsScope() const
{
	return family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&addr.v6) && scope == 0;
}

bool IPAddress::IsUnicastDestination() const
{
	if (!IsSet())
		return false;

	//L'adresse non spécifiée n'est pas une destination : c'est une demande de
	//latch, et c'est à l'appelant (SetRemotePort) de la traiter comme telle.
	if (IsUnspecified())
		return false;

	if (IsMulticast())
		return false;

	//Link-local sans zone : le noyau ne saurait pas par quelle interface sortir.
	if (NeedsScope())
		return false;

	return true;
}

bool IPAddress::IsAnnounceable() const
{
	if (!IsSet())
		return false;

	if (IsUnspecified() || IsLoopback() || IsMulticast())
		return false;

	//Une link-local ne veut rien dire chez le pair, zone ou pas : la zone est
	//un index d'interface LOCAL, il ne se transporte pas.
	if (IsLinkLocal())
		return false;

	return true;
}

/***********************************
* Sortie texte
***********************************/
std::string IPAddress::ToString() const
{
	char buf[INET6_ADDRSTRLEN];

	if (family == AF_INET)
	{
		if (!inet_ntop(AF_INET, &addr.v4, buf, sizeof(buf)))
			return std::string();
		return std::string(buf);
	}

	if (family == AF_INET6)
	{
		//inet_ntop de la glibc est conforme RFC 5952 : minuscules, zéros de
		//tête supprimés, plus longue suite de zéros compressée, et JAMAIS un
		//groupe unique compressé (la condition est `best.len > 1`).
		if (!inet_ntop(AF_INET6, &addr.v6, buf, sizeof(buf)))
			return std::string();

		std::string out(buf);

		if (scope)
		{
			char name[IF_NAMESIZE];
			if (if_indextoname(scope, name))
			{
				out += "%";
				out += name;
			}
			else
			{
				char index[16];
				snprintf(index, sizeof(index), "%%%u", (unsigned)scope);
				out += index;
			}
		}

		return out;
	}

	//Adresse vide : chaîne vide, jamais NULL, jamais un tampon partagé.
	return std::string();
}

std::string IPAddress::ToUrlString() const
{
	if (family != AF_INET6)
		return ToString();

	std::string out = "[";

	//RFC 6874 : dans une URL, le '%' d'une zone s'écrit "%25". Le cas est
	//théorique (publier une link-local n'a pas de sens, IsAnnounceable la
	//refuse), mais produire une URL invalide en serait un autre.
	const std::string text = ToString();
	const size_t      pct  = text.find('%');

	if (pct == std::string::npos)
		out += text;
	else
		out += text.substr(0, pct) + "%25" + text.substr(pct + 1);

	out += "]";
	return out;
}

/***********************************
* Conversion vers les API noyau
***********************************/
IPEndpoint IPAddress::To(WORD port) const
{
	return IPEndpoint(*this, port, false);
}

IPEndpoint IPAddress::ToDualStack(WORD port) const
{
	return IPEndpoint(*this, port, true);
}

socklen_t IPAddress::FillSockaddr(sockaddr_storage& out, WORD port) const
{
	memset(&out, 0, sizeof(out));

	if (family == AF_INET)
	{
		sockaddr_in* in = (sockaddr_in*)&out;
		in->sin_family = AF_INET;
		in->sin_addr   = addr.v4;
		in->sin_port   = htons(port);
		return sizeof(sockaddr_in);
	}

	if (family == AF_INET6)
	{
		sockaddr_in6* in6 = (sockaddr_in6*)&out;
		in6->sin6_family   = AF_INET6;
		in6->sin6_addr     = addr.v6;
		in6->sin6_port     = htons(port);
		in6->sin6_scope_id = scope;
		return sizeof(sockaddr_in6);
	}

	return 0;
}

socklen_t IPAddress::FillDualStackSockaddr(sockaddr_storage& out, WORD port) const
{
	if (family == AF_INET)
		return MappedToV6().FillSockaddr(out, port);

	return FillSockaddr(out, port);
}

WORD IPAddress::PortOf(const sockaddr* addr)
{
	if (!addr)
		return 0;

	if (addr->sa_family == AF_INET)
		return ntohs(((const sockaddr_in*)addr)->sin_port);

	if (addr->sa_family == AF_INET6)
		return ntohs(((const sockaddr_in6*)addr)->sin6_port);

	return 0;
}

/***********************************
* Comparaison
***********************************/
bool IPAddress::operator==(const IPAddress& other) const
{
	const IPAddress a = Unmapped();
	const IPAddress b = other.Unmapped();

	if (a.family != b.family)
		return false;

	if (a.family == AF_UNSPEC)
		return true;   //deux adresses vides

	if (a.family == AF_INET)
		return a.addr.v4.s_addr == b.addr.v4.s_addr;

	if (memcmp(&a.addr.v6, &b.addr.v6, sizeof(a.addr.v6)) != 0)
		return false;

	//La zone ne départage QUE si les deux en portent une : un candidat reçu
	//avec zone et le même sans zone désignent le même hôte pour le latching.
	if (a.scope && b.scope)
		return a.scope == b.scope;

	return true;
}

bool IPAddress::operator<(const IPAddress& other) const
{
	const IPAddress a = Unmapped();
	const IPAddress b = other.Unmapped();

	if (a.family != b.family)
		return a.family < b.family;

	if (a.family == AF_INET)
		return memcmp(&a.addr.v4, &b.addr.v4, sizeof(a.addr.v4)) < 0;

	if (a.family == AF_INET6)
		return memcmp(&a.addr.v6, &b.addr.v6, sizeof(a.addr.v6)) < 0;

	//La zone est volontairement HORS de l'ordre : l'égalité la traite comme un
	//joker (scope 0), ce qui n'est pas transitif ; l'inclure ici donnerait un
	//ordre incohérent avec operator==. Deux adresses ne différant que par leur
	//zone sont donc équivalentes pour un std::map — ce que dit ipaddress.h.
	return false;
}


/***********************************
* IPEndpoint
***********************************/
IPEndpoint::IPEndpoint(const IPAddress& address, WORD port, bool dualStack)
{
	len = dualStack ? address.FillDualStackSockaddr(storage, port)
	                : address.FillSockaddr(storage, port);
}

IPEndpoint IPEndpoint::FromSockaddr(const sockaddr* sa, socklen_t sl)
{
	IPEndpoint out;

	if (!sa || sl == 0 || sl > (socklen_t)sizeof(out.storage))
		return out;

	memcpy(&out.storage, sa, sl);
	out.len = sl;
	return out;
}

IPAddress IPEndpoint::Address() const
{
	if (!len)
		return IPAddress();

	return IPAddress::FromSockaddr((const sockaddr*)&storage);
}

WORD IPEndpoint::Port() const
{
	if (!len)
		return 0;

	return IPAddress::PortOf((const sockaddr*)&storage);
}

std::string IPEndpoint::ToString() const
{
	if (!len)
		return std::string();

	char port[8];
	snprintf(port, sizeof(port), ":%u", (unsigned)Port());

	//Ici les crochets sont légitimes : un port suit (RFC 3986 §3.2.2).
	return Address().ToUrlString() + port;
}

bool IPEndpoint::operator==(const IPEndpoint& other) const
{
	if (!len || !other.len)
		return len == other.len;

	return Port() == other.Port() && Address() == other.Address();
}
