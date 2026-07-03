# API XML-RPC JSR-309 du mediaserver

Documentation de l'API XML-RPC exposée par `mcu/src/jsr309/xmlrpcjsr309.cpp`
(table de commandes `jsr309CmdList`, montée par `main.cpp` sur le gestionnaire
`JSR309Manager`).

Cette interface est le pilotage « bas niveau » du media server, calqué sur le
modèle JSR-309 (Media Server Control API). C'est elle qu'utilise le client Java
`XmlRpcMcuClient` / la couche `jsr309impl/`. Elle est la cible visée pour
l'implémentation de l'interface Mediaserveur du projet
[elixip](https://github.com/neutrino38/elixip).

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

### Cycle de vie type d'un appel entrant

1. `EventQueueCreate` → `queueId`
2. `MediaSessionCreate(tag, queueId)` → `sessionId`
3. `EndpointCreate(sessionId, name, audio, video, text)` → `endpointId`
4. Sécurité : `EndpointSetLocalCryptoSDES` / `EndpointSetRemoteCryptoSDES` ou
   DTLS / STUN selon le transport.
5. `EndpointStartReceiving(sessionId, endpointId, media, rtpMap)` → port local
   d'écoute (à publier dans le SDP local).
6. `EndpointStartSending(sessionId, endpointId, media, ip, port, rtpMap)` (à
   partir du SDP distant).
7. Attaches média (vers un mixer, un player, un transcoder…).
8. Fermeture : `EndpointStopSending` / `EndpointStopReceiving` /
   `EndpointDelete`, puis `MediaSessionDelete`, puis `EventQueueDelete`.

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

1. `EventQueueCreate` → `queueId`.
2. Passer ce `queueId` à `MediaSessionCreate` : les événements de la session y
   seront routés.
3. Ouvrir en parallèle `GET http://<host>:8080/events/jsr309/<queueId>`.

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

| Type | Nom | Tuple |
|------|-----|-------|
| 1 | PlayerEndOfFileEvent | `(int type, string sessionTag, string playerTag)` |
| 2 | ExternalFIRRequestedEvent | `(int type, string sessionTag, int joinableId, int media, int role)` |

- **PlayerEndOfFileEvent** : un `Player` a atteint la fin du fichier.
  `playerTag` = nom passé à `PlayerCreate`.
- **ExternalFIRRequestedEvent** : un endpoint distant a demandé une image
  complète (Full Intra Request). `joinableId` = `endpointId` concerné,
  `media`/`role` selon les énumérations §4. À traiter typiquement par un
  `VideoTranscoderFPU` ou une régénération d'image clé.

Réf. : `mcu/src/jsr309/JSR309Event.h`, `MediaSession.h` (PlayerEndOfFileEvent),
`RTPEndpoint.cpp` (ExternalFIRRequestedEvent).

---

## 6. Référence des méthodes

Pour chaque méthode : la signature des paramètres (dans l'ordre) et le contenu
de `returnVal` en cas de succès. `returnVal = []` signifie tableau vide.

### 6.1 Files d'événements

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `EventQueueCreate` | — | `[ int queueId ]` |
| `EventQueueDelete` | `i queueId` | `[]` |

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
`PlayerEndOfFileEvent` (§5).

### 6.4 Recorders (enregistrement)

| Méthode | Paramètres | `returnVal` |
|---------|-----------|-------------|
| `RecorderCreate` | `i sessionId, s name` | `[ int recorderId ]` |
| `RecorderRecord` | `i sessionId, i recorderId, s filename` | `[]` |
| `RecorderStop` | `i sessionId, i recorderId` | `[]` |
| `RecorderDelete` | `i sessionId, i recorderId` | `[]` |
| `RecorderAttachToEndpoint` | `i sessionId, i recorderId, i endpointId, i media` | `[]` |
| `RecorderAttachToAudioMixerPort` | `i sessionId, i recorderId, i mixerId, i portId` | `[]` |
| `RecorderAttachToVideoMixerPort` | `i sessionId, i recorderId, i mixerId, i portId` | `[]` |
| `RecorderDettach` | `i sessionId, i recorderId, i media` | `[]` |

`media` = `MediaFrame::Type` (§4).

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
| `EndpointStartReceiving` | `i sessionId, i endpointId, i media, S rtpMap` | `[ int recvPort ]` |
| `EndpointStopReceiving` | `i sessionId, i endpointId, i media` | `[]` |
| `EndpointRequestUpdate` | `i sessionId, i endpointId, i media` | `[]` |

- **`rtpMap`** : struct XML-RPC dont **chaque clé est un payload type** (chaîne,
  ex. `"96"`) et **chaque valeur est un code codec entier** (`i`) selon
  `AudioCodec::Type` / `VideoCodec::Type` / `TextCodec::Type`. Exemple :
  `{ "0": 0, "8": 8, "101": 100 }` (PCMU, PCMA, telephone-event).
- **`properties`** (`EndpointSetRTPProperties`) : struct XML-RPC clé→valeur, les
  deux **chaînes** (paramètres RTP additionnels : rtcp-mux, ssrc, etc.).
- `EndpointStartReceiving` renvoie le **port local** ouvert à publier dans le
  SDP local.
- `EndpointRequestUpdate` : force une mise à jour / image clé (FIR) vers ce
  média.

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

## 8. Notes pour l'intégration elixip

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

*Source : `mcu/src/jsr309/xmlrpcjsr309.cpp` (table `jsr309CmdList`),
`mcu/src/xmlhandler.cpp`, `mcu/src/xmlstreaminghandler.cpp`,
`mcu/src/jsr309/*`. Codecs : `mcu/include/codecs.h`, `mcu/include/mosaic.h`,
`libmedikit/medkit/media.h`.*
