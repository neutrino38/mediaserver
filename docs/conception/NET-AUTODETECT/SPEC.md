# Adresses du serveur déduites de ses interfaces

> Statut : **conception validée**. Rien n'est codé.
>
> Vocabulaire : celui de [`NETWORK-CONFIGURATION.md`](../../NETWORK-CONFIGURATION.md).
> Un **profil** est une adresse que le serveur lie et annonce : `publicv4`,
> `publicv6`, `internalv4`, `internalv6`. Un **côté** est public ou
> interne.

## 1. Objectif

Aujourd'hui, l'exploitant écrit les adresses du serveur dans sa ligne de
commande (`--public-ip`, `--internal-ip`). Il les recopie donc depuis la
machine, et une adresse qui change (nouvelle machine, nouveau bail, clone
d'une VM) casse le démarrage.

Il pourra écrire plutôt :

```sh
--public-if ens18 --public-family both \
--internal-if ens19 --internal-family v4
```

Ce qui veut dire : « le côté public est l'interface `ens18`, en IPv4 et en
IPv6 ; le côté interne est `ens19`, en IPv4 seulement ». Le serveur lit les
adresses de ces interfaces au démarrage, et remplit la table des profils avec
elles.

L'adresse écrite à la main reste possible. Elle sert en particulier quand une
interface porte plusieurs adresses de la même famille.

## 2. Ce qui existe

- `--public-ip` et `--internal-ip` donnent une adresse, une fois par famille.
  `--internal-ip` doit être portée par la machine ; `--public-ip` non, parce
  qu'elle peut être l'adresse vue de l'extérieur d'un NAT.
- Sans aucune des deux, le serveur prend l'adresse que résout le nom de la
  machine. À défaut, la première adresse annonçable d'une interface
  (`AddressProfiles::FirstAnnounceableLocal`). Le choix ne connaît que la
  famille, pas l'interface.
- `--nat` donne l'adresse publique v4 vue de l'extérieur ; `--nat auto` la
  découvre par STUN. Rien ne change pour lui.
- La table est figée au démarrage (`AddressProfiles::Freeze`). Rien ne change
  non plus ici.

## 3. Les options

| Option | Valeurs | Rôle |
|---|---|---|
| `--public-if <nom>` | nom d'interface (`ens18`) | le côté public est cette interface |
| `--public-family <f>` | `v4`, `v6`, `both` | familles à prendre sur `--public-if` ; `v4` par défaut |
| `--internal-if <nom>` | nom d'interface | le côté interne est cette interface |
| `--internal-family <f>` | `v4`, `v6`, `both` | familles à prendre sur `--internal-if` ; `v4` par défaut |

La famille par défaut est `v4` : sans `--public-family`, le serveur se
comporte comme aujourd'hui, où l'auto-détection préfère l'IPv4.

`--public-ip` et `--internal-ip` restent. Avec l'interface du même côté, ils
désignent une adresse de cette interface (§4.3). Chaque côté se configure
seul : `--public-if eth0` avec `--internal-ip 10.0.0.5` est valide.

Exemples :

```sh
# Une seule carte, IPv4 et IPv6
--public-if eth0 --public-family both

# Derrière un NAT 1:1 : l'interface donne l'adresse privée, STUN la publique
--public-if eth0 --nat auto

# Une carte publique, une carte de service
--public-if ens18 --public-family both --internal-if ens19
```

`--public-family both` oblige les **deux** familles. Si l'interface n'a pas
d'adresse IPv6 utilisable, le serveur refuse de démarrer. L'exploitant a
demandé l'IPv6 : un serveur qui démarrerait sans elle ne le révélerait qu'au
premier appel IPv6.

L'interface se désigne par son **nom**, jamais par son adresse MAC. Un nom
qui change d'une version de système à l'autre (`eth0` → `ens18`) se fige
dans la configuration réseau (systemd-networkd, udev), pas dans le
mediaserver.

## 4. Choisir l'adresse d'une interface

### 4.1 Quelles adresses comptent

Le serveur lit les adresses de l'interface, puis écarte celles qu'on ne peut
pas annoncer. La base est la règle d'une adresse écrite à la main
(`IPAddress::IsAnnounceable`), plus des règles par côté et par famille :

| Adresse | Côté public | Côté interne | Pourquoi |
|---|---|---|---|
| loopback, link-local (`169.254/16`, `fe80::/10`) | écartée | écartée | joignable de nulle part ailleurs |
| v4 privée (`10/8`, `172.16/12`, `192.168/16`) | gardée | gardée | le cas nominal du NAT, côté public |
| v4 publique | gardée | **refusée** | même règle que `--internal-ip` |
| v6 ULA (`fc00::/7`) | écartée | gardée | une ULA ne se route pas sur Internet |
| v6 **temporaire** (RFC 8981, « privacy ») | écartée | écartée | elle change toutes les quelques heures, alors que la table est figée |
| v6 **dépréciée**, ou **tentative** (DAD en cours) | écartée | écartée | le noyau ne l'emploie déjà plus, ou pas encore |

« Refusée » veut dire que le serveur ne démarre pas : une adresse v4
publique sur l'interface interne est presque à coup sûr une erreur de
câblage ou d'interface. « Écartée » veut dire qu'elle ne compte pas, et que
les autres adresses de l'interface sont examinées.

**Piège** : `getifaddrs`, que le serveur emploie pour ses contrôles, ne dit
pas si une adresse IPv6 est temporaire, dépréciée ou tentative. Il faut lire
les drapeaux de l'adresse par netlink (`RTM_GETADDR`, drapeaux
`IFA_F_TEMPORARY`, `IFA_F_DEPRECATED`, `IFA_F_TENTATIVE`). Sans eux, le
serveur annoncerait une adresse temporaire dans ses SDP, et les appels
casseraient à sa rotation.

