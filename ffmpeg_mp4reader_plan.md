# Plan — `mp4reader` basé sur ffmpeg/libavformat (lecture de MP4 sans hint)

> Objectif : remplacer la **lecture** MP4 de libmedikit (aujourd'hui basée sur
> les *hint tracks* RTP de mp4v2) par un demux **libavformat**, afin de pouvoir
> lire n'importe quel MP4 — hinté ou non — aussi bien dans **`app_mp4`** que
> dans le **`MP4Streamer` du mediaserver** (chemin player JSR309).
>
> L'**écriture** (`mp4recorder`/`mp4writer`) reste sur mp4v2 pour l'instant.
> C'est un split *reader = ffmpeg / writer = mp4v2* parfaitement viable, et la
> première brique concrète du chantier décrit dans `supp_mp4v2.md`.
>
> Statut : **IMPLÉMENTÉ** (2026-07-08). P0→P7 réalisés ; les deux appelants
> (`MP4Streamer` mcu + `mp4format.cpp`/`app_mp4.c` Asterisk) migrés vers
> `Mp4FfReader`. Build **mediaserver vert**. Reste : validation **in-vivo**
> (serveur réel + pair SIP/WebRTC ; env Asterisk pour `app_mp4`) et un cleanup
> optionnel du code mort mp4v2 côté lecture (cf. §7.e).

---

## 0. Décisions de conception (à acter avant de coder)

1. **Reader autonome qui ouvre le fichier lui-même.** Le nouveau `mp4reader`
   possède son propre `AVFormatContext*` (ouvert via `avformat_open_input`).
   On **ne** fait **pas** transiter un handle ouvert par l'appelant.
   → Cela s'écarte de `supp_mp4v2.md §3` (« passer l'`AVFormatContext*` dans
   l'API »), mais c'est ce qui permet de **partager le même reader** entre
   `MP4Streamer` (qui faisait `MP4Read` lui-même) et `app_mp4` sans dupliquer la
   logique d'ouverture. L'appelant passe **un chemin de fichier**.

