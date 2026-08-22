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
- `mcu/Makefile` : `libmedkit.a` lié inconditionnellement ; plus aucun `.o`
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
- `mcu/Makefile` : `USEMEDKIT=yes` ; `-I$(MEDKITDIR)` placé **avant**
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

### Palier 2 — Audio ffmpeg via FfAudioEncoder/Decoder ✅ (côté libmedkit fait)
Fichiers : déjà faits `libmedikit/ffaudiocodec.{h,cpp}` ; `libmedikit/audio.cpp` ;
`mcu/src/audio.cpp` ; `install.ksh` (MEDKIT_OBJS += `ffaudiocodec.o`).
- ✅ `FfAudioEncoder(const Properties&, AVCodecID, AudioCodec::Type)` /
  `FfAudioDecoder(AVCodecID, AudioCodec::Type)` en API ffmpeg 5/6
  (`avcodec_send_frame`/`receive_packet`, `send_packet`/`receive_frame`),
  `AVChannelLayout` (pas `ctx->channels`), conversion S16↔fltp via `SwrContext`
  (libswresample), `numFrameSamples` depuis `ctx->frame_size`.
- ✅ G722 (`AV_CODEC_ID_ADPCM_G722`) et AAC (`AV_CODEC_ID_AAC`) **réécrits** pour
  dériver de `FfAudioEncoder`/`FfAudioDecoder` (et non supprimés : `g722codec.*`
  et `aacencoder.*` ne contiennent plus que constructeur + spécificités, ex.
  `GetClockRate()=8000` pour G722). Validés par roundtrip.
- ⏳ Nelly (`AV_CODEC_ID_NELLYMOSER`) : reste à porter sur la même base.
- ⏳ `mcu/src/audio.cpp` : factory à faire déléguer G722/AAC/Nelly à medkit ;
  retirer leurs `.o` des OBJS. `-lswresample` est déjà dans le `LDFLAGS` mcu.

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
- `mcu/Makefile` : retirer le bloc `ifeq ($(USEMEDKIT),yes)…endif`, rendre
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
- **`VideoFrame::PacketizeH263()` non définie** : `libmedikit/video.cpp`
  l'appelle (`AudioFrame`/`VideoFrame::Packetize` → `PacketizeH263(mtu)`) mais
  seule `PacketizeH264` est implémentée → symbole non résolu au **lien final du
  `mcu`**. À implémenter dans `libmedikit/video.cpp` (porter depuis la
  packetisation H263 de `mcu/src`) avant le palier 1. Sans impact sur la
  construction de `libmedkit.a` (archive), bloquant seulement à l'édition de
  liens du binaire.

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
- `mcu/Makefile`, `install.ksh` (fonction `compile_libmedkit`)

## Avancement réalisé dans libmedkit (paliers 0/2 + ffvideocodec)

Travaux effectués et validés contre ffmpeg 5.1 (libavcodec 59, libswresample,
sans libavresample) :

**Audio (palier 2) — `libmedikit/ffaudiocodec.{h,cpp}` créés :**
- `FfAudioEncoder` : API send/receive, `AVChannelLayout` mono, resampler
  S16→format natif via `SwrContext`, `EnsureFrame()` (allocation paresseuse du
  tampon d'entrée gérant les codecs à `frame_size` variable + restauration de la
  capacité entre appels). `ctx`/`codec` `protected` pour la config des dérivés.
- `FfAudioDecoder` : `send_packet`/`receive_frame`, conversion vers S16 mono
  (resampler créé à la volée si le décodeur sort en planar/float), fifo +
  restitution par tranches de `numFrameSamples`.
- `g722/g722codec.*` et `aac/aacencoder.*` réécrits pour dériver de ces bases.
- `libmedikit/audio.cpp` : factory — cas G722 (enc+dec) et AAC (enc) actifs.

