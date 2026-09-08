# `/status/general` — ce que le serveur sait de lui-même

Ce point HTTP décrit le mediaserver : sa version, ses capacités réelles, son
chiffrement, son adressage et sa charge.

Il existe pour une raison précise. Un contrôleur qui ne peut pas **demander** ce
que le serveur sait faire finit par le **déclarer** de son côté. Cette copie
dérive. C'est arrivé : elixip offrait H.264 et VP8 alors que le serveur portait
AV1 depuis des mois, et un appel AV1 ↔ AV1 est mort en 488 avec un audio parfait
des deux côtés.

## 1. Interroger

```sh
# JSON — pour un contrôleur ou un script
curl http://mediaserver:8080/status/general

# Texte aligné — pour un humain
curl 'http://mediaserver:8080/status/general?format=text'
```

Deux rendus, **un seul état**. Le serveur collecte les faits une fois, puis les
rend deux fois. Les deux vues ne peuvent donc pas se contredire.

Comment le format est choisi :

| Requête | Rendu |
|---|---|
| `?format=text` | texte |
| `?format=json` | JSON |
| `Accept:` cite `text/html` ou `text/plain`, sans `application/json` | texte |
| tout le reste (`curl` par défaut, `Accept: */*`, aucun en-tête) | JSON |

Un navigateur reçoit donc la version lisible. Un contrôleur reçoit du JSON.

Le point ne demande **aucune authentification**, comme les autres `/status/*`.
Le réserver au réseau d'administration.

## 2. La réponse JSON

```json
{
  "server": {
    "product": "mediaserver",
    "version": "1.14.0",
    "buildDate": "2026-08-22 22:00:00 +0200",
    "hostname": "mcu-01.example.net",
    "pid": 4711,
    "startedAt": "2026-08-27T20:13:37Z",
    "uptimeSecs": 274353,
    "ffmpeg": "5.1.10"
  },
  "capabilities": {
    "audio": { "decode": ["OPUS","PCMU","..."], "encode": ["OPUS","PCMU","..."] },
    "video": { "decode": ["H264","VP8","AV1","VP6"], "encode": ["H264","VP8","AV1"] },
    "text":  { "rfc4103": true, "rfc4103Redundancy": true,
               "rfc8865": true, "websocket": true },
    "hardware": { "vaapi": false },
    "bfcp": true
  },
  "security": {
    "modes": ["none","sdes-srtp","dtls-srtp"],
    "sdesSuites": ["AES_CM_128_HMAC_SHA1_80","..."],
    "dtls": { "available": true,
              "srtpSuite": "AES_CM_128_HMAC_SHA1_80",
              "fingerprintSha256": "03:E9:..." }
  },
  "network": {
    "defaultProfile": "publicv4",
    "profiles": [
      { "name": "publicv4", "available": true, "default": true,
        "bindAddress": "172.21.105.71", "announcedAddress": "203.0.113.9" }
    ],
    "rtpPortRange": { "min": 49152, "max": 65535 },
    "websocketUrl": "ws://203.0.113.9:19090",
    "moteli": false,
    "eventQueueExpiresSecs": 60
  },
  "load": { "conferences": 1, "participants": 3, "mediaSessions": 2 }
}
```

## 3. Les champs qui demandent une explication

### 3.1 `decode` et `encode` ne sont pas la même liste

C'est le point le plus important de cette réponse.

`decode` = ce que le serveur sait **recevoir**. `encode` = ce qu'il sait
**émettre**. Les deux ne coïncident pas : ffmpeg décode des codecs qu'il
n'encode pas. **VP6** est le cas d'école — il arrive dans les flux RTMP
entrants, et aucun encodeur VP6 n'existe.

Un contrôleur doit donc lire :

- `decode` pour construire ce qu'il **offre** en réception ;
- `encode` pour savoir ce qu'il peut **demander** au serveur de produire.

Confondre les deux, c'est demander au serveur d'émettre un codec qu'il ne sait
que recevoir.

Les listes viennent des fabriques de libmedikit
(`AudioCodecFactory`/`VideoCodecFactory`), seule autorité sur ce que la
bibliothèque compilée sait vraiment faire. Elles sont **ordonnées par
préférence**. Elles ne sont écrites à la main nulle part.

