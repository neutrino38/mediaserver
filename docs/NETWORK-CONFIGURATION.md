# Configuration réseau du mediaserver

Ce document s'adresse à **l'exploitant** : celui qui installe le mediaserver sur
une machine, lui donne ses adresses, ouvre les ports et doit comprendre pourquoi
un appel « se connecte mais reste muet ». Il ne suppose aucune connaissance du
code.

Il procède **par cas d'usage**. Trouvez celui qui décrit votre machine, appliquez
la configuration, vérifiez avec le §7. Les trois cas couvrent la quasi-totalité
des déploiements :

| | Votre situation | Aller au |
|---|---|---|
| **Cas 1** | Le serveur porte lui-même une adresse publique, joignable telle quelle | [§4](#4-cas-1--une-adresse-publique-portée-par-le-serveur) |
| **Cas 2** | Le serveur porte une adresse privée, un routeur/cloud la translate 1:1 vers une adresse publique | [§5](#5-cas-2--une-adresse-publique-nattée-11) |
| **Cas 3** | Le serveur a deux pieds : un côté extérieur, un côté réseau interne de service | [§6](#6-cas-3--deux-adresses-une-publique-une-interne) |

---

## 1. La seule notion à comprendre avant de commencer

Un serveur média manipule **deux adresses qu'il ne faut pas confondre** :

- l'**adresse liée** — celle qui est réellement portée par une carte réseau de la
  machine. C'est par elle que les paquets sortent, c'est elle qui décide de
  l'interface empruntée ;
- l'**adresse annoncée** — celle que le serveur écrit dans le SDP de chaque appel,
  c'est-à-dire l'adresse que le correspondant va **utiliser pour lui envoyer**
  l'audio et la vidéo.

Sur une machine directement exposée, les deux sont identiques et la question ne
se pose pas. Derrière un NAT, elles **diffèrent** : la machine porte
`192.168.1.10`, le monde la voit en `198.51.100.7`. Annoncer l'adresse liée
donnerait à chaque correspondant une adresse qu'il ne peut pas joindre — l'appel
s'établit (la signalisation, elle, passe par le contrôleur) mais **aucun média
n'arrive**. C'est de très loin la panne de configuration la plus fréquente.

Le mediaserver décrit donc son adressage sous forme de **profils**. Un profil,
c'est un couple *(adresse liée, adresse annoncée)* pour un côté du réseau et une
famille d'adresses :

| Profil | Côté | Famille |
|---|---|---|
| `publicv4` | extérieur | IPv4, éventuellement nattée |
| `publicv6` | extérieur | IPv6, jamais nattée |
| `internalv4` | réseau interne de service | IPv4 privée |
| `internalv6` | réseau interne de service | IPv6 |

La plupart des déploiements n'en utilisent **qu'un seul** (`publicv4`) et n'ont
jamais à prononcer le mot « profil » : c'est le cas 1 et le cas 2. Les profils
deviennent visibles au cas 3, où le contrôleur choisit, appel par appel, par quel
côté le média doit passer.

---

## 2. Où s'écrit la configuration

Toutes les options réseau sont des **options de ligne de commande**, et sous
systemd elles vivent dans la variable `OPTIONS` du fichier
`/etc/sysconfig/mediaserver` :

```sh
# /etc/sysconfig/mediaserver
OPTIONS="--public-ip 198.51.100.7"
```

Puis :

```sh
systemctl restart mediaserver
journalctl -u mediaserver -n 40      # ou : tail -n 40 /var/log/mcu.log
```

> ⚠️ Ne mettez jamais `-f` dans `OPTIONS` : sous systemd le processus doit rester
> en avant-plan.

**Toute erreur de configuration réseau empêche le démarrage**, avec un message
qui dit laquelle. C'est délibéré : un serveur qui démarre en annonçant une
mauvaise adresse échoue appel après appel pendant des mois sans que rien ne le
signale, alors qu'un serveur qui refuse de démarrer se voit tout de suite.

---

## 3. Les ports à ouvrir

| Port (défaut) | Protocole | Usage | Qui doit l'atteindre |
|---|---|---|---|
| `8080` | TCP | API de contrôle XML-RPC + flux d'événements | le **contrôleur** uniquement |
| `1935` | TCP | RTMP | les clients RTMP, si utilisés |
| `9090` | TCP | WebSocket (texte temps réel, signalisation Web, BFCP) | les clients Web, si utilisés |
| `49152`–`65535` | **UDP** | RTP / RTCP : tout l'audio, la vidéo et le texte | **tous les correspondants** |

La plage RTP se règle avec `--min-rtp-port` / `--max-rtp-port`. Elle peut être
réduite (une conférence consomme quelques ports par participant et par média —
compter large, et au moins quelques centaines de ports), mais elle doit être
ouverte **en UDP et en entrée**, sans quoi le média n'arrive jamais.

> Le port `8080` porte l'API qui pilote **tout** le serveur média. Il n'a rien à
> faire sur une interface publique : filtrez-le, ou déclarez un réseau interne
> (cas 3), ce qui l'y restreint automatiquement.

### Sur quelles interfaces le serveur écoute

| Plan | Écoute par défaut | Restriction possible |
|---|---|---|
| Média RTP/RTCP | toutes interfaces | l'adresse du profil demandé par l'appel (cas 3) |
| RTMP, WebSocket | toutes interfaces | aucune |
| API de contrôle XML-RPC | toutes interfaces | **l'adresse interne, dès que `--internal-ip` est donnée** (cas 3) |

Sauf restriction, toutes les écoutes acceptent **indifféremment IPv4 et IPv6** :
une seule socket entend les deux familles. Il n'y a donc rien à configurer pour
qu'un client IPv4 et un client IPv6 joignent le même serveur.

---

## 4. Cas 1 — Une adresse publique, portée par le serveur

**Reconnaître ce cas** : `ip addr` sur la machine affiche l'adresse par laquelle
les correspondants la joignent. Serveur en datacenter, VM avec IP publique
directe, ou serveur sur un réseau d'entreprise où tout le monde est dans le même
plan d'adressage.

```
  correspondants  ──────────►  198.51.100.7  [ mediaserver ]
                               (portée par la machine)
```

### Configuration

```sh
OPTIONS="--public-ip 198.51.100.7"
```

C'est tout. L'adresse est liée **et** annoncée ; le média part par l'interface
qui la porte.

**Vous pouvez aussi ne rien mettre du tout.** Sans `--public-ip` ni
`--internal-ip`, le serveur résout son propre nom d'hôte (`/etc/hosts` puis DNS,
enregistrements A et AAAA, IPv4 préférée) et prend la première adresse
annonçable. Sur une machine bien nommée, c'est le bon résultat.

Préférez néanmoins l'option explicite si :

- la machine a **plusieurs cartes réseau** — l'auto-détection en choisit une, pas
  forcément la vôtre ;
- `/etc/hosts` fait pointer le nom d'hôte sur `127.0.0.1` ou sur une adresse
  périmée — un grand classique, qui donne un serveur qui démarre et n'achemine
  rien ;
- vous voulez que la configuration soit lisible dans un fichier plutôt que
  déduite du DNS au démarrage.

### En IPv6

Rien de particulier : la famille est déduite de la valeur.

```sh
OPTIONS="--public-ip 2001:db8:1::7"
```

Sur un hôte **uniquement IPv6**, ajoutez `--default-profile publicv6` : sans
cela, un contrôleur qui ne précise rien demandera le profil `publicv4`, qui
n'existe pas sur cette machine, et tous les appels échoueront.

Les deux familles peuvent coexister — `--public-ip` est répétable, au plus une
fois par famille :

```sh
OPTIONS="--public-ip 198.51.100.7 --public-ip 2001:db8:1::7"
```

Le serveur porte alors deux profils publics, et c'est le contrôleur qui choisit
lequel employer pour chaque appel (voir §6, « le contrôleur choisit »).

### Vérification

Le démarrage journalise la table d'adressage :

```
-Profils d'adressage :
publicv4 : bind 198.51.100.7 [defaut]
publicv6 : indisponible
internalv4 : indisponible
internalv6 : indisponible
```

Une seule ligne, pas de mention `annoncee` : liée et annoncée sont la même
adresse, c'est bien le cas 1.

---

## 5. Cas 2 — Une adresse publique, nattée 1:1

**Reconnaître ce cas** : `ip addr` affiche une adresse **privée** (`10.x`,
`172.16–31.x`, `192.168.x`), et un équipement en amont la fait correspondre à une
adresse publique. C'est le cas d'une VM cloud avec IP flottante / elastic IP,
d'un serveur derrière une box ou un pare-feu d'entreprise en NAT statique.

```
  correspondants ──► 198.51.100.7  [routeur NAT 1:1]  ──► 192.168.1.10  [ mediaserver ]
                     (annoncée)                            (liée)
```

### Configuration

Deux façons, au choix.

**Vous connaissez l'adresse publique** (le cas normal — c'est vous qui l'avez
demandée à votre hébergeur) :

```sh
OPTIONS="--public-ip 192.168.1.10 --nat 198.51.100.7"
```

`--public-ip` porte l'adresse **réellement attachée à la machine**, `--nat`
l'adresse **vue de l'extérieur**. Le serveur lie ses sockets sur la première et
annonce la seconde.

**Vous ne la connaissez pas, ou elle peut changer** :

```sh
OPTIONS="--public-ip 192.168.1.10 --nat auto --stun-server stun.exemple.fr:3478"
```

Le serveur interroge un serveur STUN au démarrage pour découvrir l'adresse vue de
l'extérieur. `--stun-server` vaut `stun.l.google.com:19302` par défaut, ce qui
permet d'essayer l'option sans rien installer — **mais un déploiement de
production doit poser le sien** : dépendre d'un tiers pour démarrer, c'est un
point de panne de plus.

### Pourquoi le serveur exige un NAT **1:1**, et refuse de démarrer sinon

Le mediaserver n'annonce pas qu'une adresse : il annonce aussi, pour chaque
média, un **numéro de port**. Un NAT qui translate les ports en même temps que
l'adresse rend donc faux tout ce qu'il publie — le correspondant émet vers un
port que le routeur n'a jamais ouvert, et l'appel est parfaitement muet, dans les
deux sens, sans un mot dans les journaux.

`--nat auto` vérifie donc que les ports sont **conservés**, en sondant deux fois
depuis deux ports locaux différents (une seule sonde ne prouverait rien : le NAT
peut avoir conservé ce port-là par hasard). Si le verdict n'est pas 1:1, le
serveur **refuse de démarrer** et affiche ce qu'il a observé.

Conséquence concrète pour votre routeur : la règle de NAT doit être **statique et
sans traduction de port**, sur **toute la plage RTP** (`49152–65535` en UDP par
défaut), et pas seulement sur quelques ports. Une redirection port par port ne
convient pas.

### Ce que la sonde STUN ne dit pas

- **rien sur le filtrage** de votre pare-feu : elle prouve que le NAT conserve les
  ports, pas que le trafic entrant est autorisé. La règle de pare-feu reste à
  écrire ;
- **rien sur la durée** : la découverte a lieu **au démarrage**, et l'adresse
  annoncée est ensuite figée. Si votre adresse publique change, il faut
  redémarrer le service.

### Variante historique (toujours acceptée)

```sh
OPTIONS="--public-ip 198.51.100.7"      # adresse publique NON portée par la machine
```

Passer directement l'adresse publique alors qu'elle n'est attachée à aucune carte
du serveur reste valide : le serveur écoute alors sur toutes les interfaces et se
contente d'annoncer cette adresse. C'est le fonctionnement d'avant l'introduction
de `--nat`, conservé tel quel pour ne pas casser les installations existantes.
`--public-ip` + `--nat` est préférable pour un nouveau déploiement : il décrit la
réalité (deux adresses distinctes) au lieu de la maquiller, et il fixe l'interface
d'émission.

### Pas de NAT en IPv6

`--nat` n'est accepté qu'avec une adresse IPv4 ; l'utiliser sur un profil IPv6
est un refus explicite au démarrage. C'est un choix : un déploiement IPv6 correct
délègue un préfixe et **filtre**, il ne translate pas.

### Vérification

```
-NAT auto: adresse publique 198.51.100.7, NAT 1:1 confirme (ports conserves)
-Profils d'adressage :
publicv4 : bind 192.168.1.10, annoncee 198.51.100.7 (NAT) [defaut]
publicv6 : indisponible
internalv4 : indisponible
internalv6 : indisponible
```

La ligne `bind … , annoncee … (NAT)` est exactement la situation que ce cas
décrit : deux adresses différentes, et le serveur sait laquelle sert à quoi.

### Note : le NAT **des correspondants**

Ne pas confondre avec le NAT du serveur : quand c'est le *client* qui est derrière
un NAT symétrique, l'adresse qu'il annonce dans son propre SDP est fausse. Le
mediaserver sait rattraper ce cas (« latching » : il ré-aiguille son envoi vers
l'adresse d'où il reçoit réellement), mais cela s'active **par appel**, via une
propriété RTP posée par le contrôleur — ce n'est pas une option de ligne de
commande. Voir `MCU-API.md` §6.7, `natLatch`.

---

## 6. Cas 3 — Deux adresses, une publique, une interne

**Reconnaître ce cas** : la machine a deux cartes réseau, ou deux adresses, et
sert **deux populations différentes** — des correspondants extérieurs d'un côté,
un réseau de service (autres serveurs, Asterisk, postes internes) de l'autre.
C'est le fonctionnement en SBC.

```
  correspondants ──► 198.51.100.7  [ mediaserver ]  172.16.0.5 ──► réseau interne
                        (publicv4)                  (internalv4)
```

### Configuration

```sh
OPTIONS="--public-ip 198.51.100.7 --internal-ip 172.16.0.5"
```

Les deux options sont répétables, **au plus une fois par famille** ; la famille
est déduite de la valeur, et l'ordre des options n'a aucune importance. Un
déploiement complet peut donc déclarer jusqu'à quatre adresses :

```sh
OPTIONS="--public-ip 198.51.100.7 --public-ip 2001:db8:1::7 \
         --internal-ip 172.16.0.5 --internal-ip fd00:1::5"
```

Contraintes propres à `--internal-ip` :

- l'adresse doit être **réellement portée par la machine** — elle sert justement à
  choisir l'interface de service ; une adresse qui n'est sur aucune carte est une
  faute de frappe, et le démarrage la refuse ;
- en **IPv4**, elle doit être **privée** (`10/8`, `172.16/12`, `192.168/16`,
  `100.64/10`, `169.254/16`). Une adresse publique déclarée comme interne est
  presque toujours une erreur, et le démarrage la refuse aussi ;
- en **IPv6**, aucune contrainte de plage : une adresse globale déléguée par
  l'opérateur est légitime pour un réseau interne, dont le caractère interne tient
  au routage et au filtrage, pas à la plage. Le démarrage vous le rappelle
  simplement dans le journal (`unicast global (protection par filtrage
  uniquement)`).

### ⚠️ Déclarer un réseau interne **restreint l'API de contrôle**

C'est la conséquence à connaître avant d'ajouter `--internal-ip`.

Sans réseau interne déclaré, l'API XML-RPC (port `8080`) écoute sur toutes les
interfaces. **Dès qu'un `--internal-ip` est donné, elle s'y restreint** : elle
pilote entièrement le serveur média, elle n'a rien à faire sur une interface
publique.

Deux effets pratiques :

- la **loopback cesse d'être une porte d'entrée**. Un script local, une supervision
  ou un `curl` qui tapaient `http://127.0.0.1:8080/mcu` doivent désormais viser
  l'adresse interne ;
- si vous déclarez **deux** adresses internes (v4 et v6), l'API ne peut écouter
  que sur une famille : **l'IPv4 l'emporte**, et le démarrage le journalise.

Le journal est explicite :

```
-XmlRpcServer: ecoute restreinte au reseau interne [172.16.0.5:8080] (--internal-ip)
```

Les autres plans (RTMP, WebSocket) continuent d'écouter sur toutes les interfaces.

### Le contrôleur choisit, appel par appel

Avoir deux adresses ne sert à rien si personne ne dit laquelle employer. C'est le
**contrôleur** qui le fait, jambe par jambe : les méthodes de démarrage de flux
(`StartSending` / `StartReceiving` côté MCU, `EndpointStartSending` /
`EndpointStartReceiving` côté JSR-309) acceptent un dernier paramètre facultatif
nommant le profil (`publicv4`, `publicv6`, `internalv4`, `internalv6`).

Trois conséquences pour l'exploitant :

1. **Un appel qui ne demande rien obtient le profil par défaut**, c'est-à-dire
   `publicv4`, ou celui que désigne `--default-profile`. Un contrôleur qui ignore
   la notion de profil se comporte donc exactement comme avant — mais il
   n'exploitera **jamais** votre adresse interne. Si votre contrôleur ne sait pas
   poser ce paramètre, une configuration à deux adresses ne vous apportera rien :
   vérifiez ce point avant de câbler.
2. **Un profil demandé mais indisponible est un échec d'appel explicite**, jamais
   un repli silencieux vers une autre adresse. C'est voulu : un repli enverrait le
   média par la mauvaise interface et personne ne le saurait avant que le
   correspondant ne constate l'absence de son.
3. **Le contrôleur peut — et doit — demander au serveur ce qui existe**, avec la
   méthode `GetNetworkProfiles` (§7). Une liste de profils recopiée à la main dans
   la configuration du contrôleur finit toujours par diverger de la réalité du
   serveur.

`--default-profile` sert précisément à débloquer le cas d'un contrôleur qui ne
sait rien des profils sur une machine où `publicv4` n'existe pas (hôte v6-only,
ou serveur purement interne) :

