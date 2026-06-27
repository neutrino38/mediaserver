# Plan — libmedkit comme source unique de codecs

## Contexte

Le portage AlmaLinux 9 / GCC 11 / ffmpeg 5-6 du mediaserver bute sur les codecs
de `mcu/src` qui utilisent une API ffmpeg supprimée (`avcodec_decode_video2`,
`avcodec_encode_video2`, `avcodec_*_audio*`, `avcodec_register_all`,
`AVStream::codec`, `FF_MIN_BUFFER_SIZE`, `CODEC_FLAG_*`…). Plutôt que de
re-porter ces copies locales, on adopte la décision suivante : **libmedkit**
(sous-module `third_party/fontventa/libmedikit`, branche
`migration/almalinux_9`, déjà porté ffmpeg 5/6 pour `FfVideoEncoder`/
`FfVideoDecoder`) devient la **seule** source de codecs.

Décisions validées :
1. **Dépacketisation RTP portée dans libmedkit** : ajout de `DecodePacket()` à
   l'interface `VideoDecoder` de medkit et aux décodeurs.
2. **Tous les codecs migrent dans libmedkit** : les codecs ffmpeg via
   `FfVideoEncoder`/`FfVideoDecoder` et de nouvelles classes
   `FfAudioEncoder`/`FfAudioDecoder` ; les codecs en libs natives (VP8/libvpx,
   H264-encode/libx264, Opus, Speex, GSM, G711, G722.1) sont **déplacés** dans
   libmedkit. Tout le code codec quitte `mcu/src`.
3. Les codecs Opus, VP8 sont convertis en FFVideoEncoder / FfVideoDecoder en
   privilégiant l'implémentation ffmpeg.
4. Idem pour les codecs audio speex, gsm et opus qui sont nativement supportés
   par ffmpeg.
4. **Suppression du mode « sans medkit »** : `USEMEDKIT` devient le seul mode.
5. **Socle compilable d'abord**, puis itération codec par codec — chaque palier
   doit compiler et linker.

## Risque central : divergence ABI mcu ↔ medkit

Aujourd'hui `USEMEDKIT` n'est qu'un interrupteur Makefile : aucun source mcu
n'inclut un en-tête `medkit/`. Dès qu'on lie `libmedkit.a`, on mélange deux
définitions **incompatibles** des types de base, sans erreur de compilation
(corruption mémoire silencieuse) :

- `MediaFrame` (media.h) : medkit ajoute `ownsbuffer`, change le constructeur,
  et `RtpPacketization`/`AddRtpPacket` a un argument `mark` en plus (5 args vs 4).
- `VideoFrame` : medkit ajoute `useStartCode`, `naluSizeLen`, méthodes NALU.
- `AudioFrame` : medkit ajoute `packetization`.
- `VideoDecoder` : mcu a `DecodePacket`, medkit non (vtable différente).
- **`AudioCodec::Type` : valeurs divergentes** — `OPUS` 111(mcu)/98(medkit),
  `G7221` 99(mcu)/119(medkit). `VideoCodec::Type` : identiques (OK).
- `Properties` (config.h) : identiques (OK).

Stratégie retenue : faire de `medkit/*.h` le **sur-ensemble ABI-compatible** de
mcu (palier 0), garder les en-têtes mcu synchronisés pendant la migration, puis
basculer tout l'include path vers `medkit/` et supprimer les homonymes (palier 4).

## Architecture cible

- `medkit/*.h` = interfaces uniques (VideoEncoder/Decoder, AudioEncoder/Decoder,
  factories, MediaFrame/VideoFrame/AudioFrame, enums).
- Codecs ffmpeg → `FfVideoEncoder`/`FfVideoDecoder` (param `AVCodecID`) et
  `FfAudioEncoder`/`FfAudioDecoder` (param `AVCodecID`).
- Codecs natifs à migrer vers ffmpeg : VP8, OPUS, SPEEX, gsm
- Codecs natifs → classes dédiées dans libmedkit (libx264, 
  libg722_1, table G711).
- `mcu/Makefile.rpm` : `libmedkit.a` lié inconditionnellement ; plus aucun `.o`
  de codec dans `OBJS`/`OBJS2` ; libs natives `-l*` dans `LDFLAGS` mcu.

## Paliers (chacun compile)

### Palier 0 — medkit = sur-ensemble ABI (medkit seul, mcu inchangé)
Fichiers : `libmedikit/medkit/{video,audio,codecs,media,config}.h`,
`libmedikit/ffvideocodec.{h,cpp}`.
- `medkit/codecs.h` : aligner `AudioCodec::Type` sur libmedkit (OPUS=08, G7221=119),
  ajouter `SLIN` à une valeur libre.
