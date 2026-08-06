# Texte temps réel sur WebSocket (JSR-309)

> Statut : **phases serveur 1-3 LIVRÉES** (2026-08-06), phases contrôleur 4-7 à
> écrire. Portage de la capacité « texte T.140 sur WebSocket » de la passerelle
> Java historique (`mediagw-b2bua`, `src/java/fr/ives/sbc/b2bua/media`) vers le
> couple **serveur média JSR-309 + contrôleur SIP elixip/kelixip**.
>
> Le plan média WebSocket est **vérifié bout en bout contre le binaire** :
> WebSocket accepté sur son token, texte WS → RTP T.140 et RTP → WS,
> U+FFFD à la fermeture, texte tamponné rejoué à la connexion, token inconnu
> refusé. Reste la moitié signalisation, côté contrôleur (§5).
>
> Le média serveur ne parle pas SIP : la signalisation et le SDP sont gérés par
> un **contrôleur SIP** externe (elixip), qui pilote le serveur par l'API
> XML-RPC JSR-309. « Contrôleur » désigne cet appelant dans tout le document.

## 1. Objectif

Permettre à un client WebRTC de tenir une conversation **texte temps réel**
(T.140, RFC 4103) dans un appel dont l'audio et la vidéo passent par WebRTC,
alors que son navigateur **ne sait pas transporter du T.140** : `m=text` sur un
profil RTP n'est pas implémenté par `RTCPeerConnection`. La solution historique,
spécifiée par IVeS et Omnitor et déployée en production, remplace le transport
RTP de cette seule `m=` par un **WebSocket** :

```
m=text 9091 TCP/WSS t140
a=setup:passive
a=connection:new
a=wss://192.168.5.5:9091/jsr309/17/9f0c4b2e-1a3d-4c77-8f10-2b5e6c9d0a11
```

Le navigateur ouvre ce WebSocket **directement vers le serveur média** et y
échange du texte UTF-8 brut ; le serveur média fait la conversion
T.140/RED ⇄ WebSocket et le pontage vers l'autre patte.

Périmètre : la patte WebSocket d'un endpoint JSR-309 (`m=text`, format `t140`),
son pontage vers une patte T.140 sur RTP (avec redondance RFC 4103) ou vers une
autre patte WebSocket. **Hors périmètre** : l'API conférence (MCU), le partage
de document, le DataChannel WebRTC (autre mécanisme, autre code).

## 2. Le fait structurant : la passerelle ne terminait pas le WebSocket

C'est le point à comprendre avant tout le reste, et il rend le portage bien plus
petit qu'attendu. `WebSocketLeg.java` (238 lignes) **ne contient aucune socket,
aucun thread, aucun octet de texte** : pas de `javax.websocket`, pas de
`@ServerEndpoint`, pas de servlet WS dans tout `mediagw-b2bua/src/java`. Son rôle
entier est de synthétiser du SDP et de faire **deux** appels XML-RPC.

Le plan média est déjà **entièrement** dans le serveur C++ :

```
navigateur ──WebSocket──► serveur média :9090 /jsr309/<sessionId>/<token>
                            │  WSEndpoint (WSEndpoint.cpp)
                            │    ├─ WS → RTP : RedCodec::Encode, Multiplex
                            │    └─ RTP → WS : RedCodec::Decode, SendMessage
                            ▼
                          autre patte (RTPEndpoint T.140, ou autre WSEndpoint)

contrôleur SIP ──XML-RPC──► serveur média :9090 /jsr309   (signalisation seule)
```

Séquence Java, `WebSocketLeg.configureMediaConnection`
(`mediagw-b2bua/.../WebSocketLeg.java:88-120`) :

```java
String token = UUID.randomUUID().toString();
client.ConfigureMediaConnection(sessionId, endpointId, TEXT, VIDEO_MAIN, WS, token, "t140");
wsaddr = client.GetMediaCandidates(sessionId, endpointId, WS, TEXT);
wsurl  = "//" + wsaddr[0].addr + ":" + wsaddr[0].port
       + "/jsr309/" + sessionId + "/" + token;
```

puis l'URL est publiée dans le SDP (`WebSocketLeg.java:123-142`). Le `token` est
la **seule** association entre une URL et le quadruplet
`(endpointId, media, role, protocol)` — il est enregistré côté serveur par
`ConfigureMediaConnection` (`MediaSession.cpp:2024-2039`) et retrouvé à
l'ouverture du WebSocket (`MediaSession.cpp:2047-2073`).

