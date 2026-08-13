# Audit IPv6 du mediaserver

État des lieux au 2026-08-12, branche `fix/c++-renovation`.

> **Suivi.** Les bugs que cet audit a mis au jour et qui étaient **réels dès
> aujourd'hui, en IPv4** ont été corrigés (§11). Une suite de tests adverses
> décrivant la cible IPv6 existe et est **volontairement en échec** :
> `make -C mcu check-ipv6` (voir §12). Aucun support IPv6 n'a été
> ajouté : l'arbitrage du §7 reste ouvert.

Ce document **recense**, il ne migre pas. Il liste, zone par zone, tout ce qui
empêche aujourd'hui le mediaserver de fonctionner en IPv6, avec `fichier:ligne`,
la nature de l'obstacle et ce qu'il faudrait pour le lever. Il est destiné à servir
de base d'arbitrage : décider *si* on passe le serveur en dual-stack, et dans quel
ordre.

---

## 0. Résumé exécutif

Le mediaserver est **exclusivement IPv4**. Aucune ligne du code C++ compilé ne
manipule `AF_INET6`, `sockaddr_in6` ni `sockaddr_storage` — la seule exception est
le sous-module `libbfcp`, écrit dual-stack mais initialisé en IPv4.

Concrètement, aujourd'hui :

- **aucune socket n'écoute en IPv6** — ni RTP, ni RTMP, ni WebSocket, ni XML-RPC ;
- **aucune adresse IPv6 ne peut être configurée** — `--public-ip` la refuse
  explicitement ;
- **aucune adresse IPv6 ne peut être annoncée** dans un SDP ni dans un candidat ICE ;
- **un hôte purement IPv6 ne démarre pas du tout** : `main()` refuse de démarrer
  faute d'adresse IPv4 annonçable.

La bonne nouvelle : **le contrat de contrôle n'est pas en cause.** L'adresse
distante traverse toute l'API XML-RPC sous forme de **chaîne opaque** —
`xmlrpcmcu.cpp` → `MultiConf::StartSending` → `RTPParticipant` →
`RTPSession::SetRemotePort` — sans jamais être interprétée. La rupture est
entièrement dans la couche transport. Les signatures publiques (`char *sendIp`)
n'ont pas à bouger.

Trois verrous structurants, dans cet ordre d'importance :

1. `in_addr_t` / `sockaddr_in` comme type d'adresse dans `RTPSession` ;
2. `inet_addr` / `inet_ntoa` comme unique conversion texte ↔ binaire ;
3. tous les `bind()` en `AF_INET` + `INADDR_ANY`.

Périmètre mesuré : 137 occurrences de
`AF_INET|PF_INET|sockaddr_in|inet_addr|inet_ntoa|INADDR_ANY|gethostbyname|in_addr`
dans `mcu/src` + `mcu/include`.

---

## 1. RTPSession — le cœur du problème

C'est ici que se concentre l'essentiel du travail : `RTPSession` porte à la fois
l'état d'adressage, les sockets média, le latching NAT, ICE et l'adresse annoncée.

### 1.1 État interne IPv4 par construction

`mcu/include/rtpsession.h` :

| Ligne | Déclaration | Obstacle |
|---|---|---|
| 436-437 | `sockaddr_in sendAddr` / `sockaddr_in sendRtcpAddr` | destination RTP/RTCP, 4 octets d'adresse |
| 453 | `in_addr_t recIP` | source réellement observée, 32 bits |
| 454 | `in_addr_t iceRemoteIP` | pair ICE retenu, 32 bits |
| 428-429 | `bool NatCorrectable(in_addr_t)` / `static bool IsRFC1918(in_addr_t)` | logique de plages privées v4 |
| 244 | `void OnICEConnectivityConfirmed(sockaddr_in* from)` | signature exposée à `dtls`/ICE |

Initialisation dans `mcu/src/rtpsession.cpp` : `recIP = INADDR_ANY` (l. 224),
`sendAddr.sin_family = AF_INET` et `sendRtcpAddr.sin_family = AF_INET` (l. 304-305),
`memset(&sendAddr, 0, sizeof(struct sockaddr_in))` (l. 295-296).

**Cible** : `sockaddr_storage` + `socklen_t`, et remplacement des comparaisons
`a.sin_addr.s_addr != b.sin_addr.s_addr` par un prédicat unique
`SameAddr(const sockaddr*, const sockaddr*)`.

Cas particulier de `IsRFC1918` (`rtpsession.cpp` l. 818-833) : la fonction reconnaît
10/8, 172.16/12, 192.168/16, 100.64/10 (RFC 6598) et 169.254/16 (RFC 3927). Son
équivalent v6 serait `fc00::/7` (ULA) et `fe80::/10` (link-local) — mais il faut
noter que **le rattrapage NAT n'a pratiquement pas de sens en IPv6** : la question
à trancher est plutôt de désactiver `NatCorrectable` sur les jambes v6 que de le
porter.

### 1.2 Sockets média liées en IPv4 seul

