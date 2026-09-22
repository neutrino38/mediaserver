# BFCP sur la pile interne : TCP et UDP, pour la MCU seule

## 1. Objectif

Le partage de document de la MCU repose sur le sous-module `third_party/libbfcp`.
Ce chantier le remplace par la pile BFCP interne du dépôt (`mcu/src/bfcp/`,
`mcu/include/bfcp/`), puis sort le sous-module du build.

Périmètre :

- **MCU seulement.** L'API JSR-309 n'a pas de BFCP aujourd'hui et n'en aura pas
  à l'issue du chantier.
- **TCP et UDP seulement.** Pas de TLS. Une offre `TLS/BFCP` est refusée.
- **L'API de contrôle ne change pas.** Mêmes méthodes XML-RPC, mêmes
  paramètres, mêmes événements. Les schémas MOTELI n'ont donc rien à suivre.

Le gain : un seul BFCP, en C++, dans l'arbre, couvert par la suite GoogleTest,
et battu par le réacteur du mcu au lieu de deux threads `pthread` par
conférence.

## 2. Le fait structurant : la pile interne est un modèle, pas un codec

La pile interne vient du BFCP « web » de Medooze. Elle contient :

- un **modèle fidèle à la RFC 4582** : une classe par primitive (`BFCPMsg*`),
  une par attribut (`BFCPAttr*`), avec la grammaire de chaque message et le
  dessin de chaque TLV en commentaire ;
- un **serveur de contrôle de parole** complet, `BFCPFloorControlServer`, avec
  `BFCPUser` et `BFCPFloorRequest` : requête, octroi, refus, révocation,
  libération, abonnement par FloorQuery, notification des abonnés, rappel
  d'un `Listener` (« le chair »).

Elle ne contient **pas** :

- de **format filaire binaire**. `BFCPMessage::Parse` lit une chaîne JSON, et
  `Stringify` en produit une. L'en-tête commun et les TLV de la RFC 4582 sont à
  écrire ;
- de **transport**. `BFCPUser` tient un `WebSocket*`, et
  `BFCPUser::CloseTransport` dépend d'un `bfcp.h` qui n'existe pas dans le
  dépôt. Le fichier ne compile pas ;
- de **fiabilité UDP** (RFC 8855) : retransmission, cache des réponses,
  acquittements ;
- des primitives **Goodbye**, **GoodbyeAck**, **FloorRequestStatusAck**,
  **FloorStatusAck** ;
- de **tests**.

Le chantier ajoute donc trois couches sous le modèle existant : le codec
binaire, le transport TCP, le transport UDP. Le serveur de contrôle de parole
est gardé, avec les corrections de la section 4.5.

## 3. Ce qui existe (inventaire vérifié)

### 3.1 Ce que le mcu demande à libbfcp

Seul appelant : `mcu/src/shareddocmixer.cpp`. Il n'utilise que :

| Appel libbfcp | Quand | Rôle |
|---|---|---|
| `new BFCP_Server(…, confId, 0, BFCP_FLOOR_ID=1, …)` | premier participant BFCP | un serveur par conférence, un seul floor |
| `OpenTcpConnection("0.0.0.0", port, …, PASSIVE)` | premier participant BFCP | écoute TCP de la conférence |
| `AddUser(partId)`, `RemoveUserInConf(partId)` | `StartReceiving` / départ | le `partId` XML-RPC est le `userId` BFCP |
| `OpenUdpConnection(partId, "0.0.0.0", port)` | participant en `UDP/BFCP` | une socket UDP par participant |
| `GetServerInfo` / `GetConnectionInfo` | retour de `StartReceiving` | le port à annoncer dans le SDP |
| `SendHello(partId, sendIp, sendPort)` | `StartSending(Application)` avec IP publique | fixe la destination UDP et envoie un Hello serveur |
| `FloorRequestRespons(…, ACCEPTED, …, false)` puis `(…, GRANTED, …, false)` | `AcceptDocSharingRequest` | octroi |
| `FloorRequestRespons(…, REVOKED, …)` | refus, arrêt, requête concurrente | révocation |
| `SendFloorStatus(…)` | HelloAck, libération, changement de mosaïque | FloorStatus |
| événements `BFCP_ACT_FloorRequest`, `FloorRelease`, `HelloAck` | rappel `OnBfcpServerEvent` | remontée au contrôleur |