2. **On conserve le contrat `MediaFrame` + `RtpPacketizationInfo`.** Le reader
   continue de retourner, via `GetNextFrame()`, une `MediaFrame` **déjà
   packetisée RTP** (comme aujourd'hui après `Packetize(1400)`).
   → On **ne** part **pas** sur l'« option D / packetisation différée » de
   `supp_mp4v2.md`. Raison : `MP4Streamer::DispatchRtp` (mcu) et
   `Mp4PlayerPlayNextFrame`/`MediaFrameToAstFrame2` (app_mp4) **itèrent tous les
   deux `GetRtpPacketizationInfo()`**. En préservant ce contrat, le code aval
   des deux appelants **ne change quasiment pas** : on ne remplace que la
   *source* des trames (libavformat au lieu des hint tracks mp4v2).

3. **Packetisation par le packetiseur maison existant, pas de BSF ffmpeg.**
   `VideoFrame::PacketizeH264` (video.cpp) sait déjà lire l'**AVCC**
   (NALU préfixés par longueur) via `SetH264NalSizeLength(nalLengthSize)`
   (video.h:66) — pas seulement l'Annex-B. On lit donc l'échantillon AVCC tel
   que libavformat le fournit, on règle la taille de préfixe, on préfixe SPS/PPS
   (extradata `avcC`) sur les trames intra, puis `Packetize(1400)`.
   → Pas besoin du bitstream filter `h264_mp4toannexb`. (Il reste une option de
   repli si un flux pose problème.)

4. **Passthrough natif d'abord, transcodage ensuite.** Version initiale : on
   ré-émet le codec du fichier tel quel — vidéo H264/H263/VP8, audio **codecs
   télécom** PCMU/PCMA/AMR/G722/OPUS. **AAC exclu de la v1** (décision c) : le
   Player est purement passthrough, or un flux AAC réémis en RTP vers un pair
   SIP/WebRTC (PCMU/PCMA/OPUS) est inexploitable sans transcodage. Un fichier
   ffmpeg à audio AAC jouera donc sa vidéo, audio ignoré. Le transcodage
   (décoder + ré-encoder vers le codec négocié) est un chantier séparé qui
   réutilisera `FfVideoDecoder`/`FfVideoEncoder` / `FfAudioCodec`.

5. **Portée limitée à la lecture.** `mp4recorder`/`mp4writer`/hinting mp4v2 :
   **inchangés**.

---

## 1. Constat technique (pourquoi le hint bloque, ce qui est réutilisable)

### Ce qui casse aujourd'hui
- `mp4reader::OpenTrack(...)` (mp4reader.cpp) **ne découvre les pistes que via
  les hint tracks RTP** : boucle sur `MP4FindTrackId(mp4, i, MP4_HINT_TRACK_TYPE, 0)`.
- `toto.mp4` (ffmpeg/Lavf, vérifié : 1 piste vidéo H264 avc1, timescale 30000,
  400 samples, **aucun atome `rtp `/`hnti`/`tref`**) → aucune hint track →
  `OpenTrack` renvoie 0 → **aucune piste ouverte** → `Eof()` vrai au 1ᵉʳ
  `GetNextFrame` → `PlayLoop` ne diffuse rien → `onEnd()` en quelques ms.
  C'est le faux symptôme « joue trop vite / se termine en < 1 s ».
- `ReadFrameWithoutHint` (mp4track.cpp:210) est de toute façon cassé (appelle
  `MP4ReadRtpHint` sur un `hinttrack` invalide, ligne 229).

### Ce qui est réutilisable tel quel (ne pas réécrire)
- **Le packetiseur RTP** : `VideoFrame::Packetize`/`PacketizeH264`/`PacketizeH263`
  (video.cpp), `AudioFrame::Packetize` (audio.cpp:114). H264 gère AVCC **et**
  Annex-B via `SetH264NalSizeLength()`.
- **Le préfixage des paramètres H264** : `VideoFrame::PrependWithFrame(paramFrame)`
  (utilisé en mp4track.cpp:788) + logique `ReadH264Params`. On reconstruira le
  `paramFrame` depuis l'`extradata avcC` de libavformat au lieu de
  `MP4GetTrackH264SeqPictHeaders`.
- **Le modèle de cadencement** : horloge murale `startPlaying` + `next[]` +
  `waittime = t - now`. On le **garde à l'identique** pour ne pas toucher
  `MP4Streamer::PlayLoop` ni la boucle de `mp4_play`.
- **Le binding Asterisk** : `MediaFrameToAstFrame2` (frameutils.cpp:115) et
  `Mp4PlayerPlayNextFrame` (mp4format.cpp) restent tels quels tant que la
  `MediaFrame` porte sa `RtpPacketizationInfo`.
- **Le pattern demux ffmpeg** : déjà présent dans `logo.cpp`
  (`avformat_open_input` → `avformat_find_stream_info` → `avcodec_find_decoder`
  → `codecpar`). À recopier.

---

## 2. Structure interne du nouveau `mp4reader`

Fichier : `third_party/fontventa/libmedikit/mp4reader.cpp` + `medkit/mp4reader.h`
(réécriture de l'implémentation, en conservant au maximum la **signature
publique** — voir §3).

### 2.1 État (membres privés)
```
AVFormatContext*  fmtctx;                 // ouvert par le reader
int               videoStreamIdx;         // -1 si absent
int               audioStreamIdx;         // -1
int               textStreamIdx;          // -1 (phase 2)
VideoCodec::Type  videoCodec;             // codec natif du fichier
AudioCodec::Type  audioCodec;
VideoFrame*       videoParamFrame;        // SPS/PPS reconstruits depuis avcC
VideoFrame        videoFrameBuf;          // buffers réutilisables (comme aujourd'hui)
AudioFrame        audioFrameBuf;
DWORD             videoNalLengthSize;     // depuis avcC (1..4), toto.mp4 = 4
QWORD             next[...];              // prochaine échéance par média (ms)
int64_t           firstPtsMs[...];        // normalisation start_time
timeval           startPlaying;           // horloge murale (inchangé)
QWORD             currentTs;
// file d'attente 1 paquet look-ahead (voir §5.3)
AVPacket*         pending;                // paquet lu mais pas encore dû
```

### 2.2 Cycle de vie
- **Constructeur** : `mp4reader(void* ctxdata, const char* filename)` →
  `avformat_open_input` + `avformat_find_stream_info`. (Nouvelle signature :
  chemin au lieu de `MP4FileHandle` — voir §3, impact appelants.)
- **Destructeur** : `avformat_close_input`, libère `pending`, `videoParamFrame`,
  filtre BSF éventuel.

---

## 3. API publique et impact sur les appelants

On garde les **noms** de méthodes pour minimiser la diffusion des changements,
mais deux points changent :

### 3.1 Ouverture
| Avant | Après |
|---|---|
| `mp4reader(void*, MP4FileHandle)` + l'appelant fait `MP4Read`/`MP4Close` | `mp4reader(void*, const char* filename)` : le reader ouvre/ferme via libavformat |

- **`MP4Streamer::Open` (mcu/src/mp4streamer.cpp)** : supprimer `MP4Read`,
  `MP4Close`, le membre `MP4FileHandle mp4`, l'`#include <mp4v2/mp4v2.h>`.
  Construire `reader = new mp4reader(NULL, filename)`. `Close()` ne fait plus que
  `delete reader`.
- **`Mp4PlayerCreate` (mp4format.cpp) + `app_mp4.c`** : `app_mp4.c` fait
  aujourd'hui `MP4Read(cformat1)` puis passe le handle. → passer `cformat1`
  (le chemin) à `Mp4PlayerCreate`, qui construit le reader avec le chemin.
  Supprimer `MP4Read`/`MP4Close` de `app_mp4.c`.

### 3.2 Sélection des pistes (`OpenTrack`)
Les 3 surcharges `OpenTrack` (audio/vidéo/texte) sont **conservées en
signature** mais réinterprétées : elles ne cherchent plus une hint track ; elles
**sélectionnent le meilleur stream libavformat** parmi les codecs demandés.
- Retour `>0` si un stream compatible est trouvé, `0` sinon (contrat inchangé).
- `prefCodec` : priorité. `cantranscode` : ignoré en v1 (passthrough) — documenté
  comme réservé au futur transcodage.
- **IMPLÉMENTÉ (2026-07-08, cf. P6)** : la sélection est **différée du ctor à
  `OpenTrack`**. Le ctor ne détecte plus que la piste texte ; `OpenTrack`
  re-scanne `fmtctx`, retient la piste dont le codec == `prefCodec` (priorité
  absolue), sinon la mieux classée dans `outputCodecs`, puis (re)crée les buffers
  et calcule `videoNalLengthSize`/`BuildVideoParams` pour la piste choisie.
  Indispensable pour les enregistrements maison **multi-codec alternatifs**
  (même média en PCMU ET PCMA) : sans ça, la 1ʳᵉ piste gagnait toujours.
- Alternative plus propre à discuter : ajouter un simple `Open()` qui
  sélectionne tout automatiquement et **déprécier** les `OpenTrack`. Mais garder
  `OpenTrack` limite le diff chez les deux appelants → **recommandé pour la v1**.

### 3.3 Méthodes inchangées (comportement identique)
`GetNextFrame`, `Rewind`, `Eof`, `Seek`, `PreSeek`, `Tell`, `GetDuration`,
`GetVideoWidth/Height/Bitrate/Framerate`, `GetAVCDescriptor`,
`HasAudio/Video/TextTrack`, `GetCodec`, `GetVideoCodec`. → **aucun changement
côté `MP4Streamer::PlayLoop` ni côté `mp4_play`.**

---

## 4. Détails par sous-système

### 4.1 Mapping codec ffmpeg ↔ medkit
Aucune table `AV_CODEC_ID_* ↔ Codec::Type` n'existe encore dans libmedikit → à
créer (petit helper dans `mp4reader.cpp`, ou mutualisé dans un `.h`).

| `AVCodecID` | `VideoCodec::Type` / `AudioCodec::Type` |
|---|---|
| `AV_CODEC_ID_H264` | `VideoCodec::H264` |
| `AV_CODEC_ID_H263`, `H263P` | `H263_1996` / `H263_1998` |
| `AV_CODEC_ID_VP8` | `VP8` |
| `AV_CODEC_ID_AAC` | `AudioCodec::AAC` |
| `AV_CODEC_ID_PCM_MULAW` / `PCM_ALAW` | `PCMU` / `PCMA` |
| `AV_CODEC_ID_AMR_NB` | `AMR` |
| `AV_CODEC_ID_OPUS` | `OPUS` |
| `AV_CODEC_ID_ADPCM_G722` | `G722` |
| `AV_CODEC_ID_GSM_MS` / `GSM` | `GSM` |

Codec non mappé → la piste n'est pas sélectionnée (comme un « format non
supporté »).

