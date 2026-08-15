#ifndef _IPADDRESS_H_
#define _IPADDRESS_H_

#include <sys/socket.h>
#include <netinet/in.h>
#include <list>
#include <string>

#include "config.h"

class IPEndpoint;   // adresse + port, défini plus bas

/**
 * IPAddress — adresse IP d'une famille ou de l'autre, valeur copiable.
 *
 * C'est le « type maison sur sockaddr_storage » arbitré au §13 de `ipv6.md` :
 * il porte exactement ce que la suite adverse `mcu/tests/test_ipv6.cpp` exige,
 * et rien de plus. Il remplace partout le triplet historique
 * `in_addr_t` + `inet_addr` + `inet_ntoa`, dont les trois défauts sont :
 * une famille câblée dans le type, une erreur de conversion indiscernable
 * d'une adresse valide (`INADDR_NONE` == 255.255.255.255), et un tampon
 * statique partagé pour la sortie texte.
 *
 * ------------------------------------------------------------------------
 * TROIS INVARIANTS, dont tout le reste découle :
 *
 *  1. UNE ADRESSE N'EST JAMAIS À MOITIÉ CONSTRUITE. Une instance est soit
 *     vide (`!IsSet()`, famille AF_UNSPEC), soit une adresse valide. Il n'y a
 *     pas de valeur sentinelle : c'est `IsSet()` qui dit « pas encore de
 *     destination », plus `INADDR_ANY` (§1.5 de ipv6.md) — lequel redevient
 *     ce qu'il est, une adresse d'écoute légitime.
 *
 *  2. LE PORT N'EST PAS ICI. « 2001:db8::1:5000 » est une adresse v6 entière
 *     et valide, PAS « l'adresse 2001:db8::1, port 5000 ». Aucune API ne doit
 *     donc découper une chaîne « adresse:port » en v6 : le port reste un
 *     argument séparé, ce que verrouille le test
 *     `IPv6Notation.AdresseEtPortNeSeConcatenentJamais`. Les fonctions qui ont
 *     besoin des deux prennent le port en paramètre (`FillSockaddr`).
 *
 *  3. `::ffff:a.b.c.d` ET `a.b.c.d` SONT LE MÊME PAIR. C'est le piège n°1 du
 *     dual-stack : avec `IPV6_V6ONLY=0` (option arbitrée : socket unique), un
 *     pair IPv4 arrive mappé. Toute comparaison naïve conclurait « la source a
 *     changé » et déclencherait un rattrapage NAT parasite sur des appels v4
 *     qui marchaient. Ici, `operator==` et toute la classification
 *     (`IsPrivate`, `IsLoopback`, …) travaillent SUR LA FORME DÉ-MAPPÉE.
 *
 * ------------------------------------------------------------------------
 * CE QUE CETTE CLASSE NE FAIT PAS, volontairement :
 *   - pas de crochets à l'entrée : `[2001:db8::1]` appartient à la syntaxe des
 *     URL (RFC 3986 §3.2.2), pas à celle des adresses. `Parse` les refuse ;
 *     seul `ToUrlString()` les pose, en sortie. En SDP/ICE les champs sont
 *     séparés par des espaces : jamais de crochets (§6 de test_ipv6.cpp) ;
 *   - pas de socket, pas de connexion, pas de politique : elle dit ce qu'une
 *     adresse EST, jamais ce qu'il faut en faire. La politique de rattrapage
 *     NAT reste dans `RTPSession::NatCorrectable`, qui s'appuiera sur
 *     `IsPrivate()` ;
 *   - pas de couple adresse+port : c'est le rôle d'`IPEndpoint`, déclaré plus
 *     bas dans ce même fichier, qui porte la `sockaddr` prête à passer aux
 *     appels système (remplaçant de `sendAddr`/`sendRtcpAddr`).
 */
class IPAddress
{
public:
	// -----------------------------------------------------------------
	// Construction
	// -----------------------------------------------------------------

