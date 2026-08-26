# T.140 sur data channel WebRTC

> Statut : **phases 0, 1 et 2 faites** (build, couture DTLS, pile SCTP + DCEP +
> T.140). Branche `feat/t140-over-data-channel`.
>
> Le serveur média ne parle pas SIP. La signalisation et le SDP sont tenus par un
> contrôleur externe (elixip), qui pilote le serveur par XML-RPC. « Contrôleur »
> désigne cet appelant dans tout le document.
>
> Arbitrages : [ADR 001](../../architecture/adr-001-t140-sur-datachannel.md).

## 1. Objectif

Permettre à un client WebRTC de tenir une conversation texte temps réel (T.140)
sur un **data channel**, dans un appel comme dans une conférence.

Le besoin est le même que celui du texte sur WebSocket : un navigateur ne
transporte pas de `m=text` sur profil RTP. La réponse est différente. Le
WebSocket est une deuxième connexion, hors de la `RTCPeerConnection`, avec son
port, son URL et son token. Le data channel est **dans** la
`RTCPeerConnection` : même ICE, même DTLS, même chiffrement, aucune URL à
signer, aucun secret à faire voyager dans la signalisation.

Le transport est normalisé de bout en bout :

| Sujet | Norme |
|---|---|
| Data channel WebRTC | RFC 8831 |
| Protocole d'établissement de canal (DCEP) | RFC 8832 |
| SCTP sur DTLS dans le SDP (`m=application`) | RFC 8841 |
| Négociation de canal par le SDP (`a=dcmap`, `a=dcsa`) | RFC 8864 |
| **T.140 sur data channel** | **RFC 8865** |
| T.140 sur RTP, avec redondance | RFC 4103 |
| Le format texte lui-même | ITU-T T.140 |

Périmètre : la jambe data channel d'un endpoint JSR-309 et d'un participant de
conférence, et son pontage vers une jambe T.140 sur RTP (avec redondance
RFC 4103), vers un WebSocket, ou vers le mixeur texte.

Hors périmètre : le BUNDLE (§13.1), les autres usages du data channel (partage
de fichier, signalisation applicative), le partage de document BFCP.

## 2. Le fait structurant : un data channel est une charge utile, pas un transport neuf

C'est le point à comprendre avant tout le reste, et il rend le chantier bien
plus petit qu'il n'y paraît.

Un data channel WebRTC, c'est du T.140 dans SCTP, dans DTLS, dans UDP, sous ICE.
Les trois couches du bas, le serveur les a déjà, écrites, déployées et
éprouvées : `RTPSession` **est** un porteur ICE + DTLS + UDP. Elle lie un port,
répond aux binding requests STUN, latche l'adresse du pair, mène le handshake
DTLS dans les deux rôles, retransmet ses flights, applique le profil
d'adressage, surveille l'inactivité, et tourne sur un unique thread `poll`.

Il ne manque que deux choses :

1. **les données applicatives DTLS ne sortent pas.** `DTLSConnection::Write`
   (`mcu/src/dtls.cpp:541`) appelle bien `SSL_read`, mais jette le résultat — le
   DTLS ne sert aujourd'hui qu'à dériver les clés SRTP. Personne n'écrit non
   plus par `SSL_write`.
2. **la pile SCTP.** Elle n'existe pas.

D'où la forme de la conception : on ouvre une **couture unique** au bas de la
pile — la frontière DTLS de `RTPSession` — et on empile SCTP, DCEP et T.140
au-dessus. Rien dans ICE, rien dans le SDP côté serveur, rien dans le
démultiplexage.

Conséquence sur l'API de contrôle, et c'est le gain principal : la jambe data
channel se configure avec **les appels XML-RPC qui existent** — empreinte DTLS,
credentials STUN, candidat ICE, `StartReceiving` qui rend le port,
`StartSending` qui pose la destination. Un seul appel nouveau par API : celui
qui dit « cette jambe texte est un data channel », et un getter pour les deux
paramètres SCTP que le contrôleur doit publier.

## 3. Ce qui existe (inventaire)

