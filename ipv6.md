# Audit IPv6 du mediaserver

État des lieux au 2026-08-12, branche `fix/c++-renovation`.

> **Suivi.** Les bugs que cet audit a mis au jour et qui étaient **réels dès
> aujourd'hui, en IPv4** ont été corrigés (§11). Une suite de tests adverses
> décrivant la cible IPv6 existe et est **volontairement en échec** :
> `make -C mcu check-ipv6` (voir §12).
>
> **2026-08-15, branche `feat/ipv6` — l'arbitrage du §7 est RENDU** (réponses
> inscrites dans le §7 lui-même), et le modèle d'adressage retenu est décrit au
> **§14 : quatre profils d'adressage, choisis par le contrôleur appel par appel**.
> Première étape livrée : `IPAddress`/`IPEndpoint` (`mcu/include/ipaddress.h`,
> `mcu/src/ipaddress.cpp`, 52 tests actifs dans `make check`). Aucun appelant
> n'est encore branché : le tableau de bord du §12 est toujours à 27 échecs.

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

> **Fait le 2026-08-15** (étape 5) : `RTPSession` porte son adressage en
> `IPAddress`/`IPEndpoint`, ses sockets sont créées dans `socketFamily`
> (AF_INET6 + `IPV6_V6ONLY=0` par défaut, la famille de l'adresse de bind si un
> profil en impose une), et `SetBindAddress()` — à poser avant `Init` — choisit
> l'interface. `inet_addr`/`inet_ntoa` ont disparu du fichier ; `SetRemotePort`
> et `AddICECandidate` acceptent les deux familles, zone comprise. La classe de
> trafic passe par `IPV6_TCLASS` en v6, `IP_TOS` en v4 : poser le mauvais niveau
> ne remonte aucune erreur mais laisse le média non marqué.
>
> **Une régression attrapée par les tests, et qui dit tout du chantier** :
> l'adresse non spécifiée. `SetRemotePort("0.0.0.0")` signifie « latche-moi »,
> et l'ancien code le codait en posant `INADDR_ANY` dans `sendAddr` — la même
> valeur servant de sentinelle « pas de destination ». Avec un `IPEndpoint`, une
> destination `0.0.0.0` est une destination *renseignée* : `HasRemote()` rendait
> vrai et le média partait vers `0.0.0.0`. Il faut donc laisser l'endpoint
> **vide**. C'est exactement la confusion que l'étape 3 avait nommée, et il
> aurait été facile de la réintroduire ici sans le test de latching.

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

> **Fait le 2026-08-15** (étape 3). `rtpsession.h` porte désormais quatre
> prédicats inline — `HasRemote()`, `HasRemoteRtcp()`, `HasRecIP()`,
> `HasIceRemote()` — plus `SameAddr(a,b)` par où passent **toutes** les
> comparaisons d'adresse, et `SetRemoteIp()` qui pose la cible sur les deux
> jambes d'un coup (les deux appelants le faisaient ligne à ligne : en oublier
> une donnait un RTCP émis vers l'ancien pair). Vingt-huit sites convertis, à
> comportement **strictement constant** — les suites `RtpLatching`,
> `RtpRtcp` et `RtpRenegotiation` en font foi.
>
> Ne restent que trois `INADDR_ANY`, et ils sont légitimes : l'initialisation de
> `recIP`, qui *définit* la sentinelle, et les deux tests sur `ipAddr` dans
> `SetRemotePort` — celui-là est l'**argument du contrôleur**, pas notre état :
> « 0.0.0.0 » y veut dire « latche-moi » (§1.3).
>
> `IsRFC1918` a disparu, remplacée par `IPAddress::IsPrivateV4()` via une
> passerelle `V4Address(in_addr_t)` locale, qui s'en ira avec l'état v4 à
> l'étape 5. C'est le point où le vocabulaire compte : `NatCorrectable` consulte
> `IsPrivateV4()` et **non** `IsPrivate()`, qui répond « non routable » et
> couvrirait donc les plages de documentation ou réservées — nullement NATées.
> Un test le verrouille (`RtpLatching.DoesNotReAimFromANonRoutableButNonPrivateAnnouncement`,
> sur `192.0.2.1`), en plus de celui qui existait déjà sur `240.0.0.1`.

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

