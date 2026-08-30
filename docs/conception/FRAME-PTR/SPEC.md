# Trames encodées partagées : `EncodeFrame` rend un `shared_ptr`

> Statut : **lots 0 à 3 faits** (2026-08-30), lot 4 (recette) à jouer. Branche de travail : `feat/shared-media-frames`
> (le correctif d'ordre dans `videostream.cpp` du 2026-08-30 en est le point de départ).
>
> Deux dépôts : le sous-module `third_party/fontventa/libmedikit` porte l'API et les
> encodeurs ; `mcu/` porte les consommateurs.

## 1. Problème

Un encodeur (`FfVideoEncoder`, `FfAudioEncoder`) rend la trame encodée par **pointeur
brut sur un membre** qu'il recycle : `FfVideoEncoder::frame`, `FfAudioEncoder::out`.
Cette trame vit « jusqu'au prochain appel sur l'encodeur ». Ce contrat n'est écrit
nulle part.

`OpenCodec()` fait `delete frame`. Or `SetFrameRate()` peut rouvrir le codec
(`ShouldReopenForBitrate`, `ShouldReopenForFps`). Un appelant qui appelle
`SetFrameRate()` entre `EncodeFrame()` et la dernière lecture de la trame lit de la
mémoire libérée. C'est le crash du 2026-08-30 : Linphone coupe sa caméra, re-INVITE,
`StartSending` vidéo, première image à 10 240 kbps puis « Restore bitrate » à
2 048 kbps → réouverture → `VideoStream::SendVideo` envoie une trame morte →
corruption du tas → SIGSEGV dans `malloc`.

L'entrée des encodeurs est déjà partagée et sûre : `PictPtr = shared_ptr<Pict>`,
`SamplesPtr`. Seule la **sortie** est restée un prêt de pointeur brut.

## 2. Objectif

Une trame encodée est un objet **neuf à chaque image**, possédé par un
`std::shared_ptr`. Elle vit tant que quelqu'un la tient. L'encodeur n'en garde
aucune. Aucune réouverture, aucun `EncodeFrame` suivant, aucune destruction
d'encodeur ne peut l'invalider.

Même geste que `Pict` : une seule façon de partager, la copie interdite.

## 3. Hors périmètre

- Le **stockage** des octets reste le tampon `malloc` de `MediaFrame`. Passer à un
  `AVPacket` refcompté est un autre chantier, utile seulement quand le writer MP4
  passera à libavformat.
- Les **décodeurs** (`Decode(BYTE*, len)`) et le chemin de réception (dépaquetiseurs
  H.264/VP8, `mp4track` avec son `MediaFrame* frame` + `Clone()`) ne changent pas.
- `MediaFrame::Listener::onMediaFrame(MediaFrame&)` reste par référence : l'appel est
  synchrone, le récepteur copie s'il veut garder. On écrit ce contrat (§6).
- `RTPPacket` / `RTPPacketSched` : inchangés.

## 4. Conception

### 4.1 Types

Dans `medkit/video.h` et `medkit/audio.h`, à côté de `PictPtr` :

```cpp
typedef std::shared_ptr<VideoFrame> VideoFramePtr;
typedef std::shared_ptr<AudioFrame> AudioFramePtr;
```

Pas de `MediaFramePtr` tant qu'aucun appelant n'en a besoin.

### 4.2 API des encodeurs

```cpp
virtual VideoFramePtr VideoEncoder::EncodeFrame(PictPtr pic) = 0;
virtual AudioFramePtr AudioEncoder::EncodeFrame(SamplesPtr samples) = 0;
```

`nullptr` garde son sens : pas de trame produite (encodeur fermé, image bufferisée,
FIFO audio pas assez pleine). La boucle de purge audio ne change pas de forme :

```cpp
for (AudioFramePtr f = enc->EncodeFrame(samples); f; f = enc->EncodeFrame(nullptr))
```

### 4.3 `FfVideoEncoder`

- Le membre `VideoFrame* frame` **disparaît**. `EncodeFrame` alloue une
  `VideoFramePtr` locale (`std::make_shared<VideoFrame>(type, bufSize)`), la remplit,
  la packetise, la rend.
- `bufSize` garde la règle d'aujourd'hui (`1.5 * bitrate / fps`, plancher
  `AV_INPUT_BUFFER_MIN_SIZE`). `AppendMedia` agrandit de toute façon.
- `OpenCodec()` et `~FfVideoEncoder()` n'ont plus de trame à libérer.
- `PacketizeFrame()` prend la trame en paramètre : `virtual void
  PacketizeFrame(VideoFrame& frame)`. Quatre implémentations à adapter : défaut
  (`ffvideocodec.cpp`), `VP8Encoder`, `AV1Encoder`, `H264Encoder`.

Option écartée : garder un membre `VideoFramePtr frame` réassigné à chaque image,
pour ne pas toucher la signature de `PacketizeFrame`. Elle laisse un état caché
dans l'encodeur, ce que ce chantier veut justement supprimer.

### 4.4 `FfAudioEncoder`

- Le membre `AudioFrame* out` **disparaît**. `EncodeFromFifo()` produit une
  `AudioFramePtr` neuve par trame codec (`make_shared<AudioFrame>(type,
  GetClockRate())`, 2 048 octets comme aujourd'hui).
- `Open()` (ligne ~243, `out = new AudioFrame(...)`) ne fait plus cette allocation.
- Aucune sous-classe audio ne surcharge `EncodeFrame` : une seule implémentation à
  changer.

Coût : une allocation de 2 Ko par trame de 20 ms et par participant. Négligeable
devant l'encodage.

### 4.4 bis Pas de pool de trames

Décision : les trames sont allouées par `make_shared`, sans pool, audio comme
vidéo.

- glibc recycle déjà : une taille fixe demandée et rendue en boucle est servie par
  le tcache (audio, 2 Ko) ou par les bins du tas (vidéo, 100 à 200 Ko, sous le
  seuil `mmap` dynamique). Le coût est bien sous le µs ; une image 720p s'encode en
  22 ms.
- Le smoother alloue déjà un `RTPPacketSched` par paquet RTP et un
  `RtpPacketization` par fragment : une allocation de plus par image est du bruit.
- Un pool ajoute un état partagé (taille des slots qui change à chaque
  réouverture, verrou entre threads, deleter personnalisé) et fuit dès qu'un
  consommateur garde une trame, ce que le `shared_ptr` rend possible.

Critère de révision, mesuré et non supposé : sur un appel long sous
`perf record`, si `malloc` + `free` dépassent 1 % du temps CPU du process, ou si
le RSS dérive. Dans ce cas, la réponse est le stockage `AVPacket` avec
`av_buffer_pool` (§3), pas un pool maison.

### 4.5 Consommateurs

Le type de la variable locale change ; le corps ne change pas, car tous copient
déjà la trame de façon synchrone (smoother → `RTPPacketSched`, `memcpy` vers un
`RTPPacket`, FLV, MP4).

Dans `mcu/src` (9 fichiers, 16 sites) :

| Fichier | Sites |
|---|---|
| `videostream.cpp` | 625 |
| `audiostream.cpp` | 504-505 |
| `audioencoder.cpp` | 194-195 |
| `rtmpparticipant.cpp` | 606, 763-764 |
| `FLVEncoder.cpp` | 411-412, 608 |
| `jsr309/VideoEncoderWorker.cpp` | 637 |
| `jsr309/AudioEncoderWorker.cpp` | 291-292 |
| `jsr309/Recorder.cpp` | 214-215 |

Dans libmedikit hors encodeurs :

| Fichier | Compilé ici ? |
|---|---|
| `picturestreamer.cpp:57` | oui |
| `ffmp4reader.cpp:640-641` | oui |
| `transcoder.cpp:199` | **non** (`ASTERISK=yes` seulement) — appelle déjà une signature disparue (`EncodeFrame(dstV, decodedPicSize)`) : ce fichier ne compile plus depuis la migration `PictPtr`, il n'est pas touché ici |
| `mp4format.cpp:95` | **non** (`ASTERISK=yes` seulement) |

`mp4format.cpp` est modifié à l'aveugle et signalé dans la merge request :
personne ne peut le compiler sur cette machine.

`videostream.cpp` : le déplacement du « Restore bitrate » après `SendFrame` (correctif
du 2026-08-30) reste en place. Il n'est plus nécessaire à la sûreté, mais garder
l'appel après l'envoi évite de rouvrir le codec pendant qu'une image attend de
partir. Son commentaire est réécrit pour ne plus parler de mémoire libérée.

### 4.6 Tests

libmedikit (`tests/`) : `test_audio_codecs`, `test_h264_encoder_config`,
`test_video_encoder_reconfig`, `test_vp8_realtime`, `test_mp4_transcode`,
`test_mp4_roundtrip`, `test_mp4_prologue`, `test_mp4_read_order`,
`test_h264_hwaccel`.

mcu (`mcu/tests/`) : `test_video_encoder_inline`, `test_audio_pipes`,
`test_transcoder_characterization`, `test_transcoder_recette`,
`test_video_decoder_inline`.

Changement mécanique du type des locales. Ces tests sont le filet de non-régression.

## 5. Lots

Chaque lot compile et passe les deux suites. Un commit par lot ; le sous-module
d'abord, puis le pointeur de sous-module dans `mcu`.

### Lot 0 — caractériser le défaut (rouge avant, vert après)

Nouveau test libmedikit `tests/test_encoder_frame_lifetime.cpp` :

1. Ouvrir un `VP8Encoder`, encoder une image, garder la trame rendue.
2. Appeler `SetFrameRate` avec un débit divisé par 5 (réouverture garantie par
   `ShouldReopenForBitrate`).
3. Lire `GetType()`, `GetLength()`, `GetRtpPacketizationInfo()` de la trame gardée.
4. Même scénario audio : `OPUSEncoder`, garder la trame, appeler `EncodeFrame` une
   seconde fois, relire la première.

Aujourd'hui ce test est un use-after-free : il ne plante pas forcément, il **doit**
être joué sous `ASAN=yes` (cible `mcu/Makefile`, à reproduire dans le Makefile de
libmedikit si elle n'existe pas) pour être rouge de façon fiable. Après le lot 1 il
est vert sans instrumentation.

### Lot 1 — API et encodeurs (libmedikit)

`medkit/video.h`, `medkit/audio.h` (typedefs, signatures), `ffvideocodec.{h,cpp}`,
`ffaudiocodec.{h,cpp}`, `vp8/vp8encoder.cpp`, `av1/av1codec.cpp`,
`h264/h264encoder.{h,cpp}` (`EncodeFrame` délègue au parent : type de retour
seulement), `picturestreamer.cpp`, `ffmp4reader.cpp`, `transcoder.cpp`,
`mp4format.cpp`, les 9 tests de §4.6.

Vérification : `make -C third_party/fontventa/libmedikit check`.

### Lot 2 — consommateurs mcu

Les 9 fichiers de §4.5, les 5 tests de §4.6, réécriture du commentaire de
`videostream.cpp`.

**Piège** : un en-tête de libmedikit change la disposition des classes. `make clean`
dans `mcu/` est **obligatoire** avant de rebâtir, sinon des `.o` périmés lient un
binaire qui plante ailleurs.

Vérification : `cd mcu && make clean && make check`, puis `make check ASAN=yes`.

Résultat du 2026-08-30 : 612 tests verts en build normal. Sous ASAN, la seule
erreur est préexistante et hors périmètre : `RTPSession::SetLocalCryptoSDES`
trace la clé SRTP (30 octets binaires, sans NUL) avec `%s`, lecture hors du
tampon de pile de `DTLSConnection::SetupSRTP` (`rtpsession.cpp:457`). À corriger
à part : ne pas tracer la clé.

Côté libmedikit sous ASAN : 160 verts ; les deux tests `VideoEncoderReconfig.*Av1*`
sont exclus du passage ASAN car le destructeur de SVT-AV1 (`svt_enc_handle_dctor`)
s'y bloque sur un sémaphore interne, dans la bibliothèque, uniquement instrumenté.

### Lot 3 — contrat écrit

`docs/reference/trames-encodees.md` (un fichier par sujet technique ;
`CODECS.md` traite du `fmtp`, pas de la mémoire) :
- entrée et sortie des encodeurs sont des `shared_ptr` ;
- `onMediaFrame(MediaFrame&)` prête la trame le temps de l'appel, le récepteur
  copie s'il garde ;
- un encodeur ne possède aucune trame encodée.

### Lot 4 — recette

Rejouer le scénario du crash : Bob (baresip) puis Alice (Linphone) dans la même
conférence, Alice coupe sa caméra, la rallume, recommence trois fois. Attendu :
aucun `No smoother for frame`, aucun core, vidéo de retour chez Alice à la
réactivation.

## 6. Ce qu'un relecteur doit vérifier

- Aucun `VideoFrame*` / `AudioFrame*` brut ne sort plus d'un encodeur (`grep
  'Frame\* *EncodeFrame'` vide dans les deux dépôts).
- Aucun `delete` de trame dans `ffvideocodec.cpp` ni `ffaudiocodec.cpp`.
- Les fichiers `ASTERISK=yes` ont bien été modifiés même s'ils ne compilent pas ici.
- Le test du lot 0 est joué sous ASAN au moins une fois (trace dans la merge request).
- Le pointeur de sous-module du commit mcu pointe sur le commit libmedikit du lot 1.
