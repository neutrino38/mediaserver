# Problème

Les images décompressées sont transportées sous formes de buffer YUV au sein du mediaserver.
Beaucoup de traitement se font via FFMPEG et l'on doit souvent basculer de ces buffers vers un AVFrame
et vice et versa.

Un cas clair :  la chaine RTP -> VideoDecodeur -> VideoStream -> Resize -> Mosaique

# Décision 

Je souhaite m'appuyer sur ffmpeg pour le maximum de traitement média et en particulier pour la vidéo
et considérer l'utilisation de filtres et de graphes. Dans ce cadre, je souhaite que de façon générale
les trames vidéo décompressées circules sous forme d'AVFrame * au sein du média serveur

# Travail demandé 

- évaluer la faisabilité de remplacer tout les buffer vidéo par des avframe
- si les avframe sont des objets refcountés, créer un modèle d'owership
- inclure le fait de supprimer le Framescaler au profit d'un filtre ffmpeg intégré dans les classes
- focus sur video encodeur, video decoder -> GetFrame retourne un avframe
- examen des classe videopipe avec le systeme de double buffer.
- focus sur les scénario de transcodage et de MCU
- préparer l'implémentation des mosaiques vidéos par des graph avfilter

Effectuer une conception logicielle et un plan d'implémentation à consigner dans ce document.

---

# Décisions d'implémentation (chantier manuel, 2026-07-16)

Le chantier est repris à la main par le mainteneur. L'approche `VideoBuffer` décrite
plus bas (§3-6) est **remplacée** par ce qui suit ; la conception §7-8 (transcodage,
mixer, mosaïques avfilter) reste la cible.

- **Type de transport = `PictPtr = std::shared_ptr<Pict>`** (défini dans `medkit/video.h`).
  `Pict` est un wrapper RAII minimal autour d'un `AVFrame*` : destructeur `av_frame_free`,
  copie interdite (partage uniquement par `shared_ptr`), accesseurs `GetAVFrame()/GetWidth()/GetHeight()`.
- **Interfaces migrées** (`medkit/video.h`) : `VideoDecoder::GetFrame()→PictPtr`,
  `VideoEncoder::EncodeFrame(PictPtr)`, `VideoInput::GrabFrame()→PictPtr`,
  `VideoOutput::NextFrame(PictPtr)`. Migration **remplaçante** (pas additive) : tous les
  implémenteurs doivent basculer, build rouge tant qu'ils ne le sont pas tous.
- **Piège `avcodec_receive_frame` évité par construction** : `FfVideoDecoder::Decode`
  reçoit chaque trame dans un `AVFrame` NEUF (un `Pict` par trame) — pas d'`av_frame_ref`
  du `picture` réutilisé. `GetFrame()` renvoie ce `PictPtr` (partage zéro-copie).

- **POLITIQUE GPU : pas de redescente implicite.** `FfVideoDecoder::GetFrame()` renvoie la
  trame **telle quelle**, y compris une surface matérielle `AV_PIX_FMT_VAAPI`, pour
  préserver un pipeline GPU de bout en bout. C'est au **consommateur** de décider :
  - `Pict::IsGPUPict()` — la trame est-elle une surface GPU (VAAPI) ?
  - `Pict::DownloadToCPU()` — redescente **explicite** GPU→CPU (renvoie un nouveau `Pict`
    YUV420P, ou nullptr). N'est appelé que sur un `Pict` GPU.
  - `Pict::UploadToGPU(PictPtr& out)` — envoi **explicite** CPU→GPU vers une surface VAAPI.
    Renvoie 0 et remplit `out` en cas de succès ; renvoie un code `AVERROR` (<0) sinon, en
    particulier `AVERROR(ENOSYS)` si VAAPI n'est pas disponible. S'appuie sur un contexte
    de device VAAPI partagé (créé au plus une fois). Impl. dans `video.cpp`.
  Tout consommateur ne sachant pas traiter le GPU (encodeur logiciel, mp4writer, mosaïque
  CPU…) DOIT tester `IsGPUPict()` et appeler `DownloadToCPU()` avant d'accéder aux plans ;
  un consommateur GPU peut au contraire `UploadToGPU()` une trame CPU.

# Conception

## 1. Cartographie de l'existant (état des lieux)

### 1.1 Le « type » de trame décompressée aujourd'hui

