#ifndef _STUNCLIENT_H_
#define _STUNCLIENT_H_

#include <string>

#include "config.h"
#include "ipaddress.h"

/**
 * StunClient — découverte de l'adresse publique par un serveur STUN, et
 * vérification que le NAT traversé est bien un **NAT 1:1**.
 *
 * Sert exactement un cas : `--nat auto` (§14.2 de ipv6.md). Le serveur est
 * derrière un NAT, son `--public-ip` porte une adresse RFC 1918 réellement
 * attachée, et il faut découvrir l'adresse publique à ANNONCER dans les SDP.
 *
 * ------------------------------------------------------------------------
 * POURQUOI VÉRIFIER LE 1:1, ET PAS SEULEMENT LIRE L'ADRESSE.
 *
 * Le mediaserver annonce des PORTS, pas seulement une adresse : la ligne `m=`
 * de chaque SDP porte le port RTP local qu'il vient d'allouer. Si le NAT
 * translate aussi les ports, le port annoncé est faux pour tout le monde — le
 * pair émet vers un port que le NAT n'a jamais ouvert, et l'appel est muet dans
 * un sens ou dans les deux. Découvrir l'adresse sans vérifier la conservation
 * des ports produirait donc une configuration qui a l'air juste et ne marche
 * pas : exactement le genre de panne que ce chantier cherche à supprimer.
 *
 * La sonde est faite DEUX FOIS, depuis deux ports locaux différents. Une seule
 * ne prouverait rien : un NAT à traduction de ports peut, par chance, avoir
 * conservé celui-là. Deux ports conservés ET la même adresse publique, c'est la
 * signature d'un mapping 1:1 (« endpoint-independent mapping », RFC 4787 §4.1,
 * avec conservation du port).
 *
 * Ce que cette classe NE prouve PAS, et qu'il ne faut pas lui faire dire :
 *   - que le NAT n'a pas de filtrage (RFC 4787 §5) : le média entrant peut
 *     rester bloqué tant que nous n'avons pas émis vers le pair. C'est le rôle
 *     du rattrapage NAT et de l'amorçage (`ArmNATPriming`), pas le nôtre ;
 *   - que le mapping durera : un NAT peut expirer une association. La découverte
 *     a lieu au démarrage, et l'adresse annoncée est figée ensuite.
 */
class StunClient
{
public:
	//Résultat d'une sonde : ce que le serveur STUN dit voir de nous.
	struct Mapping
	{
		IPAddress address;      //adresse publique vue de l'extérieur
		WORD      port = 0;     //port public associé à notre port local
		WORD      localPort = 0;
	};

	/**
	 * Probe — une interrogation STUN Binding depuis `localBind`.
	 *
	 * La socket est liée à `localBind` (l'adresse RFC 1918 de la machine) sur un
	 * port éphémère, hors de la plage RTP : la découverte ne doit pas consommer
	 * un port que le média utilisera. Retransmissions RFC 5389 (3 tentatives,
	 * 500 ms / 1 s / 2 s) — un datagramme perdu ne doit pas faire échouer un
	 * démarrage.
	 */
	static bool Probe(const IPAddress& localBind, const IPEndpoint& server,
	                  Mapping& out, std::string& error);

	/**
	 * Discover — deux sondes, verdict.
	 *
	 * `publicAddress` reçoit l'adresse publique, `oneToOne` dit si le mapping
	 * conserve les ports et l'adresse (voir plus haut). Rend false — avec
	 * `error` renseigné — si le serveur STUN ne répond pas, répond n'importe
	 * quoi, ou si les deux sondes se contredisent.
	 *
	 * ATTENTION : `oneToOne == false` n'est PAS une erreur de cette fonction.
	 * C'est un fait sur le réseau, que l'appelant doit traiter comme un refus de
	 * configuration — d'où deux sorties distinctes.
	 */
	static bool Discover(const IPAddress& localBind, const IPEndpoint& server,
	                     IPAddress& publicAddress, bool& oneToOne, std::string& error);

	/**
	 * ParseServer — « hôte », « hôte:port », « adresse:port ».
	 *
	 * Port par défaut 3478 (RFC 5389). Un littéral IPv6 doit être entre crochets
	 * ici, et SEULEMENT ici : c'est une syntaxe d'URL, imposée par le « : » du
	 * port (RFC 3986 §3.2.2). Le nom est résolu en v4 : `--nat auto` ne sert que
	 * le NAT IPv4, il n'y a pas de NAT IPv6 dans ce produit.
	 */
	static bool ParseServer(const char* text, IPEndpoint& out, std::string& error);

	//Serveur interrogé à défaut de --stun-server.
	static const char* DefaultServer();
};

#endif // _STUNCLIENT_H_