`RTPSession::Init`, `mcu/src/rtpsession.cpp` l. 1268-1340 :

- `sockaddr_in recAddr` (l. 1274), `recAddr.sin_family = AF_INET` (l. 1280) ;
- `socket(PF_INET, SOCK_DGRAM, 0)` pour RTP (l. 1303) et RTCP (l. 1323) ;
- `bind(..., sizeof(struct sockaddr_in))` (l. 1315 et 1329).

**Cible recommandée** : socket unique en `AF_INET6` avec `IPV6_V6ONLY = 0`. Les
pairs v4 arrivent alors en `::ffff:a.b.c.d` sur la même socket, ce qui évite de
doubler la plage de ports RTP (`--min-rtp-port` / `--max-rtp-port`) et de dupliquer
le thread `Run`. Contrepartie à arbitrer : le latching NAT et les comparaisons
d'adresse manipulent alors des adresses mappées, donc `SameAddr` doit savoir qu'une
`::ffff:1.2.3.4` et une `1.2.3.4` sont la même chose.

### 1.3 Conversion texte → binaire : `inet_addr`

Deux points d'entrée, tous deux bloquants :

**`SetRemotePort(char *ip, int sendPort)`** — l. 859-906. `inet_addr(ip)` (l. 862).
Une adresse v6 littérale rend `INADDR_NONE` (`0xFFFFFFFF`), qui n'est pas testé :
la destination est silencieusement posée à `255.255.255.255`. Échec muet.

Sous-point à ne pas manquer : la convention interne « le contrôleur passe `0.0.0.0`
pour demander explicitement le latch » (l. 864-868, `natLatch = true`) est
elle-même une convention IPv4. Il faut lui trouver un équivalent (`::`, ou mieux, un
drapeau explicite dans les propriétés RTP).

**`AddICECandidate(const char* candidate)`** — l. 1121-1190. Le `sscanf` lit déjà
l'adresse en `%127s` (l. 1140), donc le **parseur accepterait un candidat v6** ;
c'est `inet_addr(address)` (l. 1167) qui le rejette avec « adresse invalide ». Le
trickle ICE est donc IPv4-seul alors que sa moitié haute serait prête.

**Cible** : `inet_pton` essayé sur les deux familles, ou `getaddrinfo` avec
`AI_NUMERICHOST` (qui gère aussi le scope `%iface` des adresses link-local).

### 1.4 Conversion binaire → texte : `inet_ntoa`

Une vingtaine d'occurrences. La plupart sont dans des `Log`/`Debug`/`Error` :
l. 874, 992, 1056, 1112, 1477, 1501, 1779, 1793, 1873, 1975, 2015, 2061, 2100, 3217.

Deux points d'attention :

- l. 1815 et 2052 : `ProcessRTCPPacket(rtcp, inet_ntoa(from_addr.sin_addr))`.
  Vérification faite, `fromAddr` n'est **utilisé que dans deux `Log`/`Debug`**
  (`rtpsession.cpp` l. 2632 et 2644) : la chaîne ne sert pas de clé
  d'identification, la conversion peut donc être portée sans effet de bord ;
- l. 140 : dans `DetectAnnouncedIp()`, voir §1.6.

**Cible** : `inet_ntop` / `getnameinfo`. Gain collatéral indépendant d'IPv6 :
`inet_ntoa` rend un **buffer statique**, donc non réentrant — les logs
multi-threads actuels sont déjà exposés à un entrelacement.

### 1.5 `INADDR_ANY` comme sentinelle « pas encore de destination »

Le test `sendAddr.sin_addr.s_addr == INADDR_ANY` sert partout de garde « je ne sais
pas encore où émettre » : l. 957, 985, 1034, 1072, 1195, 1213, 1241, 1403, 1465,
1783, 2395, 2401, 2406 — treize sites, plus les variantes sur `recIP` (l. 1098,
1468, 1487, 1924) et `iceRemoteIP` (l. 1902, 2019).

Sur `sockaddr_storage` cette sentinelle disparaît : il faut une garde explicite
(`ss_family == AF_UNSPEC`, ou un booléen `hasRemote`).

**Ce point est un préalable, et il est utile indépendamment d'IPv6** : remplacer ces
treize tests dispersés par un prédicat unique `HasRemote()` est un refactor à
comportement constant, testable seul, qui rend toute la suite mécanique.

### 1.6 Adresse annoncée dans le SDP

`mcu/src/rtpsession.cpp` l. 101-197. Point sensible : `CLAUDE.md` pose comme
invariant qu'il n'existe **qu'une seule source** pour l'adresse annoncée. C'est donc
le seul endroit à corriger — mais son contrat est IPv4 de bout en bout.

**`DetectAnnouncedIp()`** (l. 108-152), auto-détection à défaut de `--public-ip` :

- `gethostbyname(hostname)` (l. 122) — ne rend que des enregistrements A ;
- rejet explicite : `if (!localHost || localHost->h_addrtype != AF_INET)` (l. 125) ;
- `inet_ntoa` (l. 140) ;
- exclusion de la loopback par **comparaison littérale** à `"127.0.0.1"` (l. 143) —
  `::1` ne serait pas reconnue comme loopback ;
