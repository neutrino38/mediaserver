#include "addressprofiles.h"

#include <ifaddrs.h>
#include <string.h>

#include <mutex>

/**
 * addressprofiles.cpp — voir addressprofiles.h pour le modèle et le cycle de vie.
 *
 * L'état est un tableau de quatre entrées, posé au démarrage puis figé. Il est
 * statique pour la même raison que `RTPSession::announcedIp` l'était : c'est une
 * propriété du PROCESSUS, pas d'une session, et la dupliquer par objet la ferait
 * diverger. Le verrou n'est pas là pour la contention — la table est écrite une
 * fois — mais pour la visibilité mémoire entre le thread de démarrage et les
 * threads média qui la liront ensuite.
 */

namespace
{
	struct Profile
	{
		IPAddress bind;        //réellement attachée : ce que la socket lie
		IPAddress announced;   //ce que voit le pair ; == bind sauf NAT
		bool      set = false;
	};

	Profile                    profiles[AddressProfiles::Count];
	IPAddress                  natAddress;         //--nat, en attente de Freeze
	AddressProfiles::Id        defaultId    = AddressProfiles::PublicV4;
	bool                       defaultSet   = false;
	bool                       frozen       = false;
	std::mutex                 mutex;

	const char* const kNames[AddressProfiles::Count] =
	{
		"publicv4", "publicv6", "internalv4", "internalv6"
	};

	//Le profil qui correspond à une adresse, selon sa famille et le côté demandé.
	AddressProfiles::Id IdFor(const IPAddress& addr, bool internal)
	{
		if (internal)
			return addr.IsV6() ? AddressProfiles::InternalV6 : AddressProfiles::InternalV4;

		return addr.IsV6() ? AddressProfiles::PublicV6 : AddressProfiles::PublicV4;
	}

	//Contrôles communs aux deux côtés. Le verrou est déjà tenu par l'appelant.
	//`requireAttached` : voir AddPublic/AddInternal — la publique y échappe pour
	//ne pas casser les déploiements derrière NAT d'avant ce modèle.
	bool CheckBindAddress(const IPAddress& addr, bool internal, bool requireAttached,
	                      std::string& error)
	{
		if (frozen)
		{
			error = "la table d'adressage est deja figee";
			return false;
		}

		if (!addr.IsSet())
		{
			error = "adresse invalide";
			return false;
		}

		//Loopback, multicast, non spécifiée, link-local : le pair ne pourrait
		//rien en faire, et c'est justement ce qu'on va lui annoncer.
		if (!addr.IsAnnounceable())
		{
			error = "adresse " + addr.ToString() +
			        " non annoncable (loopback, multicast, link-local ou non specifiee)";
			return false;
		}

		//La v4 interne DOIT être privée ; la v6 interne n'a aucune contrainte
		//de plage (NETWORK-CONFIGURATION.md). Ce contrôle passe AVANT celui de
		//l'attachement : à un exploitant qui déclare une adresse publique comme
		//interne, « hors des plages privées » dit ce qu'il a fait de travers,
		//là où « attachée à aucune interface » l'enverrait chercher ailleurs.
		if (internal && addr.IsV4() && !addr.IsPrivateV4())
		{
			error = "adresse interne " + addr.ToString() +
			        " hors des plages privees v4 (10/8, 172.16/12, 192.168/16, 100.64/10, 169.254/16)";
			return false;
		}

		if (requireAttached && !AddressProfiles::IsLocallyAttached(addr))
		{
			error = "adresse " + addr.ToString() +
			        " attachee a aucune interface locale : elle ne pourra jamais etre liee";
			return false;
		}

		const AddressProfiles::Id id = IdFor(addr, internal);
		if (profiles[id].set)
		{
			error = std::string("profil ") + kNames[id] + " deja renseigne (" +
			        profiles[id].bind.ToString() + ") : une seule adresse par profil";
			return false;
		}

		return true;
	}
}