L'événement vers le contrôleur passe par `MultiConf::onRequestDocSharing`
puis `MCU::onParticipantRequestDocSharing`, avec les états `WAITING_ACCEPT`,
`ACTIVE` et `NONE`. Ce chemin ne change pas.

### 3.2 Ce que libbfcp met réellement sur le fil

Relevé dans le sous-module, à reproduire ou à corriger en connaissance de cause :

- **Version 1 partout, UDP compris.** libbfcp n'émet et n'accepte que la
  version 1 de la RFC 4582. En UDP, elle pose un bit maison dans les bits
  réservés au lieu du drapeau R de la RFC 8855. Un message version 2 est jeté
  en silence, sans réponse d'erreur. Les endpoints qui marchent aujourd'hui
  acceptent donc du BFCP version 1 sur UDP.
- **Fiabilité UDP** : temporisateur unique T1 à 500 ms, doublé à chaque
  réémission, abandon au-delà de 16 s. Chaque réponse émise est gardée en cache
  et rejouée à l'identique si la requête revient. Les FloorRequestStatus
  intermédiaires (Pending, Accepted) ne sont pas retransmis.
- **Association d'un datagramme à un utilisateur** : une socket UDP par
  utilisateur, verrouillage sur la première adresse source reçue, et contrôle du
  `userId` de l'en-tête à chaque requête.
- **TCP** : en-tête fixe de 12 octets, longueur de charge en mots de 4 octets,
  lectures partielles accumulées. Une connexion par utilisateur, associée au
  `userId` au premier Hello.
- **Hello serveur en UDP** : après avoir répondu HelloAck à un Hello client
  reçu en UDP, le serveur envoie lui-même un Hello. L'événement `HelloAck`
  remonté au mcu est produit à ce moment, pas à la réception d'un HelloAck du
  client, qui est ignoré.
- **Messages émis par le serveur** : toujours un FloorRequestStatus au seul
  demandeur, avec une FLOOR-REQUEST-INFORMATION portant OVERALL-REQUEST-STATUS,
  une FLOOR-REQUEST-STATUS par floor, BENEFICIARY-INFORMATION et
  REQUESTED-BY-INFORMATION. Le drapeau `informAll` ajoute un FloorStatus à tous
  les utilisateurs. Le mcu l'utilise ainsi : Pending à la requête, Accepted puis
  Granted à l'octroi, Revoked à la révocation.
- **Goodbye** : le serveur répond GoodbyeAck puis retire l'utilisateur. Le
  retrait d'un utilisateur par le mcu envoie un Goodbye au client.
- **HelloAck** émis par le serveur : SUPPORTED-PRIMITIVES = FloorRequest,
  FloorRelease, FloorQuery, FloorStatus, Hello, HelloAck, Goodbye, GoodbyeAck,
  Error ; plus la liste SUPPORTED-ATTRIBUTES.
- Les primitives FloorRequestQuery, UserQuery et ChairAction reçoivent une
  erreur UnknownPrimitive.

### 3.3 L'infrastructure du mcu

- **Réacteur** : `RtpSessionSet` (`mcu/include/rtpsessionset.h`) bat n'importe
  quel `PollHandler` (`mcu/include/pollhandler.h`), pas seulement des sessions
  RTP. Un handler rend ses descripteurs à chaque tour, jusqu'à
  `PollHandler::MaxPollFds`, soit 4. `Add` est permis depuis le thread du
  réacteur. `Remove` est synchrone hors du réacteur, ce qui rend la fermeture du
  descripteur sûre. Règles d'écriture d'un callback : `docs/reference/threads-rtp.md`.