- message d'échec : « no non-loopback IPv4 address found » (l. 148).

**`SetAnnouncedIp(const char* ip)`** (l. 154-179) :

- `inet_pton(AF_INET, ip, &addr) != 1` → **`--public-ip` refuse toute adresse
  IPv6**, avec le message « is not a valid IPv4 address » (l. 164-165).

**Problème de contrat, au-delà du code** : `announcedIp` est une `std::string`
unique (l. 101). Un serveur dual-stack doit en annoncer **deux** — une par famille —
ce qui change la signature de `GetAnnouncedIp()` et donc celle de ses deux seuls
appelants : `Endpoint::GetMediaCandidates` (§2) et `StartReceiving` (§2).

**Cible** : `getaddrinfo` en remplacement de `gethostbyname`, stockage d'une paire
(v4, v6), et `GetAnnouncedIp(family)`.

### 1.7 STUN / ICE — attributs d'adresse

`mcu/src/stunmessage.cpp`, **dupliqué à l'identique** dans
`third_party/fontventa/libmedikit/stunmessage.cpp` :

- `AddAddressAttribute(sockaddr_in* addr)` (l. 341-355) ;
- `AddXorAddressAttribute(sockaddr_in* addr)` (l. 356-378).

Dans les deux : la **famille est codée en dur à `1`** (`aux[1] = 1`, l. 348 et 363)
et l'adresse est toujours copiée sur 4 octets (l. 352 et 370). Déclarations dans
`mcu/include/stunmessage.h` l. 114-115. Appelants : `rtpsession.cpp` l. 1727
et 1866.

Ce n'est **pas une simple extension de boucle** : la RFC 5389 impose pour IPv6
`family = 0x02`, 16 octets, et un XOR avec le magic cookie **suivi du transaction
ID** — alors que le code actuel n'XORe que sur le cookie (l. 372-375).

**Doublon à traiter** : le fichier existe en deux exemplaires (mcu + libmedikit).
Toute correction doit être faite deux fois, ou le doublon supprimé — comme l'a été
`mcu/src/red.cpp` lors des tests libmedikit.

---

## 2. API XML-RPC `StartSending` / `StartReceiving`

### 2.1 `StartSending` — transparent, rien à changer

`mcu/src/xmlrpcmcu.cpp` l. 1481-1549. Signature `(iiisiSi)` (l. 1494), avec repli
sans `role` en `(iiisiS)` (l. 1503). `sendIp` est lu comme `char*` et passé tel quel
à `conf->StartSending` (l. 1541).

**Aucune modification d'API n'est nécessaire** : l'adresse est opaque jusqu'au
transport, l'échec est reporté à `inet_addr` (§1.3). Cela vaut pour toute la chaîne
de signatures `char *sendIp` :

- `mcu/include/multiconf.h` l. 121 ;
- `mcu/include/rtpparticipant.h` l. 53 ;
- `mcu/include/audiostream.h` l. 34, `videostream.h` l. 34, `textstream.h` l. 26 ;
- `mcu/include/mediabridgesession.h` l. 73, 80, 86 ;
- `mcu/src/jsr309/Endpoint.h` l. 145, `MediaSession.h` l. 129.

Même transparence pour la passerelle : `MediaGatewayStartSendingVideo` /
`…Audio` / `…Text` (`mcu/src/xmlrpcmediagateway.cpp` l. 124, 294, 462).

### 2.2 `StartReceiving` — le seul vrai point de contrat

`mcu/src/xmlrpcmcu.cpp` l. 2012-2167. Depuis S4, la valeur de retour porte
l'adresse annoncée :

```
returnVal[0] = port           (inchangé, lu par mcuGold)
returnVal[1] = announcedIp    (l. 2135, = RTPSession::GetAnnouncedIp())
returnVal[2] = fmtp négocié   (P8a)
```

`returnVal[1]` est **une seule valeur scalaire** : un serveur dual-stack ne peut pas
annoncer les deux familles au contrôleur sans changer le contrat. Trois options :

| Option | Description | Coût |
|---|---|---|
| (a) | `returnVal[1]` suit la famille de l'offre du contrôleur | contrat inchangé, mais le mcu doit connaître la famille — donc un paramètre d'entrée en plus |
| (b) | ajout d'un `returnVal[3]` listant les adresses par famille | purement additif, les contrôleurs actuels l'ignorent |
| (c) | propriété RTP `ipFamily` posée par `SetRTPProperties` avant l'appel | cohérent avec `natLatch`, mais un aller-retour de plus |

Côté JSR-309, diagnostic identique : `EndpointStartSending`
(`mcu/src/jsr309/xmlrpcjsr309.cpp` l. 987-1046) et `EndpointStartReceiving`
(l. 1143-1250).