### 4.2 Ouverture H264 : extradata avcC → paramFrame + nalLengthSize
- `codecpar->extradata` contient le **`avcC`** : `nalLengthSizeMinusOne`
  (→ `videoNalLengthSize = (extradata[4] & 0x03) + 1`, = 4 pour `toto.mp4`) et
  les SPS/PPS.
- Parser SPS/PPS depuis l'`avcC` (offsets standard : après l'octet
  `numOfSequenceParameterSets`, boucles `spsLength`/`ppsLength`) et construire
  `videoParamFrame` (NALU en AVCC, `SetH264NalSizeLength(videoNalLengthSize)`),
  exactement comme `ReadH264Params` le faisait depuis mp4v2.
- Alimenter aussi `GetAVCDescriptor()` depuis l'`avcC` (mêmes champs :
  profile/level/nalLengthSize/SPS/PPS) — utilisé par la signalisation JSR309.

### 4.3 Boucle de lecture `GetNextFrame` — cadencement par dts, estampille par pts
**Décision b actée** : on s'appuie sur l'entrelacement natif d'`av_read_frame`
(curseur de démux unique) avec **1 paquet d'avance** (`pending`), et on **pace
l'émission par `dts`** en estampillant le RTP par `pts`. `toto.mp4` a des
B-frames (`has_b_frames=2`) donc `dts ≠ pts` : pacer par `dts` (ordre de
décodage, monotone = ordre `av_read_frame`) et estampiller RTP par `pts→90 kHz`
est le comportement RTP correct (le récepteur réordonne). On **ne** rejoue
**pas** la logique `next[]` par piste (elle forcerait des seeks par piste, mal
supportés par libavformat).
- **échéance dts → ms** : `t = (pkt->dts - stream->start_time) * av_q2d(time_base) * 1000`.
- **timestamp RTP** : `pts` converti à la clock du codec (vidéo 90 kHz :
  `(pkt->pts - start_time) * av_q2d(time_base) * 90000`).