Il n'existe **aucune** classe de trame YUV décompressée. Une trame circule comme un
`BYTE*` pointant un **YUV420P (I420) planaire contigu**, taille `w*h*3/2` (Y = `w*h`,
puis U = `w*h/4`, puis V = `w*h/4`), les dimensions/strides voyageant *à côté*, à la
main. Les frontières où ce contrat `BYTE*` est imposé :

- `medkit/video.h` — `VideoDecoder::GetFrame()→BYTE*`, `VideoEncoder::EncodeFrame(BYTE*,DWORD)`,
  `VideoInput::GrabFrame(timeout)→BYTE*`, `VideoOutput::NextFrame(BYTE*)`.
- `framescaler.h` — `FrameScaler::Resize(...)` (deux variantes, plans séparés ou contigu).
- `videostream.cpp` — sites 502 (`GrabFrame`), 576 (`EncodeFrame`), 888/908 (`GetFrame`),
  930 (`NextFrame`).
- `transcoder.cpp:174-199` — découpage manuel des plans + `scaler->Resize` + `EncodeFrame`.
- `mosaic.cpp`/`partedmosaic.cpp`/`asymmetricmosaic.cpp`/`pipmosaic.cpp` — `Update(pos,BYTE*,w,h)`,
  `GetFrame()→BYTE*`, blits `memcpy` plan par plan et overlays.

`VideoFrame`/`MediaFrame` (`medkit/media.h`, `medkit/video.h`) ne décrivent QUE le
**bitstream compressé** (buffer `malloc`/`realloc`, drapeau `ownsbuffer`, `Clone()` =
copie profonde). Aucun refcount nulle part.

### 1.2 Les extrémités sont déjà « AVFrame-natives »

Point capital pour la faisabilité :

- **`FfVideoDecoder` décode déjà dans un `AVFrame* picture`** (`avcodec_receive_frame`) et
  expose même `AVFrame* GetAVFrame()` (ffvideocodec.h:106). `GetFrame()` (ffvideocodec.cpp:804-880)
  ne fait qu'une **copie AVFrame → BYTE\*** ligne par ligne pour retirer les `linesize`
  (+ un `av_hwframe_transfer_data` si la trame est en `AV_PIX_FMT_VAAPI`).
- **`FfVideoEncoder::EncodeFrame`** (ffvideocodec.cpp:455-585) fait l'inverse *sans copie* :
  il fait pointer `picture->data[0..2]` **dans le `BYTE*` d'entrée** (ffvideocodec.cpp:471-473),
  puis `avcodec_send_frame`. Pour VAAPI il fait un `av_hwframe_transfer_data` vers `hw_frame`.

Autrement dit, la donnée *naît* et *meurt* déjà sous forme d'`AVFrame` aux deux bouts ;
la représentation `BYTE*` contiguë est une couche intermédiaire imposée artificiellement,
qui **force au moins une copie au décodage** et **casse la propagation VAAPI** (téléchargement
GPU→CPU systématique dans `GetFrame`).

### 1.3 Le double buffer des pipes = un substitut manuel au refcount

`VideoPipe` / `PipeVideoInput` maintiennent `BYTE* imgBuffer[2]` + `imgPos`/`imgNew`/`grabPic`
+ mutex/cond. `NextFrame`/`SetFrame` écrit (resize) dans le buffer *libre* pendant que le
consommateur lit l'autre via `GrabFrame` (retour de pointeur sans copie). `PipeVideoOutput`
utilise, lui, **un buffer unique + `memcpy`** (pipevideooutput.cpp:68) sous le mutex du mixer.

Ce double buffer n'existe **que** pour garantir qu'un producteur n'écrase pas la trame
qu'un consommateur est en train de lire. C'est précisément le problème que résout le
comptage de références d'`AVFrame` : chaque trame publiée est un objet immuable dont la
durée de vie suit ses lecteurs.

### 1.4 Coûts mémoire par trame (chaîne MCU actuelle, par participant)