Conséquence de conception : **le portage est un travail de contrôleur**, plus
cinq correctifs serveur circonscrits (§4).

## 3. Ce qui existe déjà côté serveur C++ (inventaire)

| Élément | Emplacement | État |
|---|---|---|
| Serveur WebSocket, port unique | `mcu/src/main.cpp:344`, `:507`, défaut 9090 `:146`, `--websocket-port` `:218` | OK |
| TLS **sur le même port** | `--websocket-secure/-cert/-key` `main.cpp:162-165`, `:493-504` | OK |
| Handler `/jsr309` | `wsServer.AddHandler("/jsr309", &jsr309Manager)` `main.cpp:478` | OK |
| Parsing d'URL `/jsr309/<sessionId>/<token>` | `JSR309Manager::onWebSocketConnection` `JSR309Manager.cpp:300-359` | OK |
| Résolution du token | `MediaSession::onNewMediaConnection` `MediaSession.cpp:2047-2073` | OK |
| Bascule du port en WS | `Endpoint::onNewMediaConnection` `Endpoint.cpp:425-475` | OK |
| `ConfigureMediaConnection` | `xmlrpcjsr309.cpp:2799-2838` → `Endpoint.cpp:682-737` (`SwitchJoin` conserve l'attachement) | **cassé, §4.1** |
| `GetMediaCandidates` | `xmlrpcjsr309.cpp:2752-2797` → `Endpoint.cpp:741-781` | **schéma faux, §4.2** |
| Encapsulation/désencapsulation RED | `WSEndpoint.cpp:66-101` (WS→RTP), `:103-156` (RTP→WS), `redcodec.h:15-44` | OK |
| Activation RED et PT primaire | `Endpoint::StartReceiving` case WS `Endpoint.cpp:221-244` (lit la `rtpMap`) | OK |
| Port annoncé d'un port WS | `WSEndpoint::GetLocalPort` (statique = port du serveur WS) `WSEndpoint.cpp:195-199` | OK |
| U+FFFD sur perte de session | `WSEndpoint::SendReplacementChar` `WSEndpoint.cpp:173-181` | **stub, §4.4** |

Le cadrage WebSocket est **du texte UTF-8 brut, sans enveloppe** : un message
WebSocket ⇒ exactement un paquet RTP (`WSEndpoint.cpp:52-101`), et
réciproquement (`:226-251`, `websocketconnection.cpp:433-436`). Aucun JSON,
aucun numéro de séquence, aucun préfixe de longueur. La redondance RFC 4103 est
**terminée dans le serveur** : le WebSocket ne porte jamais que du texte
dé-redondé.

## 4. Écarts et correctifs serveur

### 4.1 `ConfigureMediaConnection` répond toujours une faute — BLOQUANT

`MediaSession::ConfigureMediaConnection` retourne `0` sur son **chemin de
succès** (`MediaSession.cpp:2041`), et `xmlrpcjsr309.cpp:2833` transforme
`!res` en `xmlerror`. Aucun contrôleur ne peut donc configurer une connexion WS.

C'est une **régression** : `git show b40ddcf6` (« changement du compilateur pour
C++17. Correction de warnings. », 2026-07-03) a ajouté ce `return 0;` à une
fonction qui n'avait aucun `return` — elle « marchait » sur la valeur de
retour accidentelle. Correctif : `return 1;`.

### 4.2 `GetMediaCandidates` ne rend jamais `wss://`

`Endpoint::GetMediaCandidates` formate `"%s://%s:%d"` avec
`MediaFrame::ProtocolToString(protocol)` (`Endpoint.cpp:780`), et
`ProtocolToString(WS) == "ws"` (`libmedikit/medkit/media.h:96-113`) — quel que
soit l'état de TLS. Le serveur est le seul à savoir s'il écoute en clair ou en
TLS ; le contrôleur ne peut pas le déduire.

Correctif : rendre `wss://` quand `WebSocketServer` est en mode sécurisé (la
même variable qui a servi à `wsServer.SetSecure`, à exposer via un accesseur
statique à côté de `WSEndpoint::SetLocalPort`/`SetLocalHost`,
`main.cpp:509-512`). Le contrôleur reprend alors le schéma **tel quel**, sans
politique locale.

### 4.3 La convention « WSS = port + 1 » est obsolète — à ne pas porter