const char* AddressProfiles::NameOf(Id id)
{
	if (id < 0 || id >= Count)
		return "";

	return kNames[id];
}

bool AddressProfiles::ParseId(const char* name, Id& out)
{
	if (!name || !*name)
		return false;

	for (int i = 0; i < Count; ++i)
	{
		if (strcmp(name, kNames[i]) == 0)
		{
			out = (Id)i;
			return true;
		}
	}

	//Nom inconnu : refus. Servir un défaut silencieux ferait passer une faute
	//de frappe du contrôleur pour un appel nominal, sur la mauvaise interface.
	return false;
}

bool AddressProfiles::AddPublic(const IPAddress& bind, std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex);

	//PAS d'exigence d'attachement ici, et c'est un choix de COMPATIBILITÉ :
	//`--public-ip` a toujours désigné « l'adresse que les pairs atteignent, qui
	//n'est pas celle liée localement » — c'est-à-dire, derrière NAT, une adresse
	//qui n'est attachée à AUCUNE interface de cette machine. Exiger l'attachement
	//casserait tous ces déploiements du jour au lendemain.
	if (!CheckBindAddress(bind, false, /*requireAttached=*/false, error))
		return false;

	const Id id = IdFor(bind, false);

	//Adresse réellement attachée -> elle sert aussi de point de bind, donc
	//d'interface d'émission. Sinon on reste sur l'écoute historique « toutes
	//interfaces » (bind vide) et l'adresse ne sert QU'À l'annonce : c'est le
	//mode hérité du NAT sans --nat, conservé tel quel.
	profiles[id].bind      = AddressProfiles::IsLocallyAttached(bind) ? bind : IPAddress();
	profiles[id].announced = bind;   //NAT éventuel appliqué par Freeze
	profiles[id].set       = true;

	return true;
}

bool AddressProfiles::AddInternal(const IPAddress& bind, std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex);

	//Ici l'attachement est EXIGÉ : une adresse interne sert précisément à
	//choisir l'interface de service. Option neuve, donc aucune compatibilité à
	//ménager — autant attraper la faute de frappe au démarrage.
	if (!CheckBindAddress(bind, true, /*requireAttached=*/true, error))
		return false;

	const Id id = IdFor(bind, true);

	//Un profil interne n'est jamais natté : l'adresse annoncée EST celle liée.
	profiles[id].bind      = bind;
	profiles[id].announced = bind;
	profiles[id].set       = true;

	return true;
}

bool AddressProfiles::SetNat(const IPAddress& announced, std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (frozen)
	{
		error = "la table d'adressage est deja figee";
		return false;
	}

	if (!announced.IsSet())
	{
		error = "adresse NAT invalide";
		return false;
	}

	//Pas de NAT en IPv6, par choix : le refus est explicite, pas un
	//silence. Un déploiement v6 correct délègue un préfixe et filtre.
	if (!announced.IsV4())
	{
		error = "--nat n'accepte qu'une adresse IPv4 : le NAT IPv6 n'est pas supporte";
		return false;
	}

	if (!announced.IsAnnounceable())
	{
		error = "adresse NAT " + announced.ToString() + " non annoncable";
		return false;
	}

	if (natAddress.IsSet())
	{
		error = "--nat deja renseigne (" + natAddress.ToString() + ")";
		return false;
	}

	//PAS de contrôle IsLocallyAttached ici : par construction cette adresse
	//vit sur le routeur, pas sur nous — c'est toute la raison de l'option.
	natAddress = announced;
	return true;
}

bool AddressProfiles::SetDefault(Id id, std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (frozen)
	{
		error = "la table d'adressage est deja figee";
		return false;
	}

	if (id < 0 || id >= Count)
	{
		error = "profil par defaut inconnu";
		return false;
	}

	//Disponibilité vérifiée par Freeze : l'ordre des options ne doit pas compter.
	defaultId  = id;
	defaultSet = true;
	return true;
}

