# WSEndpoint en mode client : le mediaserver joue le navigateur

> Statut : **lots 0 à 4 faits** — les coutures (§6), le masquage selon le
> rôle (§4.3), l'ouverture cliente en clair (§4.1, §4.2, §4.6), son pilotage
> par le réacteur depuis une URL (§4.5) et le transport TLS client (§4.4).
> Branche : `feat/wss-client`.
>
> Le serveur média ne parle pas SIP. La signalisation et le SDP sont tenus par
> un contrôleur externe (elixip), qui pilote le serveur en XML-RPC.
> « Contrôleur » désigne cet appelant dans tout le document.

## 1. Objectif

Permettre au mediaserver de tenir la jambe texte T.140 **d'un navigateur
WebRTC** : au lieu d'attendre qu'un navigateur se connecte à son serveur
WebSocket, il **se connecte lui-même** à une URL `ws://` ou `wss://` que le
contrôleur lui donne.

Le besoin vient d'elixip : pour tester un appel « total conversation » de bout
en bout, il faut un pair qui se comporte comme un navigateur. Aujourd'hui ce
pair doit être un vrai navigateur, ce qui interdit toute recette automatisée.

Périmètre retenu (arbitrage du 2026-09-20) :

| Sujet | Dedans | Dehors |
|---|---|---|
| Jambe texte | `WSEndpoint` (API JSR-309) | participant de conférence (API MCU) |
| Schémas | `ws://` et `wss://` | proxy HTTP, `permessage-deflate` |
| Média | texte T.140 | audio et vidéo, qui restent en RTP |
| Pilotage | une méthode XML-RPC nouvelle, explicite | surcharge du paramètre `token` |

Le client de test autonome (un faux navigateur sans jambe RTP) est écarté : il
ne servirait aucun appel réel.

Normes : RFC 6455 (WebSocket), RFC 7230 §6.7 (Upgrade), ITU-T T.140,
RFC 4103 (T.140 sur RTP, avec redondance).

## 2. Le fait structurant : le mode client n'ajoute que deux choses

La pile WebSocket du serveur est déjà écrite pour ce chantier, sans l'avoir
prévu. `WebSocketConnection` est une **machine à état passive** : elle ne
possède ni thread ni `poll()`, le réacteur unique de `WebSocketServer`
(`mcu/src/websocketserver.cpp:137`) l'interroge par `GetPollEvents()`,
`OnReadable()`, `OnWritable()` et `IsFinished()`. Le transport (clair ou TLS)
est déjà une interface (`mcu/include/websockettransport.h`). Et `WSEndpoint`
est déjà un `WebSocket::Listener` qui reçoit sa connexion de l'extérieur : il
ne sait pas, et n'a pas besoin de savoir, qui a ouvert la socket.

Entre un serveur WebSocket et un client WebSocket, il ne reste donc que deux
différences réelles :

1. **Avant le 101**, le client ouvre la socket, envoie la requête `GET …
   Upgrade` et vérifie la réponse — là où le serveur reçoit la requête et
   répond 101 (`mcu/src/websocketconnection.cpp:604`).
2. **Après le 101**, le client **masque** toutes ses trames sortantes
   (RFC 6455 §5.3) — le serveur n'en masque aucune.

Tout le reste — parseur de trames, découpage, ping/pong, fermeture, file de
sortie, réveil inter-thread — est commun et déjà éprouvé.

D'où la forme de la conception : on **ajoute une phase** à la connexion
existante, et on **fait vivre les connexions sortantes dans le réacteur
existant**. On n'écrit pas de seconde pile.

## 3. Ce qui existe (inventaire vérifié)

| Élément | Emplacement | État |
|---|---|---|
| Réacteur mono-thread (`poll` unique, map possédante) | `mcu/src/websocketserver.cpp:137` | réutilisé tel quel |
| Connexion passive, parseur de trames, ping/pong | `mcu/src/websocketconnection.cpp` | à étendre |
| Transport clair | `mcu/include/websockettransport.h:68` | réutilisé tel quel |
| Transport TLS (BIO mémoire, non bloquant) | `mcu/src/websockettransport.cpp` | serveur **et** client (deux `SSL_CTX`, un par rôle) |
| Parseur HTTP, mode réponse | `mcu/include/httpparser.h:174` | présent, accesseur de code manquant |
| Parseur d'URL | `mcu/include/httpparser.h:327` (`http_parser_parse_url`) | présent, inutilisé |
| Résolution DNS A + AAAA | `mcu/include/ipaddress.h:147` (`IPAddress::Resolve`) | réutilisé tel quel |
| Jambe texte, file d'attente, U+FFFD, RED | `mcu/src/jsr309/WSEndpoint.cpp` | réutilisé, un ajout |
| Événements de jambe vers le contrôleur | `mcu/src/jsr309/JSR309Event.h:30` | réutilisé (valeurs 6 et 7) |
| Test d'intégration en-processus du serveur | `mcu/tests/test_websocket_echo.cpp` | modèle du test client |
| Certificat auto-signé jetable pour les tests | `mcu/tests/dtlsfixture.h` | modèle du fixture TLS |