La passerelle Java incrémente le port pour WSS, deux fois : sur l'URL
(`WebSocketLeg.java:103-108`, `wsaddr[0].port++`) et sur la ligne `m=`
(`:78-81`, `snd.port + 1`). Le serveur actuel n'ouvre **qu'un seul port** et y
bascule TLS en place (`main.cpp:493-512`) : reproduire le `+1` pointerait un
port fermé. **Le contrôleur n'incrémente rien** ; le port du `m=` et celui de
l'URL sont tous deux celui qu'annonce le serveur.

### 4.4 `SendReplacementChar` est un stub

T.140 (§5.3, et RFC 4103 §4.3 pour son transport) demande d'insérer un
**U+FFFD** dans le flux quand une perte de session est détectée, pour que
l'utilisateur voie qu'il manque du texte. La méthode existe, est appelée aux
trois bons endroits (`onClose` `WSEndpoint.cpp:158-171`,
`onResetStream`/`onEndStream` `WSEndpoint.h:34-35`) et **ne fait rien**
(`:173-181`).

Correctif : émettre `U+FFFD` (`EF BF BD`) — vers le WebSocket quand la perte
vient du côté RTP (`onResetStream`/`onEndStream`), et vers le côté RTP quand
c'est le WebSocket qui est tombé (`onClose`), via le chemin RED déjà en place
(`RedCodec::EncodeNull`/`Encode`, `redcodec.h:15-44`). Le booléen du paramètre
existant (`SendReplacementChar(bool)`) porte déjà cette distinction.

### 4.4 bis Le parseur de chaînes rendait TOUTE connexion WebSocket impossible

**Trouvé à l'implémentation, non anticipé, et c'était le blocage réel.**
`BaseStringParser` (`mcu/include/stringparser.h:22`) prenait sa chaîne **par
valeur** :

```cpp
BaseStringParser(const _StringT str)   // copie locale
{
	buffer = (_CharT*)str.c_str();     // pointeur DANS la copie
	size = str.size();
	c = buffer;
}
```

La copie meurt à la sortie du constructeur : `buffer`/`c` pointaient de la
mémoire libérée, et tout parsing construit depuis une `std::string` lisait au
hasard de ce qui l'avait remplacée. Symptôme observé : `MatchString("/jsr309")`
échouait sur une URL parfaitement valide, donc **toute** connexion média
WebSocket répondait `404 Not found` — sans le moindre message d'erreur, puisque
c'est le seul chemin de rejet du handler qui n'en journalise pas. Le même piège
valait pour `Headers::ParseHeader` (`mcu/src/http.cpp:131`), c'est-à-dire pour
le parsing des en-têtes HTTP.

Correctif : référence constante (`const _StringT&`) sur les trois constructeurs
concernés. Le contrat — la chaîne doit survivre au parseur — est celui de la
surcharge `(buffer, size)` et les deux seuls appelants passent des variables
locales.

### 4.5 Le texte reçu avant la connexion du navigateur est perdu

`WSEndpoint::onRTPPacket` jette le paquet et journalise en `Debug` quand aucun
WebSocket n'est attaché (`WSEndpoint.cpp:152-155`). Entre l'envoi du 200 OK et
l'ouverture du WebSocket par le navigateur, il s'écoule un aller-retour SDP plus
un handshake : tout texte émis dans cet intervalle disparaît.

**Décision : tampon borné.** Une file de `TextFrame` d'au plus 32 entrées et
5 secondes, vidée dans l'ordre à `onOpen`, jetée (avec un `Log`) au-delà. Un
tampon non borné sur un flux que personne ne viendra peut-être jamais lire est
une fuite ; ne rien tamponner perd la première phrase, qui est justement celle
où l'appelant se présente.

## 5. Conception côté contrôleur (elixip)

C'est le gros du portage. Cinq points, dans l'ordre du flux d'appel.

### 5.1 Parsing : reconnaître une section texte sur WebSocket