| Étape | Opération | Copies |
|---|---|---|
| Décodage | `avcodec_receive_frame` → `GetFrame()` retire les linesize | **1 copie** (+ download GPU si VAAPI) |
| Entrée mixer | `PipeVideoOutput::NextFrame` `memcpy` | **1 copie** |
| Composition | `Mosaic::Update` → `FrameScaler::Resize` (sws + recopie tampon aligné) | **1 scale + 1 copie** |
| Overlay | `ApplyParticipantOverlay` / overlay mosaïque (blend alpha pixel à pixel) | 1 passe si overlay |
| Sortie mixer | `PipeVideoInput::SetFrame` → `FrameScaler::Resize` | **1 scale + 1 copie** |
| Encodage | `EncodeFrame` pointe dans le buffer (zéro-copie CPU) | 0 (ou upload GPU si VAAPI) |

Soit **~3 copies + 2 scales par participant et par trame composite**, plus un
aller-retour GPU↔CPU parasite quand VAAPI est actif. `FrameScaler` lui-même fait
*deux* copies (sws_scale vers un tampon aligné 32, puis recopie ligne à ligne vers la
destination) — voir framescaler.cpp:164-177.

## 2. Faisabilité et principes directeurs

**Faisabilité : élevée.** Les codecs sont déjà en ffmpeg 5.1 et manipulent des `AVFrame`
en interne. La migration consiste essentiellement à (a) **cesser d'aplatir** l'`AVFrame`
en `BYTE*` aux extrémités, (b) donner un **propriétaire refcompté** à la trame, (c)
remplacer les resizes/blits manuels par des filtres. Aucune dépendance nouvelle : `libavfilter`
et `libavutil` (déjà tirés par ffmpeg) suffisent.

Principes :

1. **`AVFrame` refcompté = unité de transport.** Une trame décompressée est un
   `AVFrame` dont les plans sont adossés à des `AVBufferRef`. On ne copie plus les pixels ;
   on partage des références.
2. **Format-agnostique.** Le transport ne présume ni `YUV420P`, ni CPU : un `AVFrame` peut
   porter un `linesize` non trivial, un pixel format arbitraire, voire une **surface
   matérielle VAAPI** (`AV_PIX_FMT_VAAPI` + `hw_frames_ctx`). C'est ce qui débloque le
   pipeline GPU de bout en bout.
3. **Migration additive.** On introduit les nouvelles signatures `AVFrame`/`VideoBuffer`
   **à côté** des anciennes `BYTE*` (méthodes virtuelles avec implémentation de repli),
   exactement comme la migration smart-pointers a ajouté des surcharges `shared_ptr`
   sans casser l'existant. On bascule site par site, build vert à chaque phase.
4. **La logique métier reste.** Placement des slots, VAD, aspect ratio, élection : ce sont
   des décisions, pas des pixels. On ne migre que les opérations pixels vers avfilter ;
   les classes `Mosaic` restent le cerveau qui *paramètre* le graphe.

## 3. Modèle d'ownership : `VideoBuffer`

### 3.1 Rappel du modèle mémoire d'`AVFrame`

Un `AVFrame` a deux niveaux de propriété :

- **le conteneur** : la struct `AVFrame` elle-même (`av_frame_alloc`/`av_frame_free`) ;
- **les données** : chaque plan pointe dans un `AVBufferRef` refcompté. `av_frame_ref(dst,src)`
  crée un *nouveau conteneur* `dst` qui **partage** les mêmes `AVBufferRef` (incrémente le
  refcount, zéro-copie). `av_frame_unref` relâche une référence. Le `AVBufferRef` (et donc
  les pixels) n'est libéré que lorsque son compteur retombe à zéro.

### 3.2 Le wrapper RAII

On introduit dans **libmedikit** (`medkit/videobuffer.h`) la classe qui manquait :