## 4. Conception

### 4.1 États de la connexion cliente

```
Connecting     connect() non bloquant en cours   → attend POLLOUT
TlsHandshake   (wss) SSL_connect                 → attend POLLIN/POLLOUT
Upgrading      GET Upgrade envoyé                → attend la réponse
Open           101 vérifié, onOpen émis          → identique au mode serveur
Failed/Closed  onError puis onClose
```

Une seule classe, `WebSocketConnection`, porte les deux rôles. Un booléen
`client` décide du parseur (`HTTP_RESPONSE` au lieu de `HTTP_REQUEST`,
`mcu/src/websocketconnection.cpp:85`), du masquage, et de la phase d'ouverture.
Le listener est fourni **à la création** en mode client : il n'y a pas
d'`Accept()`, puisque personne ne demande rien.

L'entrée est `WebSocketConnection::InitClient(fd, transport, host, path, wsl)`.
Le socket qu'elle reçoit est **déjà en cours de connexion** : `connect()` non
bloquant appartient à l'appelant (le réacteur, au lot 3). La connexion demande
alors POLLOUT, lit `SO_ERROR` — POLLOUT arrive que le `connect()` ait réussi ou
échoué — puis émet sa requête dès que le transport est prêt.

`SO_ERROR` se lit **avant** toute écriture : sinon c'est l'échec du `write` qui
parlerait, sans dire pourquoi. Et POLLOUT n'est demandé que dans `Connecting` :
un socket établi est toujours *writable*, donc le réclamer pendant le handshake
TLS ferait tourner le réacteur à vide. Au-delà, POLLOUT se demande comme pour
toute autre connexion — quand il y a quelque chose à écrire, le transport
compris.

### 4.2 La poignée de main cliente

Requête émise :

```
GET <path>[?<query>] HTTP/1.1
Host: <hôte>[:<port>]
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <16 octets aléatoires, base64>
Sec-WebSocket-Version: 13
```

La clé vient de `RAND_bytes` (OpenSSL est déjà lié). `HTTPRequest` n'a pas de
`Serialize()` — `HTTPResponse` seul en a un (`mcu/include/http.h:114`). On en
ajoute un, calqué sur l'existant.

Réponse acceptée si, et seulement si : code `101`, `Upgrade: websocket`,
`Connection: Upgrade`, et `Sec-WebSocket-Accept` égal à
`base64(SHA1(clé + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`. Ce calcul existe
déjà côté serveur (`websocketconnection.cpp:604`) : on le factorise en une
fonction, on ne l'écrit pas deux fois. Tout autre code (401, 404, 500…) est un
échec propre, journalisé avec le code.

### 4.3 Masquage des trames sortantes

Le rôle de la connexion (`WebSocketConnection::Role`, posé par `Init`) décide
seul du masquage. `Frame` prend un booléen `masked` et, en mode client, tire sa
clé au sort par trame (`RAND_bytes`).

- le XOR s'applique à la construction **et** dans `Append`, que le pong utilise
  pour recopier le corps du ping au rythme où il arrive ;
- l'offset du masque compte depuis le début du payload de **cette** trame :
  `SendMessage(BYTE*,DWORD)` découpe en fragments de 1300 octets avec
  continuation, et chaque fragment porte son propre masque, qui repart de zéro ;
- une clé nulle est écartée : `WebSocketFrameHeader` ne pose le bit MASK que si
  la clé est non nulle, et une trame cliente non masquée fait fermer le pair.

Durcissement symétrique : un client qui reçoit une trame masquée ferme
(RFC 6455 §5.1). Ce versant est exercé par le lot 2, qui apporte la connexion
cliente ouverte dont il avait besoin
(`WsClientHandshake.UneTrameMasqueeDuServeurFermeLaConnexion`).