**Vidéo — `libmedikit/ffvideocodec.{h,cpp}` : bugs ffmpeg 5 corrigés** (l'API
send/receive exige ce que l'ancienne tolérait) :
1. encodeur : recopie de `pkt->data` dans la `VideoFrame` (`SetMedia`) — la
   packetisation RTP ne fait que référencer des offsets, le tampon doit être
   rempli (le `pkt->data = frame->GetData()` initial était du code mort) ;
2. encodeur : `picture->width/height/format` renseignés avant `send_frame` ;
3. encodeur : `EAGAIN` après réception = fin de drain → retourner la trame, pas
   `NULL` (sinon la trame encodée était jetée) ;
4. décodeur : si `av_parser_init` renvoie NULL (cas H263+), pousser l'entrée
   comme trame complète (sinon segfault sur parser NULL) ;
5. décodeur : `EAGAIN`/`EOF` = fin normale (plus `goto error`), `Decode`
   retourne le succès ;
6. divers : `if(frame);` parasite, `Error` sans `return` (deref NULL),
   `avcodec_close` retiré, `av_parser_close` ajouté.
- Validé : roundtrip H263 encode→decode (176×144), G722 et AAC OK.
- `libmedikit/logo.cpp` également porté ffmpeg 5 (codecpar, send/receive).

**Build libmedkit :**
- `Makefile` : switch **`ASTERISK=no`** (exclut transcoder/mp4format/framebuffer/
  frameutils/astlog), `-I../../../staticdeps/include` (mp4v2), modules
  `ffaudiocodec.o`/`g722codec.o`/`aacencoder.o` ajoutés à `OBJS`, `-lswresample`
  dans `LDFLAGS`. `make ASTERISK=no` reconstruit `libmedkit.a` proprement.

**Reste à faire (palier 2/3) :** Nelly ; délégation côté `mcu/src/audio.cpp` ;
implémenter `VideoFrame::PacketizeH263()` (cf. pièges) ; décodeur AAC si besoin.

## État d'avancement des corrections déjà faites (hors périmètre libmedkit)

Avant cette décision, plusieurs corrections de compilation ont déjà été
appliquées dans `mcu/` pour avancer le portage ffmpeg 5/6 / AlmaLinux 9 :
- `mcu/include/stringparser.h` : `pow10()` → `pow(10.0, e)`.
- `mcu/Makefile` : ajout `-I/usr/include/ffmpeg` ; ImageMagick 7 via
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

## ÉTAT (2026-06-30) — Paliers 0, 1 et 2 atteints : binaire `bin/debug/mcu` produit ✅

**Palier 0 (medkit = sur-ensemble ABI) — fait :**
- `medkit/config.h` : `WGA`/`DCIF` (+ `GetWidth`/`GetHeight`), `ALIGNEDTO32`/`ZEROALIGNEDTO32`.
- `medkit/media.h` : `RoleToString`, enum `MediaProtocol` + `ProtocolToString`, struct `MediaStatistics`.
- `medkit/codecs.h` : `TELEPHONE_EVENT`, et **`AMRWB=120`** ajouté (+ `GetNameFor`/`GetCodecFor` AMR/AMR-WB).
- `medkit/video.h` : `DecodePacket` pur virtuel (même position vtable que mcu).
- `ffvideocodec.{h,cpp}` : `FfVideoDecoder::DecodePacket` (dispatch H264 STAP-A/FU-A via `h264_append_nals` porté de mcu ; dépaquetisation RFC 2429 H263+ ; accumulation brute MPEG4/VP6/FLV1).

**Palier 1+2 fusionnés (build vert) — fait :** (le « build vert » vidéo seul était impossible car `nelly`/`g722codec` mcu n'compilent pas en ffmpeg 5 → audio traité en même temps)
- `mcu/Makefile` : `USEMEDKIT=yes` ; `-I$(MEDKITDIR)` avant `-Iinclude/` ; `OBJS`/`OBJS2` purgés des `.o` repris par medkit (H263/MPEG4/H263-1996, h264decoder, vp6, flv1codec, g722codec, NellyCodec) ; gardés mcu : `h264encoder.o`, `h264depacketizer.o` (utilisé par `rtp.cpp`), `g7221codec.o`, VP8, gsm, speex, opus.
- `mcu/include/{media,codecs}.h` rendus **identiques** à medkit ; `video.h`/`audio.h` : `VideoFrame`/`AudioFrame` alignés (useStartCode/naluSizeLen, packetization, ctor `owns`, `Packetize` virtuel) en gardant les classes Input/Output propres à mcu.
- `AddRtpPacket` : passage **4→5 args** (`mark`) ; 8 appelants compilés convertis (audioencoder, FLVEncoder, vp8encoder, rtp, h264encoder, h264depacketizer ×3).
- `mcu/src/video.cpp` : factory délègue à `FfVideoDecoder`/`FfVideoEncoder` (H263/H263-1996/MPEG4/H264-dec/VP6/SORENSON) ; garde VP8 + H264-encode mcu. Définit `VideoFrame::Packetize`/`PacketizeH264`/`PacketizeH263`/NALU (sinon medkit `video.o` tiré → double `VideoCodecFactory`).
- `mcu/src/audio.cpp` : factory délègue G722 et **AMR/AMR-WB** à medkit (inclus via chevrons `<g722/...>`/`<amr/...>` pour viser le sous-module et non l'homonyme `mcu/src`) ; définit `AudioFrame::Packetize`. Nelly retiré (cases supprimées).
- `mcu/src/main.cpp` : suppression du gestionnaire de verrous ffmpeg (`av_lockmgr_register`/`lock_ffmpeg`, supprimé en ffmpeg 5).

**AMR-NB / AMR-WB migrés vers libmedkit (FfAudio) :** nouveau module `libmedikit/amr/amrcodec.{h,cpp}` (`AMRNBEncoder`/`AMRNBDecoder` via `AV_CODEC_ID_AMR_NB` ; `AMRWBEncoder`/`AMRWBDecoder` via `AV_CODEC_ID_AMR_WB`, bitrate de mode valide réglé avant `Open()`), câblé dans les deux factories, ajouté à `Makefile` medkit + `MEDKIT_OBJS` (install.ksh). Les `-lopencore-amrnb -lopencore-amrwb` retirés du link mcu (fournis par `libavcodec.so` : `libopencore_amrnb`/`libvo_amrwbenc`).

**Dépendances passées en dynamique système (AlmaLinux 9) dans `mcu/Makefile` (LDXMLFLAGS) :**
- OpenSSL : `-lssl -lcrypto` au lieu de `staticdeps/lib/libssl.a`/`libcrypto.a` (absents).
- XML : `-lxml2` au lieu de `libxmlrpc_xmlparse.a`/`libxmlrpc_xmltok.a` (xmlrpc-c bâti avec **backend libxml2** : `xmlParseChunk` vient de libxml2).

**Vérif :** `./install.ksh libmedkit` puis `cd mcu && make mcu` → `bin/debug/mcu` (ELF 34 Mo) ; le binaire démarre (init RTMP/WebSocket/RTP OK).

**Palier 3b — VP8 décode migré vers le décodeur NATIF ffmpeg (2026-06-30) :**
- `VideoCodec::VP8` décodé par le décodeur natif `vp8` de ffmpeg (**pas** libvpx :
  `avcodec_find_decoder` renvoie le natif par défaut).
- **VP8 encode via le wrapper `libvpx` de ffmpeg** : nouvelle classe `VP8Encoder`
  (`vp8/vp8encoder.{h,cpp}`, dérive de `FfVideoEncoder`, `AV_CODEC_ID_VP8` →
  `avcodec_find_encoder` renvoie le wrapper `libvpx`). Le code vpx natif de mcu
  (`vp8encoder.o`/`vp8decoder.o`) est **retiré du build**, et `-lvpx` retiré du
  link mcu : `libvpx.so` n'est plus tiré que **via `libavcodec.so`**. `mcu/Makefile`
  `VP8OBJ=` (vide).

**Packetisation RTP par encodeur (symétrique aux décodeurs, 2026-06-30) :**
`FfVideoEncoder::EncodeFrame` délègue désormais la packetisation RTP à une méthode
virtuelle **`PacketizeFrame()`** :
- défaut (`FfVideoEncoder::PacketizeFrame`) = schéma historique H263/MPEG4 (saut du
  start code 2 octets + préfixe RFC 2429) — comportement inchangé pour H263/MPEG4/SORENSON ;
- **`VP8Encoder::PacketizeFrame`** (override) = VP8 payload descriptor RFC 7741
  (préfixe 1 octet, S=1 sur le 1er fragment puis 0, marker RTP sur le dernier).
Membres de `FfVideoEncoder` passés en `protected`. → l'encodeur VP8 est désormais
**correct de bout en bout** (bitstream + RTP).

**Refactor DecodePacket — un dépaquetiseur par décodeur (2026-06-30) :**
Chaque décodeur porte SA dépaquetisation RTP (méthode virtuelle `DecodePacket`),
`FfVideoDecoder::DecodePacket` ne garde que le **cas par défaut** (accumulation
brute + `Decode` sur `last`, pour MPEG4/VP6/FLV1/SORENSON/H263-1996).
- `ffvideocodec.h` : membres de `FfVideoDecoder` passés en **`protected`**
  (`buffer`/`bufLen`/`bufSize`/`Decode`… accessibles aux dérivés).
- `ffvideocodec.cpp` : `DecodePacket` réduit au défaut ; `h264_append_nals` et
  `vp8_descriptor_len` retirés d'ici (déplacés dans les classes concernées).
- `h264/h264decoder.{h,cpp}` : `H264Decoder::DecodePacket` (STAP-A/FU-A,
  `h264_append_nals` local).
- `h263/h263codec.{h,cpp}` : `H263Decoder::DecodePacket` (RFC 2429).
- **nouvelle classe `vp8/vp8decoder.{h,cpp}`** : `VP8Decoder` (dérive de
  `FfVideoDecoder`, ctor `AV_CODEC_ID_VP8`) + `DecodePacket` (strip descriptor
  RFC 7741, `vp8_descriptor_len` local). Ajoutée au `Makefile` medkit + `MEDKIT_OBJS`.
- Factories (`mcu/src/video.cpp`, `libmedikit/video.cpp`) : instancient les classes
  spécifiques `H264Decoder`/`H263Decoder`/`VP8Decoder` (mcu les inclut en chevrons
  `<h264/...>`/`<h263/...>`/`<vp8/...>`) ; MPEG4/VP6/SORENSON/H263-1996 via
  `FfVideoDecoder` (défaut).
- Vérifié : build vert, symboles `{FfVideoDecoder,H264Decoder,H263Decoder,VP8Decoder}::DecodePacket` tous présents.

**Palier 4 — partiel (2026-06-30) :**
- ✅ **A.1 — mode sans-medkit supprimé** : `mcu/Makefile` lie `libmedkit.a` et
  `-I$(MEDKITDIR)` **inconditionnellement** (bloc `ifeq ($(USEMEDKIT),yes)` retiré,
  variable `USEMEDKIT` supprimée). Build vérifié vert.
- ⛔ **A.2 — bascule des `#include` vers `medkit/` + suppression des homonymes :
  NON RECOMMANDÉE en l'état.** Mesures à l'appui (diff réel, CRLF/espaces ignorés) :
  - en-têtes codec-ABI déjà alignés : `codecs.h`/`media.h` identiques, `config.h`
    sur-ensemble (gardes `__cplusplus` + `CIF` en `default`), `video.h`/`audio.h`
    ne diffèrent que par les classes `VideoInput/Output`/`AudioInput/Output`
    (absentes de medkit, 14-18 fichiers mcu les utilisent) ;
  - **utilitaires réellement divergents** : `log.h` (207), `mp4recorder.h` (179),
    `mp4player.h` (95), `tools.h` (89), `red.h` (74), `textencoder.h` (60),
    `text.h` (49) — mcu utilise ses versions propres ; il ne faut PAS les basculer.
  - **piège transitif bloquant** : `medkit/config.h` fait `#include "version.h"`
    ; via les gardes partagées (`_VERSION_H_`, `_CONFIG_H_`…), basculer vers les
    en-têtes medkit ferait prendre à mcu **`medkit/version.h`** (mauvaise version
    produit). 135 fichiers / 199 sites concernés.
  - Le risque double-ABI visé par le palier 4 est **déjà neutralisé** (en-têtes
    codec maintenus ABI-identiques). Recommandation : garder l'arrangement actuel
    (en-têtes mcu alignés sur medkit) plutôt qu'une bascule de masse risquée. Si
    unification voulue un jour : d'abord déplacer Input/Output dans medkit, retirer
    `#include "version.h"` de `medkit/config.h`, et ne basculer QUE les 4 en-têtes
    codec (pas les utilitaires).

**GSM et OPUS migrés vers libmedkit (Palier 3b audio, 2026-06-30) :**
- **GSM-FR** : nouveau module `libmedikit/gsm/gsmcodec.{h,cpp}` (`GSMEncoder`/`GSMDecoder`
  via `AV_CODEC_ID_GSM`). L'encodeur utilise le wrapper `libgsm` de ffmpeg (lié
  dynamiquement dans `libavcodec.so`) ; le décodeur est natif ffmpeg. Plus de
  `-lgsm` dans les `LDFLAGS` mcu (tiré transitivement via `libavcodec.so`).
  `gsmcodec.o` mcu retiré du build ; `gsmcodec.o` medkit ajouté à `MEDKIT_OBJS`.
- **OPUS** : nouveau module `libmedikit/opus/opuscodec.{h,cpp}` (`OPUSEncoder`/`OPUSDecoder`
  via `AV_CODEC_ID_OPUS`). L'encodeur force `ctx->sample_rate = 48000` (horloge RTP
  fixe RFC 7587) et surcharge `TrySetRate()` pour toujours retourner 48000 ; le MCU
  adapte sa cadence d'entrée. `GetClockRate()` surchargé = 48000.
  Le décodeur surcharge `GetRate()`/`TrySetRate()` = 48000 statiquement.
  La garde `#ifdef OPUS_SUPPORT` est supprimée de `mcu/src/audio.cpp` : OPUS est
  désormais toujours disponible (sans `-DVADWEBRTC`). `opusencoder.o`/`opusdecoder.o`
  mcu retirés du build ; `opuscodec.o` medkit ajouté à `MEDKIT_OBJS`.
- `mcu/src/audio.cpp` : inclusions en chevrons `<gsm/gsmcodec.h>` et `<opus/opuscodec.h>`.
- Factories mcu et medkit câblées pour les deux codecs.
- Vérif : build vert `bin/debug/mcu` (32 Mo) ; symboles `GSMEncoder`, `GSMDecoder`,
  `OPUSEncoder`, `OPUSDecoder` présents dans le binaire.

**Reste à faire :**
- Nelly (NellyMoser) : ✅ **opérationnel dans libmedkit** — `libmedikit/nelly/nellycodec.{h,cpp}` implémente `NellyEncoder`/`NellyEncoder11Khz` et `NellyDecoder`/`NellyDecoder11Khz` via `AV_CODEC_ID_NELLYMOSER` (ffmpeg dispose bien d'un encodeur `nellymoser` ET d'un décodeur). Factory `audio.cpp` câblée, `NELLYOBJ=nellycodec.o` dans le Makefile medkit. Aucune action requise.
- Validation fonctionnelle (pas de suite auto) : conférence (mixage audio + mosaïque vidéo), MP4 record/play, SRTP/DTLS, BFCP, RTMP/WebSocket.
- Palier 4 partiel (optionnel) : bascule des `#include "audio.h"/"video.h"/"codecs.h"` mcu vers `medkit/…` et suppression des en-têtes homonymes — bloquée par les classes `VideoInput/Output`/`AudioInput/Output` absentes de medkit (14-18 fichiers) et par `log.h`/`mp4*.h`/`tools.h`/`red.h` divergents (cf. analyse A.2).

**Nettoyage arbre source mcu/src — codecs migrés supprimés (2026-06-30) :**

Tous les répertoires et fichiers codec dont la fonctionnalité a été reprise par
`libmedikit` ont été supprimés de `mcu/src/`. Build vérifié vert après nettoyage.

**Répertoires entièrement supprimés de mcu/src :**
- `gsm/` — migré (GSM-FR via FfAudioEncoder/Decoder)
- `opus/` — migré (OPUS via FfAudioEncoder/Decoder)
- `vp8/` — migré (VP8 decode natif ffmpeg + encode libvpx via FfVideoEncoder)
- `vp6/` — migré (VP6 decode via FfVideoDecoder AV_CODEC_ID_VP6F)
- `h263/` — migré (H263/H263+/MPEG4 via FfVideoDecoder/FfVideoEncoder + H263Decoder medkit)
- `nelly/` — supprimé (NellyMoser : décodeur disponible dans medkit, encodeur absent de ffmpeg)
- `flv1/` — migré (Sorenson FLV1 via FfVideoDecoder AV_CODEC_ID_FLV1)
- `aac/` — code mort (jamais compilé)
- `g711/` — migré (PCMUEncoder/PCMAEncoder dans libmedkit via table G711)
- `h264/` — migré entièrement (encodeur libx264 + décodeur FfVideoDecoder + dépaquetiseur H264Depacketizer, tous dans libmedkit ; adaptateur `H264RTPDepacketizer` dans `rtp.cpp`)
- `g722/` — migré entièrement (G722 via FfAudioEncoder/Decoder ; G.722.1 via libg722_1 dans libmedkit)
- `speex/` — migré (Speex 16 kHz via FfAudioEncoder/Decoder, AV_CODEC_ID_SPEEX)

**mcu/src/audio.cpp et video.cpp supprimés — factories libmedkit utilisées directement :**
- `libmedikit/audio.cpp` : `AudioCodecFactory::CreateEncoder/Decoder` + `AudioFrame::Packetize` — tous les codecs audio câblés (G711, G722, G7221, AMR/AMR-WB, Nelly, GSM, Speex, OPUS)
- `libmedikit/video.cpp` : `VideoCodecFactory::CreateEncoder/Decoder` + `VideoFrame::Packetize/PacketizeH264/PacketizeH263/NALU` — tous les codecs vidéo câblés (SORENSON, H263_1998/1996, MPEG4, H264, VP6, VP8)
- `audio.o`/`video.o` retirés de `OBJS`/`OBJS2` dans `mcu/Makefile`

**mcu/include/media.h → forwarder libmedkit :**
- Réduit à `#include "medkit/media.h"` ; l'implémentation (`media.o`) provient de `libmedkit.a` depuis le début

**Makefile — état final :**
- Toutes les variables `*DIR`/`*OBJ` codec retirées (`G711`, `H263`, `VP6`, `VP8`, `GSM`, `NELLY`, `OPUS`, `H264`, `G722`, `SPEEX`)
- `OBJS+=` ne contient plus que `$(DOCSHARINGOBJ) $(JSR309OBJ)`
- `VPATH` : entrées codec toutes retirées
- `-lspeexdsp` maintenu pour `audiotransrater.cpp` (rééchantillonneur audio, distinct du codec)
- `-lg722_1` maintenu (libg722_1 statique, utilisé par `g7221codec.o` dans libmedkit)