> **Rappel `CLAUDE.md`** — toute modification de l'API XML-RPC `/mcu` ou `/jsr309`
> impose la mise à jour **dans le même jeu de changements** des schémas protobuf
> MOTELI v2 (`apps/elixip2/priv/proto/moteli_*.proto`, dépôt elixip). L'option (b)
> ci-dessus est la seule qui reste purement additive des deux côtés.

### 2.3 Bug réel, indépendant d'IPv6 — `Endpoint::GetMediaCandidates`

`mcu/src/jsr309/Endpoint.cpp` l. 741-804 :

```c
char url[50];                                   // l. 744
...
sprintf(url, "%s://%s:%d", scheme, host, port); // l. 790
```

Une IPv6 littérale fait jusqu'à 45 caractères ; avec `wss://`, les crochets et le
port, on dépasse largement 50 → **débordement de pile**. Et ce n'est pas seulement
un risque futur : `host` peut déjà venir de `--websocket-host`
(`GetLocalMediaHost()`, l. 770), qui est une chaîne libre non validée — un nom
d'hôte un peu long suffit à déclencher le débordement **aujourd'hui, en IPv4**.

Deux manques s'ajoutent pour IPv6 :

- l'URL WebSocket exige d'encadrer le littéral v6 par `[...]` (RFC 3986) ;
- le SDP construit par le contrôleur à partir de cette valeur exige `c=IN IP6`
  (RFC 4566) au lieu de `c=IN IP4`.

C'est le premier correctif à faire, quel que soit l'arbitrage IPv6.

---

## 3. WebSocket

### 3.1 Écoute IPv4 seule

`WebSocketServer::Init`, `mcu/src/websocketserver.cpp` :

| Ligne | Code |
|---|---|
| 65 | `sockaddr_in addr;` |
| 88 | `server = socket(AF_INET, SOCK_STREAM, 0);` |
| 96 | `addr.sin_family = AF_INET;` |
| 97 | `addr.sin_addr.s_addr = INADDR_ANY;` |
| 101 | `bind(server, (sockaddr *) &addr, sizeof(addr))` |

Un client IPv6 ne peut donc pas se connecter, ni en `ws://` ni en `wss://`.

**La migration y est indolore** : `accept(server, NULL, 0)` (l. 194) ignore
l'adresse du pair, il n'y a donc ni journalisation ni filtrage d'adresse à
convertir. C'est un `socket(AF_INET6, …)` + `IPV6_V6ONLY = 0` + `sockaddr_in6`,
rien d'autre. Le refactor mono-thread `poll()` (déjà en place) n'est pas touché.

### 3.2 `--websocket-host`

`mcu/src/main.cpp` l. 251-253 → `WSEndpoint::SetLocalHost` (l. 525-526,
implémentation `mcu/src/jsr309/WSEndpoint.cpp` l. 255-258). C'est une **chaîne
libre, jamais validée**, mémorisée telle quelle et rendue par
`WSEndpoint::GetLocalHost()`.