| Élément | Emplacement | État |
|---|---|---|
| ICE + DTLS + UDP + `poll`, un thread par jambe | `mcu/src/rtpsession.cpp` | OK, réutilisé tel quel |
| Boucle `poll` (hôte du tick SCTP) | `rtpsession.cpp:2971` | à étendre, §5.2 |
| Branche DTLS entrante | `rtpsession.cpp:2638` | à étendre, §5.2 |
| Émission DTLS vers le pair | `RTPSession::FlushDTLS` `rtpsession.cpp:1304` | OK, réutilisé |
| Handshake client DTLS, retransmissions | `rtpsession.cpp:1319-1375` | OK |
| Données applicatives DTLS | `DTLSConnection::Write` `dtls.cpp:541` | **jetées, §5.1** |
| Vérification de l'empreinte du pair | `DTLSConnection::SetupSRTP` `dtls.cpp:439` | **couplée à SRTP, §10.2** |
| Pile SCTP | — | **absente, §5.3** |
| DCEP, T.140 sur canal | — | **absent, §5.4, §5.5** |
| Port d'endpoint commutable par transport | `Endpoint::ConfigureMediaConnection` `src/jsr309/Endpoint.cpp:700` | à étendre, §6 |
| Tableau des ports d'un endpoint | `ports[4]`, `Endpoint.h:246`, `GetPort` `:200` | OK (place pour 4 médias) |
| Résolution du port RTP d'un média | `Endpoint::GetRTPEndpoint` `Endpoint.cpp:97` | à étendre, §6 |
| Port local annoncé | `Endpoint::Port::GetLocalMediaPort` `Endpoint.cpp:649` | à étendre, §6 |
| Miroir à copier côté JSR-309 | `WSEndpoint` `src/jsr309/WSEndpoint.cpp` | référence |
| Jambe texte d'un participant (RTP + pipes du mixeur) | `TextStream` `include/textstream.h`, `src/textstream.cpp:308,392` | à étendre, §7 |
| Bascule de transport d'un participant | `MultiConf::ConfigureParticipantMediaConnection` `src/multiconf.cpp:1600` | à étendre, §7 |
| Miroir à copier côté conférence | `ParticipantTextWS` `src/participanttextws.cpp` | référence |
| Encodage/décodage de la redondance RFC 4103 | `RedundentCodec` `include/redcodec.h` | OK, réutilisé |
| Énumération des transports | `MediaProtocol` `libmedikit/medkit/media.h:87` | à étendre, §5.0 |

## 4. La pile SCTP : usrsctp

**usrsctp**, paquet système EPEL 9, lié dynamiquement, résolu par `pkg-config`.

```
usrsctp.x86_64        0.9.5.0-7.el9   epel   260 ko
usrsctp-devel.x86_64  0.9.5.0-7.el9   epel    18 ko
  /usr/include/usrsctp.h
  /usr/lib64/libusrsctp.so
  /usr/lib64/pkgconfig/usrsctp.pc
```

C'est la pile de Chrome, de Firefox, de Janus et de libdatachannel : la même
pile aux deux bouts de tous les appels que nous aurons à tenir.

Elle a l'API dont cette conception a besoin, vérifiée dans l'en-tête du paquet :

| Symbole | Rôle ici |
|---|---|
| `usrsctp_init_nothreads` | pas de thread à elle : nous gardons la main |
| `usrsctp_handle_timers(ms)` | ses timers, cadencés par notre boucle `poll` |
| `usrsctp_conninput` | injecter un datagramme déchiffré par DTLS |
| `usrsctp_register_address` | associer un cookie d'association à son porteur |
| `usrsctp_socket(…, receive_cb, …)` | réception, avec `sctp_rcvinfo` (donc le PPID) |
| `usrsctp_sendv` / `SCTP_SENDV_SPA` | émission avec PPID et numéro de flux |

Le mode « sans thread » est la raison du choix autant que la pile elle-même : la
sortie SCTP arrive alors **sur notre thread de session**, celui qui tient déjà
l'objet `SSL`. OpenSSL n'est pas concurrent, et c'est ce mode qui évite d'avoir
à le protéger.

`usrsctp` ne touche aucune socket : elle reçoit des datagrammes par
`usrsctp_conninput` et en rend par un callback de sortie. « SCTP sur UDP » est
donc ici, exactement, SCTP sur DTLS sur UDP.

Justification du choix contre les autres pistes : ADR 001.

## 5. Architecture

```
            navigateur (RTCPeerConnection, createDataChannel("t140"))
                                    │
                          ICE + DTLS + UDP, un port
                                    │
  ┌─────────────────────────────────▼──────────────────────────────────┐
  │ RTPSession                     (existe)                            │
  │   ReadRTP : STUN | RTCP | DTLS | RTP                               │
  │   FlushDTLS : émission vers le pair latché                         │
  │   boucle poll : ICE, handshake, watchdog  ── + tick SCTP           │
  └─────────────────────────────────┬──────────────────────────────────┘
                    données applicatives DTLS (§5.1, §5.2)
  ┌─────────────────────────────────▼──────────────────────────────────┐
  │ SCTPTransport      usrsctp, une association                 (§5.3) │
  ├────────────────────────────────────────────────────────────────────┤
  │ DataChannel        DCEP RFC 8832, un canal                  (§5.4) │
  ├────────────────────────────────────────────────────────────────────┤
  │ T140DataChannel    RFC 8865 : un message = un T140block     (§5.5) │
  └───────────────┬────────────────────────────────┬───────────────────┘
       TextFrame  │                                │  TextFrame
  ┌───────────────▼───────────┐      ┌─────────────▼────────────────────┐
  │ DCEndpoint         (§6)   │      │ TextStream, mode DC       (§7)   │
  │ JSR-309 : Endpoint::Port  │      │ conférence : pipes du mixeur     │
  │ T140 / T140RED en RTP     │      │ TextInput / TextOutput           │
  └───────────────┬───────────┘      └─────────────┬────────────────────┘
        autre jambe (RTP T.140,               mixeur texte de la
        WebSocket, enregistreur)              conférence
```

### 5.0 Un transport de plus dans l'énumération

`MediaFrame::MediaProtocol` gagne `SCTP = 5`, et `ProtocolToString` rend
`"sctp"` (`libmedikit/medkit/media.h:87`). C'est le seul changement dans le
sous-module. Valeur **ajoutée en fin d'énumération** : l'ABI mcu ↔ libmedkit
doit rester synchronisée, et les deux arbres se rebâtissent ensemble.