### 4.4 Transport TLS client

Le transport TLS était serveur : `SSL_set_accept_state` et `SSL_accept`, sur un
contexte unique créé par `ClassInit`. La même classe porte les deux rôles, et la
façade gagne une entrée cliente :

```c++
static void WebSocketTlsTransport::SetClientConfig(bool verifyPeer, const std::string& cafile);
static bool WebSocketTlsTransport::GetClientVerifyPeer();
static std::unique_ptr<WebSocketTransport> WebSocketTlsTransport::CreateClient(
        const std::string& host, bool verifyPeer);
```

`CreateClient` crée à la demande un **second** `SSL_CTX` (`TLS_client_method`,
TLS 1.2 minimum, `SSL_CTX_set_default_verify_paths`, plus l'autorité de
`SetClientConfig` s'il y en a une). Ce contexte est créé depuis le thread
appelant, donc sous mutex. `SetClientConfig` le **défait** : le magasin
d'autorités est porté par le contexte, et `SSL_CTX_free` ne libère rien tant
qu'un `SSL` le référence.

La vérification se pose par connexion, avant `SSL_connect` :
`SSL_set_tlsext_host_name` (SNI), `X509_VERIFY_PARAM_set1_host` et
`SSL_set_verify(SSL_VERIFY_PEER)` — sans ce dernier la vérification serait
décorative, le handshake réussissant quand même. `verifyPeer=false` sert les
certificats auto-signés de laboratoire, et rien d'autre.

Une **adresse littérale n'est pas un nom** : elle ne se met pas dans un SNI
(RFC 6066 §3) et se compare aux SAN de type `iPAddress`, par
`X509_VERIFY_PARAM_set1_ip_asc`. Confondre les deux ferait échouer
`wss://127.0.0.1/` quoi qu'il arrive, et l'exploitant désarmerait la
vérification pour de mauvaises raisons.

Deux pièges, et ils se paient comptant :

1. **Le ClientHello doit partir tout seul.** Le handshake ne progressait que
   depuis `Recv` : rien n'arrive encore sur ce socket, donc rien n'appellerait
   `Recv`. C'est `Flush()` qui lance le handshake d'une connexion cliente — la
   première écriture possible est le POLLOUT qui clôt le `connect()`. Le faire
   depuis `Init()` marcherait aussi, mais écrirait dans un socket encore en
   cours de connexion, et un échec y serait sans voie de sortie : `Init()` est
   appelé hors du réacteur et sa valeur de retour n'est lue par personne.
2. **`Send` avant la fin du handshake jette les octets** (il renvoie 0 sans
   conserver le tampon). La requête d'upgrade serait perdue en silence. D'où
   `virtual bool IsReady()` au contrat du transport (toujours vrai en clair) :
   la connexion n'émet sa requête qu'une fois le transport prêt, et un `Send`
   court est traité comme un échec.

Un transport dont le handshake a échoué est **condamné** (`broken`) : toute E/S
ultérieure rend `-1`, donc le réacteur le ferme et le listener l'apprend. Sans
cela un certificat refusé laisserait une jambe muette.

### 4.5 Où vit la connexion sortante

Dans la map du réacteur existant, avec les connexions entrantes. `Connect` est
appelé depuis le thread XML-RPC ; la map, elle, reste la **propriété exclusive
du thread réacteur** (invariant écrit en `websocketserver.h`). D'où :

```c++
bool WebSocketServer::Connect(const std::string& url,
                              std::weak_ptr<WebSocket::Listener> listener);
```

1. Le thread appelant **parse l'URL** (`http_parser_parse_url`), **résout le
   DNS** (`IPAddress::Resolve`, A et AAAA), **crée la socket** et lance le
   `connect()` non bloquant. La résolution ne doit jamais avoir lieu dans le
   réacteur : un DNS lent y gèlerait **toutes** les jambes WS du serveur — le
   même piège que le callback bloquant du réacteur RTP
   (`docs/reference/threads-rtp.md`).
2. Il pousse le descripteur dans une file protégée par mutex, puis
   `wait.Signal()`.
3. Le réacteur **adopte** le socket : il crée la connexion, l'initialise par
   `InitClient` et l'insère dans la map. Elle est alors pilotée comme les autres.

Le socket est ouvert par l'appelant, et non par le réacteur : c'est ce qui rend
**tout échec synchrone**. URL illisible, schéma inconnu, hôte introuvable,
famille indisponible — `Connect` rend `false` et n'a notifié personne, ce qui
est exact : il n'y a pas de jambe. Le seul échec asynchrone restant est celui
que le `connect()` ne peut pas voir tout de suite (le pair n'écoute pas), et
celui-là se dit au listener (§4.6). Le réacteur n'a ainsi jamais à notifier un
`WebSocket::Listener` avec un `WebSocket*` nul.

Un littéral IPv6 dans l'URL s'écrit entre crochets (RFC 3986 §3.2.2) ; le
parseur d'URL les retire, l'en-tête `Host` les remet. Le port par défaut est 80
en `ws://`, 443 en `wss://`. Le fragment (`#…`) n'est jamais émis (RFC 3986
§3.5).

Le **schéma décide du transport, et lui seul** : le mode sécurisé du serveur
(`SetSecure`) ne concerne que ses connexions entrantes. Le transport est
construit dans le thread appelant, avant même le socket — un contexte TLS
inutilisable est alors un échec synchrone de plus, et non une jambe muette. Il
traverse la file jusqu'au réacteur, qui n'a toujours qu'à adopter.

### 4.6 L'échec doit se voir

`NotifyClose()` (`websocketconnection.cpp:243`) n'émet `onClose` **que si la
connexion a été upgradée**. Une connexion cliente qui échoue avant le 101 —
refus TCP, DNS mort, certificat invalide, 404 — ne notifierait donc personne :
le `WSEndpoint` attendrait indéfiniment un pair qui ne viendra pas, sans une
ligne pour le dire. En mode client, un échec avant l'ouverture émet `onError`
puis `onClose`, et journalise la cause.

### 4.7 `WSEndpoint` en mode client

Ajout minimal : `int Connect(const std::string& url)`, qui mémorise l'URL et
demande la connexion. Tout le reste est déjà en place :

- `onOpen` (`WSEndpoint.cpp:36`) associe le WebSocket et **rejoue la file
  d'attente** — exactement ce qu'il faut ici, le texte RTP pouvant arriver
  avant que la connexion sortante n'aboutisse ;
- `onMessageEnd` encapsule en T.140, avec redondance RFC 4103 si négociée ;
- `onClose` envoie l'U+FFFD au pair RTP (T.140 §5.3).

Deux décisions propres au mode client :

**Reconnexion.** Un navigateur ne reconnecte pas ; une jambe pilotée, si — le
contrôleur peut configurer la jambe avant que le pair n'écoute. Politique
proposée : 5 tentatives, backoff 1 s → 2 s → 4 s → 8 s → 8 s, tant que le port
n'a pas été terminé. Pendant les tentatives, **ne pas** envoyer l'U+FFFD au
pair RTP : une coupure d'une seconde insérerait sinon un caractère de perte à
chaque fois. L'U+FFFD part à l'abandon définitif, avec l'événement.

**Ce que le serveur publie.** En mode client, il n'y a pas d'URL locale.
`GetLocalMediaPort`/`GetLocalMediaHost` (`Endpoint.cpp:762` et `:778`) et
`GetMediaCandidates` ne doivent **pas** rendre l'adresse d'écoute : elle serait
fausse, et c'est précisément la duplication d'adresse que
`NETWORK-CONFIGURATION.md` interdit. Ils rendent l'URL distante configurée, ou
une erreur explicite.

### 4.8 API de contrôle

Une méthode nouvelle sur `/jsr309` :

```
ConnectMediaConnection(sessionId, endpointId, media, role, url) -> 1 | faute
```

Elle fait **tout** : elle bascule le port en `WS` (comme
`ConfigureMediaConnection`) puis lance la connexion. Elle n'enregistre **aucun
token** : personne n'entrera par le serveur pour cette jambe. C'est aussi
pourquoi elle ne réutilise pas `ConfigureMediaConnection`, qui exige un token
non vide pour `WS` (`MediaSession.cpp:2069`) — un token sans objet ici.

Le résultat est **asynchrone** : la connexion aboutit après le retour XML-RPC.
Le contrôleur l'apprend par la file d'événements JSR-309, avec les valeurs qui
existent déjà (`JSR309Event.h:31`) : `EndpointConnectedEvent` (7) à
l'ouverture, `EndpointDisconnectedEvent` (6) à l'abandon. Elles portent déjà
`(joinableId, media, role)`, ce qui suffit. À confirmer avec elixip : leur
sémantique actuelle est « DTLS OK + premier RTP reçu ».

Deux options de ligne de commande, à ajouter aux **deux** tableaux du
`README.md`. Le transport les attend déjà (§4.4) : il ne reste à `main()` qu'un
appel à `WebSocketTlsTransport::SetClientConfig`, avant la première connexion
sortante.

| Option | Défaut | Rôle |
|---|---|---|
| `--websocket-client-insecure` | *(absente)* | ne pas vérifier le certificat du serveur distant |
| `--websocket-client-ca <fichier>` | *(aucun)* | autorité de certification supplémentaire |

**Obligation normative** (CLAUDE.md) : toute modification de l'API XML-RPC
`/jsr309` met à jour, dans le même jeu de changements, les schémas protobuf
MOTELI v2 du dépôt elixip (`apps/elixip2/priv/proto/moteli_*.proto`).

## 5. Pièges relevés dans le code actuel

Ils sont tous vérifiés, et chacun est une panne silencieuse s'il est manqué.

1. **Le reliquat après le 101.** `ProcessData` ignorait la valeur de retour de
   `HTTPParser::Execute`. Côté serveur c'est sans conséquence : le client
   attend le 101 avant d'écrire. Côté client, **le serveur peut coller sa
   première trame T.140 au 101 dans le même segment TCP** — et cette trame
   serait perdue. Le reliquat passe au chemin « trames », pour les deux rôles —
   tenu au lot 2.
2. **`Send` avant handshake TLS jette les octets** (§4.4) : la connexion n'émet
   sa requête qu'une fois `IsReady()` vrai — tenu au lot 2, exercé au lot 4.
3. **Le ClientHello ne part pas tout seul** (§4.4) — tenu au lot 4 : c'est
   `Flush()` qui lance le handshake d'une connexion cliente.
4. **L'échec avant upgrade ne notifie personne** (§4.6) — tenu au lot 2 :
   `NotifyClose` notifie toujours une connexion cliente, et `onError` précède
   `onClose` quelle que soit la voie de l'échec.
5. **Le masque repart de zéro à chaque fragment** (§4.3) — tenu au lot 1.
6. **Le pong doit être masqué** lui aussi : il passe par `Append` — tenu au
   lot 1.
7. **DNS dans le réacteur = toutes les jambes gelées** (§4.5) — tenu au lot 3 :
   l'URL, le DNS et le `connect()` sont au thread appelant, le réacteur ne
   reçoit qu'un descripteur.
8. **`IPAddress::Resolve` écarte ce qui n'est pas ANNONÇABLE** — loopback,
   link-local, multicast (`ipaddress.h`). C'est la politique de l'adresse qu'on
   **publie** dans un SDP, appliquée ici à une **destination**. Un littéral
   court-circuite le filtre, donc `ws://127.0.0.1:9090/` et `ws://[::1]:9090/`
   marchent ; un **nom** qui ne se résout qu'en loopback, lui, échoue :
   `ws://localhost:9090/` rend « cannot resolve localhost (errno 2) ». L'échec
   est bruyant, jamais silencieux. En appel réel l'URL vient de
   `GetMediaCandidates`, donc annonçable par construction — mais la recette §7
   se fait en loopback : y écrire l'adresse, pas le nom. `RTPSession::SetRemoteHost`
   a exactement le même travers. Cf. §9.4.
9. **`EnsureRequest`** construit un `HTTPRequest` à partir de
   `parser->GetMethodStr()` : sans objet pour une réponse. Le mode client
   accumule ses en-têtes ailleurs — dans une map à clefs minuscules, la casse
   d'un en-tête HTTP étant libre — et lit le code de statut par
   `HTTPParser::GetStatusCode` (lot 0). Tenu au lot 2.

## 6. Lots

Chaque lot compile, passe `cd mcu && make check`, et se livre seul.

| Lot | Contenu | Recette |
|---|---|---|
| 0 | `HTTPParser::GetStatusCode`, `HTTPRequest::Serialize`, `WebSocketTransport::IsReady`, calcul `Sec-WebSocket-Accept` factorisé | aucun changement de comportement ; `make check` inchangé |
| 1 | Masquage client dans `Frame` + refus des trames masquées reçues côté client | test unitaire dans `test_websocket_frame.cpp` |
| 2 | Mode client de `WebSocketConnection` en clair : connect, upgrade, 101, reliquat, `onError` | test d'intégration en-processus contre `TextEchoWebsocketHandler` |
| 3 ✔ | `WebSocketServer::Connect` : file de demandes, URL, DNS hors réacteur, IPv4 et IPv6 | `tests/test_ws_client_connect.cpp` : `127.0.0.1` **et** `[::1]`, chemin + query, port fermé, URL inutilisable |
| 4 ✔ | Transport TLS client | `tests/test_ws_client_tls.cpp` : ouverture `wss://` et écho, vérification refusée **et** acceptée (autorité jetable de `tests/wstlsfixture.h`), autorité illisible |
| 5 | `WSEndpoint::Connect`, reconnexion bornée, U+FFFD, `GetMediaCandidates` | test de pontage RTP ↔ WS sortant |
| 6 | XML-RPC `ConnectMediaConnection`, événements, `docs/JSR-309-API.md`, `README.md`, protobuf MOTELI côté elixip | appel XML-RPC réel |
| 7 | Recette de bout en bout | §7 |

Les lots 0 à 4 ne touchent pas au JSR-309 : ils sont utiles seuls, et sans
risque pour l'existant.

## 7. Recette de bout en bout

Un seul mediaserver suffit : il tient les deux bouts.

1. elixip monte deux jambes JSR-309 : la jambe A garde son texte WS en mode
   serveur (URL publiée), la jambe B reçoit `ConnectMediaConnection` avec
   l'URL de A.
2. Vérifier l'événement `EndpointConnectedEvent` sur la jambe B.
3. Taper du texte des deux côtés ; vérifier qu'il arrive dans les deux sens,
   caractère par caractère, sans doublon ni perte.
4. Couper le serveur A ; vérifier la reconnexion, puis l'U+FFFD et
   `EndpointDisconnectedEvent` à l'abandon.
5. Refaire en `wss://`, avec puis sans `--websocket-client-insecure`.
6. Refaire avec une jambe RTP T.140 RED en face de la jambe cliente : c'est le
   cas réel d'un appel navigateur ↔ terminal SIP.

## 8. Ce que le chantier ne fait pas

- Le participant de conférence (`ParticipantTextWS`) reste serveur seulement.
- L'audio et la vidéo restent en RTP : « jouer le navigateur » ne concerne ici
  que le canal texte.
- Pas de sous-protocole (`Sec-WebSocket-Protocol`), pas de compression, pas de
  proxy HTTP, pas de cookie ni d'en-tête d'authentification. Si le contrôleur
  en a besoin, c'est un lot à part.
- Le data channel WebRTC (`docs/conception/T140-DC`) est l'autre réponse au
  même besoin ; les deux cohabitent sans se gêner.

## 9. À trancher avant le lot 5

1. **Reconnexion** : la politique du §4.7 (5 essais, backoff borné) est une
   proposition. Un « aucune reconnexion » est défendable si elixip préfère
   piloter lui-même la reprise.
2. **Événements** : réutiliser les valeurs 6 et 7, ou en ajouter une (8) propre
   à la jambe WS cliente. Réutiliser évite d'élargir un contrat de fil partagé
   avec elixip et les clients Java, mais élargit la sémantique des valeurs
   existantes.
3. **Erreur d'authentification** : si le serveur distant répond 401 ou 403,
   faut-il retenter ? La proposition est non — une erreur d'autorisation ne se
   résout pas par la répétition.
4. **Délai d'abandon d'une ouverture** : une jambe sortante dont le pair
   **accepte la connexion TCP puis se tait** reste ouverte indéfiniment, sans un
   événement pour le dire. Trois cas, tous plausibles : un `wss://` pointé sur
   un port en clair (le pair attend une requête HTTP, nous attendons un
   ServerHello), un pair qui n'écrit jamais sa réponse 101, un trou noir réseau.
   Le réacteur appelle `poll()` sans délai (`-1`) : il n'a aujourd'hui aucune
   notion d'échéance. Ce n'est pas propre à TLS — le lot 3 avait déjà ce trou,
   le lot 4 l'élargit. Un délai d'ouverture est la moitié manquante de la
   politique de reconnexion du §4.7, donc à trancher **avec** elle, au lot 5.
5. **Nom d'hôte en loopback** (piège 8) : faut-il que `IPAddress::Resolve`
   sache résoudre une **destination** — filtre « annonçable » désactivé — ou
   laisse-t-on `ws://localhost/` échouer ? La proposition est d'ouvrir le
   filtre par un paramètre, ce qui réparerait du même coup
   `RTPSession::SetRemoteHost`. Hors périmètre du lot 3 : il ne touche pas à
   une classe partagée sans arbitrage.