- **Écoutes TCP** : aucune classe commune. `rtmpserver`, `websocketserver` et
  `xmlrpcserver` ouvrent chacun une socket `AF_INET6` avec `IPV6_V6ONLY=0` sur
  `IPAddress::Any(AF_INET6)`. C'est le motif à suivre.
- **Adresse annoncée** : `StartReceiving` la rend déjà dans `returnVal[1]`,
  depuis la table des profils d'adressage. BFCP n'a pas à la calculer.
- **Tests** : tout `mcu/tests/test_*.cpp` est ramassé par `make check`, lié
  contre tout `$(OBJS)`. Le modèle pour un parseur binaire est
  `test_rtcp_hardening.cpp` : octets bruts, page de garde `GuardedBuffer`,
  `EXPECT_EXIT`. `test_bfcp_dualstack.cpp` teste aujourd'hui **libbfcp** et
  inclut `BFCPconnection.h` : il tombe avec le sous-module et doit être réécrit
  contre le transport interne.
- **Capacité** : `capabilities.bfcp` de `/status/general` vaut
  `AppCodec::IsSupported(BFCP)`, une constante vraie dans libmedikit. Rien à
  changer.

## 4. Conception

### 4.1 Le codec binaire

Chaque classe existante reçoit une paire binaire à côté de sa paire JSON, puis
la paire JSON est retirée (arbitrage 9.1) :

- `BFCPAttribute` : `virtual size_t Serialize(BYTE* out, size_t max) const`
  et une fabrique statique `Parse(const BYTE* in, size_t len, size_t& consumed)`
  qui rend l'attribut typé ou `nullptr`. Le TLV suit la RFC 4582 §5.2 :
  type sur 7 bits, bit M, longueur en octets sur 8 bits, bourrage à 4 octets.
  Les attributs groupés (FLOOR-REQUEST-INFORMATION, OVERALL-REQUEST-STATUS,
  FLOOR-REQUEST-STATUS, BENEFICIARY-INFORMATION, REQUESTED-BY-INFORMATION)
  parsent leurs enfants dans la limite de leur propre longueur, jamais au-delà.
- `BFCPMessage` : `static BFCPMessage* Parse(const BYTE*, size_t)` et
  `size_t Serialize(BYTE*, size_t) const`. L'en-tête commun est celui de la
  RFC 8855 §5.1 : version, drapeaux R et F, primitive, longueur de charge en
  mots de 4 octets, Conference ID sur 32 bits, Transaction ID et User ID sur
  16 bits. Un message avec F posé porte deux champs de plus (Fragment Offset,
  Fragment Length).
- Nouvelles classes : `BFCPMsgGoodbye`, `BFCPMsgGoodbyeAck`,
  `BFCPMsgFloorRequestStatusAck`, `BFCPMsgFloorStatusAck`, `BFCPMsgHelloAck`
  (avec SUPPORTED-PRIMITIVES et SUPPORTED-ATTRIBUTES), et les attributs
  `BFCPAttrSupportedPrimitives`, `BFCPAttrSupportedAttributes`,
  `BFCPAttrPriority` qui manquent.
- `IsValid()` de chaque message garde son rôle : après le parse, il vérifie la
  présence des attributs obligatoires.

Règles de robustesse, testées en adverse (section 6, lot 1) :

- une longueur d'en-tête qui dépasse le tampon fait refuser le message ;
- une longueur d'attribut nulle ou qui dépasse la charge fait refuser le message,
  et ne fait jamais boucler le parseur ;
- un attribut inconnu avec M à 0 est sauté, avec M à 1 fait refuser le message
  (RFC 4582 §5.2) ;
- une primitive inconnue produit une réponse Error UnknownPrimitive ;
- une version autre que 1 ou 2 fait jeter le message ;
- le parseur ne lit jamais un octet au-delà de la longueur qu'on lui donne.