	// Adresse vide : aucune famille, aucun octet. C'est l'état « le
	// contrôleur ne nous a pas encore donné de destination ».
	IPAddress() = default;

	/**
	 * Parse — littéral SEULEMENT, strict, sans résolution DNS.
	 *
	 * Accepte :   192.0.2.1 · 2001:db8::1 · 2001:0DB8:0000:…:0001 (casse
	 *             haute et forme longue, RFC 4291 §2.2) · ::ffff:192.0.2.1
	 *             · :: (non spécifiée) · fe80::1%eth0 (zone, cf. Scope).
	 * Refuse :    NULL · "" · espace de tête ou de queue · [2001:db8::1]
	 *             · double compression · plus de 8 groupes · groupe de plus
	 *             de 4 chiffres · ::ffff:1.2.3.4.5
	 *             · ::a.b.c.d — forme « IPv4-compatible » DÉPRÉCIÉE par la
	 *               RFC 4291 §2.5.5.1, refusée sciemment : plus personne ne
	 *               l'émet, et l'accepter en silence ferait passer 0.0.0.2
	 *               pour une adresse routable.
	 *
	 * Rend une adresse. En cas d'échec, rend une adresse vide (famille
	 * AF_UNSPEC) et pose `err` à EINVAL ; en cas de succès, une adresse valide
	 * et `err` à 0.
	 * Aucun log : c'est à l'appelant de dire pourquoi il refuse, avec son
	 * contexte (le nom du pair, la jambe, l'appel).
	 *
	 * NOTE — le paramètre ne peut pas s'appeler `errno` : c'est une MACRO de
	 * <errno.h> (`#define errno (*__errno_location())`), donc un identifiant
	 * interdit. Il s'appelle `err`, et porte un code errno.
	 *
	 * L'échec de `Parse` n'a qu'une cause (la chaîne n'est pas une adresse),
	 * donc `IsSet()` sur le retour dit déjà tout : la surcharge sans `err`
	 * existe pour ne pas imposer une variable muette à chaque appel.
	 */
	static IPAddress Parse(const char* text, int & err);
	static IPAddress Parse(const std::string & text, int & err);
	static IPAddress Parse(const char* text);
	static IPAddress Parse(const std::string & text);

	/**
	 * Resolve — littéral OU nom d'hôte (getaddrinfo, donc A et AAAA).
	 *
	 * C'est ce dont `--public-ip` et l'auto-détection de l'adresse annoncée
	 * ont besoin : sur une machine double pile, donner un nom est la seule
	 * façon de laisser le résolveur trancher (§9 de test_ipv6.cpp).
	 *
	 * SUR UN HÔTE DOUBLE PILE, LE CHOIX EST À NOUS, PAS AU RÉSOLVEUR : l'ordre
	 * de `getaddrinfo` dépend de /etc/gai.conf et peut changer d'un appel à
	 * l'autre — or cette adresse finit dans la ligne `c=` d'un SDP. La règle,
	 * déterministe et documentée, est donc appliquée ici :
	 *
	 *      1. première adresse ANNONÇABLE de la famille préférée ;
	 *      2. à défaut, première adresse annonçable de l'autre famille ;
	 *      3. à défaut : échec.
	 *
	 * Cas special, si prefer = AF_UNSPEC
	 *
	 * On tente les DEUX résolutions et s'il y en a deux, on retoure les deux
	 *
	 * « Annonçable » = `IsAnnounceable()` (ni loopback, ni multicast, ni non
	 * spécifiée, ni link-local). `prefer` vaut AF_INET (défaut, comportement
	 * historique conservé), AF_INET6, ou AF_UNSPEC pour tenter les deux résolutions
	 * et retourner toutes les adresses annonçables
	 *
	 * >>> POINT DE REVUE : est-ce bien AF_INET qu'on préfère par défaut ? <<<
	 * Le conserver garantit qu'aucun déploiement existant ne change d'adresse
	 * annoncée le jour de la bascule. Une option CLI (`--prefer-family`)
	 * pourra l'ouvrir plus tard.
	 *
	 * La liste est TRIÉE selon la règle ci-dessus (famille préférée d'abord,
	 * ordre du résolveur conservé à l'intérieur d'une famille) : l'appelant qui
	 * n'en veut qu'une prend la première, et il prend toujours la même. Liste
	 * vide = échec, et `err` dit lequel — un code errno, JAMAIS un `EAI_*` :
	 *   0        succès
	 *   EINVAL   `host` NULL ou vide
	 *   ENOENT   nom inconnu, ou aucune adresse annonçable (EAI_NONAME…)
	 *   EAGAIN   échec temporaire de résolution (EAI_AGAIN)
	 * (mélanger les deux jeux de codes serait un piège : `EAI_NONAME` vaut -2
	 * en glibc, une valeur qu'aucun test `errno` ne reconnaît.)
	 */