### 4.2 Une seule adresse : elle est prise

C'est le cas courant. Le journal de démarrage dit d'où elle vient :

```
-Profil publicv6 : 2001:db8::5 (interface ens18, auto)
```

### 4.3 Plusieurs adresses de la même famille : l'exploitant désigne

Exemple : `ens18` porte `203.0.113.5` et `203.0.113.6`.

Le serveur **refuse de démarrer**, liste les candidates et dit quoi écrire :

```
-MCU cannot start: --public-if ens18 porte 2 adresses IPv4 annonçables :
   203.0.113.5, 203.0.113.6
   Désigner celle à employer : --public-ip <adresse>
```

`--public-ip 203.0.113.6` choisit alors parmi les adresses de `ens18`. Elle
doit être portée par cette interface : sinon c'est une faute de frappe, et
le serveur refuse de démarrer.

Pourquoi ne pas prendre l'adresse « primaire » à sa place : l'ordre des
adresses suit l'ordre de leur configuration. Une deuxième adresse ajoutée
pour un autre service pourrait prendre la place sans bruit, et une mauvaise
adresse dans le SDP ne se voit qu'au premier appel, qui reste muet. Le refus
ne coûte qu'une fois, au premier démarrage.

La règle s'applique par famille. Avec `--public-family both`, une adresse v6
unique est prise, même si l'exploitant a dû désigner l'adresse v4.

### 4.4 Interface absente, ou sans adresse

Le serveur refuse de démarrer, et nomme ce qui manque :

```
-MCU cannot start: --public-if ens18 : aucune adresse IPv6 annonçable
   (adresses vues : fe80::1 link-local, 2001:db8::9 temporaire)
```

Une interface qui n'a pas encore son adresse au démarrage (DHCP, SLAAC lents)
est un problème d'ordre de démarrage, pas d'adressage. L'unité systemd
attend déjà `network-online.target` (`mediaserver.service`). Ce n'est
suffisant que si le service qui configure le réseau le signale vraiment
(`systemd-networkd-wait-online`, `NetworkManager-wait-online`). Le serveur
n'attend pas lui-même : une attente bornée retarderait seulement le même
refus.

### 4.5 Ce qui ne change pas

- L'adresse **annoncée** reste celle de bind, sauf `publicv4` derrière
  `--nat`.
- La table reste **figée** au démarrage. Une adresse qui change ensuite
  impose un redémarrage. Suivre les changements par netlink (`RTM_NEWADDR`)
  n'est pas prévu : les appels en cours garderaient l'ancienne adresse dans
  leur SDP, et la table figée est ce qui garantit qu'un appel est annoncé de
  la même façon du début à la fin.
- Un `--internal-if` restreint l'API de contrôle à l'adresse interne retenue,
  comme `--internal-ip`.

## 5. Ce que le serveur publie

- **Le journal** de démarrage dit, pour chaque profil, l'adresse, l'interface
  et l'origine : `auto` (seule adresse annonçable de l'interface) ou `manual`
  (écrite à la main).
- **`/status/general`** et `GetNetworkProfiles` ajoutent à chaque profil
  `interface` (vide pour une adresse écrite à la main sans interface) et
  `source` (`auto` ou `manual`). L'exploitant vérifie ainsi ce que le serveur
  a retenu sans lire le journal.

## 6. Construction et plan

- **Une fonction pure** choisit les adresses. Elle reçoit la liste des
  adresses de la machine (interface, adresse, drapeaux) et les options. Elle
  rend les profils, ou l'erreur à afficher. Les tests lui donnent des listes
  écrites à la main : plusieurs adresses, temporaires, dépréciées, ULA côté
  public, v4 publique côté interne, interface absente. Aucun test n'a besoin
  d'une vraie interface.
- **La lecture netlink** est à part, et petite : elle remplit cette liste.
- `main()` appelle les deux, puis passe le résultat à `AddressProfiles`, qui
  garde ses contrôles (`Add*`, `SetNat`, `Freeze`).

| Lot | Contenu | Condition de fin |
|---|---|---|
| 1 | La fonction pure et ses tests | tests verts, sans réseau |
| 2 | Lecture netlink, options, journal, `/status/general`, `GetNetworkProfiles` | démarrage vérifié sur une VM à deux cartes, IPv4 et IPv6 |
| 3 | `NETWORK-CONFIGURATION.md` : un cas « par interface », le tableau des options | relu par un exploitant |