```cpp
// medkit/videobuffer.h
class VideoBuffer
{
public:
    // Adopte un AVFrame déjà rempli (prend possession du conteneur).
    explicit VideoBuffer(AVFrame* adopted) : frame(adopted) {}

    // Alloue un AVFrame + ses buffers (w×h, format), éventuellement depuis un pool.
    VideoBuffer(int width, int height, AVPixelFormat fmt = AV_PIX_FMT_YUV420P);

    ~VideoBuffer() { if (frame) av_frame_free(&frame); }

    // Non copiable (partage = shared_ptr du conteneur, ou Ref() pour un conteneur distinct).
    VideoBuffer(const VideoBuffer&) = delete;
    VideoBuffer& operator=(const VideoBuffer&) = delete;

    AVFrame*       GetFrame()       { return frame; }
    const AVFrame* GetFrame() const { return frame; }

    int   GetWidth()  const { return frame ? frame->width  : 0; }
    int   GetHeight() const { return frame ? frame->height : 0; }
    AVPixelFormat GetFormat() const { return frame ? (AVPixelFormat)frame->format : AV_PIX_FMT_NONE; }
    bool  IsHardware() const { return GetFormat() == AV_PIX_FMT_VAAPI; }
    int64_t GetPts()  const { return frame ? frame->pts : AV_NOPTS_VALUE; }

    // Nouveau conteneur partageant les MÊMES buffers (av_frame_ref) — pour alimenter
    // un consommateur qui va unref (ex. un buffersrc avfilter).
    std::shared_ptr<VideoBuffer> Ref() const;

    // Copie profonde (dernier recours : compat BYTE* / codec non-ffmpeg).
    std::shared_ptr<VideoBuffer> Clone() const;

    // Interop héritée : copie le YUV420P contigu -> AVFrame, et inversement.
    static std::shared_ptr<VideoBuffer> FromContiguousI420(const BYTE* buf, int w, int h);
    void CopyToContiguousI420(BYTE* dst) const;

private:
    AVFrame* frame = nullptr;
};

using VideoBufferRef = std::shared_ptr<VideoBuffer>;
```

### 3.3 Règles de propriété (le contrat)

- **Producteur** (décodeur, filtre, pipe) : alloue/reçoit un `AVFrame`, l'enveloppe dans un
  `VideoBuffer` neuf porté par un `VideoBufferRef` (`std::make_shared`). Il ne le mute plus après
  publication.
- **Partage** : un consommateur qui doit garder la trame **copie le `VideoBufferRef`**
  (partage du conteneur, immuable → sûr, zéro-copie). C'est le cas nominal (pipes, mixer).
- **`Ref()`** : uniquement quand il faut un **conteneur `AVFrame` distinct** parce que le
  consommateur va le consommer/unref — typiquement `av_buffersrc_add_frame` qui prend
  possession. On lui passe `buf->Ref()->GetFrame()` (ou `av_frame_ref` local).