- **Ordonnancement / attente** : calcul inchangé
  `now = getDifTime(&startPlaying)/1000` ; si `now < t` → garder `pending`,
  `waittime = t - now`, retour `NULL` ; sinon consommer `pending`, renvoyer la
  trame, et recalculer `waittime` sur le prochain paquet lu (nouveau `pending`).
- Construire la `MediaFrame` :
  - **Vidéo H264** : copier les données AVCC du paquet dans `videoFrameBuf`,
    `SetIntra(pkt->flags & AV_PKT_FLAG_KEY)`, `SetH264NalSizeLength(videoNalLengthSize)` ;
    si intra → `PrependWithFrame(videoParamFrame)` ; timestamp RTP = pts en 90 kHz
    (`pts * 90000 * av_q2d(time_base)`) ; puis `Packetize(1400)`.
  - **Audio** : copier l'échantillon, timestamp RTP à la clock du codec
    (8 kHz PCMU/PCMA, etc.), `Packetize(1400)`.
- **EOF** : `av_read_frame` renvoie `AVERROR_EOF` → `errcode = -1` (comme
  aujourd'hui). Marquer `next[...] = MP4_INVALID_TIMESTAMP` pour que `Eof()` soit
  cohérent.

### 4.4 Seek / Rewind
- **Rewind** : `av_seek_frame(fmtctx, -1, 0, AVSEEK_FLAG_BACKWARD)` +
  `avformat_flush`, reset `startPlaying`, vider `pending`.
- **Seek(ms)** : convertir ms → pts du stream vidéo, `av_seek_frame(videoStreamIdx,
  ts, AVSEEK_FLAG_BACKWARD)` (se cale sur la keyframe précédente), puis
  recaler l'horloge murale **exactement comme le `Seek` actuel** (mp4reader.cpp:711
  : `gettimeofday(&startPlaying)` puis soustraire `actualTime`). `PreSeek` :
  renvoyer le temps de la keyframe cible sans consommer (via
  `av_index_search_timestamp` ou lecture spéculative).

### 4.5 Métadonnées
- `GetDuration` : `fmtctx->duration / (double)AV_TIME_BASE`.
- `GetVideoWidth/Height` : `codecpar->width/height`.
- `GetVideoFramerate` : `av_q2d(stream->avg_frame_rate)`.
- `GetVideoBitrate` : `codecpar->bit_rate`.

