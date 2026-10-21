# Codecs et paramètres `fmtp`

Ce document dit, pour chaque codec, trois choses :

- ce que le serveur **annonce** dans son `fmtp` ;
- ce qu'il **lit** dans le `fmtp` du pair ;
- ce qui **borne son encodeur**, c'est-à-dire ce qu'il s'interdit d'émettre.

Le `fmtp` est la ligne SDP `a=fmtp:<pt> <paramètres>`. Elle précise un codec
pour un payload type (PT) donné, par exemple le profil H.264.

Les deux API de contrôle négocient de la même façon : `EndpointStartReceiving`
pour JSR-309 (`JSR-309-API.md`), `StartReceiving` pour le MCU (`MCU-API.md`).
Le code est `CodecNegotiator`, dans libmedikit (`medkit/negotiator.h`). La
conception d'origine est dans `design/nego_fmtp.md`.

## 1. Le déroulé

### 1.1 Ce que le contrôleur donne

**Notre configuration.** Le contrôleur pose des clés `codec.*` avant
`StartReceiving` : `EndpointSetRTPProperties` en JSR-309, `SetRTPProperties` en
MCU. Le serveur retire le préfixe `codec.` et garde la clé pour la négociation.
Exemple : `codec.h264.profile-level-id = 42e01f` devient
`h264.profile-level-id`.

**Le `fmtp` du pair.** Deux canaux existent :

| Canal | Forme | Portée |
|---|---|---|
| Paramètre `offer` de `StartReceiving` | `{ "fmtp": { "<pt>": "<paramètres>" } }` | **un PT** |
| Propriété `codec.<nomcodec>.fmtp` | `codec.h264.fmtp = "profile-level-id=42e01f;packetization-mode=1"` | **tous les PT** du codec |

Quand les deux existent pour un même PT, le canal par PT gagne. Il faut le
préférer : un navigateur offre H.264 sous six ou sept PT, chacun avec son propre
profil, et un seul `fmtp` par codec ne peut pas les décrire tous. Les
paramètres s'écrivent **seuls**, sans `a=fmtp:<pt> `. Leurs noms sont lus sans
tenir compte de la casse.

### 1.2 Ce que le serveur fait

À `StartReceiving`, pour chaque PT proposé :

1. Le PT est **refusé** si le serveur ne sait pas décoder son codec (§4). Pour
   l'AMR, il l'est aussi quand le pair demande une option que nous ne portons
   pas (§2.5).
2. Sinon, le serveur calcule le `fmtp` qu'il **annonce** sur ce PT, et les
   **bornes** de son encodeur pour ce PT.

### 1.3 Ce que le serveur rend

Le retour porte une struct `fmtpByPt` = `{ "<pt>": "<paramètres fmtp>" }` :
`[ recvPort, fmtpByPt ]` en JSR-309, `[ recvPort, ip, fmtpByPt ]` en MCU.

- **Chaque PT accepté est une clé**, même sans `fmtp` : la valeur est alors
  `""`. Exemple pour PCMA : `{ "8": "" }`.
- **Un PT absent est un PT refusé.** Le contrôleur ne doit pas l'annoncer.
- La valeur est le corps du `fmtp` seul. Le contrôleur écrit
  `a=fmtp:<pt> <valeur>` dans son SDP quand la valeur n'est pas vide.

### 1.4 Les bornes d'émission

**En JSR-309 seulement**, les bornes vont à ce qui produit le flux de
l'endpoint : un encodeur, un transcodeur ou un port de SpeechProcessor. Elles
sont prises sur le PT réellement émis, celui de la `rtpMap` de
`EndpointStartSending`. À défaut, elles sont prises sur le premier PT accepté
du codec.

**En MCU**, le `fmtp` annoncé suit les mêmes règles, mais les encodeurs de la
conférence n'utilisent que notre configuration. Une limite déclarée par le pair
(un niveau H.264, un débit Opus, un `mode-set` AMR) n'y est pas appliquée.

Une exception vaut pour les deux API : le mode de charge utile AMR
(`octet-align`). Il est appliqué par `RTPSession` (§2.5).