```sh
OPTIONS="--internal-ip 172.16.0.5 --default-profile internalv4"
```

Le profil désigné doit être disponible, sinon le serveur refuse de démarrer.

### Vérification

```
-Profils d'adressage :
publicv4 : bind 198.51.100.7 [defaut]
publicv6 : indisponible
internalv4 : bind 172.16.0.5
internalv6 : indisponible
```

---

## 7. Vérifier la configuration

### Le journal de démarrage

C'est le premier endroit à regarder, et il suffit dans la majorité des cas. La
table est journalisée à chaque démarrage :

```sh
journalctl -u mediaserver | grep -A5 "Profils d'adressage"
```

Lecture d'une ligne :

| Ce que vous lisez | Ce que ça veut dire |
|---|---|
| `bind 198.51.100.7` | liée et annoncée sur cette adresse (cas 1) |
| `bind 192.168.1.10, annoncee 198.51.100.7 (NAT)` | liée en privé, annoncée en public (cas 2) |
| `bind toutes interfaces, annoncee 198.51.100.7` | adresse publique non portée par la machine (variante historique du cas 2) |
| `indisponible` | ce profil n'est pas configuré ; un appel qui le demande sera refusé |
| `[defaut]` | profil employé par les appels qui n'en demandent aucun |

### Demander au serveur (`GetNetworkProfiles`)