### 5.1 `DTLSConnection` : les données applicatives sortent

Trois ajouts, aucun changement de comportement pour un appelant qui ne les
utilise pas.

- une interface `ApplicationListener { onDTLSApplicationData(BYTE*, DWORD); }`,
  et son setter ;
- dans `Write`, **boucler** sur `SSL_read` tant qu'il rend des octets, dans un
  tampon **propre** — et non celui de l'appelant, qui contient encore le
  datagramme entrant —, et livrer chaque bloc au listener s'il y en a un ;
- `WriteApplicationData(const BYTE*, DWORD)` : `SSL_write`, à ne faire qu'une
  fois le handshake terminé. L'appelant vide ensuite `write_bio` par le chemin
  existant.

Sans listener, `SSL_read` continue d'être appelé et son résultat ignoré : c'est
le comportement d'aujourd'hui.

### 5.2 `RTPSession` : porteur du data channel

Un seul consommateur, qui porte à la fois la livraison et la cadence :

```cpp
class RTPSession::ApplicationListener : public DTLSConnection::ApplicationListener
{
	//Hérité : un bloc applicatif déchiffré.
	//  void onDTLSApplicationData(const BYTE*,DWORD);

	//Cadence propre au consommateur. 0 (défaut) = aucune.
	virtual DWORD GetApplicationTickMs()          { return 0; }
	virtual void  onApplicationTick(DWORD elapsedMs) {}
};
```

- `SetDTLSApplicationListener(ApplicationListener*)`, relayé au
  `DTLSConnection`, à poser avant `Init` ;
- `SendDTLSApplicationData(const BYTE*, DWORD)` : `WriteApplicationData` puis
  `FlushDTLS()`. **À n'appeler que depuis le thread de la session** — c'est le
  contrat, et il est tenu par la file de §5.5 ;
- dans la boucle `poll` : la période déclarée borne l'attente, et le battement
  reçoit l'écoulement **réel** (le poll rend la main plus tôt sur un paquet
  entrant, plus tard sous charge). La pile SCTP y branche
  `SCTPTransport::HandleTimers()`, et `rtpsession.cpp` ne mentionne jamais
  usrsctp. La jambe texte d'un appel tourne alors à 100 Hz, et elle seule ;
- **rien à couper du côté RTP.** Une jambe data channel n'émet pas de RTCP sans
  qu'on le lui demande : les trois émissions autonomes de `SendSenderReport`
  sont déclenchées par l'envoi ou la réception d'un paquet RTP, et il n'y en a
  ni dans un sens ni dans l'autre. Même chose pour la journalisation des payload
  types inconnus, qui est sur le chemin de réception RTP. Un drapeau
  « cette jambe ne porte pas de RTP » ne couperait donc rien.

Ni `ReadRTP`, ni le démultiplexage, ni ICE ne changent. Un paquet DTLS entrant
suit exactement le chemin d'aujourd'hui, et ce chemin livre désormais aussi les
données applicatives.

### 5.3 `SCTPTransport` — une association

`mcu/include/sctptransport.h`, `mcu/src/sctp/sctptransport.cpp`.

- initialisation globale une fois pour le binaire :
  `usrsctp_init_nothreads(0, OnOutput, NULL)`. **`usrsctp_finish()` n'est jamais
  appelé** — l'appeler alors qu'une socket vit est un crash connu, et il n'y a
  rien à gagner : la pile tient dans quelques centaines de kilo-octets et le
  processus s'arrête de toute façon. Un compteur de références n'aurait servi
  qu'à décider d'un appel qu'on ne fait pas ;
- une association = une `struct socket` + `usrsctp_register_address(token)`. Le
  jeton est un **entier opaque**, jamais le `this` : la sortie est routée par une
  table, et un datagramme en retard sur une destruction y trouve une entrée
  disparue au lieu d'un objet libéré ;
- entrée : `usrsctp_conninput(token, data, len, 0)`, depuis le porteur ;
- sortie : **une file**, pas un appel direct. Le tour de timers d'une jambe fait
  avancer les associations des autres, donc la pile peut produire les
  datagrammes d'une jambe sur le thread d'une autre. Rien n'est chiffré dans le
  callback ; le porteur vide la file depuis son thread (`GetOutbound`), réveillé
  par `onSCTPOutboundReady`. C'est cela qui garde l'objet `SSL` mono-thread, et
  c'est le contrat que les adaptateurs doivent respecter ;
- `HandleTimers()` est **statique** et tient l'horloge : les timers d'usrsctp
  sont globaux au processus, et deux jambes cadencées à 10 ms le feraient sinon
  avancer deux fois trop vite ;
- rôle : `bind` sur notre `sctp-port` puis `connect` vers celui du pair, **des
  deux côtés et sans se soucier du rôle DTLS**. SCTP résout la collision d'INIT
  (RFC 4960 §5.2.4), c'est ce que font les implémentations de référence, et cela
  évite un `listen`/`accept` avec sa seconde socket à suivre ;