- `medkit/media.h` / `video.h` / `audio.h` : ajouter les membres/méthodes que mcu
  utilise et que medkit n'a pas (`MediaStatistics`, `MediaProtocol` étendu,
  `ProtocolToString`/`RoleToString`, `useStartCode`/`naluSizeLen`,
  `AddRtpPacket` à 5 args, etc.) ; ajouter `ALIGNEDTO32`/`ZEROALIGNEDTO32`/
  `WGA`/`DCIF` à `config.h`.
- `medkit/video.h` : déclarer `virtual int DecodePacket(BYTE*,DWORD,int,int)=0`
  dans `VideoDecoder` et l'implémenter dans `FfVideoDecoder` (voir ci-dessous).
- Vérif : `./install.ksh libmedkit` reconstruit `libmedkit.a` sans erreur.

**`FfVideoDecoder::DecodePacket`** : accumuler le payload RTP dépacketisé dans le
buffer membre existant (`buffer`/`bufLen`/`bufSize`), avec dispatch selon `type` :
- H264 : réutiliser/auditer `libmedikit/h264/h264depacketizer.*` (STAP-A/FU-A) ;
- H263/H263+/MPEG4 : strip d'en-tête payload (bits P/V/PLEN/PEBIT) — réutiliser
  `libmedikit/h263packet.*` si conforme, sinon porter depuis
  `mcu/src/h263/h263codec.cpp` (DecodePacket) ;
- sur `last` : padder `AV_INPUT_BUFFER_PADDING_SIZE` puis appeler `Decode(buffer,bufLen)`.

### Palier 1 — SOCLE : build vert via medkit pour la vidéo ffmpeg ⭐
But : `USEMEDKIT=yes` → mcu compile et linke ; H263/H263-1996/MPEG4/VP6/FLV1 et
H264-decode fournis par medkit ; audio + VP8 + H264-encode encore par mcu.
- `mcu/Makefile.rpm` : `USEMEDKIT=yes` ; `-I$(MEDKITDIR)` placé **avant**
  `-Iinclude/` ; retirer de `OBJS`/`OBJS2` les `.o` repris par medkit
  (`H263OBJ`, `H264OBJ` partiel : decoder/depacketizer, `FLV1OBJ`, `VP6OBJ`,
  `mpeg4`) ; `MEDKITLIB` actif.
- `mcu/include/video.h` et `medkit/video.h` rendus **ABI-identiques** (sinon
  double-ABI). Idem media.h/audio.h/codecs.h.
- `mcu/src/video.cpp` : `VideoCodecFactory::CreateDecoder/CreateEncoder` délègue
  aux classes medkit pour H263/MPEG4/H264-decode/VP6/FLV1 ; garde VP8 et
  H264-encode mcu.
- libmedkit : factory `libmedikit/video.cpp` étendue pour couvrir VP6/FLV1 (via
  `FfVideoDecoder(AV_CODEC_ID_VP6F/FLV1, …)`).
- Vérif : `./install.ksh localcompile` linke `bin/debug/mcu` sans double
  définition (chaque `.o` codec dans **un seul** de {OBJS mcu, libmedkit.a}).

### Palier 2 — Audio ffmpeg via FfAudioEncoder/Decoder
Fichiers : compléter `libmedikit/ffaudiocodec.{h,cpp}` ; `libmedikit/audio.cpp` ;
`mcu/src/audio.cpp` ; `install.ksh` (MEDKIT_OBJS += `ffaudiocodec.o`).
- `FfAudioEncoder(const Properties&, AVCodecID, AudioCodec::Type)` /
  `FfAudioDecoder(AVCodecID, AudioCodec::Type)` en API ffmpeg 5/6
  (`avcodec_send_frame`/`receive_packet`, `send_packet`/`receive_frame`),
  `AVChannelLayout` (pas `ctx->channels`), conversion S16↔fltp via `SwrContext`
  (libswresample), `numFrameSamples`/`frameLength` depuis `ctx->frame_size`.
- Couvre G722 (`AV_CODEC_ID_ADPCM_G722`), AAC (`AV_CODEC_ID_AAC`),
  Nelly (`AV_CODEC_ID_NELLYMOSER`).
- Supprimer du build medkit les `g722codec.cpp`/`aacencoder.cpp` dépréciés.
- `mcu/src/audio.cpp` : factory délègue G722/AAC/Nelly à medkit ; retirer leurs
  `.o` des OBJS. Ajouter `-lswresample` au link si absent.

### Palier 3 — Déplacer les codecs natifs (un sous-palier compilable par famille)
Pour chaque famille : déplacer `mcu/src/<codec>/*` → `libmedikit/<codec>/`,
ajouter le `.o` à `MEDKIT_OBJS` (install.ksh) et la règle VPATH au Makefile
medkit, ajouter la lib `-l*` au `LDFLAGS` mcu, retirer le `.o` des `OBJS` mcu,
brancher dans la factory medkit. Ordre : G711 (déjà medkit) → G722.1
(`-lg722_1`) → GSM (`-lgsm`) → Speex (`-lspeex -lspeexdsp`) → Opus (`-lopus`,
**à ajouter au Makefile**) → VP8 (`-lvpx`, porter son `DecodePacket` natif) →
H264-encode (`-lx264`, déjà `h264encoder.o` côté medkit) → VP6/FLV1/SORENSON.

