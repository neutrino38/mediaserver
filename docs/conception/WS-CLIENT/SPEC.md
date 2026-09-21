# WSEndpoint en mode client : le mediaserver joue le navigateur

> Statut : **lots 0 à 5 faits** — les coutures (§6), le masquage selon le
> rôle (§4.3), l'ouverture cliente en clair (§4.1, §4.2, §4.6), son pilotage
> par le réacteur depuis une URL (§4.5), le transport TLS client (§4.4), puis la
> jambe JSR-309 elle-même : `WSEndpoint::Connect`, la reprise, l'U+FFFD et ce
> que la jambe publie (§4.7). Les quatre points du §9 sont **tranchés**.
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
   DNS** (`IPAddress::Resolve` en `Usage::Destination`, A et AAAA, §9.5),
   **crée la socket** et lance le
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
demande la connexion. Il ne rend `0` que pour une URL **inutilisable** — schéma
inconnu, hôte absent, URL illisible : ce qui ne se réparera pas en attendant.
Tout le reste est asynchrone, donc arme la reprise et rend `1`. C'est la seule
distinction qui compte ici, et `WebSocketServer::ParseWsUrl` la porte : sans
elle, une faute de frappe du contrôleur ferait boucler le serveur pour toujours.

Le point d'entrée d'une jambe est `Endpoint::ConnectMediaConnection(media, role,
url)` : elle bascule le port en `WS` — sans token, personne n'entrera par notre
serveur pour cette jambe — puis appelle `Connect`. Le texte seul y est admis.

Le rythme de la reprise n'est pas porté par la jambe : elle pose une échéance
dans `WebSocketServer::ConnectLater`, servie par **un thread unique pour tout le
binaire**. Ce thread n'est pas le réacteur, et c'est tout l'objet : une tentative
refait un DNS et un `connect()`, qui y gèleraient toutes les jambes WebSocket
(piège 7). Le réacteur, lui, n'adopte jamais qu'un descripteur.

Tout le reste est déjà en place :

- `onOpen` (`WSEndpoint.cpp:36`) associe le WebSocket et **rejoue la file
  d'attente** — exactement ce qu'il faut ici, le texte RTP pouvant arriver
  avant que la connexion sortante n'aboutisse ;
- `onMessageEnd` encapsule en T.140, avec redondance RFC 4103 si négociée ;
- `onClose` envoie l'U+FFFD au pair RTP (T.140 §5.3).

Deux décisions propres au mode client :

**Reconnexion (arbitrage du 2026-09-21).** Un navigateur ne reconnecte pas ; une
jambe pilotée, si — le contrôleur peut configurer la jambe avant que le pair
n'écoute. La politique est : **toutes les 5 s, sans limite**, tant que la jambe
vit. Deux choses seulement l'arrêtent, et ce sont les deux formes d'une même :
`WSEndpoint::End()`, et la destruction de la jambe — le serveur ne garde qu'un
`weak_ptr` sur son listener, dont l'expiration fait mourir la demande de reprise.
Un 401 ou un 403 n'est pas un cas à part : une seule règle, aucun état
« terminé » à écrire.

Chaque coupure est une **perte annoncée** : U+FFFD vers le pair RTP quand la
connexion tombe (`onClose`), U+FFFD vers le pair WebSocket à la reconnexion. Le
texte arrivé pendant la coupure est **perdu**, jamais rejoué — le garder
afficherait, après le caractère de perte, un texte que l'autre bout croit déjà
perdu. La file d'attente ne sert donc qu'**avant la première ouverture**, là où
elle porte la phrase de présentation de l'appelant. Une **tentative**
infructueuse, elle, n'annonce rien : elle ne change pas ce que les deux pairs
savent déjà.

**Ce que le serveur publie.** En mode client, il n'y a pas d'URL locale.
`GetLocalMediaPort`/`GetLocalMediaHost` (`Endpoint.cpp:762` et `:778`) ne
rendent **pas** l'adresse d'écoute : elle serait fausse, et c'est précisément la
duplication d'adresse que `NETWORK-CONFIGURATION.md` interdit. Elles échouent
explicitement (`-1`, `NULL`), et `GetMediaCandidates` rend l'**URL distante
configurée** — la cible, pas notre écoute.