- options de socket : `SCTP_NODELAY`, `SCTP_EXPLICIT_EOR`,
  `SCTP_ENABLE_STREAM_RESET`, et abonnement aux événements `SCTP_ASSOC_CHANGE`
  et `SCTP_STREAM_RESET_EVENT`. MTU du chemin bornée à 1200 octets, comme les
  implémentations de référence : nos messages tiennent en un chunk, la
  fragmentation n'est pas un sujet, et une PMTU optimiste en est un ;
- `max-message-size` : 65536, annoncé au contrôleur (§8). Un message entrant
  plus grand est jeté avec une trace, jamais réassemblé sans borne — la règle du
  chantier de durcissement des parseurs.

### 5.4 `DataChannel` — DCEP (RFC 8832)

`mcu/include/datachannel.h`, `mcu/src/sctp/datachannel.cpp`.

Un seul canal nous intéresse, celui qui porte le texte. La couche fait donc peu :

- décoder `DATA_CHANNEL_OPEN` (PPID 50) : type de canal, priorité, paramètres de
  fiabilité, `label`, `protocol` — les deux dernières sont des longueurs
  déclarées dans l'en-tête, **à valider contre la taille reçue** avant toute
  lecture ;
- répondre `DATA_CHANNEL_ACK` sur le même flux ;
- encoder `DATA_CHANNEL_OPEN` pour le cas où c'est nous qui ouvrons : flux de
  **parité impaire**, le serveur DTLS n'ayant droit qu'à ceux-là (RFC 8832 §6) ;
- retenir le numéro de flux, et le rendre.

Le canal demandé par RFC 8865 est **fiable et ordonné** : type
`DATA_CHANNEL_RELIABLE`, aucun paramètre de fiabilité partielle. Un canal
entrant qui demande autre chose est accepté quand même, avec une trace : c'est
le client qui a tort, et refuser lui coûterait sa conversation.

Les PPID (RFC 8831 §8) :

| PPID | Sens | Usage ici |
|---|---|---|
| 50 | DCEP | établissement du canal |
| 51 | WebRTC String | **un T140block** |
| 52 | WebRTC Binary Partial (obsolète) | jeté, avec trace |
| 53 | WebRTC Binary | jeté, avec trace |
| 54 | WebRTC String Partial (obsolète) | jeté, avec trace |
| 56 | WebRTC String Empty | T140block vide |
| 57 | WebRTC Binary Empty | jeté, avec trace |

Un message SCTP de longueur nulle n'existe pas : le T140block vide se dit par
son PPID, avec un octet de bourrage que le pair ignore.

### 5.5 `T140DataChannel` — RFC 8865

`mcu/include/t140datachannel.h`, `mcu/src/sctp/t140datachannel.cpp`.

La couche que les deux API partagent, et la seule qui connaisse T.140.

- **sélection du canal** : celui dont le `protocol` DCEP vaut `t140`. À défaut,
  et si aucun canal texte n'est encore lié, le premier canal ouvert est pris,
  avec une trace. Le WebSocket a enseigné qu'un client déployé ne se corrige
  pas : on est indulgent à l'entrée, exact à la sortie ;
- **réception** : un message SCTP = un T140block. Livré tel quel en
  `TextFrame`, sans découpage, sans accumulation ;
- **émission** : `SendText(data, size)`, sûre depuis n'importe quel thread — et
  il en vient d'au moins deux : le thread du mixeur côté conférence, celui de la
  jambe pontée côté JSR-309. Rien n'est chiffré ici : la pile est verrouillée en
  interne et le résultat atterrit dans la file de sortie du transport (§5.3).
  **Le passage de thread est donc celui de cette file, et il n'en faut pas un
  second** ;
- **tampon d'avant-ouverture** : une file, quand le canal n'est pas encore
  ouvert, bornée à **32 trames et 5 secondes**. Entre le 200 OK et l'ouverture
  du canal il s'écoule un aller-retour SDP, un ICE et un handshake DTLS : sans
  tampon, la première phrase — celle où l'appelant se présente — est perdue. Une
  file non bornée sur un flux que personne ne viendra peut-être jamais lire est
  une fuite. C'est la politique du WebSocket, à l'identique, et elle vit **ici**
  et non dans les deux adaptateurs : cette couche est la seule qui sache si le
  canal est ouvert ;
- **pas de redondance.** SCTP est fiable et ordonné : la redondance RFC 4103
  n'a aucun sens sur ce canal, et RFC 8865 l'interdit. Elle reste l'affaire de
  la jambe RTP d'en face ;
- **U+FFFD** vers le côté qui survit dès que le canal ou l'association tombe
  (T.140 §5.3). C'est la seule trace qu'un utilisateur ait qu'il manque du
  texte ;
- **BOM seul** : plomberie de keepalive T.140, pas de la conversation. Non
  transmis, dans les deux sens — c'est ce que fait déjà `ParticipantTextWS`.

## 6. Côté JSR-309 : `DCEndpoint`, le miroir de `WSEndpoint`

`src/jsr309/DCEndpoint.{h,cpp}`, à écrire en partant de `WSEndpoint.cpp` : la
conversion T.140 ⇄ RTP y est déjà, redondance comprise, et elle est prouvée.

**`DCEndpoint : public RTPEndpoint`.** `RTPEndpoint` est déjà
`RTPSession` + `Endpoint::Port` : en dériver donne le porteur, le port local, le
profil d'adressage, le watchdog et la publication d'événements pour rien. Le
constructeur pose `proto = MediaFrame::SCTP` et `SetDataChannelOnly(true)`.