	static std::list<IPAddress> Resolve(const char* host, int & err, int prefer = AF_INET);
	static std::list<IPAddress> Resolve(const std::string & host, int & err, int prefer = AF_INET);

	// Depuis ce que rend le noyau (`recvfrom`, `getsockname`, `accept`).
	// Une sockaddr_in6 v4-mappée est dé-mappée à l'entrée : le reste du
	// programme n'a jamais à savoir par quelle famille de socket le paquet
	// est arrivé (invariant 3). Rend une adresse vide si `addr` est NULL ou
	// d'une famille inconnue.
	static IPAddress FromSockaddr(const sockaddr* addr);

	// Adresse d'écoute « toutes interfaces » : `::` ou `0.0.0.0`.
	static IPAddress Any(int family = AF_INET6);

	// -----------------------------------------------------------------
	// État et famille
	// -----------------------------------------------------------------

	bool IsSet()  const { return family != AF_UNSPEC; }
	int  Family() const { return family; }
	bool IsV4()   const { return family == AF_INET;  }
	bool IsV6()   const { return family == AF_INET6; }

	// -----------------------------------------------------------------
	// Mapping v4 ↔ v6 (invariant 3)
	// -----------------------------------------------------------------

	// La valeur stockée est-elle `::ffff:a.b.c.d` ? Normalement JAMAIS vrai
	// pour une adresse issue de `Parse`/`FromSockaddr`, qui dé-mappent toutes
	// deux ; le prédicat existe pour les cas où l'on construit à la main.
	bool IsV4Mapped() const;

	// `::ffff:192.0.2.1` -> `192.0.2.1`. Sans effet sur le reste.
	IPAddress Unmapped() const;

	// `192.0.2.1` -> `::ffff:192.0.2.1`. C'est la forme qu'attend une socket
	// AF_INET6 avec IPV6_V6ONLY=0 quand la destination est v4 : voir
	// `FillDualStackSockaddr`, qui l'applique pour vous.
	IPAddress MappedToV6() const;

	// -----------------------------------------------------------------
	// Classification — toujours calculée sur la forme dé-mappée
	// -----------------------------------------------------------------

	bool IsUnspecified() const;   // 0.0.0.0 · ::   (= « latche-moi », pas une destination)
	bool IsLoopback()    const;   // 127.0.0.0/8 · ::1
	bool IsMulticast()   const;   // 224.0.0.0/4 · ff00::/8
	bool IsLinkLocal()   const;   // 169.254.0.0/16 · fe80::/10