### 4.2 Le transport : une interface, deux implémentations

`BFCPUser::transport` devient un `BFCPTransport*` :

```cpp
class BFCPTransport {
public:
	virtual bool Send(const BFCPMessage& msg) = 0;
	virtual void Close() = 0;
	virtual bool IsReliable() const = 0;
};
```

Le serveur de contrôle de parole ne connaît que cette interface. Les deux
implémentations sont des `PollHandler` inscrits dans un groupe `RtpSessionSet`
dédié au BFCP, un seul pour tout le processus (arbitrage 9.3) :

- **`BFCPTcpListener`** : la socket d'écoute d'une conférence. Dual-stack sur
  `IPAddress::Any(AF_INET6)`, `IPV6_V6ONLY=0`, non bloquante. Sur `accept`,
  crée une `BFCPTcpConnection` et l'inscrit dans le groupe. Le port est tiré
  dans la plage RTP et lié directement sur la socket d'écoute, sans socket
  sonde.
- **`BFCPTcpConnection`** : une connexion acceptée. Lit dans un tampon, découpe
  sur la longueur de l'en-tête, gère les lectures partielles et **plusieurs
  messages dans une même lecture**. Elle n'appartient à aucun utilisateur tant
  qu'un Hello valide n'est pas arrivé ; à ce moment le serveur l'attache au
  `BFCPUser` du `userId` de l'en-tête. Un message d'un autre `userId` sur la même
  connexion reçoit UnauthorizedOperation.
- **`BFCPUdpEndpoint`** : une socket UDP par participant, créée à
  `StartReceiving`, dual-stack, port dans la plage RTP. Elle porte la fiabilité
  RFC 8855 (section 4.3). Sa destination vient de deux sources : `StartSending`
  (adresse et port du SDP distant, l'ancien `SendHello`) et le premier datagramme
  reçu, qui verrouille l'adresse source. Un datagramme d'une autre source est
  jeté et journalisé.

Contraintes du réacteur, obligatoires pour chaque handler :

- aucun appel bloquant dans `OnPollEvents` ; l'écriture TCP est non bloquante,
  ce qui ne part pas est mis en file et repris sur `POLLOUT` ;
- `GetNextTimeoutMs` porte les échéances de retransmission UDP et rend -1 si
  rien n'est armé ;
- un handler ne se détruit jamais depuis le thread du réacteur : la fermeture
  passe par `RtpSessionSet::Remove` depuis le thread appelant, puis `close`.

### 4.3 La fiabilité UDP (RFC 8855)

Reproduit ce que libbfcp fait, avec les drapeaux de la RFC :

- **Version reflétée.** En réception, les versions 1 et 2 sont acceptées. En
  émission vers un utilisateur, la version est celle de son dernier message
  reçu ; avant tout message reçu, version 2 avec le drapeau R selon la RFC.
  C'est ce qui garde les endpoints actuels, en version 1, sans fermer la porte à
  un endpoint conforme.
- **Transactions initiées par le serveur** (FloorRequestStatus et FloorStatus de
  notification, Hello serveur, Goodbye) : Transaction ID tiré par le serveur,
  réémission à T1 = 500 ms doublé à chaque fois, abandon au-delà de 16 s. La fin
  de transaction est l'acquittement du client : FloorRequestStatusAck,
  FloorStatusAck, HelloAck, GoodbyeAck. À l'abandon, l'utilisateur est déclaré
  déconnecté : ses requêtes sont révoquées, le contrôleur reçoit `NONE`.
- **Réponses aux requêtes du client** : cache par (`userId`, Transaction ID) le
  temps d'un cycle T1 complet. Une requête dupliquée reçoit la même réponse,
  sans repasser par le serveur de contrôle de parole.
- **Pending et Accepted** : émis une seule fois, comme libbfcp. Seuls Granted,
  Denied, Revoked et Released sont fiabilisés.
- **Fragmentation** : non émise, nos messages tiennent dans un datagramme. En
  réception, un message avec F posé est jeté et journalisé (section 8).

### 4.4 Le Hello

`ProcessHello` de la pile interne est un écho de test (`TODO`). Il devient :

1. répondre HelloAck avec SUPPORTED-PRIMITIVES et SUPPORTED-ATTRIBUTES, la même
   liste que libbfcp (section 3.2) ;
2. attacher le transport à l'utilisateur (première connexion TCP, ou socket UDP
   de ce participant) ;