La même information est interrogeable à chaud, sur les deux API de contrôle
(`/mcu` et `/jsr309`), sans paramètre :

```sh
curl -s -X POST http://172.16.0.5:8080/mcu \
     -H 'Content-Type: text/xml' \
     -d '<?xml version="1.0"?><methodCall><methodName>GetNetworkProfiles</methodName><params/></methodCall>'
```

La réponse donne, pour **chacun des quatre profils** : son nom, s'il est
disponible, son adresse liée (vide = toutes interfaces), son adresse annoncée, et
s'il est le profil par défaut. C'est ce que le contrôleur doit interroger plutôt
que de tenir sa propre liste.

### Vérifier que le média circule vraiment

Le test qui compte : passer un appel et regarder si des paquets UDP arrivent.

```sh
ss -lun | head                      # les ports RTP en écoute
tcpdump -ni any udp portrange 49152-65535 -c 20
```

Aucun paquet entrant pendant un appel actif = le correspondant n'atteint pas le
serveur : adresse annoncée fausse, ou plage UDP fermée en entrée.

---

## 8. Diagnostic

| Symptôme | Cause la plus probable | Correction |
|---|---|---|
| L'appel s'établit, **aucun son ni image**, dans les deux sens | Le serveur annonce une adresse que le correspondant ne peut pas joindre | Cas 2 : poser `--public-ip <privée> --nat <publique>` |
| Idem, et le journal montre `bind` = `annoncee` = une adresse privée | NAT non déclaré | idem |
| Le correspondant **reçoit** mais le serveur ne reçoit rien | Plage UDP RTP fermée en entrée sur le pare-feu | Ouvrir `49152–65535/udp` (ou la plage réglée) |
| Média muet alors que l'adresse annoncée est bonne | NAT qui translate aussi les **ports** | Passer le routeur en NAT 1:1 ; `--nat auto` le détecte et refuse de démarrer |
| Le média marchait, puis plus après un changement d'IP publique | L'adresse annoncée est figée au démarrage | Redémarrer le service |
| Le serveur **refuse de démarrer** | Voir §9 | — |
| Le contrôleur ne joint plus l'API sur `127.0.0.1:8080` | `--internal-ip` a restreint l'API au réseau interne | Viser l'adresse interne |
| Tous les appels échouent avec « profil indisponible » | Le contrôleur demande un profil non configuré | Configurer l'adresse correspondante, ou corriger le contrôleur ; `GetNetworkProfiles` dit ce qui existe |
| Les appels n'empruntent jamais l'adresse interne | Le contrôleur ne pose pas le paramètre de profil | Voir §6, « le contrôleur choisit » ; à défaut, `--default-profile` |