	// Adresse privée / non routable sur l'Internet public : le pair qui
	// l'annonce est derrière un NAT (ou nous ment), son média ne peut pas nous
	// parvenir de là. C'est le SEUL cas qui ouvre droit au rattrapage — la
	// règle historique `RTPSession::IsRFC1918`, transposée :
	//   v4 : 10/8, 172.16/12, 192.168/16, 100.64/10 (RFC 6598), 169.254/16
	//   v6 : fc00::/7 (ULA, RFC 4193) — l'analogue direct du RFC 1918.
	//
	// >>> POINT DE REVUE : Teredo (2001::/32) et 6to4 (2002::/16) sont
	// classés NON privés. Ce sont des tunnels v6-sur-v4 : globaux au sens du
	// routage, mais encapsulant une adresse v4 NATée. Les compter comme privés
	// rouvrirait le rattrapage NAT sur des adresses globales — l'inverse de la
	// prudence recherchée. Ils restent des destinations parfaitement valides
	// (`IsUnicastDestination`), et `IsTeredo()`/`Is6to4()` permettent à un
	// appelant d'en décider autrement en connaissance de cause. <<<
	bool IsPrivate() const;

	// La MOITIÉ v4 de la règle ci-dessus, isolée : vrai uniquement pour une
	// adresse v4 privée — 10/8, 172.16/12, 192.168/16, 100.64/10, 169.254/16 —
	// y compris à travers le mapping, et TOUJOURS faux pour une v6.
	//
	// C'est ce prédicat, et non `IsPrivate()`, que doit consulter la politique
	// de rattrapage NAT (`RTPSession::NatCorrectable`) : le rattrapage n'a de
	// sens qu'en IPv4, puisqu'on ne supporte pas le NAT en v6 (§14 de ipv6.md).
	// Les garder distincts évite qu'une ULA — légitimement « privée » au sens
	// de la portée — ouvre par ricochet un rattrapage qui n'a aucune raison
	// d'exister en v6.
	bool IsPrivateV4() const;

	bool IsTeredo() const;        // 2001::/32
	bool Is6to4()   const;        // 2002::/16

	// Adresse link-local v6 SANS identifiant de zone : syntaxiquement valide,
	// pratiquement inatteignable (le noyau ne sait pas par quelle interface
	// sortir). Mieux vaut la refuser que d'émettre dans le vide.
	bool NeedsScope() const;

	// Peut-elle servir de destination unicast RTP ? Faux pour multicast,
	// non spécifiée, et link-local sans zone. C'est le prédicat que
	// `SetRemotePort` appliquera.
	bool IsUnicastDestination() const;

	// Peut-elle être publiée dans un SDP ? Plus strict : faux aussi pour la
	// loopback (annoncer ::1 publie une adresse que personne ne peut joindre)
	// et pour toute link-local, zone ou pas — une zone n'a aucun sens chez le
	// pair. C'est le prédicat de `SetAnnouncedIp`.
	bool IsAnnounceable() const;

	// -----------------------------------------------------------------
	// Identifiant de zone (link-local uniquement)
	// -----------------------------------------------------------------

	// Index d'interface (`if_nametoindex`), 0 si absent. Renseigné par
	// `Parse("fe80::1%eth0")` et par `FromSockaddr` (sin6_scope_id).
	DWORD Scope() const { return scope; }
	void  SetScope(DWORD index) { scope = index; }

	// -----------------------------------------------------------------
	// Sortie texte
	// -----------------------------------------------------------------

	/**
	 * ToString — forme CANONIQUE, RFC 5952 : minuscules, zéros de tête
	 * supprimés, plus longue suite de zéros compressée (et une suite d'UN
	 * seul groupe jamais compressée). C'est la chaîne que verra le pair dans
	 * le SDP : deux écritures de la même adresse doivent produire la même
	 * sortie, sinon les comparaisons de chaînes du contrôleur sont fausses.
	 *
	 * Une zone est suffixée `%<nom>` (`if_indextoname`, à défaut l'index).
	 * Une adresse vide rend "" — jamais NULL, jamais un tampon partagé
	 * (c'est le défaut d'`inet_ntoa` qu'on élimine).
	 */
	std::string ToString() const;

