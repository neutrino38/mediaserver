#ifndef _ADDRESSPROFILES_H_
#define _ADDRESSPROFILES_H_

#include <string>

#include "config.h"
#include "ipaddress.h"

/**
 * AddressProfiles — les quatre adresses que le serveur peut employer, et ce
 * qu'il en annonce.
 *
 * Modèle du §14 de `ipv6.md`. Un **profil** est le croisement de deux axes :
 *
 *                      IPv4              IPv6
 *   publique     publicv4 (nattable)   publicv6 (jamais nattée)
 *   interne      internalv4            internalv6
 *
 * Chaque profil porte DEUX adresses, et c'est là tout l'intérêt :
 *
 *   - l'adresse de BIND, réellement attachée à une interface de la machine :
 *     c'est elle que la socket lie, donc elle qui décide de l'interface
 *     empruntée ;
 *   - l'adresse ANNONCÉE, celle que le pair verra dans le SDP. Égale à
 *     l'adresse de bind, SAUF pour `publicv4` derrière NAT (`--nat`).
 *
 * Confondre les deux — ce que faisait l'unique `RTPSession::announcedIp` — rend
 * un déploiement derrière NAT indescriptible : on ne peut pas annoncer une
 * adresse qu'on ne peut pas binder.
 *
 * ------------------------------------------------------------------------
 * CYCLE DE VIE : configurée UNE FOIS au démarrage (`Add*`, `SetNat`,
 * `SetDefault`), puis **figée** (`Freeze`) et lue partout. Après `Freeze`, toute
 * tentative de modification est refusée : une table d'adressage qui change en
 * cours de route donnerait des appels annoncés différemment selon l'heure.
 *
 * L'ORDRE DES OPTIONS N'EST PAS SIGNIFICATIF : la famille se déduit de la
 * valeur, et les contrôles croisés (un `--nat` sans `publicv4`, un profil par
 * défaut indisponible) sont faits par `Freeze`, pas au fil de la lecture.
 *
 * Tous les refus rendent `false` ET remplissent `error` d'une phrase destinée à
 * l'exploitant : c'est le seul retour qu'il aura, le serveur ne démarrera pas.
 */
class AddressProfiles
{
public:
	enum Id
	{
		PublicV4 = 0,
		PublicV6,
		InternalV4,
		InternalV6,
		Count
	};

	// -----------------------------------------------------------------
	// Noms exposés — ceux de l'API de contrôle (§14.3) et de la CLI
	// -----------------------------------------------------------------

	// "publicv4" · "publicv6" · "internalv4" · "internalv6"
	static const char* NameOf(Id id);

	// Rend false si le nom est inconnu : le contrôleur qui invente un profil
	// doit se faire refuser, pas se voir servir un défaut silencieux.
	static bool ParseId(const char* name, Id& out);

	// -----------------------------------------------------------------
	// Configuration — démarrage uniquement
	// -----------------------------------------------------------------

	// `--public-ip <addr>` : adresse publique, ou adresse locale derrière NAT
	// (l'adresse vue de l'extérieur arrive alors par `SetNat`). Aucune
	// contrainte de plage : une RFC 1918 est légitime ici, c'est même le cas
	// nominal du NAT.
	//
	// L'attachement local n'est PAS exigé, contrairement à `AddInternal`, et
	// c'est une décision de COMPATIBILITÉ : `--public-ip` a toujours désigné
	// « l'adresse que les pairs atteignent, qui n'est pas celle liée
	// localement » — donc, derrière NAT, une adresse attachée à aucune de nos
	// interfaces. L'exiger casserait ces déploiements du jour au lendemain.
	//   - adresse attachée   -> elle sert aussi d'adresse de bind (interface
	//                           d'émission choisie) ;
	//   - adresse non attachée -> bind vide, c'est-à-dire l'écoute historique
	//                           « toutes interfaces », et l'adresse ne sert
	//                           qu'à l'annonce.
	static bool AddPublic(const IPAddress& bind, std::string& error);

	// `--internal-ip <addr>` : côté réseau de service.
	//   v4 -> DOIT être privée (`IsPrivateV4`) : trente ans de NAT font qu'une
	//         adresse publique déclarée interne est presque à coup sûr une
	//         faute de frappe, et le contrôle l'attrape ;
	//   v6 -> AUCUNE contrainte de plage. Un réseau interne IPv6 est le plus
	//         souvent numéroté dans une plage globale déléguée, son caractère
	//         interne tenant au routage et au filtrage (§14.2). Exiger l'ULA
	//         réimporterait le fait dans la décision.
	//
	// L'attachement local est ici EXIGÉ, dans les deux familles : une adresse
	// interne sert précisément à choisir l'interface de service, elle n'a aucun
	// sens si elle n'est portée par aucune. Option neuve, donc aucune
	// compatibilité à ménager — autant attraper la faute de frappe au démarrage.
	static bool AddInternal(const IPAddress& bind, std::string& error);

	// `--nat <addr>` : adresse publique v4 vue de l'extérieur. Ne s'applique
	// qu'à `publicv4` ; la cohérence (existe-t-il un publicv4 ?) est vérifiée
	// par `Freeze`, pour que l'ordre des options reste sans importance.
	static bool SetNat(const IPAddress& announced, std::string& error);

	// `--default-profile <nom>` : profil employé par un appel qui n'en demande
	// aucun. Vaut `publicv4` à défaut — le comportement historique.
	static bool SetDefault(Id id, std::string& error);

	// Contrôles croisés et gel. À appeler une fois la ligne de commande lue
	// entièrement. Rend false (et refuse de figer) si :
	//   - aucun profil n'est disponible ;
	//   - `--nat` a été donné sans adresse publique v4 ;
	//   - le profil par défaut n'est pas disponible — une valeur par défaut qui
	//     échoue à chaque appel est le pire des deux mondes.
	static bool Freeze(std::string& error);

	// -----------------------------------------------------------------
	// Lecture
	// -----------------------------------------------------------------

	static bool      IsAvailable(Id id);
	static IPAddress BindAddress(Id id);        // vide si indisponible
	static IPAddress AnnouncedAddress(Id id);   // vide si indisponible
	static Id        Default();

	// Combien de profils sont disponibles.
	static int AvailableCount();

	// Une ligne par profil, disponible ou non : c'est ce que le serveur
	// journalise au démarrage, et la matière de l'API d'introspection (§14.4).
	// Un contrôleur qui ne peut pas DEMANDER ce que le serveur sait de lui-même
	// finit par le déclarer de son côté, et cette copie dérive.
	static std::string Describe();

	// -----------------------------------------------------------------

	// L'adresse est-elle attachée à une interface de CETTE machine ?
	// (`getifaddrs`). Contrôle de démarrage : une adresse de bind qui n'est
	// attachée nulle part ne pourra jamais être liée, autant le dire tout de
	// suite. L'adresse de `--nat` y échappe par construction : elle vit sur le
	// routeur, pas sur nous.
	static bool IsLocallyAttached(const IPAddress& addr);

	// Remet la table à zéro. RÉSERVÉ AUX TESTS : en production la table est
	// posée une fois et figée.
	static void Reset();
};

#endif // _ADDRESSPROFILES_H_