Ce qu'il ajoute :

- un `T140DataChannel` monté sur sa propre session ;
- `onRTPPacket` (la jambe pontée parle) → décodage T140/T140RED par
  `RedundentCodec` → `SendText`. Ce code est celui de
  `WSEndpoint::onRTPPacket` ;
- réception du canal → `RTPPacket` T140 ou `RTPRedundantPacket` T140RED selon ce
  que la jambe d'en face a négocié → `Multiplex`. Ce code est celui de
  `WSEndpoint::onMessageEnd` ;
- `onResetStream` / `onEndStream` / perte de canal → U+FFFD du bon côté.

Quatre points de couture dans l'existant, un par fichier :

1. `Endpoint::ConfigureMediaConnection` (`Endpoint.cpp:700`) : un `case
   MediaFrame::SCTP:` qui construit un `DCEndpoint` — trois lignes, le `switch`
   est déjà écrit pour ça ;
2. `Endpoint::GetRTPEndpoint` (`Endpoint.cpp:97`) : la condition
   `GetTransport() == RTP` devient un `dynamic_cast<RTPEndpoint*>`. C'est ce
   qui fait marcher **sans y toucher** `SetRemoteCryptoDTLS`,
   `SetLocal/RemoteSTUNCredentials`, `AddICECandidate`, `ArmRTPTimeout` et
   `StartSending` sur la jambe data channel ;
3. `Endpoint::Port::GetLocalMediaPort` (`Endpoint.cpp:649`) : `case SCTP:`
   traité comme `case RTP:` — le port annoncé est celui de la session ;
   `GetLocalMediaHost` rend `NULL`, comme un port RTP ;
4. `Endpoint::StartReceiving` (`Endpoint.cpp:184`) : `case MediaFrame::SCTP:`
   qui démarre la session et l'association SCTP. La `rtpMap` n'est pas lue pour
   ce transport — il n'y a pas de payload type sur un data channel — mais elle
   **est** lue pour savoir ce que la jambe pontée attend, T140 ou T140RED,
   exactement comme le fait le `case WS`.

## 7. Côté conférence : le mode data channel de `TextStream`

Le WebSocket avait dû contourner : un `RTPParticipant` n'a pas de transport
commutable, sa `TextStream` est un membre par valeur dont les pipes sont câblés
une fois pour toutes à l'`Init`. `ParticipantTextWS` fait donc la bascule **à la
couture du mixeur**, en arrêtant le demi-plan texte RTP et en tournant contre
ses pipes.

Le data channel n'a pas besoin de ce détour, et c'est tout le propos du §2 : la
jambe **est** ICE + DTLS + UDP, et `TextStream` en possède déjà une —
`RTPSession rtp`. Ce qui change n'est pas le transport, c'est ce qu'on met
dedans.

`TextStream` gagne donc un mode :

- `SetTransport(MediaFrame::MediaProtocol)`, `RTP` par défaut ;
- en mode `SCTP`, `StartReceiving` monte un `T140DataChannel` sur `rtp` au lieu
  de poser une `rtpMap` de réception, et ne démarre **pas** le thread `RecText`
  (`textstream.cpp:308`) : les blocs entrants arrivent sur le thread de la
  session et vont droit à `textOutput->SendFrame()` ;
- le thread `SendText` (`textstream.cpp:392`) reste, inchangé dans sa forme : il
  tire du `textInput` comme aujourd'hui, et pousse dans `SendText` du canal au
  lieu d'émettre du RTP.

Aucune classe nouvelle côté conférence, aucun thread de plus, aucun token,
aucune URL. Tout le reste de `RTPParticipant` — codec, mute, statistiques,
crypto, profil d'adressage — continue de fonctionner par les mêmes chemins.

Côté `MultiConf`, `ConfigureParticipantMediaConnection` (`multiconf.cpp:1600`)
accepte `proto = SCTP` sur `media = Text` et appelle `SetTransport` sur la
`TextStream` du participant. Le `token`, obligatoire pour WS, est **ignoré**
pour SCTP : il n'y a pas d'URL à signer, donc pas de secret à faire voyager.

Une limite à connaître : l'API conférence n'expose pas `AddICECandidate`. La
jambe data channel d'un participant s'appuie donc sur l'adresse de
`StartSending` et sur le latch STUN, comme ses jambes audio et vidéo. Ce n'est
pas propre à ce chantier.

## 8. API de contrôle

Deux appels par API. Aucune méthode existante ne change de signature.

### 8.1 JSR-309

```
EndpointConfigureMediaConnection(sess, ep, media=TEXT(2), role=0, proto=SCTP(5), token="", payload="t140")
EndpointGetDataChannelParameters(sess, ep, media=TEXT(2))  -> {sctpPort, maxMessageSize, streamId}
```

puis **la séquence habituelle d'une jambe chiffrée**, sans un appel de plus :