> À ne pas confondre avec la méthode XML-RPC `GetSupportedCodecs` de l'API
> `/mcu`, qui est un tableau figé de 8 codecs audio écrit à la main, sans OPUS,
> et qui répond *media not supported* pour la vidéo. `/status/general` est la
> réponse juste.

### 3.2 Les transports du texte temps réel

| Champ | Transport | Disponibilité |
|---|---|---|
| `rfc4103` | T.140 sur RTP | toujours (codec natif) |
| `rfc4103Redundancy` | redondance RED du RFC 4103 | toujours |
| `rfc8865` | T.140 sur data channel WebRTC | **suit `security.dtls.available`** |
| `websocket` | texte sur WebSocket | toujours |

`rfc8865` dépend du DTLS parce que le canal **est** du SCTP transporté dans des
records DTLS. Sans certificat lisible, il n'y a pas de data channel. La
dépendance est structurelle, pas une option de configuration.

### 3.3 Le chiffrement dépend du certificat

`modes` contient toujours `none` (une patte en clair reste acceptée) et
`sdes-srtp` (les clés arrivent par le SDP). `dtls-srtp` n'apparaît **que si le
certificat et la clé ont été lus au démarrage** (`/etc/mediaserver/mcu.crt` et
`mcu.key` par défaut).

`sdesSuites` liste les quatre suites que `SetLocalCryptoSDES` accepte
réellement. `dtls.srtpSuite` n'en donne qu'une : le DTLS annonce du GCM dans son
extension `use_srtp`, mais l'export de clés est fixé à la longueur
`AES_CM_128` — c'est bien la seule suite qu'une patte DTLS obtient.

### 3.4 Les profils d'adressage

Les quatre profils sont **toujours listés**, disponibles ou non. Un profil
absent de la réponse serait indiscernable d'un profil que le serveur ignore, et
le contrôleur ne pourrait pas trancher.

Chaque profil porte **deux adresses distinctes** :

- `bindAddress` : celle que la socket lie, donc celle qui choisit l'interface ;
- `announcedAddress` : celle que le pair verra dans le SDP.

Les deux sont égales, **sauf** pour `publicv4` derrière NAT. Un `bindAddress`
**vide** n'est pas une panne : c'est l'écoute sur toutes les interfaces, le cas
nominal d'une adresse annoncée qui vit sur le routeur. Le rendu texte l'affiche
`*` pour cette raison.

C'est la même table que celle lue par les jambes RTP et par la méthode
`GetNetworkProfiles`, pas une copie. Modèle complet pour l'exploitant :
`NETWORK-CONFIGURATION.md`.

### 3.5 `eventQueueExpiresSecs` est un contrat

C'est le délai (`--event-queue-expires`, 60 s par défaut) au bout duquel une
file d'événements **sans lecteur** est détruite, avec les conférences et les
sessions JSR-309 qui la référencent.

Le contrôleur doit le lire : son long-poll est sa **preuve de vie**. `0` signifie
que l'expiration est désarmée.

### 3.6 `websocketUrl`

L'URL que le serveur délivre réellement aux participants : schéma `ws`/`wss`
selon `--websocket-secure`, hôte = adresse annoncée par défaut, surchargée par
`--websocket-host`. Même résolution que celle qui signe les URL des
participants. **Vide** si aucune adresse n'est annonçable.

### 3.7 `hardware.vaapi`

Le device VAAPI partagé est sondé une fois au démarrage. `false` = tout le
traitement vidéo se fait sur CPU. Décodage, encodage et composition de mosaïque
dérivent tous de ce même device.

## 4. Ce que ce point ne dit pas

- **Le détail des conférences** : c'est `/status/mcu` (liste) et
  `/status/mcu/<confId>` (une conférence).
- **Les paramètres `fmtp`** négociables par codec : `CODECS.md`.
- **Les compteurs par flux** (débits, pertes, gigue) : ils n'existent pas ici.

## 5. Où c'est implémenté

| Rôle | Fichier |
|---|---|
| Collecte + les deux rendus | `mcu/src/statushandler.cpp` |
| Structure des faits publiés | `mcu/include/statushandler.h` |
| Autorité des capacités codec | `third_party/fontventa/libmedikit/codecs.cpp` |
| Tests | `mcu/tests/test_status.cpp` (suite `Status`) |