	// Idem, mais encadrée de `[...]` si — et seulement si — elle est v6
	// (RFC 3986 §3.2.2 : sans les crochets, le « : » du port est
	// indissociable de l'adresse). À n'utiliser QUE pour bâtir une URL :
	// jamais dans un `c=`, un `a=candidate:` ni un `raddr`.
	std::string ToUrlString() const;

	// -----------------------------------------------------------------
	// Conversion vers les API noyau
	// -----------------------------------------------------------------

	// Voie courte, et celle à préférer : rend un `IPEndpoint` (adresse + port +
	// sockaddr matérialisée) directement passable à sendto/bind/connect. Voir
	// la classe plus bas.
	IPEndpoint To(WORD port) const;              // famille native
	IPEndpoint ToDualStack(WORD port) const;     // toujours AF_INET6 (v4 mappée)

	// Remplit `out` dans la famille NATIVE de l'adresse et rend la longueur à
	// passer à sendto/bind (0 si l'adresse est vide). `port` est en ordre
	// hôte : la conversion réseau est faite ici, une fois, au bon endroit.
	// (Pour les appelants qui possèdent déjà leur `sockaddr_storage`.)
	socklen_t FillSockaddr(sockaddr_storage& out, WORD port) const;

	// Remplit `out` en AF_INET6 QUELLE QUE SOIT la famille, en mappant une
	// adresse v4 en `::ffff:a.b.c.d`. C'est la forme qu'exige la socket unique
	// dual-stack (IPV6_V6ONLY=0) arbitrée pour le média : un seul chemin
	// d'émission, aucune branche par famille dans `SendPacket`.
	socklen_t FillDualStackSockaddr(sockaddr_storage& out, WORD port) const;

	// Port d'une sockaddr, en ordre hôte, quelle que soit la famille : le
	// pendant lecture des deux fonctions ci-dessus. 0 si famille inconnue.
	// (Statique et à part parce que le port n'appartient pas à l'adresse —
	// invariant 2 — mais il faut bien le lire quelque part.)
	static WORD PortOf(const sockaddr* addr);

	// -----------------------------------------------------------------
	// Comparaison
	// -----------------------------------------------------------------

	// Égalité SÉMANTIQUE : dé-mappage d'abord, donc `::ffff:192.168.0.1` est
	// égale à `192.168.0.1` (invariant 3). La zone ne compte que si les deux
	// adresses en portent une : un candidat `fe80::1%eth0` reçu du pair et le
	// même sans zone désignent le même hôte pour nos besoins (latching).
	// Une adresse vide n'est égale qu'à une autre adresse vide.
	bool operator==(const IPAddress& other) const;
	bool operator!=(const IPAddress& other) const { return !(*this == other); }

	// Ordre total arbitraire mais stable (famille puis octets) : uniquement
	// pour pouvoir mettre une adresse dans un std::map/std::set.
	bool operator<(const IPAddress& other) const;

private:
	// Représentation : les octets dans leur famille d'origine, jamais une
	// sockaddr complète — une sockaddr_storage (128 octets) pour porter 4 ou
	// 16 octets utiles coûterait cher dans les structures RTP, et rouvrirait
	// la porte au port glissé dans l'adresse (invariant 2).
	int   family = AF_UNSPEC;
	union {
		in_addr  v4;
		in6_addr v6;
	} addr = {};
	DWORD scope = 0;   // index d'interface, 0 = aucun
};