3. en UDP, envoyer le Hello serveur, comme libbfcp ;
4. appeler `Listener::onUserConnected(userId)`, nouveau rappel qui remplace
   l'événement `HelloAck` : `SharedDocMixer` y fait ce qu'il fait aujourd'hui,
   `SetDocSharingMosaic` si une mosaïque est partagée et un FloorStatus vers cet
   utilisateur.

Un HelloAck reçu du client ferme la transaction du Hello serveur. Rien d'autre.

### 4.5 Le serveur de contrôle de parole : ce qui change

- **Verrou.** `BFCPFloorControlServer` est appelé depuis deux threads : le
  réacteur BFCP (messages reçus) et le thread XML-RPC (`Grant`, `Deny`,
  `Revoke`, `AddUser`, `RemoveUser`). Un `std::mutex` unique protège `users`,
  `floors` et `floorRequests`. Il est **relâché avant chaque appel au
  `Listener`**, pour qu'un rappel puisse rappeler le serveur (par exemple
  `onFloorRequest` qui octroie aussitôt) sans interblocage. `UseMap` et `use.h`
  sortent de la pile.
- **Propriété.** `users`, `removedUsers` et `floorRequests` passent en
  `unique_ptr`. `removedUsers` disparaît : un utilisateur retiré sous le verrou
  n'est plus atteignable, il est détruit hors du verrou.
- **Séquence d'octroi.** `GrantFloorRequest` émet Accepted puis Granted, la
  séquence de libbfcp éprouvée sur les endpoints réels (arbitrage 9.4).
- **Goodbye.** `ProcessGoodbye` répond GoodbyeAck, révoque, notifie
  `onFloorReleased`, retire l'utilisateur. `RemoveUser` envoie un Goodbye avant
  de fermer le transport.
- **Acquittements.** FloorRequestStatusAck, FloorStatusAck et HelloAck ferment
  la transaction serveur correspondante. Sur TCP ils sont acceptés et ignorés.
- **FloorStatus vers un utilisateur.** Nouvelle méthode
  `NotifyFloorStatus(userId, floorId)`, la seule chose que `SendFloorStatus`
  faisait pour le mcu.
- **Erreurs de la pile actuelle corrigées au passage** : `MessageReceived`
  journalise un `userId` avec un `%d` sans argument ; `SetChair` prend le verrou
  deux fois ; `ProcessHello` est un écho.

### 4.6 `SharedDocMixer` sur la pile interne

Il implémente `BFCPFloorControlServer::Listener` et garde la même surface vers
`MultiConf` :

| Aujourd'hui (libbfcp) | Demain (pile interne) |
|---|---|
| `BFCP_ACT_FloorRequest` → mémorise `transactionId`/`floorRequestID`, `WAITING_ACCEPT` | `onFloorRequest(floorRequestId, userId, …)` → mémorise `floorRequestId` par participant, `WAITING_ACCEPT`. Si un autre participant partage déjà, sa requête est révoquée d'abord, comme aujourd'hui |
| `AcceptDocSharingRequest` → `FloorRequestRespons` ACCEPTED puis GRANTED | `GrantFloorRequest(floorRequestId)` ; `onFloorGranted` → `ShareSecondaryStream`, `ACTIVE` |
| `RefuseDocSharingRequest` → REVOKED | `DenyFloorRequest(floorRequestId)` ; `NONE` |
| `StopSharing` → REVOKED | `RevokeFloorRequest(floorRequestId)` ; `onFloorReleased` → `NONE` |
| `BFCP_ACT_FloorRelease` → FloorStatus à tous, `StopSharing` | `onFloorReleased` → `StopSharing`, `NONE` ; le FloorStatus aux abonnés est émis par le serveur |
| `BFCP_ACT_HelloAck` → `SetDocSharingMosaic` + FloorStatus | `onUserConnected` → idem via `NotifyFloorStatus` |
| `getAvailablePort()` avec socket sonde IPv4 | le port est celui que la socket a réellement lié |
| `"0.0.0.0"` | dual-stack, comme les autres écoutes du mcu |