---

## 9. Le serveur refuse de démarrer

Tous ces contrôles sont **bloquants**, et chaque message dit quoi corriger.

| Message | Cause | Correction |
|---|---|---|
| `no IP address to announce in the SDP` | Aucune option donnée et le nom d'hôte ne résout sur rien d'annonçable | Poser `--public-ip`, ou corriger `/etc/hosts` / le DNS |
| `adresse … non annoncable (loopback, multicast, link-local…)` | Adresse illisible ou inutilisable par un correspondant | Donner une adresse routable |
| `adresse … attachee a aucune interface locale` | L'adresse interne n'est portée par aucune carte | Vérifier avec `ip addr` |
| `adresse interne … hors des plages privees v4` | Adresse publique déclarée comme interne | Utiliser une adresse RFC 1918 |
| `profil … deja renseigne … : une seule adresse par profil` | `--public-ip` (ou `--internal-ip`) donné deux fois dans la même famille | N'en garder qu'une par famille |
| `--nat auto requires --public-ip <adresse RFC 1918 attachee a l'hote>` | `--nat auto` sans adresse privée locale | Ajouter `--public-ip <adresse privée de la machine>` |
| `--nat auto: … Un NAT qui translate les ports rend faux TOUS les ports RTP annonces` | Le NAT n'est pas 1:1 | Reconfigurer le routeur en NAT statique sans traduction de port |
| `--nat auto: …` (échec réseau) | Serveur STUN injoignable | Vérifier `--stun-server` et la sortie UDP |
| `--nat n'accepte qu'une adresse IPv4 : le NAT IPv6 n'est pas supporte` | `--nat` sur un montage IPv6 | Retirer `--nat` |
| `--nat donne sans adresse publique v4` | `--nat` sans `--public-ip` IPv4 | Ajouter `--public-ip <adresse locale>` |
| `profil par defaut … indisponible` / `profil par defaut inconnu` | `--default-profile` mal orthographié ou non configuré | Corriger le nom, ou configurer l'adresse correspondante |
| `aucun profil d'adressage disponible` | Rien de configurable n'a été trouvé | Poser `--public-ip` ou `--internal-ip` |