```
EndpointGetLocalCryptoDTLSFingerprint(hash)        -> notre empreinte
EndpointSetRemoteCryptoDTLS(TEXT, setup, hash, fp) -> celle du pair
EndpointSetLocalSTUNCredentials(TEXT, ufrag, pwd)
EndpointSetRemoteSTUNCredentials(TEXT, ufrag, pwd)
EndpointStartReceiving(TEXT, rtpMap)               -> notre port UDP
EndpointStartSending(TEXT, ip, port, rtpMap)
EndpointAddICECandidate(TEXT, candidate)           -> trickle, au fil de l'eau
```

`ConfigureMediaConnection` doit précéder `StartReceiving` : c'est lui qui décide
de la nature du port, et le port doit exister avant qu'on ouvre le plan de
réception. Même contrainte que pour le WebSocket.

### 8.2 Conférence

```
ConfigureParticipantMediaConnection(conf, part, media=TEXT(2), proto=SCTP(5), token="")
GetParticipantDataChannelParameters(conf, part, media=TEXT(2)) -> {sctpPort, maxMessageSize, streamId}
```

puis `SetRemoteCryptoDTLS`, `SetLocalSTUNCredentials`,
`SetRemoteSTUNCredentials`, `StartReceiving`, `StartSending`, déjà là.

### 8.3 Pourquoi un getter et non des constantes

`sctp-port` et `max-message-size` sont des propriétés du serveur. Le contrôleur
les publie dans son SDP ; il ne doit pas les deviner. C'est la règle du dépôt —
« ce que le serveur sait de lui-même, c'est à lui qu'on le demande » — et le
prix de son oubli est connu : une liste de codecs écrite à la main dans le
contrôleur a coûté un appel mort en 488 avec un audio parfait des deux côtés.

Le `streamId` rendu est celui du canal, une fois connu, ou `-1` : il ne sert que
si le contrôleur veut émettre `a=dcmap` (§9, décision C).

### 8.4 Ce que le contrôleur publie

La section attendue, dans la réponse, telle que le navigateur l'offre :

```
m=application <port> UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 <adresse annoncée>
a=setup:passive
a=fingerprint:sha-256 <notre empreinte>
a=ice-ufrag:<...>
a=ice-pwd:<...>
a=sctp-port:5000
a=max-message-size:65536
```

`m=application` **conserve le port** rendu par `StartReceiving`, et n'est jamais
mise à zéro : contrairement à la section texte sur WebSocket, celle-ci est du
vrai média, que la `RTCPeerConnection` consomme.

**Pas de `a=group:BUNDLE`** dans la réponse (§13.1).

### 8.5 Obligation de contrat

Toute modification de l'API XML-RPC `/mcu` ou `/jsr309` s'accompagne de la mise
à jour des schémas protobuf MOTELI v2 du dépôt elixip
(`apps/elixip2/priv/proto/moteli_*.proto`), dans le même jeu de changements.
Ce chantier ajoute quatre méthodes : elles y passent aussi.

## 9. Décisions

- **A. La pile SCTP est usrsctp, en mode sans thread.** ADR 001. Une pile SCTP
  écrite à la main est un projet, pas une phase ; et la même pile aux deux bouts
  de l'appel est le meilleur garant d'interop qui existe.
- **B. Le transport est une `RTPSession`.** Pas de classe de transport nouvelle.
  ICE, DTLS, latch, profil d'adressage, watchdog, thread `poll` : tout est déjà
  écrit et éprouvé. La couture est à la frontière DTLS, et c'est aussi ce qui
  rend le BUNDLE atteignable plus tard sans rien réécrire au-dessus.
- **C. DCEP en bande fait foi.** Nous acceptons le canal que le client ouvre
  (`createDataChannel("t140", {protocol: "t140"})`), et nous n'exigeons pas
  `a=dcmap`. Un client JavaScript ordinaire marche alors sans rien de spécial.
  La négociation par le SDP (RFC 8864) reste possible : elle ne fixe que le
  numéro de flux, que nous acceptons tel quel.
- **D. Nous restons `a=setup:passive`.** Donc client DTLS et client SCTP chez le
  navigateur, comme pour ses jambes audio et vidéo. Le rôle qui ouvre est celui
  qui appelle : c'est déjà vrai du WebSocket, où c'est le navigateur qui se
  connecte à nous.
- **E. Pas de redondance sur le canal.** SCTP est fiable et ordonné (RFC 8865).
  La redondance RFC 4103 reste produite pour la jambe RTP d'en face, et
  proposée à elle, comme le fait déjà le pont WebSocket.
- **F. Tampon borné, 32 trames et 5 secondes**, dans `T140DataChannel` : ni
  perte silencieuse de la première phrase, ni file infinie.
- **G. U+FFFD dans les deux sens** dès qu'un côté tombe.
- **H. Le passage de thread est la file de sortie du transport.** OpenSSL n'est
  pas concurrent, et les timers d'usrsctp sont globaux : la pile peut produire
  les datagrammes d'une jambe sur le thread d'une autre. Rien ne chiffre donc
  dans un callback de la pile ; le porteur vide la file depuis son thread. Une
  seconde file, côté texte, serait redondante.
- **I. Pas de BUNDLE dans cette phase**, et pas de `a=group` dans la réponse.
  §13.1 dit ce que ça coûte et ce que ça rapporterait.

## 10. Correctifs à faire dans l'existant

### 10.1 `SSL_read` jette les données, dans le tampon de l'appelant