Un littéral IPv6 y passerait donc — mais il ressortirait **non encadré** dans l'URL
construite par `GetMediaCandidates` (§2.3), produisant une URL invalide (`wss://
2001:db8::1:9123` est ambigu : le `:9123` ne se distingue pas de l'adresse). C'est
le même `sprintf` à corriger, en même temps que le choix `ws`/`wss` porté par
`WSEndpoint::IsLocalSecure()` (`Endpoint.cpp` l. 784-786).

### 3.3 Test associé

`mcu/tests/test_websocket_echo.cpp` l. 38-46 : `socket(AF_INET, …)` et
`inet_addr("127.0.0.1")` en dur. À dupliquer en variante `::1` pour prouver toute
migration.

---

## 4. Ligne de commande (`mcu/src/main.cpp`)

### 4.1 `--public-ip` refuse IPv6

l. 236-237 → `RTPSession::SetAnnouncedIp` (§1.6), qui valide en `AF_INET` seul.
Aucune option n'existe pour une adresse v6.

### 4.2 Le serveur refuse de démarrer sur un hôte purement IPv6

l. 378-402. Si aucune adresse IPv4 annonçable n'est trouvée — ni par `--public-ip`,
ni par auto-détection — `main()` journalise l'erreur et **rend `-1`** (l. 401).

C'est un choix délibéré et documenté (`readme.md`, *Adresse média annoncée*) : la
panne honnête plutôt qu'un serveur en apparence sain qui casse appel par appel.
Mais la conséquence est nette : **sur une machine sans IPv4, le mediaserver ne
démarre pas du tout**.

Textes à reprendre le jour venu :

- l. 189-191, l'aide : « defaults to the first non-loopback **IPv4** address of the
  host » ;
- l. 393 et 397, le message d'échec : « does not resolve to a non-loopback **IPv4**
  address » / « make "%s" resolve to the host **IPv4** address ».

### 4.3 Aucune option d'adresse d'écoute

`--http-port` (l. 218), `--rtmp-port` (l. 224), `--websocket-port` (l. 227),
`--min-rtp-port` (l. 230), `--max-rtp-port` (l. 233) : **des ports seuls**. Il
n'existe aujourd'hui aucun moyen de choisir l'interface ni la famille d'écoute —
tout est `INADDR_ANY`.

Options à prévoir, selon l'arbitrage retenu :

- `--public-ip6 <addr>` — adresse v6 annoncée, symétrique de `--public-ip` ;
- `--listen-address <addr>` — interface d'écoute, aujourd'hui inexistante ;
- ou un `--ip-family {4|6|dual}` unique, qui pilote à la fois les `bind()` et le
  choix de l'adresse annoncée.

---

## 5. Reste du projet

### 5.1 Autres serveurs TCP — même motif qu'au §3

| Serveur | Fichier | Lignes |
|---|---|---|
| RTMP | `mcu/src/rtmpserver.cpp` | 47, 60, 68-69, 73 |
| TCPEndpoint JSR-309 | `mcu/src/jsr309/TCPEndpoint.cpp` | 59, 66, 74-76, 79 |
| Partage de document | `mcu/src/shareddocmixer.cpp` | 416, 422, 438 (`PF_INET`), 450 |

Tous les trois : `sockaddr_in` + `AF_INET` + `INADDR_ANY` + `bind`. Comme le
serveur WebSocket, ils n'exploitent pas l'adresse du pair — conversion mécanique.

### 5.2 Client RTMP sortant

`mcu/src/rtmpclientconnection.cpp` l. 81-111 : `gethostbyname(server)` (l. 99) puis
`memcpy(&addr.sin_addr.s_addr, host->h_addr, host->h_length)` (l. 110) dans un
`sockaddr_in`, socket `AF_INET` (l. 96).

`gethostbyname` ne résout que les enregistrements A. À remplacer par `getaddrinfo`,
qui donne les deux familles, permet la boucle « essaie chaque adresse rendue », et
évite au passage le buffer statique non réentrant de `gethostbyname`.

### 5.3 Serveur XML-RPC (Abyss) — conditionne l'accès du contrôleur

`mcu/src/xmlrpcserver.cpp` l. 65 :

```c
ServerCreate(&srv, name, port, DEFAULT_DOCS, "http.log");
```

`ServerCreate` d'Abyss crée un socket **IPv4**. La bibliothèque système en offre
la sortie de secours : `ServerCreateSwitch` / `ServerCreateSocket` /
`ServerCreateSocket2` (`/usr/include/xmlrpc-c/abyss.h` l. 168-186) acceptent une
socket créée par nos soins — donc en `AF_INET6` avec `IPV6_V6ONLY = 0`, **sans
patch amont**.

Portée réelle : c'est ce qui conditionne l'accès **du contrôleur** au serveur en
IPv6, donc l'API `/mcu` et `/jsr309`, donc le long-poll `/events/...` — et par
ricochet la politique d'expiration par event queue, qui repose entièrement sur ce
long-poll.

À noter, sans lien avec IPv6 mais dans le même fichier : `handleReqStackSize` a dû
être porté à 1 Mo pour le rendu texte Magick — ne pas le perdre en refactorant.

### 5.4 BFCP — déjà dual-stack, seul le défaut est IPv4

`third_party/libbfcp/libbfcp/BFCPconnection.cpp` est le seul composant écrit
correctement :

- `sockaddr_storage` partout, `m_addrlen` porté à côté ;
- `inet_pton` essayé sur `AF_INET` **puis** `AF_INET6` (l. 875-889) ;
- `PrintAddress` gère les deux familles via `inet_ntop` (l. 846-857) ;
- comparaison d'adresses par famille (l. 1335-1345) ;
- gardes `sa_family != AF_INET && sa_family != AF_INET6` (l. 825, 1228).

Ne restent que deux points :

- `Client2ServerInfo::Init` (l. 1363-1378) force `m_localAddress.ss_family = AF_INET`,
  `sin_addr.s_addr = INADDR_ANY` et `m_addrlen = sizeof(struct sockaddr_in)` ;
- `EntryPoint` (l. 711-712) déclare encore un `sockaddr_in out_addr` local.

La couche C++ `mcu/src/bfcp/` ne manipule aucune adresse. **Coût faible** — c'est
la victoire rapide du chantier, et elle valide le motif `sockaddr_storage` sur du
code déjà en production.

### 5.5 Côté Java

**`jsr309impl/src/org/murillo/mscontrol/networkconnection/SdpPortManagerImpl.java`** :

- génération : `"o=- 0 0 IN IP4 "` (l. 619) et `"c=IN IP4 "` (l. 621) en dur ;
- **analyse** : `content.indexOf("\r\nc=IN IP4 ")` (l. 754 et 839) — un SDP distant
  en `c=IN IP6` n'est tout simplement **pas vu**, la recherche échoue.

**`jsr309impl/src/org/murillo/mscontrol/SubNetInfo.java`** l. 8, 25, 47, 55, 68 :
`Inet4Address.getByName(...)` lève sur une adresse v6. Toute la logique de
sous-réseau est à généraliser en `InetAddress` + longueur de préfixe variable.

**`sdp/`** : la bibliothèque d'analyse SDP **couvre déjà IPv6**. La grammaire ABNF
définit `IP6-address` et `IP6-multicast` (`sdp/src/org/murillo/abnf/sdp.abnf`
l. 110-144), et les règles générées existent (`Rule$IP6_address.java`,
`Rule$IP6_multicast.java`, `Rule$connection_address.java`). Ce n'est donc pas la
bibliothèque qui bloque, c'est **son contournement** par les `indexOf` littéraux de
`SdpPortManagerImpl`.

### 5.6 Tests

- `mcu/tests/test_rtp_latching.cpp` l. 82-85, 108-110, 161 : `sockaddr_in`,
  `inet_addr(ip)`, socket v4 en dur — c'est le test qui couvre précisément le
  latching NAT, donc celui qui devra prouver le comportement des adresses mappées ;
- `mcu/tests/test_websocket_echo.cpp` l. 38-46 (§3.3).

Les deux sont à **dupliquer** en variantes v6 plutôt qu'à convertir : il faut
continuer de prouver le chemin v4.

---

## 6. Synthèse — ordre de traitement proposé

| # | Zone | Effort | Dépend de | Gain |
|---|---|---|---|---|
| 1 | `libbfcp` : défaut dual-stack (§5.4) | faible | — | valide le motif `sockaddr_storage` sur du code réel |
| 2 | Débordement `char url[50]` (§2.3) | faible | — | **corrige un bug présent, hors IPv6** |
| 3 | Prédicats `RTPSession` : `HasRemote()`, `SameAddr()`, `IsPrivate()` (§1.1, §1.5) | moyen | — | refactor à comportement constant ; rend la suite mécanique |
| 4 | `sockaddr_storage` + sockets dual-stack dans `RTPSession` (§1.1-1.5) | **fort** | 3 | le média passe en v6 |
| 5 | Adresse annoncée par famille + contrat XML-RPC (§1.6, §2.2) | moyen | 4 | **engage elixip / MOTELI — à arbitrer avant de coder** |
| 6 | Écoutes TCP : WebSocket, RTMP, TCPEndpoint, Abyss (§3, §5.1, §5.3) | moyen | — | les plans de contrôle passent en v6 |
| 7 | STUN v6 : famille + XOR sur 16 octets (§1.7) | moyen | 4 | ICE complet en v6 |
| 8 | Java : `SdpPortManagerImpl`, `SubNetInfo` (§5.5) | moyen | 5 | la couche JSR-309 suit |

Les étapes 1, 2, 3 et 6 sont **indépendantes de l'arbitrage** et utiles en
elles-mêmes. Les étapes 4, 5, 7, 8 n'ont de sens que si la décision est prise.

---

## 7. Ce que ce document ne tranche pas

Trois questions restent ouvertes, à arbitrer avant tout codage :

1. **Socket unique dual-stack (`IPV6_V6ONLY = 0`, adresses mappées) ou deux sockets
   par famille ?** La plage de ports RTP, le nombre de threads `Run` et toute la
   logique de latching NAT en dépendent. La socket unique est recommandée ici, mais
   elle impose que `SameAddr` traite `::ffff:1.2.3.4` et `1.2.3.4` comme identiques.

2. **Le contrôleur elixip est-il prêt à recevoir une adresse v6 en `returnVal[1]`
   de `StartReceiving` ?** Sans réponse, l'option (b) du §2.2 (ajout purement
   additif d'un `returnVal[3]`) est la seule sûre.

3. **IPv6 est-il demandé pour le média (RTP), ou seulement pour les plans de
   contrôle (XML-RPC, WebSocket) ?** La réponse change radicalement le périmètre :
   les seules étapes 1, 2, 6 couvrent le second cas ; le premier impose les
   étapes 4, 5 et 7, c'est-à-dire l'essentiel du coût.

---

## 11. Correctifs livrés (2026-08-12) — aucun support IPv6 ajouté

Six défauts **réels en IPv4 aujourd'hui**, révélés par cet audit, ont été corrigés.
Build vert (`./install.ksh localcompile`), 179 tests PASS.

| # | Fichier | Défaut | Correctif |
|---|---|---|---|
| 1 | `rtpsession.cpp` `SetRemotePort` | `inet_addr` rend `INADDR_NONE` sur toute adresse illisible, et **ce retour n'était pas testé** : la destination devenait `255.255.255.255` et le serveur émettait le flux en **broadcast sur le LAN**, sans un mot dans le log | `inet_pton` (qui distingue l'erreur de l'adresse de diffusion) + refus explicite ; tous les appelants traitaient déjà `0` en erreur |
| 2 | `rtpsession.cpp` `ReadRTCP` | `sendRtcpAddr.sin_port = from_addr.sin_addr.s_addr` — le port RTCP de destination était pris dans l'**adresse** du pair. Le RTCP partait dans le vide dès qu'un binding STUN arrivait sur la socket RTCP (ICE sans rtcp-mux) | recopie de `from_addr.sin_port` |
| 3 | `rtpsession.cpp` `DetectAnnouncedIp` | `addr.s_addr = *(u_long*) h_addr_list[i]` — lecture de **8 octets sur LP64 dans un buffer de 4**, donc 4 octets hors bornes dans la structure de la résolveuse | `memcpy` de `sizeof(addr.s_addr)` |
| 4 | `Endpoint.cpp` `GetMediaCandidates` | `char url[50]` rempli par `sprintf`. `host` peut venir de `--websocket-host`, **chaîne libre jamais validée** : un nom de domaine un peu long débordait la pile | `char url[NI_MAXHOST+32]` + `snprintf` + refus explicite si troncature |
| 5 | `rtpsession.cpp` RTP symétrique | `sendAddr.sin_port = ntohs(recPort)` — conversion écrite à l'envers (`recPort` est en ordre hôte). Même résultat en pratique, intention fausse et incohérente avec le test `htons` deux lignes plus haut | `htons` |
| 6 | `rtmpclientconnection.cpp` | `Error("-Could not resolve %s\n", host)` passait `host`, qui vaut `NULL` à cet endroit par construction : le log disait toujours `(null)` au lieu de nommer l'hôte fautif | passe `server` |

Effet de bord assumé du correctif n°1 : `inet_pton` est **plus strict** qu'`inet_addr`,
qui acceptait les formes courtes et octales (`192.168.1`, `0300.0250.0.1`). Aucun
contrôleur n'émet ces formes — un SDP porte toujours un quadruplet complet.

### 11 bis. Deux bugs JSR-309 révélés par la suite de tests — sans rapport avec IPv6

L'écriture des tests du §12 a fait tomber un défaut **grave et récent**, que rien
n'exerçait jusque-là.

**A. `RTPEndpoint::Run()` masquait la boucle poll de `RTPSession` — la réception
RTP JSR-309 était morte.**

`RTPSession` dérive de `Worker`, dont `Run()` est **virtuel pur** et porte la boucle
`poll()` des sockets RTP/RTCP. Or `RTPEndpoint` déclarait de son côté un
`int Run()` — sa boucle de démultiplexage, appelée par un pthread historique créé
dans `StartReceiving()`. Même signature ⇒ **override**. Conséquence : pour tout
endpoint JSR-309, le thread du `Worker` (démarré par `RTPSession::Init`) exécutait
la boucle de démultiplexage, et `RTPSession::Run()` — donc `ReadRTP()`/`ReadRTCP()` —
**ne tournait jamais**.

Le contrôle d'accès ne protège pas : `RTPSession::Run()` est `private`, ce qui
n'empêche en rien la liaison virtuelle. Aucun avertissement du compilateur.

Preuve empirique, dans les logs : un `RTPSession` nu émet `>Run RTPSession [...]`
au démarrage de son thread ; un `RTPEndpoint` n'émettait que `RTPEndpointThread`.
La ligne `>Run RTPSession` était absente — la boucle poll ne démarrait pas.

Ce défaut date de la conversion de `RTPSession` à `Worker` (chantier SIGIO,
2026-08-11) : auparavant `RTPSession::Run` n'était pas virtuel, les deux `Run()`
coexistaient sans se voir. **C'est une régression d'un jour**, invisible parce
qu'aucun test n'exerçait la réception d'un `RTPEndpoint`.

Correctif : `RTPEndpoint::Run()` renommé `MultiplexLoop()` (son pthread l'appelle
directement) ; `RTPSession::Run()` marqué `override` avec le commentaire qui
explique le piège. Chacun retrouve son thread : `Worker` → boucle poll,
pthread de `StartReceiving` → démultiplexage.

**B. `RTPEndpoint::End()` joignait un thread avant d'arrêter sa boucle.**

`End()` appelait `RTPSession::End()` — qui joint le thread du `Worker` — **avant**
`StopReceiving()`, seul à baisser `receiving`. Comme la boucle jointe tournait sur
`while(receiving)`, le `join()` ne rendait jamais la main : **deadlock au démontage**
dès qu'un endpoint était détruit sans `StopReceiving()` explicite préalable. En
production le contrôleur appelle `EndpointStopReceiving` avant de supprimer, ce qui
masquait le défaut ; un démontage sur erreur ou sur expiration de session, non.

Correctif : `StopReceiving()` d'abord, `RTPSession::End()` ensuite — on arrête le
consommateur avant de détruire la source. L'ordre inverse laissait de surcroît le
thread de démultiplexage appeler `GetPacket()` sur une session en cours de
destruction.

**À retenir pour la suite du chantier IPv6** : ces deux bugs n'ont rien à voir avec
IPv6. Ils sont sortis parce que la suite adverse est la **première** à instancier un
`Endpoint` JSR-309 complet dans un test. Toute migration de `RTPSession` passera par
ce même chemin — d'où l'intérêt de garder ces tests exécutables.

## 12. Suite de tests adverses IPv6 (tag `:ipv6`)

`mcu/tests/test_ipv6.cpp` — **41 tests, volontairement en échec** : ils décrivent
la cible de ce document, pas l'existant.

```sh
cd mcu
make check-ipv6      # les joue tous
make check           # ne les joue PAS (c'est voulu)
```

**Depuis `mcu/`, pas depuis la racine** : `$(OBJS)` porte des noms d'objets nus,
donc `make -C mcu …` échoue sur `No rule to make target
'httpparser.o'` — et crée au passage un `media/` parasite à la racine.

Le tag est porté par une double convention de nommage — GoogleTest refuse `:` dans
un nom, c'est le séparateur de `--gtest_filter` : suites préfixées `IPv6`
(sélection), tests préfixés `DISABLED_` (exclusion par défaut).

État au 2026-08-12 : **27 FAILED, 12 PASSED, 2 SKIPPED**. Les échecs par section :

| Suite | Ce qu'elle attaque | Échecs |
|---|---|---|
| `IPv6Notation` | compression `::`, forme longue, casse hexa, crochets, ambiguïté adresse/port, zone-id, `::`-latch | 6 |
| `IPv6Canonical` | RFC 5952 (minuscules, compression), refus loopback/multicast à l'annonce | 2 |
| `IPv6Subnet` | ULA, Teredo, 6to4, multicast | 2 |
| `IPv6Mapped` | `::ffff:a.b.c.d` — égalité avec la forme v4, classification RFC 1918 à travers le mapping, dé-mapping à l'annonce | 3 |
| `IPv6DualStack` | les sockets RTP **et** RTCP écoutent-elles en v6 | 2 |
| `IPv6Ice` | candidats trickle host/srflx v6, zone-id, priorités inter-familles | 4 |
| `IPv6Stun` | famille `0x02`, 20 octets, XOR étendu au transaction ID | 2 |
| `IPv6Url` | encadrement `[...]`, longueur maximale | 2 |
| `IPv6Dns` | AAAA, `--public-ip` par nom, hôte double pile | 2 |
| `IPv6Servers` | WebSocket et RTMP acceptent-ils un client `[::1]` | 2 |

Deux tests passent **avant comme après** — ce sont les garde-fous anti-régression
IPv4, à ne jamais laisser tomber : `IPv6DualStack.LaSocketRtpEntendToujoursLIPv4`
et `IPv6Url.LAdresseV4NEstPasEncadree`.

Deux pièges de méthode, appris en écrivant cette suite :

- **un `sendto` UDP non connecté ne prouve rien** : il réussit même si personne
  n'écoute. Les tests de dual-stack se **connectent** puis lisent l'`ECONNREFUSED`
  remonté par l'ICMP du loopback (`PortEcouteVraiment`) — sans quoi les tests
  passaient à vide ;
- **les tests d'URL portent sur le vrai `Endpoint::GetMediaCandidates`**, pas sur
  une reformulation locale de la règle, qui ne prouverait que la cohérence du test
  avec lui-même.

## 13. Bibliothèque d'adresses convergentes — état de la question

La **bibliothèque standard C++ n'offre rien** : il n'y a aucune API réseau dans le
standard, à aucune version. La Networking TS (`std::experimental::net`, calquée sur
Asio) a été **abandonnée** — retirée du plan C++23 en 2021, absente de C++26. Il
n'existe donc pas de `std::ip::address`, et il n'y en aura pas à moyen terme.

Options réelles, par ordre de légèreté :

| Option | Licence | Poids | Ce qu'elle donne |
|---|---|---|---|
| **Asio standalone** (think-async/asio) | BSL-1.0 | en-têtes seuls, aucun lien | `asio::ip::address` (variant v4/v6), `make_address`, `is_loopback/is_multicast/is_link_local/is_site_local/is_v4_mapped`, `to_string()` canonique RFC 5952, `network_v6` pour les préfixes |
| **Boost.Asio** (`boost::asio::ip::address`) | BSL-1.0 | **`boost-devel` 1.75 déjà installé**, en-têtes seuls pour cet usage | identique |
| **POCO Net** (`Poco::Net::IPAddress`) | BSL-1.0 | lie `libPocoNet` + `libPocoFoundation` | équivalent, mais amène un framework |
| **type maison** sur `sockaddr_storage` | — | ~150 lignes | exactement ce que les tests du §12 exigent, rien de plus |

Recommandation : **`boost::asio::ip::address`** (ou son jumeau standalone). C'est
le type convergent le mieux supporté de l'écosystème C++, il couvre *toutes* les
exigences du §12 — parsing des notations, forme canonique RFC 5952, classification
des plages, v4-mapped, préfixes — et sur cette machine il **ne coûte aucun paquet
ni aucun lien supplémentaire**, seulement des en-têtes. Le seul vrai prix est le
temps de compilation qu'ajoute l'arbre d'inclusion d'Asio.

Le repli « type maison » reste défendable vu la sobriété assumée du build (deux
sous-modules, dépendances système minimales) et parce que `libbfcp` en contient
déjà l'esquisse (`PrintAddress`, comparaison par famille, `inet_pton` sur les deux
familles) : il y aurait là une base à extraire plutôt qu'à réécrire. À arbitrer en
même temps que la question 1 du §7.