### 4.6 Texte / T.140 — ✅ FAIT (2026-07-07)
Intégré : `Mp4FfReader` détecte la piste `mov_text`/tx3g
(`AVMEDIA_TYPE_SUBTITLE`+`AV_CODEC_ID_MOV_TEXT`), `OpenTrack(TextCodec)` l'active
(crée `SubtitleToRtt`, + `RTPRedundantEncoder` si `T140RED`), et `BuildFrame`
convertit chaque échantillon tx3g (`[2 o longueur][UTF-8]`) en **T.140
incrémental** (`GetTextDiff` + backspaces 0x08), packetisé, enrobé `red` en
option ; `GetNextFrame` gère la retransmission RTT idle (BOM/repeat).
`HasTextTrack()` reflète désormais la détection. Le câblage aval existait déjà
(`MP4Streamer::Open` ouvre `T140`, `Dispatch`→`onTextFrame`) : **le player JSR309
reçoit maintenant le texte** (T.140 nu). Bug latent de l'ancien reader corrigé
(`insert` aux arguments inversés). Validé sur un MP4 H264+`mov_text` généré.
Réserve : correctness RTP du chemin `T140RED` à revalider lors de la migration
`app_mp4`/`mp4format` (`mp4streamer` n'utilise que `T140` nu).

---

## 5. Découpage en phases / commits

- **P0 — Harnais de test hors-ligne.** Petit exécutable
  (`tools/`) qui instancie le nouveau `mp4reader` sur un fichier, imprime
  pistes détectées, codecs, et pour les N premières trames : pts(ms), intra,
  taille, `waittime`, nb de paquets RTP. Sert de garde-fou pendant tout le
  chantier. **Ne dépend pas d'Asterisk ni du mediaserver.**
- **P1 — Ouverture + métadonnées + mapping codec.** `avformat_open_input`,
  sélection streams, `GetDuration/Width/Height/...`, `GetAVCDescriptor` depuis
  `avcC`. `HasVideoTrack/HasAudioTrack` corrects sur `toto.mp4`.
- **P2 — Lecture vidéo H264 passthrough.** `GetNextFrame` : pts→ms, AVCC +
  SPS/PPS + `Packetize`. Valider avec P0 que 400 trames sortent, ~13,3 s, RTP
  cohérent.
- **P3 — Audio passthrough** (PCMU/PCMA/AAC/OPUS/AMR selon fichiers de test).
  **✅ FAIT (2026-07-07).** Validé sur Opus (mp4) et AMR-NB (3gp), seul/mixé
  avec H264. Bug corrigé : `AudioFrame::Packetize` fragmentait toute trame
  > 160 o (Opus indécodable) → `packetization` rendu codec-aware dans le ctor
  `AudioFrame` (`medkit/audio.h`) : sample-based (PCMU/PCMA/G722) = 160 o/20 ms,
  frame-based (Opus/AMR/GSM/…) = 1 trame codée = 1 paquet RTP. Build vert
  (libmedkit.a + mcu). Réserve : PCMU/PCMA/G722/GSM non muxables MP4 par ffmpeg
  et nuance format RTP AMR/GSM (RFC 4867 CMR/ToC vs storage) → validation sur
  enregistrement maison en P6/in-vivo.
- **P4 — Seek/Rewind/PreSeek** + recalage horloge.
- **P5 — Branchements appelants — ✅ FAIT (2026-07-08).**
  - `MP4Streamer` (mcu) : nouvelle construction par chemin, suppression mp4v2.
    **FAIT** (`reader = new Mp4FfReader(filename)` + `IsOpen()`, build vert).
  - `mp4format.cpp` + `app_mp4.c` (Asterisk) : **FAIT**.
    - `astmedkit/mp4format.h` : include `medkit/ffmp4reader.h` (au lieu de
      `mp4reader.h`) ; `Mp4PlayerCreate(chan, **const char\* filename**, …)`.
    - `mp4format.cpp` : `Mp4PlayerCreate` construit `new Mp4FfReader(filename)`
      + garde `IsOpen()` ; `Rewind()` **après** ouverture des pistes et **avant**
      le 1ᵉʳ `GetNextFrame` (sinon le reader ffmpeg ne cadence pas — `startPlaying`
      non amorcée) ; `Mp4PlayerPlayNextFrame`/`Destroy` castent en `Mp4FfReader*` ;
      l'ancien membre public `mp4reader::buffer` est remplacé par un **tampon local**
      dans `Mp4PlayerPlayNextFrame` (une trame est consommée dans l'appel).
    - `app_mp4.c` (`mp4_play`) : plus de `MP4Read`/`MP4Close`/`MP4FileHandle mp4`
      dans le chemin lecture ; on passe `cformat1` (le chemin) à `Mp4PlayerCreate`.
      Le chemin **enregistrement** (`mp4_save`) reste sur mp4v2 (`MP4Create`/`MP4Close`).
    - **NON compilé/testé ici** (pas d'environnement Asterisk dans ce dépôt :
      `mp4format.o` est `ASTOBJ`, exclu du build mediaserver `ASTERISK=no` ;
      `app_mp4` n'est pas dans le build mediaserver) → migration **à l'aveugle,
      revue seulement**. À valider dans l'env de build Asterisk d'IVèS.
      Le build **mediaserver reste vert** (mcu relié, binaire produit).
- **P6 — Régression fichier hinté — ✅ FAIT (2026-07-08).** Validé sur un vrai
  enregistrement maison `OLD_260120-113550_50665297.mp4` (`MP4Save asterisk
  application`, hinté, multipiste) : H264 480x360 + **PCMU** + **PCMA** (deux
  pistes audio **alternatives** = même audio sur les 50 s, 2513 paquets chacune)
  + sous-titre `mov_text` + 3 pistes hint `rtp`. Résultat via `ffmp4probe` :
  ouverture OK (hints ignorés), 4047 trames (1533 vidéo/1 intra, 2513 audio,
  1 texte), payload RTP max 1402 (1400 + en-tête FU-A, normal), **cadencement
  réel 50.42 s ≈ 50.45 s** (le bug « <1 s » est bien absent sur fichier réel).
  Codecs télécom **PCMU/PCMA passthrough validés en réel** (jamais testés
  jusqu'ici, non muxables par ffmpeg — cf. réserve P3). 1 seul IDR dans le
  fichier → `Seek(mid)`→0 (comportement correct, une seule sync frame).
  **CORRECTIF apporté** : `OpenTrack(audio/vidéo)` ignorait `prefCodec` (le ctor
  verrouillait la 1ʳᵉ piste mappable) → sur un fichier multi-codec alternatif,
  impossible de choisir PCMA. Sélection désormais **différée à `OpenTrack`** :
  re-scan de `fmtctx`, priorité absolue à `prefCodec`, repli sur l'ordre de
  `outputCodecs` (cf. §3.2). Vérifié : `prefCodec=PCMU`→PCMU, `PCMA`→PCMA, les
  deux jouent 50 s ; `toto.mp4` (vidéo seule) non régressé ; build mcu vert.
  **Négociation de codec côté Player — ✅ FAIT (2026-07-08).** Le mediaserver
  choisit désormais l'alternative acceptée par le pair, en réutilisant
  l'échafaudage `RTPMultiplexer::TryCodec` (« codec accepté par TOUS les
  endpoints attachés », consulte la `rtpMap` de sortie négociée) :
  - `Mp4FfReader` : + `HasAudioCodec/HasVideoCodec(codec)` (requête **sans
    effet de bord**, ne change pas la piste choisie).
  - `MP4Streamer` : + `HasAudioCodec/HasVideoCodec` (délégation) et
    `SetAudioCodec/SetVideoCodec(codec)` (re-`OpenTrack` **liste à 1 codec** =
    match exact non destructif, recrée le `RTPPacket` réutilisable ; refusé si
    `playing`). L'ancien `AudioCodec::PCMU`/`H264` en dur de `Open` n'est plus
    qu'un **défaut** (repli si pas de négociation).
  - `Player::NegotiateCodecs()` : pour chaque codec candidat (ordre serveur),
    si le fichier l'a ET `audio/video.TryCodec(c)==c` → `SetAudioCodec/SetVideoCodec`.
  - `MediaSession::PlayerPlay` : appelle `player->NegotiateCodecs()` **avant**
    `player->Play()` — donc **après** l'attach (contrairement à `PlayerOpen` où
    l'endpoint n'est pas encore joint). Repli sûr : aucun endpoint attaché ou
    aucun codec commun → défaut d'`Open` conservé.
  - Corrigé au passage : `RTPMultiplexer::TryCodec` avait `rez1/rez2` non
    initialisés + **aucune garde liste vide** (valeur indéterminée) → réécrit
    proprement (retourne `codec` ssi tous acceptent, `-1` sinon/si vide).
  Multi-endpoints géré nativement par `TryCodec` (intersection). Build mcu vert.
  **RESTE (validation in-vivo)** : test réel serveur + pair SIP/WebRTC (négocier
  PCMA, vérifier que le mediaserver lit bien la piste PCMA) — étape déploiement
  elixip, non faisable hors-ligne.
- **P7 (option)** — Texte T.140 **✅ FAIT (2026-07-07)** (cf. §4.6) ; reste,
  séparément, le transcodage.

Chaque phase = build vert + passage du harnais P0.

---

## 6. Plan de test

1. **Hors-ligne (P0)** sur `toto.mp4` :
   - pistes = {vidéo H264}, audio absent, durée ≈ 13,35 s ;
   - 400 trames, pts croissants réguliers (~33 ms à 30 fps) ;
   - `waittime` cumulé ≈ durée réelle (preuve que le cadencement est bon).
2. **Intégration JSR309** : `PlayerOpen("toto.mp4")` + `PlayerPlay`, mesurer que
   la lecture dure **~13 s** (et non < 1 s) ; vérifier les paquets RTP H264 émis
   (SPS/PPS présents sur les intra) côté endpoint ; `onEndOfFile` en fin réelle.
3. **app_mp4** (si buildable dans l'environnement Asterisk cible) ou au moins
   compilation de `mp4format.cpp` + revue de `MediaFrameToAstFrame2`.
4. **Régression** : rejouer un `.mp4` **enregistré par le mediaserver** (hinté)
   → doit jouer à l'identique via le chemin ffmpeg.
5. **Fichiers variés** : un MP4 audio-only (AAC), un MP4 H264+AAC, un H264 avec
   B-frames, pour valider pts vs dts et le passthrough audio.

---

## 7. Décisions tranchées (b, c, d, f) et points restants

### Tranchées
- **b. Ordonnancement — `av_read_frame` natif, pacer par `dts`, estampiller par
  `pts`.** 1 paquet d'avance (`pending`). Pas de `next[]` par piste. (cf. §4.3.)
  *Raison* : curseur de démux unique ; B-frames dans `toto.mp4` → `dts ≠ pts`.
- **c.bis TRANSCODAGE AUDIO — ✅ FAIT (2026-07-08, AAC→PCMU/PCMA).** Le repli
  transcodage lève la restriction « AAC hors v1 » pour les cibles télécom G711.
  Chaîne dans `Mp4FfReader::OpenAudioTranscoded(target)` : décodeur source
  obtenu **via la fabrique** `AudioCodecFactory::CreateDecoder(codec, extradata,
  size)` — l'AAC est une **classe `AACDecoder : public FfAudioDecoder`** (pendant
  d'`AACEncoder`, `aac/aacdecoder.{h,cpp}`), enregistrée dans la fabrique et
  mappée dans `MapAudioCodec` ; l'extradata/ASC (indispensable à l'AAC raw des
  MP4) est passée **génériquement** par la fabrique (aucun cas particulier AAC
  dans le reader ; le nouveau ctor extradata de `FfAudioDecoder` est une
  capacité générale) → **resampling libswresample** (srcRate du conteneur `codecpar->sample_rate`
  → 8000) → encodeur cible G711, nourri par **tranches de 20 ms** (`GetNextFrame`
  écoule une file `audioOutQueue`, un paquet source ⇒ 0..n trames de 160 o).
  Câblage : `MP4Streamer::SetAudioCodecTranscoded` + `Player::NegotiateCodecs`
  phase 2 (si aucun codec fichier accepté en passthrough → transcode vers un
  codec accepté par le pair). Validé hors-ligne : AAC 44,1k→PCMU et 48k→PCMA =
  150 trames/3 s, 160 o, cadencement temps réel ; passthrough non régressé.
  **Cibles v1 = PCMU/PCMA uniquement** (encodeurs G711 maison). G722/GSM/AMR/OPUS
  rendent 0 trame (encodeurs ffmpeg frame-based, sémantique de taille de trame
  non satisfaite par le découpage actuel) → exclus tant que non validés.
  PIÈGE rencontré : `pcmucodec.o` **périmé** dans `libmedkit.a` (ABI décalée,
  `Encode(160)` renvoyait 8000) → clean rebuild de libmedkit nécessaire.
- **c. AAC en RTP — non en v1.** Le Player est **purement passthrough**
  (`mp4streamer.cpp:98-100` crée les `RTPPacket` avec le codec du fichier).
  Réémettre de l'AAC vers un pair télécom (PCMU/PCMA/OPUS) est inexploitable sans
  transcodage. v1 = **codecs télécom passthrough** (PCMU/PCMA/AMR/G722/OPUS,
  = ce que contiennent les enregistrements maison). Fichiers ffmpeg (AAC) → on
  joue la vidéo, audio ignoré jusqu'au chantier transcodage. `toto.mp4` n'a pas
  d'audio → sans impact sur l'objectif immédiat.
- **d. Texte T.140 — phase 2.** Le canal texte JSR309 existe (`Endpoint.cpp`,
  `WSEndpoint.cpp`) mais pour du RTT **live** ; rejouer du `tx3g`→T140 depuis un
  fichier (via `SubtitleToRtt`+`red`) est de niche, absent de `toto.mp4`, coûteux.
  v1 : `HasTextTrack()=false`.
- **f. Contrat `RtpPacketizationInfo` — préservé en v1.** L'« option D » de
  `supp_mp4v2` (packetisation ffmpeg différée) imposerait de réécrire
  `DispatchRtp` (mcu) **et** `MediaFrameToAstFrame2`/`Mp4PlayerPlayNextFrame`
  (app_mp4). On garde les packetiseurs maison (qui gèrent déjà l'AVCC). Convergence
  vers la packetisation différée reportée au chantier transcodage.

### Restants
- **a. Signature de construction — ✅ TRANCHÉ (chemin `const char*`).** Les **deux**
  appelants construisent désormais par chemin : `MP4Streamer` (`new
  Mp4FfReader(filename)`) et `Mp4PlayerCreate(chan, filename, …)`. L'option
  `AVFormatContext*` traversant l'API est abandonnée (le chemin partage la logique
  d'ouverture sans duplication).
- **e. mp4v2 côté lecture — ✅ FAIT (2026-07-08).** Après P5, **plus aucun appel
  mp4v2 dans le chemin lecture** : `Mp4FfReader`/`ffmp4reader.h` n'incluent pas
  mp4v2, et ni `MP4Streamer` ni `Mp4PlayerCreate` n'ouvrent de `MP4FileHandle`.
  On garde `-lmp4v2` pour l'**écriture** (`mp4writer`, `mp4_save`).
  La classe **`mp4reader` historique (`mp4reader.cpp` + `medkit/mp4reader.h`,
  mp4v2) est SUPPRIMÉE** (`git rm`) et retirée du `Makefile` (`OBJS` + règle de
  dépendance) ; `libmedkit.a` ré-archivée (l'ancien `mp4reader.o` n'y est plus),
  build mcu vert.
  **`mp4track.cpp`/`Mp4Basetrack` sont CONSERVÉS** : contrairement à ce que
  laissait entendre §8, ils sont **toujours utilisés par `mp4writer.cpp`**
  (écriture), donc pas du code mort. Seul `mp4reader` était mort.
- **d.bis** : confirmer que personne ne rejoue de **fichiers RTT enregistrés**
  (sinon remonter le texte en v1). *(Texte T.140 déjà implémenté en P7, cf. §4.6.)*

---

## 8. Fichiers touchés (récapitulatif)

| Fichier | Nature du changement | Statut |
|---|---|---|
| `third_party/fontventa/libmedikit/ffmp4reader.cpp` + `medkit/ffmp4reader.h` | **Nouveau** reader libavformat (`Mp4FfReader`, ctor par chemin) | ✅ |
| `mcu/src/mp4streamer.cpp` / `.h` (mcu) | Ouverture par chemin, retrait `MP4Read/MP4Close`, handle mp4v2, API codec typée + négociation | ✅ |
| `third_party/fontventa/libmedikit/astmedkit/mp4format.h` | Include `ffmp4reader.h` ; `Mp4PlayerCreate(chan, const char* filename, …)` | ✅ |
| `third_party/fontventa/libmedikit/mp4format.cpp` | `Mp4PlayerCreate` construit `Mp4FfReader(filename)` + `Rewind()` ; casts `Mp4FfReader*` ; tampon local | ✅ (aveugle, non compilé ici) |
| `third_party/fontventa/app_mp4/app_mp4.c` | `mp4_play` ne fait plus `MP4Read/MP4Close` ; passe le chemin | ✅ (aveugle, non compilé ici) |
| `third_party/fontventa/libmedikit/Makefile` | dép `mp4format.o` : `mp4reader.h` → `ffmp4reader.h` | ✅ |
| `third_party/fontventa/libmedikit/tools/ffmp4probe.cpp` | Harnais de test P0 (hors-ligne) | ✅ (non commité) |
| `libmedikit/mp4reader.cpp` + `medkit/mp4reader.h` | **SUPPRIMÉS** (ancien reader mp4v2, code mort) + retirés du `Makefile` | ✅ |
| `libmedikit/mp4track.cpp` / `Mp4Basetrack` | **CONSERVÉS** — toujours utilisés par `mp4writer` (écriture) | — |

libmedikit et mcu **lient déjà** `-lavformat -lavcodec -lavutil -lswresample`
(cf. `mp4format`/Makefile) — pas de nouvelle dépendance de build.