bool AddressProfiles::Freeze(std::string& error)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (frozen)
	{
		error = "la table d'adressage est deja figee";
		return false;
	}

	if (natAddress.IsSet())
	{
		if (!profiles[PublicV4].set)
		{
			error = "--nat donne sans adresse publique v4 : preciser --public-ip <adresse locale>";
			return false;
		}

		//C'est ICI que les deux adresses d'un profil divergent, et c'est le
		//seul endroit du produit où cela arrive.
		profiles[PublicV4].announced = natAddress;
	}

	int available = 0;
	for (int i = 0; i < Count; ++i)
		if (profiles[i].set)
			++available;

	if (!available)
	{
		error = "aucun profil d'adressage disponible";
		return false;
	}

	if (!profiles[defaultId].set)
	{
		error = std::string("profil par defaut ") + kNames[defaultId] +
		        " indisponible" +
		        (defaultSet ? "" : " (defaut historique : preciser --default-profile)");
		return false;
	}

	frozen = true;
	return true;
}

bool AddressProfiles::IsAvailable(Id id)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (id < 0 || id >= Count)
		return false;

	return profiles[id].set;
}

IPAddress AddressProfiles::BindAddress(Id id)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (id < 0 || id >= Count || !profiles[id].set)
		return IPAddress();

	return profiles[id].bind;
}

IPAddress AddressProfiles::AnnouncedAddress(Id id)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (id < 0 || id >= Count || !profiles[id].set)
		return IPAddress();

	return profiles[id].announced;
}

AddressProfiles::Id AddressProfiles::Default()
{
	std::lock_guard<std::mutex> lock(mutex);

	return defaultId;
}

int AddressProfiles::AvailableCount()
{
	std::lock_guard<std::mutex> lock(mutex);

	int available = 0;
	for (int i = 0; i < Count; ++i)
		if (profiles[i].set)
			++available;

	return available;
}

std::string AddressProfiles::Describe()
{
	std::lock_guard<std::mutex> lock(mutex);

	std::string out;

	for (int i = 0; i < Count; ++i)
	{
		out += kNames[i];
		out += " : ";

		if (!profiles[i].set)
		{
			out += "indisponible";
		}
		else
		{
			if (profiles[i].bind.IsSet())
				out += "bind " + profiles[i].bind.ToString();
			else
				out += "bind toutes interfaces";

			if (!(profiles[i].announced == profiles[i].bind))
				out += ", annoncee " + profiles[i].announced.ToString() +
				       (profiles[i].bind.IsSet() ? " (NAT)" : "");

			//Rappel utile à l'exploitant : un interne v6 en unicast global est
			//parfaitement légitime, mais sa protection tient ENTIÈREMENT à son
			//filtrage — aucune plage ne le protège.
			if (i == InternalV6 && !profiles[i].bind.IsUniqueLocalV6())
				out += ", unicast global (protection par filtrage uniquement)";
		}

		if (i == defaultId)
			out += " [defaut]";

		out += "\n";
	}

	return out;
}

bool AddressProfiles::IsLocallyAttached(const IPAddress& addr)
{
	if (!addr.IsSet())
		return false;

	ifaddrs* list = NULL;
	if (getifaddrs(&list) != 0)
		return false;

	bool found = false;

	for (ifaddrs* p = list; p && !found; p = p->ifa_next)
	{
		if (!p->ifa_addr)
			continue;

		if (p->ifa_addr->sa_family != AF_INET && p->ifa_addr->sa_family != AF_INET6)
			continue;

		//L'égalité d'IPAddress dé-mappe et traite la zone en joker : une
		//link-local configurée avec zone est reconnue sans, et réciproquement.
		found = (IPAddress::FromSockaddr(p->ifa_addr) == addr);
	}

	freeifaddrs(list);
	return found;
}

void AddressProfiles::Reset()
{
	std::lock_guard<std::mutex> lock(mutex);

	for (int i = 0; i < Count; ++i)
		profiles[i] = Profile();

	natAddress = IPAddress();
	defaultId  = PublicV4;
	defaultSet = false;
	frozen     = false;
}