## 2. Codec par codec

### 2.1 H.264 (`VideoCodec::H264`)

| Clé `codec.*` | Défaut | Rôle |
|---|---|---|
| `h264.profile-level-id` | `42801F` | Le profil et le niveau que nous savons décoder. |

**Annoncé** :
`profile-level-id=<id>;packetization-mode=<m>;level-asymmetry-allowed=1`.

- `profile-level-id` est écrit en minuscules.
- `level-asymmetry-allowed=1` est toujours annoncé : un transcodeur décode à un
  niveau et encode à un autre.
- `sprop-parameter-sets` n'est jamais annoncé. Il n'existe qu'après la première
  image encodée, donc après `StartReceiving`.

**Lu chez le pair** (RFC 6184 §8.2.2), s'il donne un `profile-level-id` :

| Ce que dit le pair | Ce que nous annonçons | Ce qui borne l'encodeur |
|---|---|---|
| `level-asymmetry-allowed=1` | son profil, **notre** niveau | son profil, le plus bas des deux niveaux |
| pas d'asymétrie, son niveau ≤ le nôtre | son `profile-level-id` tel quel | le même |
| pas d'asymétrie, son niveau > le nôtre | son profil, **notre** niveau | son profil, notre niveau |
| `packetization-mode=1` | `packetization-mode=1` | mode 1 |
| `packetization-mode` absent ou `0` | `packetization-mode=0` | mode 0 |

**Piège** : sans `profile-level-id` chez le pair, rien n'est lu, pas même
`packetization-mode`. Nous annonçons alors notre configuration et
`packetization-mode=1`, même sur un PT que le pair a offert en mode 0.

Le troisième cas du tableau s'écarte de la RFC, qui demande de refléter ou de refuser.
Annoncer notre niveau garde la vidéo. Le journal le signale :
`offered level not decodable`.

En mode 0, l'encodeur limite chaque slice à la taille d'un paquet RTP. Il passe
alors en logiciel (libx264), car VAAPI ne sait pas borner une slice.

### 2.2 Opus (`AudioCodec::OPUS`)

| Clé `codec.*` | Défaut | Rôle |
|---|---|---|
| `opus.useinbandfec` | `0` | Correction d'erreurs dans le flux. |
| `opus.usedtx` | `0` | Émission discontinue pendant les silences. |
| `opus.maxaveragebitrate` | `0` (aucune) | Débit moyen maximal, en bit/s. |
| `opus.cbr` | `0` | Débit constant. |
| `opus.packet-loss-perc` | `10` | Pertes prévues par l'encodeur. N'apparaît pas dans le `fmtp`. |

**Annoncé** : `useinbandfec=<0|1>;usedtx=<0|1>`, toujours, puis
`;maxaveragebitrate=<n>` s'il est non nul et `;cbr=1` s'il est demandé.
Exemple par défaut : `useinbandfec=0;usedtx=0`.

**Lu chez le pair** (RFC 7587 §7) : chaque paramètre décrit ce que le pair veut
**recevoir**. Nous ne le reflétons pas, nous l'appliquons à notre encodeur.

- `useinbandfec`, `usedtx`, `cbr` : repris tels quels s'ils sont écrits.
  Absents, notre configuration reste.
- `maxaveragebitrate` : l'encodeur prend le plus petit entre celui du pair et
  le nôtre.

### 2.3 VP8 (`VideoCodec::VP8`)

| Clé `codec.*` | Défaut | Rôle |
|---|---|---|
| `vp8.max-fr` | `0` (omis) | Fréquence d'images maximale. |
| `vp8.max-fs` | `0` (omis) | Taille d'image maximale, en macroblocs. |

**Annoncé** : `max-fr=<n>;max-fs=<n>`, chacun seulement s'il est non nul. Par
défaut, rien : la valeur rendue est `""`.

**Lu chez le pair** : rien.

### 2.4 AV1 (`VideoCodec::AV1`)