---

## 10. Récapitulatif des options

| Option | Défaut | Rôle |
|---|---|---|
| `--public-ip <ip>` | auto-détectée | Adresse du côté extérieur. Liée si la machine la porte, annoncée dans le SDP. IPv4 ou IPv6, répétable une fois par famille. |
| `--nat <ip>` \| `auto` | *(aucune)* | Adresse publique vue de l'extérieur, quand `--public-ip` porte l'adresse locale d'une machine nattée. IPv4 seulement. `auto` la découvre par STUN et vérifie le 1:1. |
| `--stun-server <hôte[:port]>` | `stun.l.google.com:19302` | Serveur interrogé par `--nat auto`. À remplacer par le vôtre en production. |
| `--internal-ip <ip>` | *(aucune)* | Adresse du côté interne. Doit être portée par la machine ; privée en IPv4. **Restreint l'API de contrôle à cette adresse.** |
| `--default-profile <nom>` | `publicv4` | Profil employé par les appels qui n'en demandent aucun. |
| `--min-rtp-port` / `--max-rtp-port` | `49152` / `65535` | Plage UDP des sessions RTP/RTCP. |
| `--http-port <port>` | `8080` | Port de l'API de contrôle XML-RPC. |
| `--rtmp-port <port>` | `1935` | Port RTMP. |
| `--websocket-port <port>` | `9090` | Port WebSocket. |
| `--websocket-host <hôte>` | *(aucun)* | Nom d'hôte annoncé dans les URL des endpoints WebSocket. Utile derrière un proxy inverse ou en `wss://`, où le nom public n'est pas celui de la machine. |