> **Fait les 2026-08-15** (étapes 5 et 9).
>
> `XOR-MAPPED-ADDRESS` puis `MAPPED-ADDRESS` lisent désormais la famille **dans
> la sockaddr** au lieu de la coder en dur, et écrivent 16 octets en v6 avec le
> XOR étendu au transaction ID. La signature publique reste `sockaddr_in*` — une
> surcharge `const sockaddr*` s'y ajoute — parce que `sa_family` et `sin_family`
> occupent le même offset : aucun appelant n'a eu à changer.
>
> **LE DOUBLON EST SUPPRIMÉ, ET DANS LE SENS INVERSE DE CELUI QU'ON CROIT.** La
> copie de libmedikit était **morte** : elle n'était dans aucun `OBJS`, elle
> incluait un `crc32calc.h` que le sous-module ne possède pas (donc elle
> n'aurait pas compilé), et elle portait encore l'appel `HMAC()` déprécié
> d'OpenSSL 1. C'est donc la version du mcu — vivante, portée sur OpenSSL 3 et
> désormais v6 — qui a été **rapatriée dans le sous-module**, avec `crc32calc`
> dont le FINGERPRINT STUN est l'unique appelant. `mcu/src/stunmessage.cpp`,
> `mcu/include/stunmessage.h`, `mcu/src/crc32calc.cpp` et
> `mcu/include/crc32calc.h` sont supprimés ; le mcu consomme
> `medkit/stunmessage.h` et l'objet vient de `libmedkit.a`. Il n'y a plus qu'une
> implémentation de STUN dans le produit.
>
> **L'adresse annoncée n'a rien à faire dans XOR-MAPPED, vérification faite.**
> L'attribut d'une *réponse* Binding décrit **l'expéditeur de la requête** vu par
> nous (RFC 5389 §15.2), pas nous-mêmes : les deux appelants passent bien
> `from_addr`. La règle du §14.5 — « ce qu'on publie porte l'adresse annoncée,
> pas l'adresse de bind » — concerne les **candidats ICE**, traités à l'étape 6
> via `Endpoint::GetMediaCandidates`. Rien à corriger ici, mais il fallait le
> vérifier plutôt que le supposer.

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

> **Fait le 2026-08-15** (étape 8) : les trois écoutes passent en `AF_INET6` +
> `IPV6_V6ONLY=0`.
>
> **UN DÉFAUT PRÉEXISTANT MIS AU JOUR, À REPRENDRE À PART.** Le test
> `IPv6Servers.LeServeurRtmpAccepteUnClientV6` passe — un client `[::1]` est bien
> accepté — mais le démontage se bloque **environ une fois sur dix** :
> `RTMPServer::End()` → `DeleteAllConnections()` → `RTMPConnection::End()`, juste
> après le démarrage du thread d'écriture de la connexion (dernières lignes du
> log : `>Delete all connections`, `>End RTMP connection`, `-RTMP Write
> Connecttion Thread`). Le blocage **ne se reproduit pas sous gdb** — Heisenbug de
> synchronisation, réveil perdu au plus probable ; l'attachement à chaud est par
> ailleurs interdit ici (`ptrace_scope=1`), d'où l'absence de pile.
>
> Ce n'est PAS un défaut IPv6 : avant cette étape la connexion v6 échouait, aucune
> connexion n'était créée, et ce chemin n'était jamais parcouru. Le test le rend
> atteignable, il ne le cause pas. Il reste donc `DISABLED_` — le garder dans
> `make check` installerait un blocage aléatoire dans une suite saine, c'est-à-dire
> payer le prix d'un défaut RTMP dans le chantier IPv6 — et jouable par
> `make check-ipv6`. Scénario à reprendre : démontage d'une `RTMPConnection` dont
> le pair raccroche aussitôt après le TCP.

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

> **Fait le 2026-08-15** (sous-module `third_party/libbfcp`). Le défaut d'écoute
> est `::` en `AF_INET6`, avec `IPV6_V6ONLY=0` posé avant le `bind` — les clients
> v4 continuent d'être servis, ils arrivent mappés. Le `sockaddr_in out_addr` mort
> de `EntryPoint` est supprimé. Cinq tests actifs (`BfcpDualStack`,
> `mcu/tests/test_bfcp_dualstack.cpp`) vérifient par `getsockname` **ce à quoi la
> socket est réellement liée**, pas ce que la bibliothèque en dit.
>
> **Trois bugs réels trouvés en chemin, sans rapport avec IPv6** — la même figure
> qu'au §11 :
>
> 1. `Client2ServerInfo::SetAddress` passait la `sockaddr` **entière** à
>    `inet_pton`, qui déposait donc les octets de l'adresse à l'offset 0, sur
>    `sa_family` et `sin_port` — que les deux lignes suivantes réécrivaient
>    aussitôt. `sin_addr` restait à zéro : **toute adresse donnée sous forme de
>    chaîne valait 0.0.0.0**, silencieusement. Invisible sur un bind local
>    (`INADDR_ANY` fonctionne), fatal sur une destination UDP. Aucun accesseur ne
>    le montrait, `getLocalAdress()` renvoyant la chaîne d'entrée et non l'état —
>    d'où des tests qui lisent la socket.
> 2. `setRemoteEndpoint` **ignorait le résultat** de la conversion et rendait
>    toujours `true` : une adresse illisible était acceptée, la destination
>    restant celle d'avant.
> 3. L'envoi UDP passait `m_addrlen` — la longueur de l'adresse **locale** — à
>    `sendto` pour une destination **distante**. Sans conséquence tant que tout
>    était v4 (16 octets des deux côtés) ; une destination v6 aurait été tronquée.
>
> Plus un défaut du constructeur de copie (`m_remoteAddressStr` recevait
> `m_remoteAddressAndPort`).

> **Fait le 2026-08-15** (étape 8). Les quatre écoutes TCP passent en
> `AF_INET6` + `IPV6_V6ONLY=0` : **une** socket entend les deux familles, un
> client v4 arrivant en `::ffff:a.b.c.d`. Contrairement au média — dont la
> famille est choisie par le profil d'adressage — les plans de contrôle doivent
> tout entendre.
>
> Le cas d'Abyss demandait plus qu'un changement de constante : `ServerCreate()`
> ouvre lui-même une socket `AF_INET`. La socket est donc créée par le
> mediaserver (bind + listen) et passée telle quelle à `ServerCreateSocket()`.
>
> **Restriction de sûreté, ajoutée en cours d'étape** : dès qu'un réseau interne
> est déclaré (`--internal-ip`), l'API de contrôle XML-RPC **s'y restreint** —
> elle pilote entièrement le serveur média, elle n'a rien à faire sur une
> interface publique. Sans `--internal-ip`, on garde l'écoute historique sur
> toutes les interfaces : le déploiement simple ne doit pas casser.
> Deux conséquences à connaître :
> - une seule socket, donc **une seule famille** quand l'adresse est précise. Si
>   les deux profils internes sont configurés, l'**IPv4 l'emporte** (choix
>   déterministe, majoritaire sur les plans de contrôle) et le démarrage le
>   journalise ;
> - **la loopback n'est plus une porte d'entrée** : un script d'administration
>   local qui tapait `http://127.0.0.1:8080/mcu` doit viser l'adresse interne.
>   C'est la contrepartie directe d'un bind sur une adresse précise.