La table `transactions` et `GetBfcpInfo` disparaissent : un seul entier par
participant, le `floorRequestId` en cours.

### 4.7 Le build

- `mcu/Makefile` : les objets de `src/bfcp/`, `src/bfcp/attributes/`,
  `src/bfcp/messages/` entrent dans `OBJS`, avec leurs répertoires dans `VPATH`.
  `BFCPDIR`, `BFCPINCLUDE`, `BFCPLIBS` et le `-I$(BFCPINCLUDE)` disparaissent.
- `install.ksh` : `compile_libbfcp`, la cible `libbfcp`, la branche `clean` du
  sous-module et le test de présence dans `localcompile` disparaissent.
- `mcumediaserver.spec` : le commentaire des sous-modules ne nomme plus libbfcp.
- `.gitmodules` : le sous-module `third_party/libbfcp` est retiré (`git rm`).
- `README.md`, `CLAUDE.md`, `docs/reference/bfcp.md` : réécrits pour décrire
  l'état final, sans historique.

## 5. Pièges relevés dans le code actuel

1. **Le `userId` BFCP est le `partId` XML-RPC, et le Conference ID BFCP est le
   `confId`.** Convention implicite, jamais écrite dans `docs/MCU-API.md`. Le
   contrôleur la met dans le SDP (`a=userid`, `a=confid`). Elle est gardée, et
   documentée dans le lot 4.
2. **`SendFloorStatus(userId, beneficiaryId, …)`** : libbfcp attend un
   Transaction ID en second paramètre. Le mcu y passe un identifiant de
   bénéficiaire. Le FloorStatus part donc avec un Transaction ID arbitraire. La
   pile interne tire ses propres identifiants.
3. **Écoute IPv4 seule.** `"0.0.0.0"` en littéral, alors que le reste du serveur
   est double pile et que `test_bfcp_dualstack.cpp` croit vérifier le contraire
   sur la bibliothèque, pas sur l'appel du mcu.
4. **Messages TCP collés perdus.** libbfcp jette le surplus d'une lecture qui
   contient la fin d'un message et le début du suivant. À ne pas reproduire.
5. **Le port est choisi par une socket sonde** puis lié plus tard par la
   bibliothèque : course entre les deux. La pile interne lie directement.
6. **`mcuevent.h`** ne compile pas et n'est inclus par rien. Son
   `FLOOR_CONTROL` n'est pas le chemin de l'événement. Hors chantier, mais à ne
   pas prendre pour modèle.
7. **`FAILED`** est documenté comme état de l'événement de partage et jamais
   émis. Le chantier ne l'ajoute pas ; il le note pour le contrôleur.
8. **Le rappel applicatif tourne sur le thread réseau**, aujourd'hui comme
   demain. Ce qui change : demain ce thread est un réacteur partagé par tout le
   BFCP du processus. `MultiConf::onRequestDocSharing` pousse dans une file
   d'événements sans bloquer ; `SetDocSharingMosaic` prend `participantsLock`
   et parle au mixeur vidéo. Le lot 4 mesure la durée de ces rappels avec la
   trace `LongTurnUs` du réacteur.

## 6. Lots

Chaque lot compile, passe `make check`, et se livre seul.

### Lot 0 — La pile interne compile, sans être branchée — FAIT