Les options non réseau (journalisation, VAD, expiration des files d'événements,
certificats WebSocket/DTLS) sont décrites dans le `readme.md`.

---

## 11. Ce que le serveur ne fait pas

À savoir avant de concevoir une architecture autour :

- **pas de NAT IPv6** (NAT66, NPTv6). Un déploiement IPv6 route et filtre ;
- **pas de NAT à traduction de ports**. Le serveur annonce des ports : seul le NAT
  1:1 est exploitable ;
- **pas de re-découverte en cours de route.** L'adresse publique est déterminée au
  démarrage et figée ensuite. Une adresse dynamique impose un redémarrage ;
- **pas plusieurs adresses du même côté et de la même famille** (deux cartes sur le
  même réseau extérieur). Un profil, une adresse ;
- **pas de serveur TURN intégré.** Le mediaserver est joignable directement ou ne
  l'est pas.

---

## 12. Pour aller plus loin

| Document | Contenu |
|---|---|
| `readme.md` | Installation, démarrage, ensemble des options de ligne de commande |
| `MCU-API.md` §6.7 bis | Contrat du paramètre `profile` côté API MCU (pour le développeur du contrôleur) |
| `xmlrpc_jsr309_api.md` §6.7 bis / §6.7 ter | Idem côté API JSR-309, et `GetNetworkProfiles` |