### Palier 3b — Porter certains codects natifs vers ffmpeg
Les codecs Opus, VP8 sont convertis en FFVideoEncoder / FfVideoDecoder en
privilégiant l'implémentation ffmpeg. Idem pour les codecs audio speex, gsm et opus 
qui sont nativement supportés par ffmpeg. On supprime les flags de link -lgsm, -lopus
-lvpx, -lspeex.


### Palier 4 — Bascule des en-têtes + suppression du mode sans-medkit
- Convertir les `#include "video.h"|"audio.h"|"codecs.h"|"media.h"|…` (17 noms
  collisionnés) des sources mcu en `#include "medkit/…"` (script sed ciblé),
  puis **supprimer** ces en-têtes homonymes de `mcu/include/`.
- Convertir les appelants de `AddRtpPacket` (4→5 args, `mark`).
- `mcu/Makefile.rpm` : retirer le bloc `ifeq ($(USEMEDKIT),yes)…endif`, rendre
  `-I$(MEDKITDIR)` et `MEDKITLIB` inconditionnels, supprimer `USEMEDKIT=no` et
  tout `.o` codec résiduel des OBJS.

## Pièges à surveiller
- Double-ABI silencieux tant que les en-têtes coexistent et diffèrent → garder
  ABI-synchronisé jusqu'au palier 4.
- Valeurs `AudioCodec::Type` (OPUS/G7221) à aligner **avant** toute délégation audio.
- Double définition au link : retirer un `.o` de OBJS exactement quand on l'ajoute
  à `MEDKIT_OBJS`.
- `DecodePacket` virtuel pur → toutes les classes décodeur medkit doivent le fournir.
- Libs natives non liées dans `.a` : les `-l*` vivent dans `LDFLAGS` mcu
  (vérifier `-lopus` manquant).
- Objets medkit couplés Asterisk (`transcoder.o`, `mp4format.o`, `framebuffer.o`,
  `frameutils.o`, `astlog.o`) restent **exclus** du build (déjà fait dans
  `compile_libmedkit`).

## Vérification
- Après chaque palier : `./install.ksh localcompile` doit produire
  `bin/debug/mcu` sans erreur ni double définition.
- Palier 1 (jalon clé) : build complet vert avec la vidéo ffmpeg servie par
  `libmedkit.a`.
- Validation fonctionnelle finale (pas de suite auto) : conférence (mixage
  audio + mosaïque vidéo), enregistrement/lecture MP4, SRTP/DTLS, BFCP, chemin
  RTMP/WebSocket ; `rtmptest` pour le RTMP.

## Fichiers principaux
- `third_party/fontventa/libmedikit/medkit/{video,audio,codecs,media,config}.h`
- `third_party/fontventa/libmedikit/ffvideocodec.{h,cpp}`, nouveau `ffaudiocodec.{h,cpp}`
- `third_party/fontventa/libmedikit/{video,audio}.cpp` (factories), `Makefile`
- `mcu/src/{video,audio}.cpp` (factories), `mcu/include/{video,audio,codecs,media}.h`
- `mcu/Makefile.rpm`, `install.ksh` (fonction `compile_libmedkit`)

## État d'avancement des corrections déjà faites (hors périmètre libmedkit)

Avant cette décision, plusieurs corrections de compilation ont déjà été
appliquées dans `mcu/` pour avancer le portage ffmpeg 5/6 / AlmaLinux 9 :
- `mcu/include/stringparser.h` : `pow10()` → `pow(10.0, e)`.
- `mcu/Makefile.rpm` : ajout `-I/usr/include/ffmpeg` ; ImageMagick 7 via
  `pkg-config Magick++` (`MAGICKCFLAGS`/`MAGICKLIBS`, remplace `-lMagick++-6.Q16`).
- `mcu/include/rtpbuffer.h` : `Log2(...)` → `Log(...)` (un seul argument).
- `FF_INPUT_BUFFER_PADDING_SIZE` → `AV_INPUT_BUFFER_PADDING_SIZE` (framescaler,
  overlay, h264decoder, pipmosaic, mosaic, h263codec, h263-1996codec).
- `mcu/src/mosaic.cpp` : `overlay = false` → `overlay = NULL`.
- `mcu/src/logo.cpp` : porté ffmpeg 5/6 (suppression `av_register_all`,
  `codecpar` + `avcodec_alloc_context3`, `send_packet`/`receive_frame`,
  `avcodec_free_context`).

Ces correctifs sur les codecs de `mcu/src` (h263, etc.) deviendront caducs une
fois la migration vers libmedkit réalisée (les sources concernées quittent
`mcu/src`).

> Note : libmedkit est un sous-module git ; ses modifications seront committées
> séparément sur la branche `migration/almalinux_9` du dépôt fontventa.
