# Codecs et paramètres `fmtp` négociés

> Référence des **propriétés de codec** (`codec.*`) reconnues par le média
> serveur et du `fmtp` qu'il **génère** lors de la négociation JSR-309.
> Complète `nego_fmtp.md` (conception) et `xmlrpc_jsr309_api.md` §6.7 (API).
> Statut : phase 4 livrée (génération du `fmtp` local + filtrage) ; l'ingestion
> du `fmtp` **distant** (`codec.<x>.fmtp`) est routée mais pas encore parsée
> (phase 5).

## 1. Comment ça marche

1. Le contrôleur SIP pousse, avant `EndpointStartReceiving`, les paramètres
   locaux voulus via `EndpointSetRTPProperties` avec des clés **préfixées
   `codec.`** (ex. `codec.h264.profile-level-id = 42e01f`). Le serveur retire le
   préfixe `codec.` et **mémorise** ces clés côté endpoint (elles ne vont pas à
   la session RTP, qui les ignore).
2. À `EndpointStartReceiving`, le **négociateur** (`CodecNegotiator`, libmedikit)
   intersecte la `rtpMap` proposée avec les codecs **réellement supportés** par
   la libmedikit/ffmpeg compilée (un PT non supporté **disparaît** — décision D),
   puis **dérive le `fmtp` local** de chaque codec retenu à partir des `codec.*`
   mémorisées + défauts, **sans ouvrir de codec**.
3. Le retour `EndpointStartReceiving` = `[ recvPort, { "<pt>": "<params fmtp>" } ]`.
   Les paramètres sont **seuls** (sans `a=fmtp:<pt> ` — décision E) ; un codec
   sans `fmtp` est **absent** de la struct. Le contrôleur SIP reconstruit les
   lignes SDP.

**Important** : le `fmtp` de négociation est dérivé de la **configuration**, pas
d'un encodeur « chaud ». En particulier le H.264 `sprop-parameter-sets` (SPS/PPS)
n'est **pas** produit ici : il n'existe qu'après l'encodage d'une première trame,
donc après `StartReceiving`. Seul `profile-level-id` est annoncé.

## 2. Codecs porteurs de `fmtp`

### H.264 (`VideoCodec::H264`)

| Clé (`codec.` retiré) | Défaut | Rôle |
|---|---|---|
| `h264.profile-level-id` | `42801F` | Profil/niveau **que nous savons décoder** (annoncé dans notre SDP). Émis en minuscules (attribut insensible à la casse). |

`fmtp` généré : `profile-level-id=<id>;packetization-mode=1`.
`sprop-parameter-sets` **délibérément absent** (cf. §1).

### Opus (`AudioCodec::OPUS`)

| Clé | Défaut | Rôle |
|---|---|---|
| `opus.useinbandfec` | `0` | FEC en bande (`useinbandfec=1`). |
| `opus.usedtx` | `0` | Transmission discontinue (`usedtx=1`). |
| `opus.maxaveragebitrate` | `0` (omis) | Débit moyen max annoncé. |
| `opus.cbr` | `0` | Débit constant (`cbr=1`). |

`fmtp` généré : concaténation des paramètres non nuls (mêmes clés/défauts que le
constructeur de l'encodeur, pour éviter toute dérive).

### VP8 (`VideoCodec::VP8`)

| Clé | Défaut | Rôle |
|---|---|---|
| `vp8.max-fr` | `0` (omis) | Fréquence d'images max (`max-fr`). |
| `vp8.max-fs` | `0` (omis) | Taille de trame max en macroblocs (`max-fs`). |

### AV1 (`VideoCodec::AV1`)

| Clé | Défaut | Rôle |
|---|---|---|
| `av1.profile` | `0` (Main) | `profile`. |
| `av1.level-idx` | `5` (≈ niveau 3.1) | `level-idx`. |
| `av1.tier` | `0` (Main) | `tier`. |

`fmtp` généré : `profile=<p>;level-idx=<l>;tier=<t>`.

### T140 redondant (`TextCodec::T140RED`, RFC 4103)

Pas de clé `codec.*` : le `fmtp` du RED texte liste le payload type du **T140
primaire**, répété par génération (3 par défaut), séparé par `/` — ex. `98/98/98`
si le T140 est le PT 98. Il n'est produit que si un **T140** (`TextCodec::T140`)
est aussi proposé **et** supporté ; sinon le RED n'a pas de paramètre → `fmtp`
vide (absent de la struct).

## 3. Codecs sans `fmtp`

Ces codecs sont négociables (gardés/filtrés selon le support ffmpeg) mais
n'émettent **aucun** `fmtp` (donc absents de la struct de retour) :

- **Audio** : PCMU, PCMA, G722, GSM, AAC, AMR, Speex, Nelly.
- **Texte** : T140 (le paramétrage éventuel est porté par le T140RED, §2).

## 4. Support des codecs (`IsSupported`)

« Supporté » = libmedikit/ffmpeg a été compilée avec ce codec. Les types adossés
à ffmpeg (`H264`, `VP8`, `AV1`, `Opus`, `AAC`, `AMR`, `Speex`…) sont testés par
`avcodec_find_encoder(_by_name)` / `avcodec_find_decoder`, comme à l'ouverture
réelle ; les types toujours présents (PCMU/PCMA, T140/T140RED…) renvoient `true`
en dur. Le catalogue est calculé une fois et mémoïsé
(`AudioCodec/VideoCodec/TextCodec::IsSupported`,
`*CodecFactory::GetSupportedCodecs`).

## 5. `fmtp` distant (à venir — phase 5)

Le **canal** existe : `EndpointSetRTPProperties` accepte
`codec.<nomCodec>.fmtp = "<params reçus du pair>"` (ex.
`codec.h264.fmtp = "profile-level-id=42e01f;packetization-mode=1"`), routé et
stocké côté endpoint. Le **parsing** (pour borner notre émission au profil que le
pair sait décoder) et le câblage endpoint → producteur (transcodeur/mixer)
restent à livrer, H.264 en premier.