### 4.7 bis Délai d'abandon d'une ouverture

Une ouverture cliente a une **échéance : 10 s** (arbitrage du 2026-09-21). Un pair
qui accepte le TCP puis se tait — un `wss://` pointé sur un port en clair, un 101
jamais écrit, un trou noir réseau — ne produira jamais l'événement qui
réveillerait le réacteur : sans échéance, la jambe reste ouverte pour toujours et
la reprise ne part jamais.

Le réacteur appelait `poll(-1)`. Il prend désormais le délai restant le plus
court parmi ses connexions clientes **non encore ouvertes**
(`WebSocketConnection::GetOpeningTimeLeft`, `-1` pour tout le reste, donc
`poll(-1)` quand il n'y a aucune ouverture en cours). À l'échéance, l'ouverture
échoue exactement comme un refus TCP : `onError` puis `onClose`, et la reprise
repart. Le délai est **réglable** (`SetOpeningTimeout`) pour que les tests
n'attendent pas dix secondes ; rien ne l'expose en ligne de commande.

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
existent déjà (`JSR309Event.h:31`, arbitrage du 2026-09-21) :
`EndpointConnectedEvent` (7) à **chaque** ouverture, `EndpointDisconnectedEvent`
(6) à **chaque** perte d'une connexion établie. Elles portent déjà
`(joinableId, media, role)`, ce qui suffit, et n'élargir aucun contrat de fil
partagé avec elixip et les clients Java vaut mieux qu'une huitième valeur. Leur
sémantique s'élargit en revanche : elle était « DTLS OK + premier RTP reçu ».
À dire à elixip.

Une **tentative** infructueuse ne publie rien : la reprise étant indéfinie, elle
inonderait la file d'un couple 6/7 toutes les 5 s pour un pair qui n'écoute pas.

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
8. **`IPAddress::Resolve` appliquait à une destination la politique de
   l'adresse annoncée** — loopback, link-local et multicast écartés par
   `IsAnnounceable()`. Un littéral court-circuitant le filtre,
   `ws://127.0.0.1:9090/` marchait quand `ws://localhost:9090/` rendait « cannot
   resolve localhost (errno 2) » : deux écritures du même serveur, une seule
   acceptée. **Réglé** (§9.5) : `Resolve` prend un `Usage`, et
   `WebSocketServer::Connect` demande `IPAddress::Destination`, dont le prédicat
   est `IsUnicastDestination()` — la loopback passe, le multicast non. Le filtre
   ne concerne toujours que les **noms** : un littéral est rendu tel quel.
9. **La file d'attente était toujours jetée comme périmée** — relevé au lot 5,
   et il préexistait au chantier. `SendFrame` horodatait chaque trame en attente
   par `getDifTime(&clock)`, et `onOpen` remet `clock` à zéro à la **première
   association** : l'âge se mesurait donc depuis un instant postérieur au
   stockage, en arithmétique non signée. Toute la file sortait « stale », c'est-à-
   dire la première phrase de **chaque** appel — celle que cette file existe pour
   sauver. Elle porte désormais une date absolue (`getTimeMS`). Au passage,
   `clock` n'était initialisé que dans `onOpen` alors que `SendFrame` le lit avant
   (chemin BOM) : un `timeval` non initialisé.
10. **`EnsureRequest`** construit un `HTTPRequest` à partir de
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
| 5 ✔ | `WSEndpoint::Connect`, `Endpoint::ConnectMediaConnection`, reprise indéfinie, U+FFFD, `GetMediaCandidates`, délai d'ouverture | `tests/test_ws_client_endpoint.cpp` : pontage RTP ↔ WS sortant dans les deux sens, coupure annoncée et texte perdu, reprise arrêtée par `End()`, URL inutilisable, cible publiée au lieu de l'écoute, ouverture muette abandonnée |
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
4. Couper le serveur A ; vérifier `EndpointDisconnectedEvent` et l'U+FFFD reçu
   côté RTP, puis la reconnexion 5 s plus tard, son
   `EndpointConnectedEvent` et l'U+FFFD reçu côté WebSocket. Le texte tapé
   pendant la coupure ne doit **pas** réapparaître.
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

## 9. Arbitrages

Les cinq points ont été **tranchés le 2026-09-21** — les quatre premiers avant
le lot 5, le dernier après. Ils sont décrits là où ils s'appliquent ; ce qui
suit ne dit que la décision et ce qu'elle écarte.

1. **Reconnexion** : *toutes les 5 s, indéfiniment* (§4.7). Le backoff borné à
   5 essais, proposé plus haut dans l'histoire de ce document, est écarté : une
   jambe pilotée n'a pas de raison d'abandonner tant que le contrôleur ne l'a pas
   terminée. Chaque coupure annonce la perte (U+FFFD des deux côtés) et le texte
   de la coupure est perdu.
2. **Événements** : *réutiliser 6 et 7* (§4.8), à chaque cycle, plutôt qu'une
   huitième valeur. Le contrat de fil partagé avec elixip, les clients Java et
   les protobuf MOTELI ne s'élargit pas ; la sémantique des deux valeurs, elle,
   s'élargit — à dire à elixip.
3. **Erreur d'authentification** : *aucun cas particulier*. Un 401 ou un 403 est
   retenté comme tout le reste, ce qui découle d'une reprise indéfinie : une
   seule règle, et aucun état « terminé » à écrire dans la jambe.
4. **Délai d'abandon d'une ouverture** : *10 s, portés par le réacteur* (§4.7
   bis). Sans lui, la reprise ne partirait jamais dans les trois cas qui
   motivaient le point. Pas d'option de ligne de commande : la valeur n'est
   réglable que pour les tests.
5. **Nom d'hôte en loopback** (piège 8) — *distinguer les deux usages*, tranché
   le 2026-09-21. `IPAddress::Resolve` prend un quatrième paramètre qui dit
   **pourquoi** on résout :

   | `Usage` | Prédicat appliqué | Qui le demande |
   |---|---|---|
   | `Announce` *(défaut)* | `IsAnnounceable()` | `DetectAnnouncedIp`, `SetAnnouncedIp` |
   | `Destination` | `IsUnicastDestination()` | `WebSocketServer::Connect`, `StunClient::ParseServer` |

   Le défaut reste le filtre strict : un appelant qui ne dit rien ne peut pas
   publier une loopback par distraction, et les deux appelants d'annonce ne
   changent pas d'une ligne. `IsUnicastDestination()` existait déjà dans la même
   classe — `RTPSession::SetRemotePort` s'en sert pour valider la destination
   d'un flux RTP —, il n'y avait donc aucun prédicat à inventer.

   Ce que l'arbitrage écartait : rendre `Resolve` sans politique du tout, en
   confiant le filtre d'annonce à ses appelants. Plus pur, mais cela déplace le
   risque là où il coûte le plus cher — un futur appelant qui oublie de filtrer
   met une loopback dans une ligne `c=`.

   **Correction d'un fait faux de ce document** : `RTPSession::SetRemoteHost`
   n'existe pas, et rien n'a été « réparé du même coup ». La destination RTP se
   pose par `SetRemotePort`, qui n'appelle pas `Resolve` : elle n'accepte qu'un
   littéral (`IPAddress::Parse`), donc n'a jamais vu ce filtre. Lui faire
   accepter un nom serait un autre chantier, et le contrôleur ne le demande pas.

   `StunClient::ParseServer` portait le même défaut — un serveur STUN est une
   destination — et bascule avec : `--stun-server <nom qui ne mène qu'à la
   loopback>` était refusé alors que son littéral passait. La seule liberté qui
   reste à `Resolve` en `Destination` est le **littéral**, rendu tel quel sans
   politique : `ws://[ff02::1]/` est toujours accepté.