- Entrée dans `mcu/Makefile` (objets, `VPATH`).
- `BFCPTransport` remplace `WebSocket*` ; retrait du JSON et de
  `stringparser.h` ; retrait de `use.h` ; verrou et `unique_ptr` (section 4.5).
- Nouvelles primitives et attributs, encore sans codec.
- Test `test_bfcp_server.cpp` avec un transport factice qui enregistre les
  messages émis : requête → Pending → octroi → Accepted, Granted ; refus ;
  révocation par une seconde requête ; libération par le demandeur ; retrait
  d'un utilisateur qui tient le floor ; Goodbye ; FloorQuery et notification
  des abonnés. Ce sont les scénarios que `SharedDocMixer` joue.

Deux défauts trouvés en écrivant le lot, corrigés :

- `RemoveUser` sortait l'utilisateur de la table **avant** de révoquer ses
  requêtes. Les notifications étant routées par `userId`, le Revoked ne
  trouvait plus de destinataire : le client gardait un floor que le serveur
  avait rendu. C'est le test de retrait qui l'attrape.
- `End()` bouclait sur « tant que la table n'est pas vide » en comptant sur
  l'effacement fait par `Revoke`/`Deny`. Un statut qu'aucun des deux
  n'accepte aurait figé la fin de conférence. La boucle parcourt désormais un
  instantané des identifiants.

### Lot 1 — Le codec binaire

- `Serialize` / `Parse` sur tous les attributs et toutes les primitives.
- `test_bfcp_codec.cpp` : aller-retour de chaque primitive avec tous ses
  attributs, octets attendus écrits à la main pour un FloorRequest, un
  FloorRequestStatus complet et un HelloAck, comparés à la RFC.
- `test_bfcp_codec_hardening.cpp`, suite adverse sur le modèle de
  `test_rtcp_hardening.cpp` : chaque règle de robustesse de la section 4.1 a un
  test qui échoue si elle est violée, derrière une page de garde.

### Lot 2 — Le transport TCP

- `BFCPTcpListener`, `BFCPTcpConnection`, groupe `RtpSessionSet` du BFCP.
- `test_bfcp_tcp.cpp` : un client de test en loopback envoie Hello et reçoit
  HelloAck ; un message écrit octet par octet est reconstitué ; deux messages
  dans une seule écriture sont tous deux traités ; un message d'un `userId`
  inconnu reçoit Error ; la fermeture par le client révoque le floor ; la
  connexion en IPv4 et en IPv6 réussit sur la même écoute.
- `test_bfcp_dualstack.cpp` réécrit contre `BFCPTcpListener` et
  `BFCPUdpEndpoint`, mêmes cinq cas.

### Lot 3 — Le transport UDP

- `BFCPUdpEndpoint`, fiabilité RFC 8855 (section 4.3), Hello serveur.
- `test_bfcp_udp.cpp` : un Granted sans acquittement est réémis à 500 ms puis
  1 s ; acquitté, il ne l'est plus ; l'abandon au-delà de 16 s révoque ; une
  requête dupliquée reçoit une réponse identique octet pour octet ; un
  datagramme d'une autre source est jeté ; un client en version 1 reçoit du
  version 1, un client en version 2 du version 2 avec R ; le drapeau F fait
  jeter le message.

### Lot 4 — La bascule et le retrait de libbfcp

- `SharedDocMixer` sur la pile interne (section 4.6).
- Retrait du sous-module et de tout ce qui le nomme (section 4.7).
- Documentation : `docs/reference/bfcp.md` réécrit ; `CLAUDE.md` ; `README.md` ;
  `docs/MCU-API.md` complété de la convention `userId = partId`,
  `confid = confId`, et de la liste des transports acceptés. ADR 002 pour la
  décision (section 9.5).
- Mesure des rappels sur le réacteur (piège 8).

### Lot 5 — Recette réelle

Section 7. Le chantier n'est clos qu'après.

## 7. Recette de bout en bout

Un appel MCU par transport, avec un endpoint SIP qui fait du BFCP :