/**
 * IPEndpoint — adresse + port, avec la `sockaddr` déjà matérialisée.
 *
 * C'est la pièce qui rend l'interface socket AGRÉABLE sans rouvrir aucun des
 * pièges qu'`IPAddress` referme. Le remplaçant direct de `sendAddr` /
 * `sendRtcpAddr` / `from_addr` dans `RTPSession`, et de tous les
 * `(sockaddr*)&addr, sizeof(addr)` semés dans les serveurs TCP.
 *
 * POURQUOI UNE CLASSE À PART, ET PAS UN CAST SUR `IPAddress` :
 *   - une `sockaddr` porte un PORT. Exposer un `operator const sockaddr*()`
 *     sur `IPAddress` obligerait la classe à porter un port fantôme, contre
 *     l'invariant 2 ;
 *   - un accesseur qui rendrait un pointeur vers un tampon interne à une
 *     temporaire (`addr.ToSockaddr()->…`) serait exactement le défaut
 *     d'`inet_ntoa` qu'on élimine : ici c'est l'OBJET qui possède la mémoire,
 *     sa durée de vie est visible à l'appel.
 *
 * Écriture attendue aux points d'usage :
 *
 *     IPEndpoint to = remote.ToDualStack(port);        // émission
 *     sendto(fd, data, len, 0, to, to.Len());
 *
 *     IPEndpoint from;                                 // réception
 *     recvfrom(fd, buf, size, 0, from.Data(), from.LenPtr());
 *     if (from.Address() == expected) { ... }
 *
 * `Data()`/`LenPtr()` forment le couple d'ÉCRITURE : `LenPtr()` repose la
 * capacité (`sizeof(sockaddr_storage)`) avant de rendre le pointeur, sans quoi
 * un `recvfrom` sur un objet déjà rempli tronquerait l'adresse source — le
 * classique de l'API socket.
 */
class IPEndpoint
{
public:
	// Vide : famille AF_UNSPEC, longueur 0.
	IPEndpoint() = default;

	// `dualStack` force AF_INET6 en mappant une adresse v4 (voir
	// IPAddress::FillDualStackSockaddr). Préférer les fabriques
	// `IPAddress::To()` / `IPAddress::ToDualStack()`, plus lisibles.
	IPEndpoint(const IPAddress& address, WORD port, bool dualStack = false);

	// Recopie ce que rend le noyau. Dé-mappe l'adresse (invariant 3) mais
	// conserve la sockaddr TELLE QUELLE : c'est elle qu'il faudra rendre au
	// noyau pour répondre à ce pair, sur la socket d'où elle vient.
	static IPEndpoint FromSockaddr(const sockaddr* sa, socklen_t len);

	bool      IsSet()   const { return len != 0; }
	IPAddress Address() const;
	WORD      Port()    const;   // ordre hôte

	// Lecture — ce qu'on passe à sendto/bind/connect.
	const sockaddr* Sockaddr() const { return (const sockaddr*)&storage; }
	socklen_t       Len()      const { return len; }

	// Conversion implicite : `sendto(fd, b, n, 0, to, to.Len())` compile tel
	// quel. Une seule conversion est déclarée, donc pas d'ambiguïté possible.
	operator const sockaddr*() const { return Sockaddr(); }

	// Écriture — le couple recvfrom/getsockname/accept. LenPtr() repose la
	// capacité AVANT de rendre le pointeur (cf. commentaire de classe).
	sockaddr*  Data()   { return (sockaddr*)&storage; }
	socklen_t* LenPtr() { len = sizeof(storage); return &len; }

	// « 192.0.2.1:5004 » ou « [2001:db8::1]:5004 » — pour les logs et pour les
	// URL : ici les crochets sont LÉGITIMES, puisqu'un port suit (RFC 3986
	// §3.2.2). C'est la seule sortie texte qui les porte avec le port.
	std::string ToString() const;

	// Égalité SÉMANTIQUE, adresse (dé-mappée) ET port. Pour comparer la seule
	// adresse — ce que fait le latching NAT — passer par `Address()`.
	bool operator==(const IPEndpoint& other) const;
	bool operator!=(const IPEndpoint& other) const { return !(*this == other); }

private:
	sockaddr_storage storage = {};
	socklen_t        len     = 0;   // 0 = vide ; sinon sizeof(sockaddr_in{,6})
};

#endif // _IPADDRESS_H_
