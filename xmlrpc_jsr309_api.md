# API XML-RPC JSR-309 du mediaserver

Documentation de l'API XML-RPC exposée par `mcu/src/jsr309/xmlrpcjsr309.cpp`
(table de commandes `jsr309CmdList`, montée par `main.cpp` sur le gestionnaire
`JSR309Manager`).

Cette interface est le pilotage « bas niveau » du media server, calqué sur le
modèle JSR-309 (Media Server Control API). C'est elle qu'utilise le client Java
`XmlRpcMcuClient` / la couche `jsr309impl/`. Elle est la cible visée pour
l'implémentation de l'interface Mediaserveur d'un **contrôleur SIP** externe
(p. ex. le projet [elixip](https://github.com/neutrino38/elixip)), qui gère la
signalisation et le SDP en pilotant le média serveur par cette API.

> Toutes les chaînes de caractères (noms/tags) sont attendues et renvoyées en
> **UTF-8** (le serveur les repasse par un `UTF8Parser` → `std::wstring`).

---

## 1. Transport et points d'entrée HTTP

Le serveur HTTP interne écoute par défaut sur le port **8080**
(`--http-port`). Trois familles d'URL concernent JSR-309 :

| URL | Méthode | Rôle |
|-----|---------|------|
| `POST http://<host>:8080/jsr309` | XML-RPC | Appels de commande (cette API) |
| `GET  http://<host>:8080/events/jsr309/<queueId>` | HTTP *chunked* | Flux d'événements asynchrones (voir §5) |
| `ws://<host>:9090/jsr309` | WebSocket | Transport média WebRTC/WS (hors périmètre de ce document) |

Le `POST /jsr309` est un XML-RPC standard :

- `Content-Type: text/xml`
- `Content-Length` obligatoire
- corps = `<methodCall>` XML-RPC classique

### Codes de méthode XML-RPC (types)

Dans les signatures ci-dessous on note les types au format `xmlrpc-c` utilisé
par le serveur :

| Notation | Type XML-RPC | Sens |
|----------|--------------|------|
| `i` | `<int>` | entier 32 bits |
| `s` | `<string>` | chaîne UTF-8 |
| `b` | `<boolean>` | booléen |
| `S` | `<struct>` | structure (map clé→valeur) |
| `A` | `<array>` | tableau |

---

## 2. Format de réponse commun

**Toutes** les méthodes renvoient une structure XML-RPC avec la même enveloppe.

### Succès

```
{
  "returnCode": 1,          // int, toujours 1 en cas de succès
  "returnVal":  [ ... ]     // array, contenu dépendant de la méthode
}
```

- Pour les commandes « void » (attach, delete, start…), `returnVal` est un
  **tableau vide** `[]`.
- Pour les commandes de création / requête, `returnVal` contient les valeurs de
  retour (id créé, port, url…) — détaillé méthode par méthode.

### Erreur

```
{
  "returnCode": 0,          // int, 0 = échec
  "errorMsg":   "..."       // string, message d'erreur (anglais)
}
```

> ⚠️ Piège d'implémentation : le serveur distingue le succès de l'erreur par le
> champ **`returnCode`**, et non par une *fault* XML-RPC. Une réponse HTTP 200
> avec `returnCode: 0` est un échec applicatif. Une vraie *fault* XML-RPC
> (HTTP 500) n'arrive qu'en cas d'erreur de parsing des paramètres.

Réf. : `xmlok()` / `xmlerror()` dans `mcu/src/xmlhandler.cpp`.

---

## 3. Modèle objet et conventions

L'API est **orientée session**. La hiérarchie des objets :

```
JSR309Manager
 └── MediaSession            (sessionId)   ← conteneur racine
      ├── Endpoint           (endpointId)  ← une connexion RTP/SRTP/DTLS (audio+video+text)
      ├── Player             (playerId)    ← lecteur de fichier média
      ├── Recorder           (recorderId)  ← enregistreur
      ├── AudioMixer         (mixerId)
      │    └── AudioMixerPort (portId)
      ├── VideoMixer         (mixerId)
      │    ├── VideoMixerPort (portId)
      │    └── Mosaic         (mosaicId)    ← composition visuelle
      ├── AudioTranscoder    (transcoderId)
      └── VideoTranscoder    (videoTranscoderId)
```

### Conventions d'appel

- Sauf `EventQueueCreate/Delete`, `MediaSessionCreate` et
  `EndpointGetLocalCryptoDTLSFingerprint`, **le premier paramètre est toujours
  `sessionId`**.
- Les identifiants (`endpointId`, `playerId`, `mixerId`, `portId`,
  `mosaicId`, `transcoderId`…) sont des **entiers ≥ 0** attribués par le serveur
  à la création. `-1`/valeur négative signale un échec de création.
- Le modèle est celui des **joinables** JSR-309 : on crée des objets, puis on les
  **attache** (`Attach…`) entre eux par type de média. Un même flux peut être
  routé d'un endpoint vers un mixer, un player, un autre endpoint, etc.

### Cycle de vie type d'un appel (résumé)

1. `EventQueueCreate` → `queueId, sourceName`
2. `MediaSessionCreate(tag, queueId)` → `sessionId`
3. `EndpointCreate(sessionId, name, audio, video, text)` → `endpointId`
4. Sécurité : `EndpointSetLocalCryptoSDES` / `EndpointSetRemoteCryptoSDES` ou
   DTLS / STUN selon le transport.
5. `EndpointStartReceiving(sessionId, endpointId, media, rtpMap)` → port local
   d'écoute **et** `fmtp` par PT accepté (`returnVal[1]`, à publier dans le SDP
   local). Précéder au besoin d'`EndpointSetRTPProperties(codec.*)` pour piloter
   le `fmtp` local (voir `CODECS.md`).
6. `EndpointStartSending(sessionId, endpointId, media, ip, port, rtpMap)` (à
   partir du SDP distant).
7. Attaches média (vers un autre endpoint, un mixer, un player, un transcoder…).
8. `EndpointStartRTPTimeout(…, timeoutMs)` **après émission du SDP answer** pour
   armer la surveillance d'inactivité.
9. Fermeture : `EndpointStopSending` / `EndpointStopReceiving` /
   `EndpointDelete`, puis `MediaSessionDelete`, puis `EventQueueDelete`.

> 📎 Le déroulé **détaillé** entrant / sortant, avec la correspondance
> SDP offer/answer ↔ RPC (ordre exact, crypto, ICE, armement du watchdog,
> re-INVITE, terminaison), est en **§9**. C'est la référence à suivre pour
> implémenter le contrôleur SIP.

---

## 4. Énumérations

Valeurs entières à passer telles quelles dans les paramètres `i`.

### `MediaFrame::Type` — type de média
(`libmedikit/medkit/media.h`)

| Valeur | Nom |
|--------|-----|
| 0 | Audio |
| 1 | Video |
| 2 | Text |
| 3 | Application |

### `MediaFrame::MediaProtocol` — protocole de transport
| Valeur | Nom |
|--------|-----|
| 0 | RTP |
| 1 | RTMP |
| 2 | WS (WebSocket) |
| 3 | TCP (MSRP, BFCP…) |
| 4 | UDP |

### `MediaFrame::MediaRole` — rôle du flux vidéo
| Valeur | Nom |
|--------|-----|
| 0 | VIDEO_MAIN |
| 1 | VIDEO_SLIDES |

### `AudioCodec::Type`
(`mcu/include/codecs.h`)

| Valeur | Nom |
|--------|-----|
| 0 | PCMU |
| 3 | GSM |
| 8 | PCMA |
| 9 | G722 |
| 97 | AAC |
| 98 | OPUS |
| 99 | SLIN |
| 100 | TELEPHONE_EVENT |
| 117 | SPEEX16 |
| 118 | AMR |
| 119 | G7221 |
| 120 | AMRWB |
| 130 | NELLY8 |
| 131 | NELLY11 |

### `VideoCodec::Type`
| Valeur | Nom |
|--------|-----|
| 34 | H263_1996 |
| 103 | H263_1998 |
| 104 | MPEG4 |
| 99 | H264 |
| 100 | SORENSON |
| 106 | VP6 |
| 107 | VP8 |
| 108 | ULPFEC |
| 109 | RED |

### `TextCodec::Type`
| Valeur | Nom |
|--------|-----|
| 105 | T140RED |
| 106 | T140 |

### `AppCodec::Type`
| Valeur | Nom |
|--------|-----|
| 150 | BFCP |

### `Mosaic::Type` — type de composition vidéo
(`mcu/include/mosaic.h`)

| Valeur | Nom | Disposition |
|--------|-----|-------------|
| 0 | mosaic1x1 | plein écran |
| 1 | mosaic2x2 | 2×2 |
| 2 | mosaic3x3 | 3×3 |
| 3 | mosaic3p4 | 3+4 |
| 4 | mosaic1p7 | 1 grand + 7 |
| 5 | mosaic1p5 | 1 grand + 5 |
| 6 | mosaic1p1 | 1+1 |
| 7 | mosaicPIP1 | incrustation 1 |
| 8 | mosaicPIP3 | incrustation 3 |
| 9 | mosaic4x4 | 4×4 |
| 10 | mosaic1p4 | 1 grand + 4 |
| 11 | mosaic2p8 | 2+8 |

> Le paramètre `size` des mosaïques et des SetCodec vidéo est un **code de
> taille/résolution** (index d'un tableau de résolutions prédéfinies dans
> `codecs.h`, ex. CIF, VGA, HD…), pas une largeur en pixels.

---

## 5. Événements asynchrones (file d'événements)

Le serveur ne rappelle pas le client : celui-ci **récupère** les événements par
un GET HTTP long-poll / *chunked*.

### Mise en place

1. `EventQueueCreate` → `queueId` **et** `sourceName` (chemin de la file).
2. Passer ce `queueId` à `MediaSessionCreate` : les événements de la session y
   seront routés.
3. Ouvrir en parallèle `GET http://<host>:8080<sourceName>` (soit
   `…/events/jsr309/<queueId>`).

### Flux d'événements

La réponse est en `Transfer-Encoding: chunked`, `Content-Type: text/xml`. Le
serveur maintient la connexion ouverte (attente jusqu'à 30 s par cycle) et :

- envoie, pour chaque événement, une **réponse XML-RPC sérialisée**
  (`<methodResponse>` contenant le tuple de l'événement) ;
- envoie un **keep-alive** `\r\n` s'il n'y a pas d'événement dans le délai ;
- ferme le flux quand la file est détruite (`EventQueueDelete`).

Chaque événement est un tuple dont le **premier entier est le type d'événement**
(`JSR309Event::Events`).

### Types d'événements

> ⚠️ **Contrat de fil** : ces codes numériques sont partagés avec le contrôleur
> SIP et les clients Java (`JSR309Event::Events`). Ils ne doivent jamais être réordonnés ni
> réutilisés. Source unique : `mcu/src/jsr309/JSR309Event.h`.

| Type | Nom | Tuple |
|------|-----|-------|
| 1 | PlayerEndOfFileEvent | `(int type, string sessionTag, string playerTag)` |
| 2 | ExternalFIRRequestedEvent | `(int type, string sessionTag, int joinableId, int media, int role)` |
| 3 | PlayerStartedEvent | `(int type, string sessionTag, string playerTag)` |
| 4 | RecorderStartedEvent | `(int type, string sessionTag, string recorderTag)` |
| 5 | RecorderStoppedEvent | `(int type, string sessionTag, string recorderTag, int reason)` |
| 6 | EndpointDisconnectedEvent | `(int type, string sessionTag, int joinableId, int media, int role)` |

- **PlayerEndOfFileEvent** (1) : un `Player` a atteint la fin du fichier.
  `playerTag` = nom passé à `PlayerCreate`.
- **ExternalFIRRequestedEvent** (2) : un endpoint distant a demandé une image
  complète (Full Intra Request). `joinableId` = `endpointId` concerné,
  `media`/`role` selon les énumérations §4. À traiter typiquement par un
  `VideoTranscoderFPU` ou une régénération d'image clé.
- **PlayerStartedEvent** (3) : émis après un `PlayerPlay` réussi.
- **RecorderStartedEvent** (4) : émis après un `RecorderRecord` réussi.
- **RecorderStoppedEvent** (5) : émis à l'arrêt d'un enregistrement. `reason` :
  `0` = arrêt explicite (`RecorderStop`), `1` = durée max atteinte (voir
  `maxDuration` de `RecorderRecord`), `2` = silence, `3` = DTMF (2/3 non encore
  implémentés).
- **EndpointDisconnectedEvent** (6) : le **watchdog d'inactivité RTP** n'a plus
  reçu de paquet depuis le seuil armé (voir `EndpointStartRTPTimeout`, §6.7).
  `joinableId` = `endpointId`, `media`/`role` selon §4. Émis **une seule fois**
  par transition actif→inactif.

Réf. : `mcu/src/jsr309/JSR309Event.h`, `MediaSession.h` (events Player/Recorder),
`RTPEndpoint.cpp` (ExternalFIR / EndpointDisconnected).

---

## 6. Référence des méthodes

Pour chaque méthode : la signature des paramètres (dans l'ordre) et le contenu
de `returnVal` en cas de succès. `returnVal = []` signifie tableau vide.

### 6.1 Files d'événements

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `EventQueueCreate` | — | `[ int queueId, string sourceName ]` |
| `EventQueueDelete` | `i queueId` | `[]` |

`sourceName` est le **chemin HTTP relatif** de la file d'événements à ouvrir en
long-poll, p.ex. `"/events/jsr309/7"`. Le client doit l'utiliser tel quel
(préfixé de `http://<host>:8080`) plutôt que de reconstruire l'URL à la main.
> Compat : historiquement `returnVal` ne contenait que `[ queueId ]` ; le
> `sourceName` est un ajout (gap 6). Un client tolérant lit `returnVal[1]` s'il
> est présent, sinon retombe sur `"/events/jsr309/<queueId>"`.

### 6.2 Sessions média

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `MediaSessionCreate` | `s tag, i queueId` | `[ int sessionId ]` |
| `MediaSessionDelete` | `i sessionId` | `[]` |

`tag` : nom lisible de la session (renvoyé dans les événements comme
`sessionTag`).

### 6.3 Players (lecture de fichiers)

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `PlayerCreate` | `i sessionId, s name` | `[ int playerId ]` |
| `PlayerOpen` | `i sessionId, i playerId, s filename` | `[]` |
| `PlayerPlay` | `i sessionId, i playerId` | `[]` |
| `PlayerSeek` | `i sessionId, i playerId, i timeMs` | `[]` |
| `PlayerStop` | `i sessionId, i playerId` | `[]` |
| `PlayerClose` | `i sessionId, i playerId` | `[]` |
| `PlayerDelete` | `i sessionId, i playerId` | `[]` |

À la création, un handler d'événement vidéo est posé : la fin de lecture émet un
`PlayerEndOfFileEvent` (§5). `PlayerPlay` émet `PlayerStartedEvent`.

### 6.4 Recorders (enregistrement)

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `RecorderCreate` | `i sessionId, s name` | `[ int recorderId ]` |
| `RecorderRecord` | `i sessionId, i recorderId, s filename [, i maxDuration [, i waitVideo [, i echoVideo]]]` | `[]` |
| `RecorderStop` | `i sessionId, i recorderId` | `[]` |
| `RecorderDelete` | `i sessionId, i recorderId` | `[]` |
| `RecorderAttachToEndpoint` | `i sessionId, i recorderId, i endpointId, i media` | `[]` |
| `RecorderAttachToAudioMixerPort` | `i sessionId, i recorderId, i mixerId, i portId` | `[]` |
| `RecorderAttachToVideoMixerPort` | `i sessionId, i recorderId, i mixerId, i portId` | `[]` |
| `RecorderDettach` | `i sessionId, i recorderId, i media` | `[]` |

`media` = `MediaFrame::Type` (§4).

#### Médias enregistrés (MP4)

Le Recorder attaché à un endpoint enregistre les **trois médias** dans le MP4 :

- **Vidéo** : H.264 tel quel (piste `avc1` + hint track RTP). L'enregistrement
  vidéo démarre à la première I-frame reçue.
- **Audio** : PCMU/PCMA écrits tels quels ; **tout autre codec (Opus, G.722,
  AMR…) est transcodé en AAC-LC** à la fréquence native du décodeur
  (Opus → AAC mono 48 kHz). Aucune configuration côté client.
- **Texte temps réel** : T.140 nu ou **T140RED (RFC 4103)** — la redondance est
  décodée (récupération des paquets perdus), les keepalives (BOM UTF-8, trames
  vides) sont filtrés. Écrit en piste **sous-titres 3GPP (`tx3g`)**, timestamps
  recalés sur l'axe temps de l'enregistrement.

#### Paramètres optionnels de `RecorderRecord`

- **`maxDuration`** (4e, ms, `0`/absent = illimité) : à expiration,
  l'enregistrement est arrêté automatiquement et un
  `RecorderStoppedEvent(reason=1)` est émis.
- **`waitVideo`** (5e, `0`/`1`, défaut `1`) : si `1`, audio et texte sont
  **jetés tant que la première I-frame vidéo n'est pas arrivée** (les pistes
  démarrent ensemble). Mettre `0` pour enregistrer audio/texte immédiatement.
  **Désactivation automatique** : si aucune source vidéo n'est attachée au
  recorder, ou si la vidéo attachée n'a pas été négociée (pas de
  `EndpointStartReceiving(Video)`), le serveur force `waitVideo=0` de lui-même
  — sinon le MP4 resterait vide. Trace : `Recorder: no negotiated video
  source, disabling waitVideo`.
- **`echoVideo`** (6e, `0`/`1`, défaut `0`) : le Recorder **renvoie en écho
  chaque paquet vidéo reçu vers l'endpoint source** (l'appelant se voit pendant
  qu'il s'enregistre). L'écho s'éteint au `RecorderStop`. **Prérequis côté
  client** : `EndpointStartSending(Video)` doit avoir été appelé **avant**
  `RecorderRecord` (sinon chaque paquet est refusé avec la trace `trying to
  send packet on an inactive RTP EP`), et la carte de codecs d'émission doit
  contenir le PT vidéo reçu. Sans effet si la source du média vidéo n'est pas
  un endpoint (port de mixer).

#### Événements et conseils d'intégration

- `RecorderRecord` réussi émet `RecorderStartedEvent` ; `RecorderStop` émet
  `RecorderStoppedEvent(reason=0)` (voir §5).
- **Ne pas armer `EndpointStartRTPTimeout` sur le média Text** pendant un
  enregistrement : le T.140 n'émet que pendant la frappe, un silence de
  quelques secondes est normal et déclencherait un faux
  `EndpointDisconnectedEvent`.

Séquence type « enregistrement de message » (vidéo négociée) :

```
EndpointStartReceiving(Audio|Video|Text)   # cartes PT -> codec
EndpointStartSending(Video, ip, port, map) # requis pour l'écho
RecorderCreate / RecorderAttachToEndpoint (Audio, Video, Text)
RecorderRecord(sessionId, recId, "msg.mp4", maxDuration, 1, 1)
... RecorderStoppedEvent ou RecorderStop ...
RecorderDettach ×3 / RecorderDelete
```

### 6.5 Endpoints — cycle de vie et attaches

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `EndpointCreate` | `i sessionId, s name, b audioSupported, b videoSupported, b textSupported` | `[ int endpointId ]` |
| `EndpointDelete` | `i sessionId, i endpointId` | `[]` |
| `EndpointGetStatistics` | `i sessionId, i endpointId` | `[ stats… ]` (voir §7) |
| `EndpointAttachToPlayer` | `i sessionId, i endpointId, i playerId, i media` | `[]` |
| `EndpointAttachToEndpoint` | `i sessionId, i endpointId, i sourceId, i media` | `[]` |
| `EndpointAttachToAudioMixerPort` | `i sessionId, i endpointId, i mixerId, i portId` | `[]` |
| `EndpointAttachToVideoMixerPort` | `i sessionId, i endpointId, i mixerId, i portId` | `[]` |
| `EndpointAttachToAudioTranscoder` | `i sessionId, i endpointId, i transcoderId` | `[]` |
| `EndpointAttachToVideoTranscoder` | `i sessionId, i endpointId, i videoTranscoderId` | `[]` |
| `EndpointDettach` | `i sessionId, i endpointId, i media` | `[]` |

`EndpointAttachToEndpoint` : `sourceId` est l'endpoint source dont on route le
média `media` vers `endpointId`.

### 6.6 Endpoints — sécurité (SRTP / DTLS / ICE-STUN)

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `EndpointSetLocalCryptoSDES` | `i sessionId, i endpointId, i media, s suite, s key` | `[]` |
| `EndpointSetRemoteCryptoSDES` | `i sessionId, i endpointId, i media, s suite, s key` | `[]` |
| `EndpointSetRemoteCryptoDTLS` | `i sessionId, i endpointId, i media, s setup, s hash, s fingerprint` | `[]` |
| `EndpointGetLocalCryptoDTLSFingerprint` | `s hash` | `[ string fingerprint ]` |
| `EndpointSetLocalSTUNCredentials` | `i sessionId, i endpointId, i media, s username, s pwd` | `[]` |
| `EndpointSetRemoteSTUNCredentials` | `i sessionId, i endpointId, i media, s username, s pwd` | `[]` |

- `suite` SDES : ex. `AES_CM_128_HMAC_SHA1_80`. `key` : clé base64 du SDP.
- `EndpointGetLocalCryptoDTLSFingerprint` : `hash` ∈ {`sha-1`, `sha-256`}
  (insensible à la casse). **Ne prend pas de `sessionId`** — c'est un service
  global de certificat DTLS. Renvoie une chaîne fingerprint pour le SDP local.
- DTLS `setup` : `active` / `passive` / `actpass` ; `hash` : nom d'algo du
  fingerprint distant ; `fingerprint` : empreinte du SDP distant.

### 6.7 Endpoints — média RTP (send / receive)

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `EndpointSetRTPProperties` | `i sessionId, i endpointId, i media, S properties` | `[]` |
| `EndpointStartSending` | `i sessionId, i endpointId, i media, s sendIp, i sendPort, S rtpMap` | `[]` |
| `EndpointStopSending` | `i sessionId, i endpointId, i media` | `[]` |
| `EndpointStartReceiving` | `i sessionId, i endpointId, i media, S rtpMap` | `[ int recvPort, S fmtpByPt ]` |
| `EndpointStopReceiving` | `i sessionId, i endpointId, i media` | `[]` |
| `EndpointRequestUpdate` | `i sessionId, i endpointId, i media` | `[]` |
| `EndpointAddICECandidate` | `i sessionId, i endpointId, i media, s candidate` | `[]` |
| `EndpointStartRTPTimeout` | `i sessionId, i endpointId, i media, i timeoutMs` | `[]` |

- **`rtpMap`** : struct XML-RPC dont **chaque clé est un payload type** (chaîne,
  ex. `"96"`) et **chaque valeur est un code codec entier** (`i`) selon
  `AudioCodec::Type` / `VideoCodec::Type` / `TextCodec::Type`. Exemple :
  `{ "0": 0, "8": 8, "101": 100 }` (PCMU, PCMA, telephone-event).
- **`properties`** (`EndpointSetRTPProperties`) : struct XML-RPC clé→valeur, les
  deux **chaînes**. Deux familles de clés cohabitent :
  - **transport** (rtcp-mux, ssrc, tmmbr, extensions, `rtpTimeout`…) : appliquées
    à la session RTP ;
  - **codec** (préfixe `codec.`, ex. `codec.h264.profile-level-id`,
    `codec.opus.useinbandfec`) : depuis la phase 4 de `nego_fmtp.md`, elles sont
    **routées vers le stockage local de l'endpoint** et consommées par le
    négociateur pour dériver le `fmtp` local (voir `CODECS.md` pour la liste des
    clés par codec). La session RTP les ignore.
- `EndpointStartReceiving` renvoie (§5.2 de `nego_fmtp.md`, **livré phase 4**) :
  - `returnVal[0]` = **port local** ouvert à publier dans le SDP local (inchangé,
    clients actuels OK) ;
  - `returnVal[1]` = struct `{ "<pt>": "<paramètres fmtp>" }` listant **chaque
    payload type réellement accepté** issu du `rtpMap` d'entrée. Le serveur est
    **autoritatif** : le contrôleur SIP reconstruit la m-line et les `a=fmtp`
    directement à partir de cette struct. Règles :
    - **présence de la clé = PT accepté** ; **absence de la clé = PT filtré**
      (non supporté par le serveur). C'est ainsi que le contrôleur déduit les
      PT retenus.
    - la valeur est le corps du `fmtp` **seul** — exactement ce qui suit
      `a=fmtp:<pt> ` dans le SDP, **sans** le préfixe `a=fmtp:` ni le numéro de PT.
    - un codec **sans `fmtp`** (PCMU, PCMA, G722, T140…) est **présent** avec la
      valeur **chaîne vide `""`**.
    - un codec **avec `fmtp`** porte la chaîne complète, ex.
      `"profile-level-id=42801f;packetization-mode=1"` (H264).
    - cas particuliers : `telephone-event` → sa plage de tonalités (ex. `"0-16"`) ;
      `red` / T140RED → la liste de redondance référençant le PT T.140 accepté
      (ex. `"106/106/106"`), cohérente avec la numérotation des PT acceptés.
- `EndpointRequestUpdate` : force une mise à jour / image clé (FIR) vers ce
  média.

> Compat : historiquement `returnVal` ne contenait que `[ recvPort ]` ; la struct
> `fmtpByPt` est un ajout **strictement additif**. Un client qui ne lit que
> `returnVal[0]` (le port) n'est pas impacté ; un client tolérant lit
> `returnVal[1]` pour construire ses `a=fmtp` et déduire les PT acceptés.

> 🔷 **Négociation entrante (`fmtp` distant) — encore en cours** (phase 5 de
> `nego_fmtp.md`). Le **canal** existe déjà : `EndpointSetRTPProperties` accepte
> une clé `codec.<nomCodec>.fmtp` portant les paramètres `fmtp` **reçus du pair**
> (p. ex. `codec.h264.fmtp = "profile-level-id=42e01f;packetization-mode=1"`), et
> ces clés sont bien stockées côté endpoint. En revanche leur **parsing** (pour
> contraindre l'**émission** : borner notre encodeur au profil du pair) et le
> câblage endpoint → producteur (transcodeur/mixer) restent à livrer (H.264 en
> premier). Cf. §9.7 pour l'ordre d'appel exact selon le sens de l'appel.
- **`EndpointAddICECandidate`** (trickle ICE, Niveau 1) : `candidate` est une
  ligne d'attribut SDP `candidate:` (avec ou sans le préfixe `candidate:`), p.ex.
  `candidate:1 1 UDP 2130706431 192.168.1.5 54321 typ host`. Le serveur ne
  retient que la composante **RTP (1) UDP** de type `host`/`srflx` et, si sa
  priorité dépasse celle du candidat courant, **reconfigure la cible d'envoi**.
  À appeler pour chaque candidat arrivant *après* le SDP initial. Combiné à
  l'apprentissage d'adresse par STUN entrant. (Il n'y a pas d'agent ICE complet :
  pas d'appairage ni de connectivity checks priorisés.)
- **`EndpointStartRTPTimeout`** (watchdog d'inactivité RTP) : `timeoutMs > 0`
  **arme** le watchdog (seuil en ms) ; `timeoutMs == 0` le **désarme**. À armer
  **juste après l'émission du SDP answer** (voir call flow §9) : le chrono part
  de cet instant, ce qui évite les faux positifs pendant la sonnerie et détecte
  aussi le cas « appel répondu mais aucun média reçu ». Le dépassement émet un
  `EndpointDisconnectedEvent` (type 6). Désarmer sur mise en attente
  (`sendonly`/hold) puis ré-armer à la reprise. La propriété RTP `rtpTimeout`
  (via `EndpointSetRTPProperties`) ne fait que **pré-régler le seuil** sans
  armer.

### 6.8 Audio mixers

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `AudioMixerCreate` | `i sessionId, s name` | `[ int mixerId ]` |
| `AudioMixerDelete` | `i sessionId, i mixerId` | `[]` |
| `AudioMixerPortCreate` | `i sessionId, i mixerId, s name` | `[ int portId ]` |
| `AudioMixerPortSetCodec` | `i sessionId, i mixerId, i portId, i codec` | `[]` |
| `AudioMixerPortDelete` | `i sessionId, i mixerId, i portId` | `[]` |
| `AudioMixerPortAttachToEndpoint` | `i sessionId, i mixerId, i portId, i endpointId` | `[]` |
| `AudioMixerPortAttachToPlayer` | `i sessionId, i mixerId, i portId, i playerId` | `[]` |
| `AudioMixerPortDettach` | `i sessionId, i mixerId, i portId` | `[]` |

`codec` = `AudioCodec::Type` (§4).

### 6.9 Video mixers et mosaïques

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `VideoMixerCreate` | `i sessionId, s name` | `[ int mixerId ]` |
| `VideoMixerDelete` | `i sessionId, i mixerId` | `[]` |
| `VideoMixerPortCreate` | `i sessionId, i mixerId, s name, i mosaicId` | `[ int portId ]` |
| `VideoMixerPortDelete` | `i sessionId, i mixerId, i portId` | `[]` |
| `VideoMixerPortSetCodec` | `i sessionId, i mixerId, i portId, i codec, i size, i fps, i bitrate, i intraPeriod` | `[]` |
| `VideoMixerPortAttachToEndpoint` | `i sessionId, i mixerId, i portId, i endpointId` | `[]` |
| `VideoMixerPortAttachToPlayer` | `i sessionId, i mixerId, i portId, i playerId` | `[]` |
| `VideoMixerPortDettach` | `i sessionId, i mixerId, i portId` | `[]` |
| `VideoMixerMosaicCreate` | `i sessionId, i mixerId, i comp, i size` | `[ int mosaicId ]` |
| `VideoMixerMosaicDelete` | `i sessionId, i mixerId, i mosaicId` | `[]` |
| `VideoMixerMosaicSetSlot` | `i sessionId, i mixerId, i mosaicId, i num, i portId` | `[]` |
| `VideoMixerMosaicSetCompositionType` | `i sessionId, i mixerId, i mosaicId, i comp, i size` | `[]` |
| `VideoMixerMosaicSetOverlayPNG` | `i sessionId, i mixerId, i mosaicId, s overlayPath` | `[]` |
| `VideoMixerMosaicResetOverlay` | `i sessionId, i mixerId, i mosaicId` | `[]` |
| `VideoMixerMosaicAddPort` | `i sessionId, i mixerId, i mosaicId, i portId` | `[]` |
| `VideoMixerMosaicRemovePort` | `i sessionId, i mixerId, i mosaicId, i portId` | `[]` |

- `codec` = `VideoCodec::Type`, `comp` = `Mosaic::Type`, `size` = code de taille
  (§4).
- `bitrate` en bits/s, `fps` en images/s, `intraPeriod` en nombre d'images entre
  deux images clés.
- `VideoMixerMosaicSetSlot` : `num` = index du slot dans la mosaïque, `portId` =
  port vidéo à y afficher (ou valeur de « verrouillage »/vide selon convention).
- `VideoMixerMosaicSetOverlayPNG` : `overlayPath` = chemin serveur d'un PNG.

### 6.10 Audio transcoders

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `AudioTranscoderCreate` | `i sessionId, s name` | `[ int transcoderId ]` |
| `AudioTranscoderDelete` | `i sessionId, i transcoderId` | `[]` |
| `AudioTranscoderSetCodec` | `i sessionId, i transcoderId, i codec, S properties` | `[]` |
| `AudioTranscoderAttachToEndpoint` | `i sessionId, i transcoderId, i endpointId` | `[]` |
| `AudioTranscoderDettach` | `i sessionId, i transcoderId` | `[]` |

`codec` = `AudioCodec::Type`. `properties` = struct clé→valeur (chaînes).
`AudioTranscoderAttachToEndpoint` attache le transcodeur sur le flux **audio**
de l'endpoint.

### 6.11 Video transcoders

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `VideoTranscoderCreate` | `i sessionId, s name` | `[ int videoTranscoderId ]` |
| `VideoTranscoderDelete` | `i sessionId, i videoTranscoderId` | `[]` |
| `VideoTranscoderSetCodec` | `i sessionId, i videoTranscoderId, i codec, i size, i fps, i bitrate, i intraPeriod, S properties` | `[]` |
| `VideoTranscoderFPU` | `i sessionId, i videoTranscoderId` | `[]` |
| `VideoTranscoderAttachToEndpoint` | `i sessionId, i videoTranscoderId, i endpointId` | `[]` |
| `VideoTranscoderDettach` | `i sessionId, i videoTranscoderId` | `[]` |

`VideoTranscoderFPU` = Fast Picture Update (force une image clé), typiquement en
réponse à un `ExternalFIRRequestedEvent`.

### 6.12 Connexions média génériques (WebRTC / ICE)

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `GetMediaCandidates` | `i sessionId, i endpointId, i protocol, i media` | `[ string url ]` |
| `ConfigureMediaConnection` | `i sessionId, i endpointId, i media, i role, i protocol, s token, s expectedPayload` | `[]` |

- `protocol` = `MediaFrame::MediaProtocol`, `media` = `MediaFrame::Type`,
  `role` = `MediaFrame::MediaRole` (§4).
- `GetMediaCandidates` renvoie une chaîne (URL / description de candidats) pour
  le transport donné (ex. candidats ICE/WS).
- `ConfigureMediaConnection` : `token` d'association de la connexion,
  `expectedPayload` = payload attendu.

---

## 7. Format des statistiques (`EndpointGetStatistics`)

`returnVal` est un **tableau** : un tuple par média actif de l'endpoint. Chaque
tuple a la forme (`(siiiiiiiiiii)`, réf. `xmlserialize()` dans
`mcu/src/xmlhandler.cpp`) :

| Pos | Type | Champ |
|-----|------|-------|
| 0 | string | media (nom du média : `"audio"`, `"video"`, `"text"`) |
| 1 | int | isReceiving (0/1) |
| 2 | int | isSending (0/1) |
| 3 | int | lostRecvPackets |
| 4 | int | numRecvPackets |
| 5 | int | numSendPackets |
| 6 | int | totalRecvBytes |
| 7 | int | totalSendBytes |
| 8 | int | bwOut (bande passante sortante) |
| 9 | int | bwIn (bande passante entrante) |
| 10 | int | sendingCodec (code codec) |
| 11 | int | receivingCodec (code codec) |

---

## 8. Notes pour l'intégration du contrôleur SIP

- **Enveloppe** : toujours vérifier `returnCode == 1` avant d'exploiter
  `returnVal` ; sinon lire `errorMsg`.
- **File d'événements** : prévoir un process/tâche dédié qui maintient le GET
  *chunked* sur `/events/jsr309/<queueId>` ouvert et décode chaque
  `<methodResponse>` reçu ; gérer le keep-alive `\r\n` (pas un événement).
- **Encodage** : envoyer les chaînes en UTF-8.
- **rtpMap / properties** : ce sont des `struct` XML-RPC ; en Elixir, une map
  `%{ "clé" => valeur }`. Attention : dans `rtpMap`, la **clé est le payload
  type** (string) et la **valeur le code codec** (integer).
- **Ordre des paramètres** : respecter strictement l'ordre du tableau ci-dessus
  (l'API ne nomme pas les paramètres, c'est du positionnel).
- **Négatif = échec** : à la création, un id < 0 (ou `returnCode 0`) indique un
  échec — ne pas le stocker.

---

## 9. Call flows détaillés (SDP offer/answer ↔ RPC)

Le media server **ne parle pas SIP** : c'est le contrôleur SIP qui gère la
signalisation et le SDP. Le serveur ne connaît que ses commandes XML-RPC. Cette
section décrit la correspondance exacte entre le SDP offer/answer et les appels
RPC, dans l'ordre, pour un appel **entrant** puis **sortant**.

Toutes les opérations média sont **par `media`** (`0`=audio, `1`=video,
`2`=text) : pour un appel audio+vidéo, répéter les étapes média pour chaque
`media` présent dans le SDP. On note `EP` = `endpointId`, `S` = `sessionId`.

### 9.0 Correspondance SDP ↔ RPC

| Élément SDP | Sens | RPC |
|-------------|------|-----|
| `m=<media> <port> …` **local** (le nôtre) | ← | `port` = retour de `EndpointStartReceiving` |
| `c=`/candidat `host` **local** | ← | `GetMediaCandidates(RTP, media)` → `rtp://ip:port` |
| payload types **locaux** (ce qu'on accepte) | ← | **clés** de `returnVal[1]` de `EndpointStartReceiving` (proposés via `rtpMap`, PT non supportés **filtrés** du retour) |
| `a=fmtp:<pt>` **local** (nos paramètres) | ← | `returnVal[1]` de `EndpointStartReceiving` (`{ "<pt>":"<params>" }`, valeur `""` = codec accepté **sans** `a=fmtp`) |
| paramètres codec **locaux** à imposer (ex. `h264.profile-level-id`) | → | `EndpointSetRTPProperties(codec.<x>.<param>)` **avant** `EndpointStartReceiving` |
| `a=crypto` **local** (SRTP-SDES) | ↔ | clé fournie à `EndpointSetLocalCryptoSDES` = celle publiée dans notre SDP |
| `a=fingerprint`/`a=setup` **local** (DTLS) | ← | `EndpointGetLocalCryptoDTLSFingerprint(hash)` |
| `a=ice-ufrag`/`a=ice-pwd` **local** | ↔ | credentials fournis à `EndpointSetLocalSTUNCredentials` = ceux publiés |
| `m=`/`c=` **distant** (ip:port) | → | `EndpointStartSending(media, ip, port, rtpMap)` |
| payload types **distants** (ce qu'on envoie) | → | `rtpMap` de `EndpointStartSending` |
| `a=crypto` **distant** (SRTP-SDES) | → | `EndpointSetRemoteCryptoSDES(suite, key)` |
| `a=setup`/`a=fingerprint` **distant** (DTLS) | → | `EndpointSetRemoteCryptoDTLS(setup, hash, fingerprint)` |
| `a=ice-ufrag`/`a=ice-pwd` **distant** | → | `EndpointSetRemoteSTUNCredentials(user, pwd)` |
| `a=rtcp-mux`, ssrc, extensions… **distant** | → | `EndpointSetRTPProperties(properties)` |
| candidats trickle **distants** (post-SDP) | → | `EndpointAddICECandidate(candidate)` |

> Une seule pile de sécurité par média selon le SDP : **SDES** (SRTP par clé
> dans le SDP), **DTLS-SRTP** (fingerprint + handshake), ou **rien** (RTP clair).
> Poser les credentials/clefs **avant** de démarrer le média (émission/réception)
> pour qu'aucun paquet ne circule avant l'établissement des clés.

### 9.1 Appel entrant — media server = UAS (on reçoit l'offre, on renvoie la réponse)

```
Pair (offre) ──INVITE+SDP──▶ contrôleur SIP ──XML-RPC──▶ mediaserver
                             contrôleur SIP ◀─200 OK+SDP── (réponse construite ici)
```

Une seule fois (réutilisable entre appels) : `EventQueueCreate` → `queueId,
sourceName` + ouvrir le long-poll sur `sourceName`.

Par appel :

1. `MediaSessionCreate(tag, queueId)` → `S`
2. `EndpointCreate(S, name, audio, video, text)` → `EP`
   (flags = médias présents dans l'offre)

Pour **chaque média** de l'offre :

3. `EndpointSetRTPProperties(S, EP, media, {rtcp-mux, …})` — attributs de l'offre
4. Sécurité **distante** (depuis l'offre), selon le cas :
   - SDES : `EndpointSetRemoteCryptoSDES(S, EP, media, suite, key)`
   - DTLS : `EndpointSetRemoteCryptoDTLS(S, EP, media, setup, hash, fingerprint)`
   - ICE : `EndpointSetRemoteSTUNCredentials(S, EP, media, ufrag, pwd)`
5. Sécurité **locale** (pour la réponse) :
   - SDES : `EndpointSetLocalCryptoSDES(S, EP, media, suite, key)` (clé qu'on
     publiera)
   - DTLS : `EndpointGetLocalCryptoDTLSFingerprint(hash)` → fingerprint pour notre SDP
   - ICE : `EndpointSetLocalSTUNCredentials(S, EP, media, ufrag, pwd)` (à publier)
6. `EndpointStartReceiving(S, EP, media, rtpMap)` → `[recvPort, fmtpByPt]`.
   `rtpMap` = les PT de l'offre qu'on veut accepter ; `fmtpByPt` (`returnVal[1]`)
   liste **chaque PT réellement accepté** (clé présente, valeur `""` si le codec
   n'a pas de `fmtp`) et **omet les PT filtrés** — c'est la liste autoritative des
   PT retenus, avec leurs paramètres `a=fmtp` (§6.7).
7. `GetMediaCandidates(S, EP, RTP=0, media)` → `rtp://ip:port` (adresse locale)
8. `EndpointStartSending(S, EP, media, remoteIp, remotePort, rtpMap)`
   (ip/port pris dans l'offre)

Puis :

9. **Router le média** (selon le scénario) : pont vers l'autre patte
   (`EndpointAttachToEndpoint`), mixers (`…AttachToAudioMixerPort` /
   `…VideoMixerPort`), player, transcoder… (cf. §9.3).
10. **Construire et envoyer le 200 OK** : `recvPort` (étape 6) + candidat local
    (étape 7) + crypto locale (étape 5). Les **payload types de la m-line** =
    les **clés** de `fmtpByPt` (étape 6) ; pour chaque clé de valeur non vide,
    ajouter une ligne `a=fmtp:<pt> <valeur>` (une clé à valeur `""` = codec
    accepté sans `a=fmtp`).
11. `EndpointStartRTPTimeout(S, EP, media, timeoutMs)` — **après** l'envoi du
    200 OK, pour armer le watchdog (voir §6.7).
12. Trickle : à chaque candidat distant reçu ensuite,
    `EndpointAddICECandidate(S, EP, media, candidate)`.

> Étapes 3-8 : l'ordre entre « sécurité » et « start » compte (clés avant média).
> `EndpointStartSending` peut être appelé avant l'envoi du 200 OK (l'offre porte
> déjà l'adresse distante) — **mais l'armement (étape 11) doit venir après**.

### 9.2 Appel sortant — media server = UAC (on génère l'offre, on reçoit la réponse)

```
contrôleur SIP ──INVITE+SDP(offre)──▶ Pair
contrôleur SIP ◀──200 OK+SDP(réponse)── Pair
contrôleur SIP ──ACK──▶ Pair
```

Une seule fois : `EventQueueCreate` + long-poll (comme §9.1).

Construction de l'**offre** :

1. `MediaSessionCreate(tag, queueId)` → `S`
2. `EndpointCreate(S, name, audio, video, text)` → `EP`

Pour **chaque média** offert :

3. Sécurité **locale** (pour l'offre) :
   - SDES : `EndpointSetLocalCryptoSDES(S, EP, media, suite, key)`
   - DTLS : `EndpointGetLocalCryptoDTLSFingerprint(hash)`
   - ICE : `EndpointSetLocalSTUNCredentials(S, EP, media, ufrag, pwd)`
4. `EndpointStartReceiving(S, EP, media, rtpMap)` → `[recvPort, fmtpByPt]`.
   `fmtpByPt` (`returnVal[1]`) liste **chaque PT accepté** (clé présente, valeur
   `""` si sans `fmtp`) et exprime nos **capacités brutes** (config + défauts) ;
   la contrainte du pair n'arrive qu'à l'étape 7 (voir phase 5).
5. `GetMediaCandidates(S, EP, RTP=0, media)` → adresse locale
6. **Construire et envoyer l'INVITE** avec l'offre (recvPort + candidat + crypto
   locale). Les **payload types de la m-line offerte** = les **clés** de
   `fmtpByPt` ; pour chaque clé de valeur non vide, une ligne
   `a=fmtp:<pt> <valeur>` (une clé à valeur `""` = codec offert sans `a=fmtp`).

À réception du **200 OK** (réponse du pair) :

7. `EndpointSetRTPProperties(S, EP, media, {rtcp-mux, …})` — attributs de la réponse
8. Sécurité **distante** (depuis la réponse) : `EndpointSetRemoteCryptoSDES` /
   `…RemoteCryptoDTLS` / `…RemoteSTUNCredentials` selon le cas
9. `EndpointStartSending(S, EP, media, remoteIp, remotePort, rtpMap)`
   (ip/port de la réponse ; `rtpMap` = codecs réellement retenus)
10. **Envoyer l'ACK**.
11. Router le média (§9.3).
12. `EndpointStartRTPTimeout(S, EP, media, timeoutMs)` — **après** traitement de
    la réponse / envoi de l'ACK.
13. Trickle : `EndpointAddICECandidate(...)` pour les candidats reçus ensuite.

### 9.3 Routage du média — pont B2B et conférence

**Pont entre deux pattes** (`A` et `B`, deux endpoints de la même session) —
attacher **dans les deux sens et pour chaque média** :

```
EndpointAttachToEndpoint(S, A, B, AUDIO)   // média audio de B → A
EndpointAttachToEndpoint(S, B, A, AUDIO)   // média audio de A → B
EndpointAttachToEndpoint(S, A, B, VIDEO)   // idem vidéo
EndpointAttachToEndpoint(S, B, A, VIDEO)
```

> `EndpointAttachToEndpoint(S, endpointId, sourceId, media)` route le média de
> **`sourceId` vers `endpointId`**. Un pont bidirectionnel = deux appels.

**Conférence** (mélange) : créer les mixers une fois
(`AudioMixerCreate`/`VideoMixerCreate`), puis par participant un port
(`AudioMixerPortCreate`/`VideoMixerPortCreate`) attaché à l'endpoint
(`AudioMixerPortAttachToEndpoint` / `VideoMixerPortAttachToEndpoint`). La
composition vidéo passe par une mosaïque (`VideoMixerMosaic*`, §6.9).

### 9.4 Renégociation (re-INVITE) et mise en attente

- **Mise en attente** (`sendonly`/`recvonly`, le média entrant s'arrête
  légitimement) : **désarmer** le watchdog par
  `EndpointStartRTPTimeout(S, EP, media, 0)` pour éviter un faux
  `EndpointDisconnectedEvent`. À la reprise, ré-armer avec `timeoutMs > 0`.
- **Changement d'adresse/codec distant** : `EndpointStopSending(S, EP, media)`
  puis `EndpointStartSending(...)` avec la nouvelle adresse / le nouveau `rtpMap`.
- **Nouveaux candidats** (trickle) : `EndpointAddICECandidate(...)`.

### 9.5 Terminaison

Par média puis par objet, dans l'ordre :

```
EndpointStopSending(S, EP, media)          // par média
EndpointStopReceiving(S, EP, media)
EndpointDettach(S, EP, media)              // si attaché
EndpointDelete(S, EP)
MediaSessionDelete(S)
EventQueueDelete(queueId)                  // si la file n'est plus réutilisée
```

Le `MediaSessionDelete` libère en cascade les objets restants de la session ;
détruire proprement les endpoints d'abord reste préférable.

### 9.6 Points de vigilance pour le contrôleur

- **Armement du watchdog** : toujours **après** l'émission du answer (entrant) ou
  le traitement du answer + ACK (sortant). Jamais pendant la sonnerie. Désarmer
  sur hold. Traiter `EndpointDisconnectedEvent` (type 6) comme une perte de
  média (raccrocher / réémettre selon la politique).
- **`sessionTag`** : le `tag` passé à `MediaSessionCreate` est renvoyé tel quel
  dans chaque événement — l'utiliser pour router l'événement vers le bon appel.
- **`playerTag`/`recorderTag`** : de même, ce sont les `name` passés à la
  création ; les events de cycle de vie (1, 3, 4, 5) les portent.
- **rtpMap asymétrique** : le `rtpMap` de `StartReceiving` (ce qu'on accepte) et
  celui de `StartSending` (ce qu'on émet) peuvent différer selon la négociation.
- **Idempotence / erreurs** : vérifier `returnCode == 1` après **chaque** appel ;
  en cas d'échec en cours de montage, dérouler la terminaison (§9.5) pour ne pas
  fuiter de session/endpoint côté serveur.

### 9.7 Négociation des codecs et des `fmtp` (dynamique cible)

> 🔷 Le **filtrage des codecs** et la **production du `fmtp` local** (sens
> réception) sont **livrés** (phases 1-4 de `nego_fmtp.md`) : `EndpointStartReceiving`
> renvoie `[recvPort, fmtpByPt]` (§6.7). Reste en cours (phase 5) la
> **négociation entrante** : parsing de la clé `codec.<x>.fmtp` reçue du pair pour
> **contraindre l'émission**. Un contrôleur tolérant teste la présence de
> `returnVal[1]`.

**Principe.** Le média serveur devient l'autorité sur les codecs : il **filtre**
la `rtpMap` proposée par ses codecs réellement supportés et **produit les
paramètres `fmtp`** du SDP local. Le contrôleur SIP n'a plus à coder en dur
`profile-level-id`, `sprop-parameter-sets`, `useinbandfec`, etc.

**Rappel de sémantique SDP.** Un SDP décrit la capacité **en réception** de celui
qui l'émet. L'offer (INVITE) = ce que le distant sait **décoder** ; l'answer
(200 OK) = ce que **nous** savons décoder. L'answer est un **sous-ensemble** de
l'offer (RFC 3264) : l'offer sert donc de **menu** dans lequel on pioche notre
propre réception, en réutilisant les **numéros de PT de l'offer**. Le `fmtp` de
l'offer (capacité de décodage du distant) contraint surtout **notre émission** ;
notre `fmtp` d'answer reste l'expression de **notre** capacité de décodage.

**Appel entrant (UAS — on reçoit l'INVITE, on construit l'answer).** Le `fmtp`
distant est connu dès l'offer → le pousser **avant** `StartReceiving` :

```
Par média de l'offre :
1. EndpointSetRTPProperties(S, EP, media, { "codec.<x>.fmtp": "<fmtp de l'offre>", … })
2. EndpointStartReceiving(S, EP, media, rtpMap_offre)
     → returnVal = [ recvPort, { "<pt>": "<fmtp local>" } ]   # clés = PT acceptés
3. Construire l'answer : m-line = clés de returnVal[1] ; pour chaque valeur non vide,
   une ligne a=fmtp:<pt> <valeur> ("" = codec accepté sans a=fmtp) ; + recvPort,
   crypto/candidats locaux.
4. Envoyer le 200 OK.
5. EndpointStartSending(S, EP, media, remoteIp, remotePort, rtpMap_answer)
     → l'émission respecte d'emblée les contraintes du pair (étape 1).
6. EndpointStartRTPTimeout(...)   # après le 200 OK
```

**Appel sortant (UAC — on génère l'INVITE, on reçoit l'answer).** Aucun `fmtp`
distant à `StartReceiving` → l'ingestion vient **avant** `StartSending` :

```
Par média offert :
1. EndpointStartReceiving(S, EP, media, rtpMap_qu_on_veut_offrir)
     → returnVal = [ recvPort, { "<pt>": "<fmtp local = nos capacités>" } ]
       (clés = PT acceptés ; H.264 : sprop-parameter-sets best-effort / omis — pas encore encodé)
2. Construire et envoyer l'INVITE : m-line = clés de returnVal[1] ; pour chaque
   valeur non vide, une ligne a=fmtp:<pt> <valeur> ("" = offert sans a=fmtp).
À réception du 200 OK :
3. EndpointSetRTPProperties(S, EP, media, { "codec.<x>.fmtp": "<fmtp de l'answer>", … })
4. EndpointStartSending(S, EP, media, remoteIp, remotePort, rtpMap_answer)
     → l'encodeur est configuré selon le fmtp du pair (étape 3).
5. Envoyer l'ACK ; 6. EndpointStartRTPTimeout(...)
```

**Points de vigilance.**
- Le `rtpMap` d'entrée de `StartReceiving` est le **menu** ; le média serveur
  peut en retirer des PT (non supportés) → se fier à `returnVal[1]` pour la
  m-line, pas à ce qu'on a proposé.
- `codec.<x>.fmtp` et les propriétés transport (`rtcp-mux`, `ssrc`…) coexistent
  dans le même `EndpointSetRTPProperties` : le serveur route les clés `codec.*`
  vers la négociation et les autres vers le transport.
- Les paramètres `fmtp` renvoyés sont **sans** `a=fmtp:<pt> ` : le contrôleur SIP
  préfixe lui-même avec le PT (qu'il connaît déjà, clé de la struct).

---

*Source : `mcu/src/jsr309/xmlrpcjsr309.cpp` (table `jsr309CmdList`),
`mcu/src/xmlhandler.cpp`, `mcu/src/xmlstreaminghandler.cpp`,
`mcu/src/jsr309/*`. Codecs : `mcu/include/codecs.h`, `mcu/include/mosaic.h`,
`libmedikit/medkit/media.h`.*