| Clé `codec.*` | Défaut | Rôle |
|---|---|---|
| `av1.profile` | `0` (Main) | Profil. |
| `av1.level-idx` | `5` (niveau 3.1) | Niveau. |
| `av1.tier` | `0` (Main) | Tier. |

**Annoncé** : `profile=<p>;level-idx=<l>;tier=<t>`, toujours notre
configuration. Chaque côté déclare ce qu'il sait recevoir.

**Lu chez le pair** : l'encodeur prend, pour chacun des trois paramètres, le
plus petit entre le pair et nous. Un paramètre que le pair omet vaut son défaut
de la spécification. **Piège** : un pair qui n'écrit que `profile=0` déclare
donc aussi `level-idx=5`, soit le niveau 3.1, et l'encodeur s'y limite. Sans
aucun `fmtp` du pair, rien n'est borné.

### 2.5 AMR et AMR-WB (`AudioCodec::AMR`, `AudioCodec::AMRWB`, RFC 4867)

Pas de clé `codec.*` : tout se déduit du `fmtp` du pair.

| Ce que dit le pair | Ce que fait le serveur |
|---|---|
| `octet-align=1` | Annonce `octet-align=1`. Ce paramètre doit être identique des deux côtés. |
| `octet-align` absent | Annonce sans `octet-align` : le mode est **bandwidth-efficient**, le défaut de la RFC. |
| `mode-set=0,1,2` | Annonce le même `mode-set`. L'encodeur émet le mode le plus haut de la liste : ici le mode 2, soit 12,65 kbit/s en AMR-WB. |
| `mode-set` sans aucun mode valide | **Refuse le PT.** |
| `crc=1`, `robust-sorting=1`, `interleaving` | **Refuse le PT.** |

Sans `mode-set`, l'encodeur émet 23,85 kbit/s en AMR-WB et 12,2 kbit/s en AMR.

Le mode vaut **par PT**. Une offre VoLTE propose souvent l'AMR-WB deux fois :
un PT en `octet-align=1`, un autre sans. Chacun repart avec son propre `fmtp`.

**Piège.** Sur le fil, une trame AMR n'est pas la trame du codec. Elle est
précédée d'un octet CMR et d'une table des matières. En bandwidth-efficient,
ses bits ne sont même pas alignés sur l'octet. `RTPSession` traduit à l'entrée
et à la sortie (`amr/amrpayload.h` dans libmedikit), selon le mode négocié pour
le PT. À l'intérieur du serveur, un paquet AMR porte donc des trames au
**format stockage** : un octet d'en-tête puis la trame, comme dans un fichier
3GP. Un paquet reçu peut en porter plusieurs.

### 2.6 telephone-event (RFC 4733)

**Annoncé** : `0-16`, toujours. Ce sont les touches 0 à 9, `*`, `#`, A à D et
le flash. **Lu chez le pair** : rien.

### 2.7 T.140 redondant (`TextCodec::T140RED`, RFC 4103)

**Annoncé** : le PT du T.140 primaire, répété trois fois et séparé par `/`.
Exemple : `98/98/98` si le T.140 est le PT 98. Il faut pour cela qu'un T.140
soit aussi proposé. Sans T.140 proposé, la valeur est `""`. **Lu chez le
pair** : rien.

## 3. Codecs sans `fmtp`

Ces codecs sont acceptés ou refusés selon §4. Leur valeur dans le retour est
toujours `""` :

- audio : PCMU, PCMA, G.722, GSM, AAC, Speex, Nelly ;
- texte : T.140.

## 4. Quels codecs sont acceptés

Un PT est accepté si le serveur sait **décoder** son codec : c'est le sens de
la réception, celui que l'on négocie ici.

- Les codecs audio et vidéo sont testés auprès de ffmpeg, G.711 compris : un
  ffmpeg bâti sans un codec le fait refuser.
- telephone-event, T.140 et T.140 redondant sont toujours acceptés : le serveur
  les traite lui-même.
- Tout type inconnu est refusé.

La liste complète, par direction, est rendue par `/status/general`
(`docs/reference/status-http.md`). Le décodage et l'encodage ne coïncident pas
toujours : VP6 se décode mais ne s'encode pas.