Aujourd'hui `MediaServer.Mendooze.Sdp.parse/1` renvoie ces sections en **stub**
`supported?: false` (le transport n'est pas un profil RTP), et l'adaptateur les
**omet** de la réponse (`ws_text_section?/1`, livré pour le client Elioz).

À faire, dans la couche message (une seule lecture, `CLAUDE.md`) :

- reconnaître `m=text <port> <proto> t140` avec `proto` ∈ {`TCP/WS`,
  `TCP/WSS`, `TLS/WS`, `TLS/WSS`} et produire un `media_desc` de plein droit,
  marqué `transport: :ws` ;
- les quatre graphies, pas seulement les deux de la passerelle Java : elle ne
  reconnaissait que `TCP/*` et faisait tomber `TLS/*` dans la branche RTP
  (`MediaBridge.java:247`), ce qui est un bug, pas un contrat ;
- le `fmt` est le **jeton littéral** `t140`, jamais un payload type numérique —
  ExSDP le laisse en chaîne pour un proto non-RTP, ce qui est correct ici ;
- lire `a=setup` (RFC 4145) : `active`/`actpass` ⇒ nous répondons `passive`.
  `passive` de la part du pair est un refus (personne ne se connecterait) et
  doit décliner la section, comme le faisait `WebSocketLeg.java:170-172` ;
- exposer `a=ws`/`a=wss` s'il y en a un (cas où c'est le pair qui héberge).

### 5.2 Négociation : deux RPC de plus, avant `StartReceiving`

Pour une section texte-sur-WS acceptée, et **avant** `EndpointStartReceiving` —
la bascule du port en `WSEndpoint` doit précéder l'ouverture du plan de
réception :

```
token = UUID v4
EndpointConfigureMediaConnection(sess, ep, TEXT=2, role=0, proto=WS=2, token, "t140")
GetMediaCandidates(sess, ep, proto=WS=2, media=TEXT=2)  → "ws://host:port" | "wss://host:port"
EndpointStartReceiving(sess, ep, TEXT, rtpMap)          → port (= port du serveur WS)
```

- `role = 0` (`VIDEO_MAIN`) pour du texte : ce n'est pas une erreur, c'est le
  port « principal » de n'importe quel média (`Endpoint.cpp:432-435`).
- La `rtpMap` de `StartReceiving` **pilote la redondance** : `T140RED` présent
  ⇒ `SetUseRed(true)`, `T140` présent ⇒ `SetPrimaryPayloadType`
  (`Endpoint.cpp:221-244`). Elle est donc envoyée exactement comme pour une
  patte T.140 sur RTP, avec les payload types de l'autre patte.
- L'URL est `<schéma reçu>://<host>:<port>/jsr309/<sessionId>/<token>`. Le
  schéma vient du serveur (§4.2) ; le contrôleur n'en invente pas.
- Un token **par (re)configuration**. Une re-négociation qui recrée la patte
  frappe une nouvelle URL, qu'il faut re-signaler. Le serveur tolère une
  reconnexion sur le même token (`WSEndpoint::onOpen` ferme la précédente,
  `WSEndpoint.cpp:30-44`).

### 5.3 La réponse SDP

```
m=text <port> TCP/WSS t140
a=setup:passive
a=connection:new
a=wss://<host>:<port>/jsr309/<sessionId>/<token>
```

Contraintes exactes, transcrites de `WebSocketLeg.java:68-142` :

- proto **mirroir de l'offre** (`TCP/WSS` pour une offre `TCP/WSS`) ;
- format : le jeton `t140`, seul ;
- `a=setup:passive` (nous sommes le serveur WebSocket) et `a=connection:new`
  (RFC 4145) ;
- **nom d'attribut** : `ws` pour un schéma `ws`, `wss` pour `wss` ; valeur
  **avec** le schéma et `//`, c'est-à-dire l'URL complète. La passerelle Java
  émettait une URL *relative au protocole* (`//host:port/...`) parce que son
  client re-préfixait le schéma lui-même (§6) — nous émettons l'URL entière,
  qui est ce qu'un client correct attend, **et** nous acceptons les deux formes
  en lecture ;
- **ni `a=rtpmap`, ni `a=fmtp`, ni signalisation de redondance** sur cette
  section : la redondance est interne au serveur média, elle n'est pas négociée
  ici. C'est ce que faisait la passerelle (`addCodecsToMd` remplace entièrement
  la version RTP, `Leg.java:1293-1341`).

**Conséquence sur l'omission livrée** (commit elixip `768ef5c`) : aujourd'hui la
section texte-sur-WS est *omise* de la réponse parce que nous ne savons pas y
répondre et que le client Elioz ne digère pas l'écho port-0. Dès que nous
répondons **pour de vrai**, il faut la **réintroduire** — le client attend
précisément cette section, et son `a=ws`, pour activer son chat ; il la retire
lui-même avant `setRemoteDescription` (§6). L'omission reste le comportement de
repli quand la configuration WS échoue.

### 5.4 Pontage