`DTLSConnection::Write` (`dtls.cpp:541`) appelle `SSL_read(ssl, buffer, size)`
sur **le tampon qu'on vient de lui donner** et ignore ce qu'il rend. Sans
listener, c'est sans conséquence visible. Avec, deux défauts : un seul
`SSL_read` par datagramme alors qu'un record DTLS peut en libérer plusieurs, et
une écriture dans un tampon dont l'appelant se sert encore. Correctif : tampon
propre, boucle, livraison au listener.

### 10.2 La vérification d'empreinte est enfouie dans `SetupSRTP`

`SetupSRTP` (`dtls.cpp:439`) fait deux choses : il vérifie l'empreinte du
certificat du pair — la **seule** authentification du pair qu'il y ait — puis il
exporte le matériel de clé SRTP. Les deux sont dans le bon ordre, donc rien
n'est cassé aujourd'hui ; mais une jambe qui ne porte que des données
applicatives n'a pas de clés SRTP à exporter, et il ne faut pas que l'un puisse
un jour emporter l'autre.

Correctif, en deux temps : `VerifyRemoteFingerprint()` devient une fonction à
elle, appelée **sans condition** à la fin du handshake ; et l'export SRTP est
conditionné à la négociation de l'extension `use_srtp` (RFC 5764), lue par
`SSL_get_selected_srtp_profile`. Un pair qui ne l'a pas négociée ne reçoit plus
de clés dérivées d'un accord qui n'a pas eu lieu.