1. **TCP/BFCP** : l'endpoint demande le partage ; le contrôleur reçoit
   `WAITING_ACCEPT` ; il accepte ; l'endpoint reçoit Granted et le flux SLIDES
   monte dans la mosaïque partagée ; l'endpoint libère ; le contrôleur reçoit
   `NONE`. Puis la même chose avec refus, puis avec `StopDocSharing` côté
   contrôleur.
2. **UDP/BFCP** : même scénario, plus une capture réseau pour vérifier la
   version émise, les retransmissions et les acquittements.
3. **Deux endpoints** : le second demande pendant que le premier partage ; le
   premier reçoit Revoked.
4. **IPv6** : un endpoint qui joint l'écoute BFCP en IPv6.
5. **Non-régression** : un appel sans BFCP, et un appel JSR-309, inchangés.

Les endpoints de recette sont à nommer par le mainteneur : la SPEC ne sait pas
lesquels sont en production (section 9, à trancher).

## 8. Ce que le chantier ne fait pas

- **TLS/BFCP.** Hors périmètre par décision. Une offre TLS est refusée par
  `StartReceiving`.
- **BFCP pour JSR-309.** Aucune méthode, aucun événement ajoutés.
- **Fragmentation RFC 8855.** Non émise, jetée en réception.
- **Chair distant, ChairAction, UserQuery, FloorRequestQuery.** Refusés par
  Error UnknownPrimitive, comme aujourd'hui.
- **Plusieurs floors par conférence.** Un seul, identifiant 1, comme aujourd'hui.
- **Changer l'API XML-RPC ou les événements.** Rien ne bouge côté contrôleur ;
  seule la documentation se complète.
- **Corriger `mcuevent.h` ou émettre `FAILED`.**

## 9. Arbitrages

### Décidés

- **9.1 Le JSON sort de la pile.** Il n'a plus de transport, dépend d'un en-tête
  absent, et doublerait chaque classe. Le garder n'aurait de sens que pour un
  BFCP WebSocket que personne ne demande.
- **9.2 Un serveur de contrôle de parole par conférence**, comme aujourd'hui.
  Le Conference ID de l'en-tête permettrait une écoute unique pour tout le
  processus, mais c'est le port par conférence que le SDP annonce et que les
  déploiements connaissent.
- **9.3 Un seul groupe `RtpSessionSet` pour tout le BFCP du processus.** Le
  trafic est minuscule. Un groupe par conférence ferait un thread de plus par
  conférence ; mettre BFCP dans les groupes audio ou vidéo exposerait le média à
  un rappel BFCP lent. Écarté : un thread propre par serveur, contraire à la
  règle « aucune session ne porte son propre thread ».
- **9.4 Accepted puis Granted à l'octroi.** La RFC permet Pending → Granted,
  mais la séquence avec Accepted est celle éprouvée sur les endpoints réels.
  Une réponse de plus ne coûte rien ; un endpoint qui l'attend et ne la voit pas
  coûterait une recette.
- **9.5 Une ADR.** Remplacer un sous-module éprouvé par une pile interne, et
  restreindre le transport à TCP et UDP, est un vrai arbitrage entre options
  sérieuses (garder libbfcp et le corriger ; prendre une autre bibliothèque ;
  réinternaliser). Elle s'écrit au lot 4, quand la décision est mise en œuvre :
  `docs/architecture/adr-002-bfcp-pile-interne.md`.
- **9.6 Version UDP reflétée**, pas fixée à 1 ni à 2 (section 4.3).

### À trancher

- **Les endpoints de recette.** Lesquels font du BFCP en production, en TCP et
  en UDP ? Sans cette liste, le lot 5 ne peut pas être joué, et la décision 9.6
  ne peut pas être vérifiée.
- **Le Hello serveur en UDP.** Gardé par fidélité à libbfcp. Si la recette
  montre qu'aucun endpoint n'en a besoin, il se retire au lot 5.