Rien de spécifique : le texte n'est **jamais transcodé**
(`MediaBridge.needTranscoding` rend 0 pour `TEXT`,
`MediaBridge.java:610-613`), et un `WSEndpoint` est un `Joinable::Listener`
comme un `RTPEndpoint` (`WSEndpoint.h:13-18`). Le pontage passe par les
`EndpointAttachToEndpoint` habituels, dans les deux sens.

Ce que le contrôleur doit faire, en revanche, c'est **proposer T140RED à
l'autre patte** même si le pair WS n'en parle pas : c'est sur la patte RTP que
la redondance a un sens, et c'est le serveur qui la produit. La passerelle le
faisait explicitement (`WebSocketLeg.java:192-201`, commentaire *« we add
redundancy T140 support for other leg »*, et `Leg.buildSndRtpMapText`
`Leg.java:941-973`).

### 5.5 Cycle de vie

- **Rien de nouveau au démontage** : `EndpointStopReceiving`/`StopSending`/
  `EndpointDelete` ferment le WebSocket côté serveur (`WSEndpoint::End`
  `WSEndpoint.cpp:213-223`).
- **Pas de keepalive applicatif.** Le serveur répond aux Ping WS
  (`websocketconnection.cpp:340-345`) et n'en émet jamais ; le client
  historique compensait par 10 reconnexions à 1 s d'intervalle
  (`WebRTComm.js:4316-4331`). Ne pas inventer de keepalive : la reconnexion
  sur le même token est le mécanisme de résilience.