**Ce second point touche un chemin en production**, celui de toutes les jambes
RTP chiffrées. RFC 5764 rend l'extension obligatoire et le contexte SSL de ce
serveur la propose toujours, donc la condition passe pour tout appel qui marche
aujourd'hui. Le garde-fou est un test : `DTLSApplicationData.
LeHandshakeAboutitEtLesClesSRTPSortent` mène un vrai handshake et exige les
clés. Le nom du profil retenu est de plus journalisé.

### 10.3 `GetRTPEndpoint` filtre sur le transport

`Endpoint::GetRTPEndpoint` (`Endpoint.cpp:97`) teste
`GetTransport() == MediaFrame::RTP`, donc rendrait `NULL` pour une jambe data
channel — et toute la configuration crypto et ICE tomberait. Correctif :
`dynamic_cast<RTPEndpoint*>`. La question posée devient la bonne : « ce port
a-t-il un transport de forme RTP ? », et non « ce port porte-t-il du RTP ? ».

## 11. Phasage

| # | Périmètre | Livrable | Dépend de |
|---|---|---|---|
| 0 | Build | ~~`usrsctp` : `pkg-config` dans `mcu/Makefile`, `install.ksh prereq`, `BuildRequires`/`Requires` du spec~~ **fait** | — |
| 1 | Transport | ~~Données applicatives DTLS (§5.1, §10.1), couture `RTPSession` (§5.2), scission de `SetupSRTP` (§10.2)~~ **fait** | 0 |
| 2 | Pile | ~~`SCTPTransport`, `DCEP`, `T140DataChannel` (§5.3-5.5), testés en boucle locale~~ **fait** | 0 |
| 3 | JSR-309 | `MediaFrame::SCTP`, `DCEndpoint`, les 4 coutures du §6, les 2 méthodes XML-RPC | 1, 2 |
| 4 | Conférence | mode data channel de `TextStream` (§7), les 2 méthodes XML-RPC | 1, 2 |
| 5 | Contrôleur | SDP `m=application`, réponse sans BUNDLE, protobuf MOTELI (§8.5) | 3, 4 |
| 6 | Interop | Campagne navigateur, puis appel réel ponté vers une jambe T.140 sur RTP | 5 |

Les phases 1 et 2 sont **sans risque de régression** : la première n'ajoute que
du code mort tant que personne ne pose de listener, la seconde n'est branchée
sur rien. Elles peuvent partir seules.

Les phases 3 et 4 sont **indépendantes l'une de l'autre**. Si une seule doit
partir, c'est la 3 : le JSR-309 est l'API des appels, et le besoin de chat en
conférence n'est pas exprimé aujourd'hui.

## 12. Tests

Suite `mcu/tests/` (GoogleTest), `cd mcu && make check`.

- **DCEP, unitaire** : `DATA_CHANNEL_OPEN` bien formé, réponse `ACK` ; label et
  `protocol` de longueur déclarée **plus grande que le message** (le défaut
  classique de ce genre de parseur) ; message tronqué ; type de canal inconnu ;
  ré-ouverture sur un flux déjà lié. La technique de la page de garde `mmap`
  `PROT_NONE` du chantier de durcissement s'applique telle quelle.
- **SCTP, boucle locale** : deux `SCTPTransport` dos à dos dans le même
  processus, la sortie de l'un injectée dans le `conninput` de l'autre, **sans
  DTLS et sans socket**. C'est le test qui vaut le plus : il couvre association,
  DCEP et T.140 de bout en bout, en quelques millisecondes et sans réseau.
- **T.140, unitaire** : un message = un T140block ; T140block vide (PPID
  « String Empty ») ; UTF-8 multi-octets non coupé ; message plus grand que
  `max-message-size` jeté ; BOM seul non transmis ; U+FFFD des deux côtés ;
  rejeu du tampon à l'ouverture, et péremption au-delà de 5 s.
- **Conversion, unitaire** : T140block → `RTPPacket` T140, et → `RTPRedundantPacket`
  T140RED quand la jambe d'en face l'a négocié ; et le retour. Parité avec
  `WSEndpoint`, qui sert de référence.
- **Intégration** : deux `RTPSession` sur la boucle locale, handshake DTLS réel,
  un T140block traversant les deux piles. Les tests IPv6 montrent que ce genre
  de test tient dans cette suite.
- **Livrés en phase 2** : `tests/test_datachannel.cpp` (DCEP : aller-retour,
  longueurs mensongères de label et de protocol, somme des deux qui déborderait
  un WORD, toutes les troncatures, type inconnu, canal non fiable, et la preuve
  par page de garde qu'une longueur mensongère ne fait pas lire hors du message)
  et `tests/test_sctp_loopback.cpp` (deux piles dos à dos sans socket ni DTLS :
  association, canal ouvert des deux côtés, T140block dans les deux sens, UTF-8
  multi-octets intact, ordre des frappes, rejeu du tampon d'avant-ouverture,
  T140block vide, message binaire écarté, canal sans sous-protocole accepté,
  perte signalée).
- **Livrés en phase 1** : `tests/test_dtls_appdata.cpp` (deux `DTLSConnection`
  dos à dos en mémoire, certificat généré à l'exécution : handshake et clés
  SRTP, empreinte fausse, bloc traversant, octets binaires, plusieurs blocs dans
  un même transfert, absence de consommateur, écriture avant la fin du
  handshake) et `tests/test_rtp_application_tick.cpp` (la cadence est battue,
  et une jambe qui n'en demande pas ne se réveille jamais).
- **Recette live** : un navigateur, `createDataChannel("t140", {protocol:
  "t140"})`, ponté vers un endpoint T.140 sur RTP ; frappe dans les deux sens ;
  coupure du canal → U+FFFD chez le survivant ; re-négociation.

## 13. Risques

### 13.1 BUNDLE

Le serveur n'a jamais su faire de BUNDLE, et ce chantier ne l'apprend pas : la
jambe data channel a son propre port, son ICE et son DTLS, comme les jambes
audio et vidéo.

Un navigateur en `bundlePolicy` par défaut (`balanced`) rassemble des candidats
pour chaque `m=` : une réponse sans `a=group:BUNDLE` lui va. Un client en
`max-bundle` n'offre qu'un seul transport et **échouera**.

Ce que coûterait le BUNDLE, le jour où un client l'impose : ne pas créer de
session dédiée, et poser le `T140DataChannel` sur la session **audio** de
l'endpoint. Les quatre couches au-dessus ne changent pas d'une ligne. C'est
précisément pour ça que la couture est à la frontière DTLS et pas ailleurs.

### 13.2 Cadence de la boucle de session

Une jambe data channel fait tourner sa boucle `poll` à 100 Hz au lieu de dormir
entre deux paquets. Une jambe par appel, un `poll` par tour : mesuré nulle part
à ce stade, à mesurer en phase 6 sur une machine chargée. Si c'était un
problème, `usrsctp_handle_timers` accepte l'écoulement réel : on peut le cadencer
plus lentement quand aucune donnée ne circule.

### 13.3 `usrsctp_finish` et la fin de vie

Terminer la bibliothèque alors qu'une socket vit la fait crasher. Le risque est
écarté de la seule manière sûre : **on ne la termine jamais** (§5.3). Ce qui
reste à surveiller est l'inverse — une association détruite alors qu'un
datagramme lui arrive encore —, et c'est le rôle de la table de jetons. La suite
de tests crée et détruit une paire d'associations par test.

### 13.4 Ce que la production ne joue pas

Nous répondons toujours `a=setup:passive`, donc c'est le navigateur qui ouvre
l'association et le canal. Deux chemins du code ne sont donc jamais empruntés en
appel réel : l'initiative de l'association, et `OpenChannel`. Ils sont **joués
par la boucle locale du §12**, qui les exerce à chaque `make check` — c'est la
seule façon de ne pas laisser du code mort mentir.

### 13.5 Le canal peut ne jamais s'ouvrir

Rien n'oblige le client à ouvrir le canal qu'il a négocié dans son SDP. Le
tampon borné couvre le retard ; au-delà, la conversation est silencieuse et le
serveur ne le sait pas. Il n'y a pas de watchdog à armer là-dessus : le texte
est légitimement silencieux, et c'est déjà la règle sur les jambes texte.

## 14. Non fait, et pourquoi

- **BUNDLE** : §13.1.
- **Autres usages du data channel** : rien n'est exprimé. La couche
  `DataChannel` accepte pourtant plusieurs canaux par association — c'est le
  protocole qui le veut, pas une provision.
- **Enregistrement** : le T.140 d'un data channel arrive en `TextFrame` sur les
  mêmes pipes que celui d'une jambe RTP. L'enregistreur MP4 le voit donc déjà.
  Rien à faire, à vérifier en recette.
- **Introspection des capacités** : le jour où le serveur saura répondre ce
  qu'il porte (`codec_capabilities_plan.md`), `t140` sur `sctp` fait partie de
  la réponse.