- **Immuabilité** : une trame publiée est *read-only*. Toute transformation produit un
  **nouveau** `VideoBuffer` (sortie d'un filtre/scaler). Cela supprime tout risque de
  data-race sans double buffer.
- **Fin de vie** : automatique. Le dernier `VideoBufferRef` détruit le `VideoBuffer`, dont le
  destructeur `av_frame_free` relâche la dernière référence aux `AVBufferRef`.

### 3.4 Pool de trames (anti-churn)

Allouer/libérer un `AVFrame` complet par trame recrée le coût que l'on veut éviter.
On adosse `VideoBuffer(w,h,fmt)` à un **`AVBufferPool`** (`av_buffer_pool_init`) par
(largeur, hauteur, format), encapsulé dans un petit `VideoBufferPool` singleton-par-pipe.
`av_frame_get_buffer` peut être remplacé par une allocation depuis le pool ; les buffers
recyclés ne sont réutilisés que lorsque leur refcount est nul — cohérent avec l'immuabilité.
Le pool est une optimisation de **Phase 6** (mesurer d'abord), pas un prérequis.

## 4. Refonte des interfaces (`medkit/video.h`)

Migration **additive** : on ajoute les variantes `VideoBufferRef` avec un repli qui
pontifie l'ancien chemin `BYTE*`. Les décodeurs/encodeurs non-ffmpeg (nelly, vp6, flv1
restés dans `mcu/src`) continuent de fonctionner via le repli sans être réécrits.

```cpp
class VideoDecoder {
public:
    // ... existant conservé ...
    virtual BYTE* GetFrame() = 0;                       // hérité (repli)
    // NOUVEAU : rend une trame refcomptée. Défaut = enveloppe GetFrame() (1 copie).
    virtual VideoBufferRef GetFrameBuffer() {
        BYTE* p = GetFrame();
        if (!p) return nullptr;
        return VideoBuffer::FromContiguousI420(p, GetWidth(), GetHeight());
    }
};

class VideoEncoder {
public:
    virtual VideoFrame* EncodeFrame(BYTE* in, DWORD len) = 0;   // hérité (repli)
    // NOUVEAU : encode directement une trame refcomptée.
    virtual VideoFrame* EncodeFrame(const VideoBufferRef& buf) {
        BYTE tmp; /* défaut : aplatir en contigu puis EncodeFrame(BYTE*) */
        // (impl. réelle : buffer temporaire via buf->CopyToContiguousI420)
        ...
    }
};

class VideoInput {
public:
    virtual BYTE* GrabFrame(DWORD timeout) = 0;                 // hérité
    virtual VideoBufferRef GrabFrameBuffer(DWORD timeout) { ... } // NOUVEAU
};

class VideoOutput {
public:
    virtual int NextFrame(BYTE* pic) = 0;                       // hérité
    virtual int NextFrame(const VideoBufferRef& buf) { ... }    // NOUVEAU
};
```

Overrides « natifs » (zéro-copie) dans libmedikit :

- **`FfVideoDecoder::GetFrameBuffer()`** — remplace la copie de ffvideocodec.cpp:864-873 par
  un simple `av_frame_ref(nouveau, picture)` (ou, si `picture->format==AV_PIX_FMT_VAAPI` et que
  l'on veut rester CPU, un `av_hwframe_transfer_data` vers un `AVFrame` neuf — sinon on
  **garde la surface VAAPI**). Retour = `make_shared<VideoBuffer>(nouveau)`. **Zéro copie CPU.**
  ⚠️ obligatoire : `avcodec_receive_frame` fait `av_frame_unref(picture)` au décodage suivant,
  donc on **doit** rendre une *référence* (conteneur distinct), jamais `picture` nu.
- **`FfVideoEncoder::EncodeFrame(VideoBufferRef)`** — alimente `avcodec_send_frame` avec
  `buf->GetFrame()` directement (respecte les `linesize`, gère l'upload VAAPI comme
  aujourd'hui). Supprime la contrainte « contigu sans padding » et la vérif de taille
  ffvideocodec.cpp:462-466.

## 5. Suppression du `FrameScaler` → `VideoFilter` (avfilter)

`FrameScaler` (swscale YUV420P→YUV420P + tampon aligné + recopie ligne à ligne) est
remplacé par une classe **`VideoFilter`** (libmedikit, `medkit/videofilter.h`) qui
encapsule un `AVFilterGraph` persistant :

```cpp
class VideoFilter {
public:
    // Construit/reconfigure le graphe à partir d'une description avfilter.
    // ex. "scale=640:480", ou "scale=640:360,pad=640:480:0:60:black".
    bool Configure(int inW, int inH, AVPixelFormat inFmt,
                   const std::string& filterDesc,
                   AVBufferRef* hwFramesCtx = nullptr);

    // Pousse une trame, récupère la/les trame(s) filtrée(s) (refcomptées).
    VideoBufferRef Filter(const VideoBufferRef& in);
private:
    AVFilterGraph*   graph   = nullptr;
    AVFilterContext* src     = nullptr;   // buffer
    AVFilterContext* sink    = nullptr;   // buffersink
    /* mémorise inW/inH/inFmt/desc pour ne reconfigurer qu'au changement */
};
```

- Le **scaling simple** (pipes, transcodage) = graphe `scale=w:h` (ou
  `scale_vaapi` en chemin GPU). L'aspect (letterbox/pillarbox de partedmosaic.cpp:144-205)
  devient `scale=...:force_original_aspect_ratio=decrease,pad=W:H:(ow-iw)/2:(oh-ih)/2:color`
  — **une ligne** au lieu de ~60 lignes d'arithmétique de plans.
- Reconfiguration paresseuse (mêmes W/H/format → réutilise le graphe), comme le cache
  de `SwsContext` actuel (framescaler.cpp:64-66).
- `FrameScaler` est **supprimé** des deux copies (`mcu/src` et `libmedikit`) une fois tous
  ses appelants migrés (§1.1 liste les 8 sites).

## 6. Refonte des pipes — disparition du double buffer

Avec des trames immuables refcomptées, `VideoPipe`/`PipeVideoInput`/`PipeVideoOutput`
se réduisent à **un emplacement « dernière trame » protégé par mutex/cond** :

```cpp
class VideoPipe : public VideoInput, public VideoOutput {
    std::mutex mutex; std::condition_variable cond;
    VideoBufferRef last;      // remplace imgBuffer[2]/imgPos/imgNew/grabPic
    std::shared_ptr<VideoFilter> scaler;   // remplace FrameScaler resizer

    int NextFrame(const VideoBufferRef& in) override {          // producteur
        VideoBufferRef out = (scaler ? scaler->Filter(in) : in);
        { std::lock_guard l(mutex); last = std::move(out); }     // publie une réf
        cond.notify_all();
    }
    VideoBufferRef GrabFrameBuffer(DWORD timeout) override {     // consommateur
        std::unique_lock l(mutex);
        cond.wait_for(...);
        return last;   // COPIE du shared_ptr : la trame survit tant que l'encodeur la tient
    }
};
```

Bénéfices : plus de swap ni de `memcpy` de publication (pipevideooutput.cpp:68 disparaît),
le resize n'a lieu **que si nécessaire** (souvent la sortie composite a déjà la bonne
taille), et l'absence de tearing est garantie par le refcount, pas par un jeu de deux
tampons. Le redimensionnement peut même migrer *hors* du pipe (dans le graphe mosaïque),
le pipe devenant un simple point de rendez-vous.

## 7. Scénarios cibles

### 7.1 Transcodage direct (sans mixer) — `transcoder.cpp`

Chaîne cible : `decoder->GetFrameBuffer()` → `VideoFilter (scale)` → `encoder->EncodeFrame(VideoBufferRef)`.
Disparaissent : le découpage manuel des plans (transcoder.cpp:174-186), `decodedPic`/
`decodedPicSize`, `FrameScaler`, et le **bug `EncodeFrame(dstV, ...)`** (transcoder.cpp:199,
passe le plan V au lieu de la base du buffer) qui n'a plus de raison d'exister puisqu'on
passe un `AVFrame` structuré. Chemin **GPU possible** : `scale_vaapi` sans redescente CPU.

### 7.2 MCU (mixer)

Chaîne cible par participant : `decoder->GetFrameBuffer()` → `PipeVideoOutput` (publie une
réf, plus de memcpy) → **graphe mosaïque avfilter** (§8) → `PipeVideoInput` (publie une réf)
→ `encoder->EncodeFrame(VideoBufferRef)`. On passe de ~3 copies + 2 scales à
**≤1 scale par entrée dans le graphe + 0 copie de transport**.

## 8. Mosaïques par graphe avfilter

> **FAIT** — réalisé par le chantier `mosaic_avfilter_plan.md` (Phases 0-6,
> 2026-07-16 → 2026-08-03) : composition par `MosaicCompositor` (graphe unique,
> liseré noir, overlays, chemin GPU VAAPI avec repli CPU), chemins BYTE\*/
> `FrameScaler` supprimés du mcu (`framescaler` ne survit qu'en objet
> Asterisk-only de libmedikit pour `transcoder.cpp`, non compilé ici),
> `VideoRescaler` déplacé dans libmedikit.

### 8.1 Principe

Chaque `Mosaic` possède un `AVFilterGraph` persistant :

```
[in0] scale=w0:h0,pad=... ┐
[in1] scale=w1:h1,pad=... ┤ overlay=x1:y1 → overlay=x2:y2 → ... ┐
 ...                       ┘                                     ├→ [sink]
[color=background:WxH] ───────────────────────────────────────┘
[logo/text buffersrc] ── overlay=... (optionnel) ──────────────┘
```

- **N `buffersrc`** (un par slot actif) + une source de fond (`color`, remplace le `memset -128`
  de mosaic.cpp:69) + éventuelles sources d'overlay (image/SVG/texte en `yuva420p`).
- Chaînes `scale`+`pad` par entrée (aspect ratio), puis cascade d'`overlay` aux positions
  calculées par les `GetLeft/GetTop/GetWidth/GetHeight(pos)` **existants** des sous-classes.
- Un `buffersink` produit l'`AVFrame` composite.

### 8.2 Table de correspondance (opérations manuelles → filtres)

| Opération manuelle actuelle | Filtre avfilter |
|---|---|
| `FrameScaler::Resize` (scale + recopie) | `scale` (ou `scale_vaapi`) |
| Blit vignette (`memcpy` vers sous-fenêtre) | `overlay=x:y` |
| Letterbox/pillarbox (calcul `diff`, décalage plans) | `scale:force_original_aspect_ratio` + `pad` |
| Fond `-128`/noir (`memset`) + `Clean` | source `color` |
| Fond PIP « à trous » (pipmosaic.cpp:172-254) | `overlay` empilés (le PIP par-dessus le fond) |
| Overlay image/SVG/texte + alpha (blend pixel, overlay.cpp:466-747) | `overlay` avec entrée `yuva420p` |
| Logo slot vide (videomixer.cpp:16-23) | source `movie`/`color` + `overlay` |
| VU-mètre (rectangles `memset`, mosaic.cpp:899-963) | `drawbox` (ou conservé hors graphe) |

### 8.3 Ordonnancement et synchronisation

La composition est **événementielle**, pas à FPS fixe (videomixer.cpp:139-188, réveil sur
`mixVideoCond`, force chaque ~500 ms). L'`overlay` avfilter attend en principe des entrées
synchronisées (framesync). On conserve donc **notre** tick de composition : à chaque tick,
on pousse dans chaque `buffersrc` la **dernière trame connue** du slot (mémorisée comme
`VideoBufferRef`), puis on tire le sink une fois. On règle `overlay` avec
`eof_action=pass`/`repeatlast=1` et on évite `fps`. La reconstruction du graphe n'a lieu
qu'au **changement de topologie** (nombre de slots, type de mosaïque, positions VAD),
pas à chaque trame — même stratégie de cache que les `SwsContext` par slot aujourd'hui.

### 8.4 Découpage des responsabilités

`Mosaic` (et dérivées) **restent** propriétaires de la logique de placement/VAD/aspect ;
elles ne calculent plus de pixels mais **émettent une description de graphe** (positions,
tailles, fond) qu'une nouvelle classe `MosaicCompositor` (libmedikit) matérialise en
`AVFilterGraph`. `videomixer.cpp` continue d'appeler `Update`/`GetFrame` — dont
l'implémentation bascule du blit manuel vers le push/pull du graphe.

## 9. Risques et points d'attention

- **Cycle de vie du `picture` décodeur** : `GetFrameBuffer()` DOIT `av_frame_ref` (jamais
  rendre `picture` nu) car `avcodec_receive_frame` l'unref au tour suivant. C'est le piège
  n°1 de la migration.
- **VAAPI mixte** : trames matérielles et logicielles peuvent coexister (participants
  hétérogènes). Le graphe doit soit tout ramener CPU (`hwdownload`) soit tout monter GPU
  (`hwupload`) ; prévoir une politique par mosaïque. Chemin GPU = optimisation ultérieure,
  pas de la première livraison.
- **Formats non-420** : garder le transport format-agnostique, mais insérer un `format=yuv420p`
  en tête de graphe tant que les encodeurs attendent du 420.
- **Codecs non-ffmpeg résiduels** (nelly/vp6/flv1) : couverts par le repli `BYTE*` de §4,
  non réécrits.
- **Double définition de `FrameScaler`** (mcu + libmedikit) : supprimer les deux copies
  ensemble.
- **Makefile ne suit pas les headers** : `rm *.o` avant rebuild après tout changement de
  `medkit/video.h` (ABI mcu↔medkit à resynchroniser — piège déjà connu).
- **Overlays ImageMagick** : `Overlay` (Magick++ → `yuva420p`) reste pour le *rendu* du
  contenu ; seul le *blit* migre vers `overlay` avfilter.

---

# Plan d'implémentation

Migration additive, **build vert à chaque phase** (via `./install.ksh localcompile`), sur le
modèle de la migration smart-pointers. `rm -f` des `.o` concernés avant chaque rebuild de
header partagé.

### Phase 0 — Fondations `VideoBuffer` (aucun comportement changé)
- Créer `medkit/videobuffer.h`/`.cpp` (§3.2) : RAII, `Ref()`, `Clone()`,
  `FromContiguousI420`/`CopyToContiguousI420`, accesseurs.
- Ajouter au Makefile libmedikit. Compiler à vide (aucun appelant).
- **Livrable** : type disponible, build vert, aucun flux modifié.

### Phase 1 — Interfaces additives dans `medkit/video.h`
- Ajouter `GetFrameBuffer()`, `EncodeFrame(VideoBufferRef)`, `GrabFrameBuffer()`,
  `NextFrame(VideoBufferRef)` avec **implémentations de repli** pontant le `BYTE*` (§4).
- `rm` des `.o` mcu/medkit, rebuild. **Aucun appelant** n'utilise encore les nouvelles voies.
- **Livrable** : surface d'API en place, comportement identique, build vert.

### Phase 2 — Overrides natifs codecs ffmpeg (zéro-copie aux extrémités)
- `FfVideoDecoder::GetFrameBuffer()` → `av_frame_ref(picture)` (gérer VAAPI), sans copie.
- `FfVideoEncoder::EncodeFrame(VideoBufferRef)` → `avcodec_send_frame(buf->GetFrame())`.
- Tests : décodage→ré-encodage d'un flux, comparer sortie à l'ancien chemin.
- **Livrable** : extrémités AVFrame-natives, anciens `BYTE*` toujours dispo, build vert.

### Phase 3 — `VideoFilter` (avfilter) + bascule du transcodage
- Créer `medkit/videofilter.h`/`.cpp` (§5) : graphe `buffer→scale[→pad]→buffersink`,
  reconfiguration paresseuse.
- Réécrire `VideoTranscoder::ProcessFrame`/`HandleResize` sur
  `GetFrameBuffer → VideoFilter → EncodeFrame(VideoBufferRef)`. Supprimer `decodedPic`,
  le découpage manuel des plans et le bug `EncodeFrame(dstV,...)`.
- Valider un transcodage réel (résolutions différentes → force le scale).
- **Livrable** : chemin transcodage 100 % AVFrame, `FrameScaler` plus référencé côté transcoder.

### Phase 4 — Pipes sur `VideoBuffer` (retrait du double buffer)
- `VideoPipe`/`PipeVideoInput`/`PipeVideoOutput` : emplacement `VideoBufferRef last` +
  mutex/cond ; `NextFrame(VideoBufferRef)`/`GrabFrameBuffer` (§6). Resize via `VideoFilter`
  interne (ou délégué).
- `VideoStream::RecVideo` : `GetFrameBuffer()` → `videoOutput->NextFrame(VideoBufferRef)`.
  `VideoStream::SendVideo` : `videoInput->GrabFrameBuffer()` → `EncodeFrame(VideoBufferRef)`.
  Gérer le cas muet (`logo`) en enveloppant la trame logo dans un `VideoBuffer`.
- Valider un appel point-à-point (participant ↔ participant) transcodé.
- **Livrable** : chaîne RTP→pipe→RTP sans copie de transport ni double buffer, build vert.

### Phase 5 — Mosaïques par graphe avfilter
- Créer `MosaicCompositor` (§8.4) : construit/maintient l'`AVFilterGraph`
  (`color` fond + N `scale/pad` + cascade `overlay` + `buffersink`), reconstruit au
  changement de topologie.
- Faire émettre par `Mosaic`/`PartedMosaic`/`AsymmetricMosaic`/`PIPMosaic` une description
  (positions/tailles/fond) au lieu de blitter ; `Update` mémorise la `VideoBufferRef` du slot,
  `GetFrame()` pousse toutes les entrées et tire le sink.
- Intégrer les overlays (`overlay` avfilter avec entrée `yuva420p` issue de `Overlay`) et le
  fond ; VU-mètre en `drawbox` ou conservé hors graphe.
- Valider chaque type de mosaïque (1x1, 2x2, 3x3, 4x4, asymétriques, PIP), aspect ratio,
  overlays, VAD.
- **Livrable** : composition MCU par graphe, `FrameScaler` supprimé des deux copies.

### Phase 6 — Optimisations
- `VideoBufferPool` (`av_buffer_pool`) par (w,h,format) pour éliminer le churn d'allocation.
- Chemin **GPU de bout en bout** (VAAPI) : `scale_vaapi`/`overlay_vaapi`, éviter les
  aller-retours CPU (débranche le download de `GetFrame` sur ce chemin).
- Mesures avant/après (copies/scale par trame, CPU mixer, latence).
- **Livrable** : gains mémoire/CPU chiffrés, chemin GPU optionnel activable.

### Nettoyage final
- Supprimer les surcharges `BYTE*` devenues mortes (là où plus aucun appelant), en
  gardant le repli pour les codecs non-ffmpeg résiduels.
- Supprimer `framescaler.{h,cpp}` (mcu + libmedikit).
- Resynchroniser l'ABI mcu↔medkit, doc CLAUDE.md.