### 5.5 Côté Java

**`jsr309impl/src/org/murillo/mscontrol/networkconnection/SdpPortManagerImpl.java`** :

- génération : `"o=- 0 0 IN IP4 "` (l. 619) et `"c=IN IP4 "` (l. 621) en dur ;
- **analyse** : `content.indexOf("\r\nc=IN IP4 ")` (l. 754 et 839) — un SDP distant
  en `c=IN IP6` n'est tout simplement **pas vu**, la recherche échoue.

**`jsr309impl/src/org/murillo/mscontrol/SubNetInfo.java`** l. 8, 25, 47, 55, 68 :
`Inet4Address.getByName(...)` lève sur une adresse v6. Toute la logique de
sous-réseau est à généraliser en `InetAddress` + longueur de préfixe variable.

> **Fait le 2026-08-15** (étape 10) — avec une réserve à lire avant de relire le
> code : **rien de tout ceci n'est compilé sur cette machine**. Il n'y a qu'un
> JRE (`java` 21), pas de JDK ni d'`ant` : `javac` n'existe pas ici. Les
> changements Java ont donc été relus, pas vérifiés par un compilateur. Ils sont
> à bâtir avant toute mise en service.
>
> **`SubNetInfo`** est réécrit : il raisonnait en **entier 32 bits** — lecture par
> `Inet4Address.getByName()`, qui LÈVE sur une v6, et préfixe calculé par
> décalage sur un `int`. Une adresse v6 fait 128 bits, aucune des deux hypothèses
> ne survit. La comparaison se fait maintenant **octet par octet**, ce qui vaut
> pour 4 comme pour 16. Une règle qui n'existait pas y est posée : **deux
> familles différentes ne se contiennent jamais**. `isPrivate` couvre en plus les
> plages qui manquaient — 100.64/10 (RFC 6598) et 169.254/16 côté v4, ULA
> `fc00::/7`, link-local `fe80::/10` et `::1` côté v6.
>
> **`SdpPortManagerImpl`** : le type d'adresse **suit l'adresse** à la génération
> (`IN IP4` / `IN IP6`, RFC 4566 §5.7), et l'analyse reconnaît les deux — les
> `indexOf("\r\nc=IN IP4 ")` littéraux ne **voyaient tout simplement pas** un
> SDP en v6. La convention de latch suit la famille : `::` en v6 là où le code
> écrivait `0.0.0.0`.
>
> **Un défaut préexistant corrigé au passage** : quand aucune ligne `c=` n'était
> trouvée, `indexOf` rendait −1 et le `substring(i+11, j)` qui suivait partait de
> l'octet 10 — l'« adresse » obtenue était un morceau de la ligne `o=`, propagé
> jusqu'au serveur média sans un mot. C'est maintenant une `SdpPortManagerException`.
>
> **`XmlRpcMcuClient` et `XmlRPCJSR309Client`** portent le paramètre de profil,
> en **surcharges** : les appels existants de `jsr309impl` compilent sans
> modification, et le profil n'est envoyé que s'il est demandé — XML-RPC étant
> positionnel, une liste plus longue serait rejetée par un serveur antérieur.
> `GetNetworkProfiles()` est exposée dans les deux clients.
>
> **Ce qui n'est PAS fait, et pourquoi** : `jsr309impl` ne demande aucun profil.
> Choisir lequel pour quelle connexion est une décision de produit — elle
> dépendrait d'une configuration par `MediaServer` que personne n'a spécifiée.
> Le client sait le faire ; le brancher demande de savoir ce qu'on veut.

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

Ordre révisé au 2026-08-15, après l'arbitrage du §7 et l'adoption du modèle de
profils du §14. Les étapes 2 et 3 de la liste d'origine sont faites.

| # | Zone | Effort | Dépend de | Gain | État |
|---|---|---|---|---|---|
| 0 | Type d'adresse commun `IPAddress`/`IPEndpoint` (§13, §14.5) | moyen | — | supprime `inet_addr`/`inet_ntoa`/`in_addr_t` du vocabulaire ; socle de tout le reste | **fait** (`556233d`) |
| 1 | `libbfcp` : défaut dual-stack (§5.4) | faible | 0 | valide le motif sur du code réel ; **3 bugs réels trouvés en chemin** | **fait** |
| 2 | Débordement `char url[50]` (§2.3) | faible | — | **corrigeait un bug présent, hors IPv6** | **fait** (§11) |
| 3 | Prédicats `RTPSession` : `HasRemote()`, `SameAddr()`, `IsPrivateV4()` (§1.1, §1.5) | moyen | 0 | refactor à comportement constant ; rend la suite mécanique | **fait** |
| 4 | Table des profils d'adressage + CLI `--public-ip`/`--nat`/`--internal-ip`/`--default-profile` (§14.1, §14.2) | moyen | 0 | le serveur SAIT ce qu'il peut annoncer, et le dit | **fait** |
| 5 | Sockets `RTPSession` : bind selon le profil, famille de la session (§1.1-1.5, §14.5) | **fort** | 3, 4 | le média passe en v6 et l'interface devient choisie, pas subie | **fait** |
| 6 | Contrat de contrôle : paramètre de profil dans `StartSending`/`StartReceiving`, MCU **et** JSR-309, + protos MOTELI (§2.2, §14.3) | moyen | 4, 5 | le contrôleur choisit sa famille et sa portée | **fait côté serveur** ; protos MOTELI à faire dans elixip |
| 7 | Introspection : méthode « quels profils as-tu ? » (§14.4) | faible | 4 | **sans elle, le contrôleur devine — le défaut déjà payé sur les codecs** | **fait** |
| 8 | Écoutes TCP : WebSocket, RTMP, TCPEndpoint, Abyss (§3, §5.1, §5.3) | moyen | 0 | les plans de contrôle passent en v6 | **fait** |
| 9 | STUN v6 : famille + XOR sur 16 octets, MAPPED-ADDRESS, et fin du doublon `stunmessage` (§1.7) | moyen | 5 | ICE complet en v6 | **fait** |
| 10 | Java : `SdpPortManagerImpl`, `SubNetInfo`, `XmlRpcMcuClient` (§5.5) | moyen | 6 | la couche JSR-309 suit | **fait, NON COMPILÉ ici** |