- **Le watchdog RTP ne s'applique pas** à cette patte : il n'y a pas de RTP à
  surveiller, et le texte est légitimement silencieux. C'est déjà la règle
  côté contrôleur (le texte n'est jamais armé).

## 6. Le client déployé (Elioz / WebRTComm) : contrat et pièges

Facts, tirés de `mediagw-b2bua/web/webRtcClient/js/`. Ils contraignent la
réponse plus que n'importe quelle RFC, parce que ce client est déployé.

1. **Le client injecte lui-même la section** dans le SDP *après* que le
   navigateur l'a produit : `m=text 60000 TCP/WS t140`, `c=IN IP4 127.0.0.1`,
   `a=setup:active`, `a=connection:new`, `a=sendrecv`
   (`WebRTComm.js:4234-4264`). Donc : **`TCP/WS`** (jamais `TCP/WSS`), **port
   60000 en dur**, adresse `127.0.0.1`.
2. **Il ne lit que `a=ws` et préfixe `"ws:"` lui-même**
   (`WebRTComm.js:4275-4278`) : il ne peut **pas** consommer un `a=wss:`. Un
   serveur en TLS n'est donc pas utilisable par *cette* version du client —
   à traiter comme une limite du client, pas comme une raison de mentir sur le
   schéma.
3. **Il retire la section `m=text` avant `setRemoteDescription`**
   (`WebRTComm.js:4282-4283`) : `RTCPeerConnection` ne sait pas parser une
   `m=` en `TCP/WS`. C'est pourquoi cette section est **de la signalisation
   pure**, et pourquoi son écho port-0 le déroutait (commit elixip `768ef5c`).
4. Le port non nul de l'offre est un **verrou de vivacité** côté passerelle
   (`MediaBridge.java:688`, `:713`) : ne pas exiger mieux qu'un port non nul.
5. Coalescence à l'émission : le texte est accumulé et envoyé dès 5 caractères
   ou après 500 ms (`MobicentsWebRTCPhoneController.js:222-252`) — donc **un
   message ≠ une frappe**, et le serveur ne doit rien supposer de la taille.
6. Un `setTimeout(1000)` retarde l'affichage des messages entrants
   (`WebRTComm.js:4377-4385`) : un délai d'une seconde observé côté client
   n'est pas un défaut serveur.

## 7. Décisions

- **A. Le WebSocket reste terminé par le serveur média.** Le contrôleur ne fait
  que de la signalisation. C'est l'architecture historique, elle a l'avantage
  d'être déjà écrite côté serveur, et elle garde le média hors du contrôleur.
- **B. Le schéma (`ws`/`wss`) est rendu par le serveur** (§4.2), jamais deviné
  par le contrôleur. Corollaire : pas de `+1` de port (§4.3).
- **C. Les quatre graphies de proto sont reconnues** en parsing
  (`TCP/WS`, `TCP/WSS`, `TLS/WS`, `TLS/WSS`), la réponse **mirroir** celle de
  l'offre. Le client déployé n'émet que `TCP/WS`, mais accepter les autres ne
  coûte rien et le bug inverse (Java) était une source de rejets silencieux.
- **D. L'URL émise porte son schéma en entier**, les deux formes sont acceptées
  en lecture (§5.3).
- **E. La redondance n'est pas négociée sur la section WS**, elle est produite
  par le serveur et proposée à l'autre patte (§5.4).
- **F. Tampon borné (32 trames / 5 s) avant la connexion du navigateur**
  (§4.5), pas de tampon infini, pas de perte silencieuse.
- **G. U+FFFD est implémenté** dans les deux sens (§4.4) : c'est la seule trace
  qu'un utilisateur a d'un texte manquant.

## 8. Phasage

| # | Périmètre | Livrable | Dépend de |
|---|---|---|---|
| 1 | Serveur | ~~`ConfigureMediaConnection` rend 1 (§4.1)~~ **fait** | — |
| 2 | Serveur | ~~Schéma réel dans `GetMediaCandidates` (§4.2)~~ **fait** | — |
| 3 | Serveur | ~~`SendReplacementChar` (§4.4), parseur (§4.4 bis), tampon (§4.5)~~ **fait** | — |
| 4 | Contrôleur | Parsing des sections texte-sur-WS (§5.1) | — |
| 5 | Contrôleur | `ConfigureMediaConnection` + `GetMediaCandidates` + URL (§5.2) | 1, 2 |
| 6 | Contrôleur | Réponse SDP, et fin de l'omission (§5.3) | 4, 5 |
| 7 | Contrôleur | T140RED proposé à l'autre patte (§5.4) | 6 |
| 8 | Interop | Campagne avec le client Elioz | 1-7 |

Les phases 1-3 sont **indépendantes et sans risque de régression** : elles
corrigent du code que personne n'exerce aujourd'hui. Elles peuvent partir
seules.

## 9. Tests

- **Serveur, unitaire/fumée** : `ConfigureMediaConnection` répond
  `returnCode: 1` ; `GetMediaCandidates` rend `ws://` en clair et `wss://` avec
  `--websocket-secure` ; une connexion WS sur `/jsr309/<sess>/<token>` est
  acceptée, une sur un token inconnu répond 404 ; du texte envoyé sur le
  WebSocket ressort en RTP T140RED sur la patte pontée, et réciproquement ;
  U+FFFD à la fermeture ; le tampon rejoue les trames à `onOpen`.
- **Contrôleur, unitaire** : parsing des quatre protos, `a=setup:passive`
  refusé, réponse octet pour octet (proto mirroir, `t140`, `a=ws`/`a=wss`,
  `a=connection:new`), absence de `rtpmap`/`fmtp`, T140RED ajouté à la map de
  l'autre patte, omission conservée quand la configuration WS échoue.
- **Bout en bout** : fixture SDP réelle du client Elioz (`m=text 60000 TCP/WS
  t140`), et un aller-retour de texte contre le binaire réel.

## 10. Risques

1. **Le client déployé ne lit pas `a=wss`** (§6.2) : un déploiement TLS exige
   une mise à jour du client. À valider avec le client Elioz réel avant de
   promettre WSS.
2. **Pas d'authentification** au-delà de l'UUID dans l'URL : quiconque le
   devine ou l'observe (il voyage dans le SDP, donc dans la signalisation)
   parle dans l'appel. En clair (`ws://`) le texte est lisible sur le réseau.
   À documenter comme tel ; `wss://` est le seul remède, ce qui rejoint le
   risque 1.
3. **Les tokens ne sont jamais retirés** de `MediaSession::tokens` — aucun code
   ne les supprime autour de `MediaSession.cpp:2024-2039`. Fuite bornée par la
   durée de vie de la session, mais une re-signalisation répétée en accumule.
   À traiter avec le nettoyage de session.
4. **Numéros de séquence synthétiques** : `WSEndpoint` fabrique ses
   `pseudoSeqNum`/timestamps (`WSEndpoint.cpp:75-93`) indépendamment de la
   patte pontée, alors que la passerelle posait `useOriSeqNum=1` sur les deux
   endpoints (`MediaBridge.java:479-484`). Cohérent en pratique (le texte n'a
   pas de contrainte de gigue), à ne pas « corriger » sans mesure.
5. Le `mode 2` (« texte sur WS ») de l'API Java était **inatteignable** —
   `MediaTranscodingSession.java:618-626` renvoyait 2 et 3 vers WSS. Ne pas
   reproduire cette confusion : ici le schéma vient du serveur (décision B).