Les étapes 1, 3 et 8 restent **indépendantes du contrat de contrôle** : elles
peuvent avancer sans elixip. Les étapes 6, 7 et 10 l'engagent — et l'étape 7 n'est
pas facultative, voir §14.4.

---

## 7. Arbitrage — rendu le 2026-08-15

Les trois questions que ce document laissait ouvertes ont été tranchées, et une
quatrième (le type d'adresse, §13) avec elles.

1. **Socket unique dual-stack (`IPV6_V6ONLY = 0`) ou deux sockets par famille ?**
   → **Socket unique**, avec `SameAddr` traitant `::ffff:1.2.3.4` et `1.2.3.4`
   comme identiques. C'est fait dans `IPAddress` : le dé-mappage a lieu **à
   l'entrée** (`Parse`, `FromSockaddr`) et toute la classification travaille sur
   la forme dé-mappée, donc aucun appelant ne peut se tromper de forme.
   **Nuance apportée par le §14** : « une socket » ne veut pas dire « une socket
   qui écoute tout ». Dès lors que le profil d'adressage désigne une **adresse
   locale de bind**, la socket est liée à cette adresse — donc à sa famille. Le
   dual-stack `IPV6_V6ONLY=0` reste le mode par défaut (bind `::`, compatibilité
   ascendante) et le mode des **plans de contrôle** ; il ne s'applique plus au
   média dès qu'un profil explicite est demandé. Voir §14.5.

2. **elixip est-il prêt à recevoir une adresse v6 en `returnVal[1]` ?** → La
   question est **dépassée** par le §14 : ce n'est plus au serveur de deviner la
   famille, c'est au contrôleur de la demander. Le contrat évolue par l'**entrée**
   (un paramètre de profil, option (a) du §2.2 généralisée), pas par la sortie ;
   `returnVal[1]` porte alors l'adresse annoncée du profil demandé, et un
   contrôleur qui n'envoie pas le paramètre obtient exactement ce qu'il obtient
   aujourd'hui. La vérification côté elixip reste à faire avant de livrer
   l'étape 6.

3. **IPv6 pour le média, ou seulement pour les plans de contrôle ?** → **Les
   deux : dual-stack complet**, étapes 1 à 10 du §6.

4. **Quel type d'adresse convergent ?** (§13) → **Type maison**, pas
   `boost::asio::ip::address` : `IPAddress`/`IPEndpoint`, livrés au §14.5.

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

> **État au 2026-08-15, après les étapes 0 à 8 : 40 PASSED, 1 SKIPPED,
> 0 FAILED.** Le tableau de bord est intégralement vert. Un seul test reste
> `DISABLED_` — `IPv6Servers.LeServeurRtmpAccepteUnClientV6` — et **il passe** :
> il est exclu de `make check` parce qu'il expose une course de démontage RTMP
> préexistante (voir §5.1), pas parce qu'IPv6 y échoue.
>
> **État au 2026-08-15, après les étapes 0 à 5 : 37 PASSED, 2 SKIPPED,
> 2 FAILED.** Les tests devenus verts ont **perdu leur préfixe `DISABLED_`**,
> comme prévu : ils sont désormais joués par `make check` et servent de
> garde-fous anti-régression. Ne restent désactivés que les deux tests d'écoute
> TCP (`IPv6Servers`), qui décrivent l'étape 8.
>
> Un test a vu sa **prémisse corrigée** : `IPv6Dns.PublicIpAccepteUnNomDHote`
> exigeait que `SetAnnouncedIp("localhost")` réussisse, ce qui ne pouvait pas
> coexister avec `IPv6Canonical.RefuseDAnnoncerLaLoopback` deux suites plus
> haut — annoncer 127.0.0.1 publie un SDP injoignable, que le contrôleur ait
> écrit l'adresse ou son nom. Le test demande maintenant ce qui était réellement
> visé : que le **nom de l'hôte** soit accepté, et que « localhost » reste
> refusé.

État au 2026-08-12 (avant travaux) : **27 FAILED, 12 PASSED, 2 SKIPPED**. Les échecs par section :

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

> **Tranché (2026-08-15) : type maison.** `mcu/include/ipaddress.h` +
> `mcu/src/ipaddress.cpp`, 53 tests dans `make check`. Voir §14.5.

---

## 14. Profils d'adressage — le contrôleur choisit, le serveur détient

Modèle retenu le 2026-08-15. Il remplace l'idée d'« une adresse annoncée, par
famille » du §1.6 et du §2.2 par quelque chose de plus large, et qui répond
aussi à un besoin qui n'était pas dans l'audit : le **fonctionnement en SBC**,
où le serveur a un pied sur un réseau interne et un pied sur l'extérieur.

### 14.1 Le modèle : quatre profils, deux adresses chacun

Un **profil d'adressage** est le croisement de deux axes :

|  | IPv4 | IPv6 |
|---|---|---|
| **publique** (côté extérieur) | profil `publicv4` — peut être **natté** | profil `publicv6` — **jamais natté** |
| **interne** (réseau de service) | profil `internalv4` — **RFC 1918 exigée** | profil `internalv6` — ULA **ou** unicast global (§14.2) |

Chaque profil porte **deux adresses distinctes**, et c'est là tout l'intérêt :

- une **adresse de bind** — réellement attachée à une interface de la machine.
  C'est elle que la socket média lie, donc elle qui décide de l'interface
  empruntée ;
- une **adresse annoncée** — celle que le pair verra dans le SDP. Elle est
  **égale** à l'adresse de bind, sauf pour `publicv4` derrière NAT, où elle vaut
  l'adresse publique du routeur.

Cette séparation est le vrai apport du modèle. Aujourd'hui les deux notions sont
confondues dans une unique variable statique (`RTPSession::announcedIp`), et
c'est précisément ce qui rend indescriptible un déploiement derrière NAT : on ne
peut annoncer une adresse qu'on ne peut pas binder.

**Pas de NAT en IPv6, par choix.** `--nat` n'est accepté qu'avec une adresse v4,
et une tentative sur un profil v6 est un refus explicite au démarrage, pas un
silence. NPTv6 et NAT66 existent — le mediaserver ne les couvrira pas : un
déploiement v6 correct délègue un préfixe et filtre, il ne translate pas.

**Le profil est une propriété de la jambe**, pas de la conférence ni du serveur :
c'est ce qui permet à une même conférence d'avoir un participant interne en
`internalv4` et un participant externe en `publicv6`. C'est le cas d'usage SBC.

### 14.2 Ligne de commande

```
--public-ip  <adresse v4 publique attachée à l'hôte>
--public-ip  <adresse v4 RFC 1918 attachée à l'hôte> --nat <adresse publique vue de l'extérieur>
--public-ip  <adresse v4 RFC 1918 attachée à l'hôte> --nat auto [--stun-server <hôte[:port]>]
--public-ip  <adresse v6 globale attachée à l'hôte>

--internal-ip <adresse v4 RFC 1918 attachée à l'hôte>
--internal-ip <adresse v6 ULA ou unicast global attachée à l'hôte>

--default-profile <publicv4|publicv6|internalv4|internalv6>
```

**Aucune de ces options ⇒ auto-détection.** Le serveur prend la première adresse
annonçable de son nom d'hôte — qui peut parfaitement être une **RFC 1918** — et
en fait son profil `publicv4`. « Public » désigne ici le côté extérieur du
serveur, pas la classe de l'adresse (§14.5). C'est le comportement historique, et
il ne change pas. **Aucune détection de NAT dans ce cas** : rien ne dit qu'il y en
a un, et aller deviner l'adresse vue de l'extérieur sans que personne ne l'ait
demandé serait une initiative que l'exploitant n'a pas prise. L'auto-détection
n'a lieu que si **ni `--public-ip` ni `--internal-ip`** n'est donné : dès que
l'exploitant décrit son adressage, le serveur s'en tient à ce qu'il a dit.

**`--nat auto`** découvre l'adresse publique en interrogeant un serveur STUN, et
**vérifie que le NAT est bien 1:1**. Réservé au cas qu'il sert : `--public-ip`
doit porter une adresse **RFC 1918 réellement attachée** — c'est l'adresse locale
depuis laquelle la sonde part, et sur une adresse publique il n'y a rien à
découvrir. Le refus est explicite si la condition n'est pas remplie.

La vérification 1:1 n'est pas un luxe : **le mediaserver annonce des PORTS**, pas
seulement une adresse. Si le NAT translate aussi les ports, le port RTP publié
dans chaque `m=` est faux pour tout le monde — le pair émet vers un port que le
routeur n'a jamais ouvert, et l'appel est muet. Découvrir l'adresse sans vérifier
la conservation des ports produirait une configuration qui a l'air juste et ne
marche pas.

La sonde est donc faite **deux fois, depuis deux ports locaux différents** : une
seule ne prouverait rien, un NAT à traduction pouvant avoir conservé ce port-là
par hasard. Verdict 1:1 = même adresse publique **et** deux ports conservés
(mapping « endpoint-independent », RFC 4787 §4.1). Sinon, refus de démarrer avec
le détail des ports observés — annoncer quand même produirait des appels muets
sans un mot dans le log.

Ce que la sonde ne prouve pas, et qu'il ne faut pas lui faire dire : rien sur le
**filtrage** du NAT (RFC 4787 §5, c'est le rôle du rattrapage et de l'amorçage),
et rien sur la **durée** du mapping — la découverte a lieu au démarrage, l'adresse
annoncée est figée ensuite.

`--stun-server` vaut `stun.l.google.com:19302` par défaut, pour que l'option
marche sans configuration. **Un déploiement de production devrait poser le
sien** : dépendre d'un tiers pour démarrer est un point de panne, et c'est
précisément pourquoi cette valeur est un défaut et non une constante enfouie.

`--public-ip` et `--internal-ip` sont **répétables**, au plus une fois par
famille ; **la famille est déduite de la valeur** (`IPAddress::Parse` la donne),
il n'y a donc pas d'option `--public-ip6` à retenir, ni d'ordre significatif
entre les options — un `--nat` s'applique à l'adresse **v4 publique**, où qu'il
soit sur la ligne. C'est le seul point où je m'écarte de la proposition initiale :
apparier `--nat` au `--public-ip` **précédent** rendrait le sens de la ligne de
commande dépendant de l'ordre des arguments, ce qui est un piège d'exploitation
classique (et invisible dans un fichier `/etc/sysconfig/mediaserver` édité à
quatre mains).

> **Fait le 2026-08-15** (étape 4) : `mcu/include/addressprofiles.h`,
> `src/addressprofiles.cpp`, câblage dans `main.cpp`, 20 tests actifs. La table
> est **figée** après lecture de la ligne de commande (`Freeze`), et le serveur
> journalise les quatre profils au démarrage — c'est déjà la matière de
> l'introspection du §14.4.
>
> **Une asymétrie assumée entre `--public-ip` et `--internal-ip`** : l'adresse
> interne DOIT être attachée à une interface locale (elle sert à choisir
> l'interface de service, elle n'a aucun sens sinon), l'adresse publique NON.
> `--public-ip` a toujours désigné « l'adresse que les pairs atteignent, qui
> n'est pas celle liée localement » : derrière NAT elle n'est attachée à aucune
> de nos interfaces, et l'exiger casserait ces déploiements du jour au
> lendemain. Une publique attachée sert donc aussi d'adresse de bind ; une
> publique non attachée laisse l'écoute historique « toutes interfaces » et ne
> sert qu'à l'annonce.
>
> Rien ne change pour un déploiement existant : l'adresse annoncée est résolue
> comme avant (option, sinon auto-détection, sinon refus de démarrer), puis
> devient l'entrée `publicv4` de la table.

`--default-profile` désigne le profil qu'emploie un appel **qui n'en demande
aucun** ; à défaut d'option, c'est `publicv4`, le comportement historique. Elle
existe pour un cas précis et réel : sur un hôte **v6-only**, un contrôleur non
mis à jour n'enverra jamais de paramètre de profil, demandera donc `publicv4`,
et **échouera systématiquement** — le déploiement serait inutilisable tant que le
contrôleur n'a pas bougé. `--default-profile publicv6` le débloque **sans toucher
au contrat de contrôle** : c'est une décision d'exploitation, prise là où vit
déjà le reste de la configuration réseau, et elle laisse intact le principe selon
lequel un contrôleur qui demande explicitement un profil obtient celui-là ou une
erreur.

Le profil désigné par `--default-profile` doit être **disponible** au démarrage,
sinon refus : une valeur par défaut qui échoue à chaque appel est le pire des
deux mondes.

Contrôles au démarrage, tous **bloquants** — mieux vaut un serveur qui refuse de
démarrer qu'un serveur qui annonce une adresse fausse pendant six mois :

- deux `--public-ip` (ou deux `--internal-ip`) de la même famille → refus ;
- `--internal-ip` **v4** hors des plages privées (`IsPrivateV4()` : 10/8,
  172.16/12, 192.168/16, 100.64/10, 169.254/16) → refus. Une adresse publique
  déclarée comme interne serait annoncée à des pairs internes qui n'y ont rien à
  faire, et masquerait une erreur de configuration ;
- `--internal-ip` **v6** : **aucune contrainte de plage**. ULA (`fc00::/7`) et
  unicast global sont acceptées à égalité — seuls s'appliquent les contrôles
  généraux ci-dessous (adresse réellement attachée, et annonçable). **Cette
  asymétrie avec la v4 est délibérée**, et elle découle directement du §14.5 :
  « interne » est une décision, « privée » est un fait. En IPv4, la décision
  *coïncide* avec le fait — trente ans de NAT font qu'une adresse publique
  déclarée interne est presque à coup sûr une faute de frappe, d'où le contrôle
  RFC 1918. En IPv6, la coïncidence n'existe pas : un réseau interne est le plus
  souvent numéroté dans **une plage globale déléguée par l'opérateur**, et son
  caractère interne tient au **routage et au filtrage**, pas à l'adresse. Exiger
  l'ULA reviendrait à réimporter le fait dans la décision — exactement ce que le
  §14.5 sépare. `IsUniqueLocalV6()` reste utile en **diagnostic** (journaliser au
  démarrage qu'un profil interne v6 est en unicast global rappelle à
  l'exploitant que la protection repose entièrement sur son filtrage), jamais en
  refus ;
- `--nat` sans `--public-ip` v4 → refus ; `--nat auto` sans `--public-ip`
  **RFC 1918 attachée** → refus (rien à découvrir depuis une adresse publique, et
  sans adresse locale la sonde ne partirait pas de la bonne interface) ;
- `--nat auto` dont la sonde STUN échoue, ou dont le verdict n'est pas 1:1 →
  refus. L'échec réseau et le verdict négatif sont deux sorties DISTINCTES du
  client STUN : les confondre ferait passer un pare-feu pour un NAT symétrique ;
- `--nat` avec une valeur v6, ou appliqué à un profil v6 → refus motivé ;
- une adresse de bind qui **n'est attachée à aucune interface locale**
  (`getifaddrs`) → refus. Seule l'adresse de `--nat` échappe à ce contrôle, par
  construction : elle vit sur le routeur, pas sur nous ;
- une adresse de bind non annonçable (loopback, multicast, link-local) → refus ;
- **aucun profil disponible du tout** → refus de démarrer, comme aujourd'hui
  (`main.cpp` l. 378-402), mais pour la bonne raison et avec un message qui dit
  laquelle des quatre cases est vide.

**Compatibilité ascendante** : `--public-ip <v4>` seul se comporte exactement
comme aujourd'hui, et son absence garde l'auto-détection actuelle (première
adresse non loopback du nom d'hôte) comme profil `publicv4`. Un déploiement
existant ne change pas de comportement.

### 14.3 Le contrat de contrôle

`StartSending` et `StartReceiving` — MCU **et** JSR-309 — reçoivent un
**paramètre de profil supplémentaire**, ajouté **en fin de liste** (XML-RPC est
positionnel : c'est la seule position qui ne casse pas les appelants).

- valeurs : `"publicv4"`, `"publicv6"`, `"internalv4"`, `"internalv6"` — des chaînes,
  pas des entiers : elles se lisent dans une trace d'exploitation, et un
  désalignement d'énuméré entre elixip et le mcu serait un bug silencieux ;
- **absent ou vide ⇒ le profil par défaut**, qui vaut `publicv4` sauf si
  `--default-profile` en désigne un autre (§14.2) — soit, dans la configuration
  historique, exactement le comportement actuel ;
- **profil demandé indisponible ⇒ échec explicite**, avec un code distinguable
  d'une erreur générique (« profil d'adressage indisponible »), de sorte que le
  contrôleur puisse retomber sur un autre profil au lieu de deviner ;
- côté MOTELI, la même information est un **enum protobuf** dont la valeur `0`
  vaut `PUBLIC_V4` : le zéro protobuf est implicite, donc la compatibilité
  ascendante tombe juste sans champ optionnel supplémentaire.

**`StartSending` et `StartReceiving` doivent s'accorder.** En RTP symétrique, la
socket est la même dans les deux sens : le profil est fixé par le premier des
deux appels, et le second, s'il en porte un **différent**, est un échec — pas une
recréation silencieuse de la socket. De même, la famille de l'adresse distante
passée à `StartSending` doit être **cohérente** avec le profil : émettre vers une
v6 depuis une socket liée en v4 est impossible, autant le dire à l'appel plutôt
qu'au premier paquet.

> **Rappel `CLAUDE.md`** : cette évolution touche l'API XML-RPC `/mcu` et
> `/jsr309`, elle **doit** donc arriver dans le même jeu de changements que la
> mise à jour des schémas protobuf MOTELI v2 (`moteli_*.proto`, dépôt elixip), et
> que celle de `MCU-API.md` / `xmlrpc_jsr309_api.md`.

> **Fait le 2026-08-15** (étape 6) : `profile` est le dernier paramètre,
> facultatif, de `StartSending`/`StartReceiving` (MCU) et
> `EndpointStartSending`/`EndpointStartReceiving` (JSR-309). Chaîne de repli
> complète : un contrôleur qui ne l'envoie pas est parsé par la signature
> précédente, exactement comme avant. Le profil descend jusqu'à `RTPSession`,
> qui relie ses sockets sur l'adresse du profil (`Rebind` — le port local change,
> d'où l'obligation de poser le profil AVANT de publier le port) et rend
> l'adresse annoncée correspondante dans `returnVal[1]` et dans les candidats
> JSR-309. Documenté dans `MCU-API.md` §6.7 bis et `xmlrpc_jsr309_api.md`
> §6.7 bis ; 7 tests (`RtpAddressProfile`).
>
> **RESTE À FAIRE, hors de ce dépôt** : les schémas protobuf MOTELI v2
> (`apps/elixip2/priv/proto/moteli_*.proto`, dépôt elixip) doivent gagner le même
> champ — un enum dont la valeur `0` vaut `PUBLIC_V4`, pour que la compatibilité
> ascendante tombe juste sans champ optionnel. `CLAUDE.md` en fait une règle :
> l'API HTTP et le transport RabbitMQ ne doivent jamais diverger.

### 14.4 L'introspection n'est pas facultative

Si le contrôleur doit **choisir** un profil, il doit pouvoir **demander lesquels
existent**. Une méthode d'interrogation (`GetNetworkProfiles`, ou l'équivalent
dans l'événement de démarrage) rendant, pour chacun des quatre profils, sa
disponibilité et son adresse annoncée, fait partie de la livraison — étape 7
du §6, et non « plus tard ».

Ce n'est pas du confort. `CLAUDE.md` porte déjà le précédent, et il a coûté un
appel : les codecs supportés ne sont interrogeables par aucune API, donc elixip a
**déclaré** de son côté ce qu'il croyait que le serveur savait faire, et un appel
AV1 ↔ AV1 est mort en 488 avec un audio parfait des deux côtés (2026-08-12). Une
capacité qui existe dans le code mais qu'aucune API ne permet d'interroger est un
**défaut**. Les profils d'adressage sont exactement la même figure : quatre cases
dont deux ou trois seront vides selon le déploiement, et un contrôleur qui, faute
de pouvoir demander, écrira la liste dans sa propre configuration — laquelle
dérivera.

> **Fait le 2026-08-15** (étape 7) : `GetNetworkProfiles`, dans les DEUX API
> (`/mcu` et `/jsr309`), sans paramètre. Elle rend les quatre profils —
> disponibles ou non, l'absence étant elle-même une information — avec, pour
> chacun, l'adresse liée, l'adresse annoncée et le drapeau « par défaut ».
> Vérifiée par un appel XML-RPC réel sur un serveur démarré derrière NAT :
> `publicv4` y ressort avec `bind 172.21.105.71` et `announced 198.51.100.7`,
> c'est-à-dire la divergence que tout le modèle sert à décrire.

### 14.5 Conséquences techniques

**Le socle est livré.** `IPAddress` (adresse + zone) et `IPEndpoint` (adresse +
port + `sockaddr` prête pour les appels système) sont dans
`mcu/include/ipaddress.h` et `mcu/src/ipaddress.cpp`, avec 53 tests joués par
`make check`. Trois invariants : une adresse est vide **ou** valide (plus de
sentinelle `INADDR_ANY`/`INADDR_NONE`) ; le port n'est pas dans l'adresse ;
`::ffff:a.b.c.d` est dé-mappée à l'entrée et toute classification travaille sur
la forme dé-mappée.

**« Privé » et « interne » sont deux mots pour deux choses, et le produit dit
désormais lequel il emploie** (arbitrage du 2026-08-15) :

- **interne** = décision d'**exploitation**. C'est un côté du serveur, celui du
  réseau de service, choisi par celui qui déploie. C'est le vocabulaire de la
  ligne de commande (`--internal-ip`) et des profils (`internalv4`,
  `internalv6`) ;
- **privée** = **fait** sur l'adresse, au sens des standards : non routable sur
  l'Internet public. Aucun exploitant n'en décide.

Trois prédicats en découlent, et les confondre coûterait cher :

| Prédicat | Répond à | Sert à |
|---|---|---|
| `IsPrivate()` | l'adresse est-elle **non routable** sur l'Internet public ? (registre IANA « special-purpose », RFC 6890 : RFC 1918, CGNAT, loopback, link-local, **documentation**, benchmarking, réservé, ULA, site-local déprécié) | diagnostic, garde-fous, refus de configuration |
| `IsPrivateV4()` | l'adresse est-elle une **v4 privée au sens propre** (10/8, 172.16/12, 192.168/16, 100.64/10, 169.254/16) ? | **la politique de rattrapage NAT** — c'est l'ancienne `IsRFC1918` |
| `IsUniqueLocalV6()` | l'adresse est-elle une **ULA** (`fc00::/7`, RFC 4193) ? | diagnostic et journal — **pas** un contrôle : un profil interne v6 peut légitimement être en unicast global (§14.2) |

`IsPrivateV4()` est un **sous-ensemble strict** d'`IsPrivate()`, et c'est là le
piège : `192.0.2.1` est non routable (plage de documentation) et pourtant
nullement NATée. Faire porter le rattrapage par `IsPrivate()` ouvrirait le
latching sur des adresses qui n'en relèvent pas — et sur les ULA, où il n'y a de
toute façon pas de NAT. `RTPSession::NatCorrectable` consulte donc
`IsPrivateV4()`, jamais `IsPrivate()`.

Le **multicast est exclu** d'`IsPrivate()` : il est routable, simplement pas
unicast. C'est `IsMulticast()` qui répond à cette question-là.

**Bind par adresse, et non plus `INADDR_ANY`.** C'est le vrai coût technique du
modèle : « utiliser la bonne interface » impose de lier la socket média à
l'adresse de bind du profil. Conséquences à assumer :

- la socket devient **mono-famille** (une adresse v4 ou une adresse v6), donc
  une session est v4 **ou** v6 — ce qui est de toute façon ce que le contrôleur
  vient de demander ;
- la plage de ports RTP est aujourd'hui globale ; elle devient **par adresse de
  bind**, ce qui réduit les collisions plutôt que l'inverse ;
- le mode par défaut (aucun profil demandé, aucun `--internal-ip`) reste le bind
  dual-stack `::` avec `IPV6_V6ONLY=0` : un déploiement qui ne configure rien ne
  voit aucune différence ;
- les **plans de contrôle** (XML-RPC/Abyss, WebSocket, RTMP) restent en écoute
  dual-stack `::` : eux doivent tout entendre, la sélection par profil ne les
  concerne pas.

**STUN et ICE portent l'adresse ANNONCÉE, pas l'adresse de bind.** Derrière NAT,
`XOR-MAPPED-ADDRESS` et les candidats ICE doivent porter l'adresse que le pair
peut joindre. C'est vrai dès aujourd'hui, mais le modèle rend l'erreur facile :
il faudra vérifier `mcu/src/stunmessage.cpp` et
`Endpoint::GetMediaCandidates` sur ce point précis à l'étape 9.

**`--websocket-host` reste une source d'adresse concurrente** (§3.2) : elle
alimente l'URL des candidats WebSocket. À terme elle devrait devenir l'adresse
annoncée d'un profil, sans quoi on aura reconstitué exactement la duplication
que `SetAnnouncedIp` avait supprimée.

### 14.6 Ce qui reste à trancher

1. ~~**Le défaut `publicv4` sur un hôte v6-only.**~~ **Tranché** :
   `--default-profile` est retenue, voir §14.2.
2. ~~**Nommage exposé.**~~ **Tranché** : `--internal-ip` et `internalv4` /
   `internalv6` d'un côté, `IsPrivate()` au sens des standards de l'autre. Voir
   §14.5. L'option historique `--public-ip` ne bouge pas — d'où la paire
   asymétrique `public`/`internal` plutôt qu'`external`/`internal` : renommer
   une option existante casserait tous les déploiements pour un gain cosmétique.
3. **Plusieurs adresses d'une même famille et d'une même portée** (deux cartes
   sur le même réseau externe) : hors modèle, volontairement. Si le besoin
   apparaît, c'est une liste par profil, pas un cinquième profil.
4. **Tests.** La suite `:ipv6` du §12 ne couvre pas les profils : il faudra une
   suite `IPv6Profile*` (résolution CLI, refus au démarrage, sélection par appel,
   échec si indisponible, introspection).
