# Refonte des mosaïques vidéo en graphe avfilter unique (scale + composition + VAAPI)

Conception détaillée + plan d'implémentation — 2026-07-16 — branche `feat/av-frame`.

Prérequis déjà en place (migration AVFrame) : transport `PictPtr = std::shared_ptr<Pict>`
(`third_party/fontventa/libmedikit/medkit/video.h`), pipes sur `VideoRescaler`
(`mcu/include/videorescaler.h`, `mcu/src/videorescaler.cpp` — patron « graphe avfilter
persistant reconfiguré paresseusement »), politique GPU « pas de redescente implicite »
(`IsGPUPict()/DownloadToCPU()/UploadToGPU()`, device VAAPI partagé dans
`libmedikit/video.cpp::GetSharedVAAPIDevice`).

État à supprimer : les **ponts temporaires** `Mosaic::Update(int, const PictPtr&)`
(aplatissement en YUV420P contigu, `mcu/src/mosaic.cpp:23-49`) et `Mosaic::GetPict()`
(copie du composite BYTE\* dans un Pict, `mosaic.cpp:53-81`), les internals BYTE\* des
4 mosaïques (blits `memcpy`, `FrameScaler** resizer`, letterbox arithmétique,
`PIPMosaic::under`), le blend alpha manuel d'`Overlay::Display`, et **`FrameScaler`**
(les deux copies : `mcu/{include,src}/framescaler.*` et
`libmedikit/framescaler.cpp` + `medkit/framescaler.h`, encore utilisé par
`libmedikit/transcoder.cpp`).

---

## 1. Topologie du graphe

### 1.1 Principe

**Un `AVFilterGraph` persistant par `Mosaic`**, qui fait à la fois le redimensionnement
(letterbox compris) et la composition :

```
[bg buffersrc: Pict de fond WxH] ──────────────────────────────┐
[in0 buffersrc] → scale=w0:h0 ─────────────────────────────────┤ overlay=x0:y0 ┐
[in1 buffersrc] → scale=w1:h1 ─────────────────────────────────┘               ├ overlay=x1:y1 ┐
[ovr1 buffersrc rgba (nom participant 1)] ──────────────────────────────────────┘              ├ overlay ...
[ovrM buffersrc rgba (overlay mosaïque plein cadre)] ───────────────────────────────────────────┘
                                                                                          → [buffersink]
```

- **1 buffersrc de fond** : trame `Pict` pré-générée à la taille de la mosaïque
  (gris neutre Y=U=V=128, équivalent du `memset -128` de `Mosaic::Mosaic`
  `mosaic.cpp:133` ; **noir** pour `mosaic1p1`, cf. le peint-en-noir du constructeur
  d'`AsymmetricMosaic` `asymmetricmosaic.cpp:13-24`). On **écarte le filtre source
  `color`** : c'est une source à cadence libre (option `r`) dont les pts ne sont pas
  pilotables par notre tick événementiel — avec framesync elle provoquerait des
  stalls/EAGAIN. Un buffersrc alimenté par le même Pict de fond à chaque tick donne
  un contrôle total des pts (voir §4).
- **N buffersrc de slot** — un par slot **actif** (= slot pour lequel une trame est
  mémorisée : trame participant OU logo « slot vide »). Un slot sans trame n'a pas
  de branche : le fond y reste visible (équivalent exact de l'actuel `Clean(pos)`
  qui repeint la case en -128).
- **Une chaîne `scale` (ou `scale_vaapi`) par entrée** dimensionnée au letterbox
  près, puis **cascade d'`overlay` (ou `overlay_vaapi`)** aux positions issues des
  `GetLeft/GetTop/GetWidth/GetHeight(pos)` **existants** des sous-classes
  (`partedmosaic.cpp:276-310`, `asymmetricmosaic.cpp:249-946`, `pipmosaic.cpp:352-422`).
- **Pas de filtre `pad`** : aujourd'hui les bandes letterbox/pillarbox laissent voir
  le fond de la mosaïque (le slot est nettoyé à -128 puis l'image est blittée
  décalée de `diff`, cf. `partedmosaic.cpp:157-204`). On reproduit ce comportement
  à l'identique en scalant à la taille effective `rzW×rzH` et en **positionnant
  l'overlay à `(GetLeft(pos)+dx, GetTop(pos)+dy)`** — le fond fait office de bandes.
  (`pad` resterait possible si l'on voulait des bandes d'une couleur distincte.)
- **M buffersrc d'overlay** (rgba, rendus par ImageMagick — voir §5) : un par
  overlay participant présent (`Mosaic::overlays`, affichés par
  `ApplyParticipantOverlay` aujourd'hui) + un pour l'overlay mosaïque plein cadre
  (`Mosaic::overlay`, affiché par `Mosaic::GetFrame` `mosaic.cpp:760-778`).
  Les overlays participants s'empilent **après** la vignette de leur slot, l'overlay
  mosaïque en **dernier** (par-dessus tout), ce qui reproduit l'ordre actuel.
- **1 buffersink** → l'`AVFrame` composite, enveloppé dans un `Pict` neuf.

**Ordre Z** : fond, puis slots par `pos` croissant (pour `PIPMosaic`, le slot 0 plein
cadre est donc bien dessous, les PIP par-dessus — le fond « à trous » de
`pipmosaic.cpp:171-254` disparaît), puis overlays participants, puis overlay mosaïque.

### 1.2 Calcul letterbox : dans le cerveau, pas dans le graphe

On n'utilise **pas** `scale=...:force_original_aspect_ratio=decrease` : la sémantique
actuelle de `ComputeAspectRatio` (`mosaic.cpp:213-236`) traite les formats à pixel
non carré (CIF 352×288 ratio brut 1,22 ; SIF 1,46) comme du 4:3 — FOAR utiliserait le
ratio brut et changerait le rendu. Le calcul existant (`picRatio` vs `ratio`,
`rzWidth/rzHeight/diff` de `partedmosaic.cpp:144-205` et
`asymmetricmosaic.cpp:119-173`) est **factorisé dans la base** :

```cpp
// mcu/include/mosaic.h — remplace l'arithmétique dupliquée des Update(BYTE*)
// Calcule la taille effective de la vignette et son décalage dans le slot
// (letterbox/pillarbox), d'après ComputeAspectRatio et keepAspect du slot.
void ComputeSlotPlacement(int pos, int inW, int inH, bool keepAspect,
                          int& outW, int& outH, int& dx, int& dy);
```

Conséquence : la taille de sortie du `scale` dépend de la **taille d'entrée** ⇒ la
géométrie d'entrée `(inW,inH,inFmt,hwFramesCtx)` de chaque slot fait partie de la clé
de (re)configuration du graphe, exactement comme dans `VideoRescaler::Configure`
(`videorescaler.cpp:38-44`). Un changement de résolution d'un participant (événement
rare, déjà tracé par `PipeVideoOutput::SizeHasChanged`) reconstruit le graphe.

**Aspect par slot** : aujourd'hui `videomixer.cpp` fait
`mosaic->KeepAspectRatio(output->IsAspectRatioKept())` juste avant chaque `Update`
(lignes 388/421/443/1225) — un drapeau global écrasé à chaque appel. Il devient
**mémorisé par slot** au moment du `Update(pos, pic)` (`slotKeepAspect[pos]`), ce qui
corrige au passage une course bénigne de l'existant.

### 1.3 Exemples concrets de chaînes (CPU)

Options communes à tous les `overlay` : `eof_action=pass:repeatlast=1:shortest=0`
(notées `<sync>` ci-dessous), `format=yuv420` implicite.

**1×1** (mosaïque 352×288, entrée 640×480 — 4:3 = ratio slot, plein cadre) :

```
buffer=video_size=640x480:pix_fmt=yuv420p:time_base=1/1000 [in0] ;
buffer=video_size=352x288:pix_fmt=yuv420p:time_base=1/1000 [bg]  ;
[in0] scale=352:288,format=yuv420p [s0] ;
[bg][s0] overlay=x=0:y=0:<sync> [out]
```

**2×2** (704×576, slots 352×288 ; entrée 1 en 1280×720 (16:9) → `rzH=(352*1000)/1777
= 198`, `dy=(288-198)/2 = 44` (pair) ; les 3 autres en 4:3) :

```
[in0] scale=352:198,format=yuv420p [s0] ;   # 16:9 letterboxé
[in1] scale=352:288,format=yuv420p [s1] ;
[in2] scale=352:288,format=yuv420p [s2] ;
[in3] scale=352:288,format=yuv420p [s3] ;
[bg][s0] overlay=x=0:y=44:<sync>    [t0] ;  # GetLeft(0)+0 : GetTop(0)+dy
[t0][s1] overlay=x=352:y=0:<sync>   [t1] ;
[t1][s2] overlay=x=0:y=288:<sync>   [t2] ;
[t2][s3] overlay=x=352:y=288:<sync> [out]
```

**PIP1** (`mosaicPIP1`, 704×576, 2 slots — slot 0 étiré plein cadre comme aujourd'hui
`pipmosaic.cpp:160`, PIP 176×144 aux coordonnées `GetLeft(1)/GetTop(1)`) :

```
[in0] scale=704:576,format=yuv420p [s0] ;   # keepAspect=false (étirement, fidèle à l'existant)
[in1] scale=176:144,format=yuv420p [s1] ;
[bg][s0] overlay=x=0:y=0:<sync>   [t0] ;
[t0][s1] overlay=x=88:y=360:<sync> [out]    # positions rendues par PIPMosaic::GetLeft/GetTop
```

**Asymétrique `mosaic1p5`** (720×576 : slot 0 = 480×384 à (0,0), slots 1-5 = 240×192,
positions données par `AsymmetricMosaic::GetLeft/GetTop`) avec un overlay « nom »
sur le slot 0 et un overlay mosaïque :

```
[in0] scale=480:384 [s0] ; [in1] scale=240:192 [s1] ; ... [in5] scale=240:192 [s5] ;
[bg][s0] overlay=0:0     [t0] ;
[t0][s1] overlay=480:0   [t1] ;
[t1][s2] overlay=480:192 [t2] ;
[t2][s3] overlay=0:384   [t3] ;
[t3][s4] overlay=240:384 [t4] ;
[t4][s5] overlay=480:384 [t5] ;
[t5][ovr0]  overlay=0:0:<sync>  [t6] ;      # nom du slot 0 (rgba 480x384, posé à GetLeft/GetTop(0))
[t6][ovrM]  overlay=0:0:<sync>  [out]       # overlay mosaïque plein cadre 720x576
```

### 1.4 Exemple GPU (2×2, entrées hétérogènes)

```
# in0 : décodé VAAPI (buffersrc paramétré avec src->hw_frames_ctx, cf. videorescaler.cpp:63-75)
[in0] scale_vaapi=w=352:h=198 [s0] ;
# in1 : CPU → montée GPU (nv12 est le format de surface nominal des pools VAAPI)
[in1] format=nv12,hwupload [u1] ; [u1] scale_vaapi=w=352:h=288 [s1] ;
# fond CPU → GPU
[bg]  format=nv12,hwupload [bgu] ;
[bgu][s0] overlay_vaapi=x=0:y=44:<sync>  [t0] ;
[t0][s1]  overlay_vaapi=x=352:y=0:<sync> [t1] ;
...
[tN] buffersink            # sortie AV_PIX_FMT_VAAPI → Pict GPU (IsGPUPict()==true)
```

Les filtres `hwupload` reçoivent leur `hw_device_ctx` (le device VAAPI partagé,
`av_buffer_ref(GetSharedVAAPIDevice())`) posé sur l'`AVFilterContext` après création,
avant `avfilter_graph_config`. Vérifié : ffmpeg 5.1 du système fournit `scale_vaapi`,
`overlay_vaapi`, `hwupload`, `hwdownload`.

### 1.5 Alternative écartée : deux graphes (participant + composition)

Envisagé : un graphe **par participant/slot** (resize + letterbox) chaîné à un graphe
**général** de composition, via un handoff `buffersink`→`buffersrc` (refcompté,
zéro‑copie). Faisable et proche de la structure actuelle (il existe déjà un
redimensionneur par slot, `FrameScaler** resizer`). **Non retenu**, car contraire à
l'objectif « max GPU » :

- **Un seul graphe = un seul device + un seul pool VAAPI** : `scale_vaapi`/`overlay_vaapi`
  acceptent toutes les entrées sans friction → GPU de bout en bout naturel.
- **Deux graphes = deux `hw_frames_ctx`** : chaque mini‑graphe peut allouer son propre
  pool de surfaces ; `overlay_vaapi` exige des entrées compatibles, il faudrait forcer un
  pool partagé sinon repli `hwupload`/`hwdownload` (copies GPU↔CPU parasites, exactement
  ce qu'on veut éviter). C'est **le** piège, pile sur le chemin à optimiser.
- Le découplage des reconfigurations est **partiel** : la cible d'un rescaler de slot
  dépend de la géométrie du slot (topologie) ⇒ un reshuffle VAD reconstruit de toute façon
  les deux étages. Seul le changement de **résolution d'entrée** d'un participant (rare)
  profiterait du deux‑graphes.
- La séparation logique « resize par participant + composition » **existe déjà** dans le
  graphe unique sous forme de **sous‑chaînes `scale` par entrée** alimentant la cascade
  `overlay` — même DAG, mais frontières = liens de filtre internes (une reconfig, un
  contexte GPU) plutôt que `buffersink`/`buffersrc`.

Décision : **graphe unique**, structuré en sous‑chaînes nommées par slot pour la lisibilité.
Si le cas « résolution d'entrée changeante » devient critique, scinder plus tard — côté CPU,
où la frontière est gratuite.

---

## 2. Politique GPU/CPU (par mosaïque)

Décidée **à chaque (re)configuration du graphe**, jamais par trame :

1. **Mode GPU** si : le device VAAPI partagé existe (`GetSharedVAAPIDevice()` non
   nul), **et** au moins un slot actif fournit des `Pict` GPU (`IsGPUPict()`), **et**
   la construction du graphe GPU réussit. Rationale : s'il n'y a aucune entrée GPU,
   monter tout le monde en VRAM pour composer serait une pure perte (uploads +
   redescente probable côté encodeur x264).
2. **Entrées hétérogènes en mode GPU** : chaque branche d'entrée CPU est préfixée de
   `format=nv12,hwupload` ; chaque branche d'entrée GPU passe son `hw_frames_ctx` au
   buffersrc (patron `videorescaler.cpp:63-75`). Le fond (toujours CPU) est uploadé.
3. **Entrées GPU en mode CPU** (VAAPI indispo, ou repli) : branche préfixée de
   `hwdownload,format=nv12,format=yuv420p` — pas de `DownloadToCPU()` hors graphe,
   la redescente reste dans le graphe (une seule traversée).
4. **Normalisation de format** : en mode CPU, chaque branche se termine par
   `format=yuv420p` (comme `VideoRescaler`, `videorescaler.cpp:87`) — couvre les
   entrées non-420 (NV12 issu d'une redescente, YUV422…). Le buffersink est
   contraint à `AV_PIX_FMT_YUV420P` (CPU) ou `AV_PIX_FMT_VAAPI` (GPU) via
   `av_opt_set_int_list(sinkCtx, "pix_fmts", ...)`.
5. **Overlays en mode GPU** : `overlay_vaapi` ne gère pas de façon fiable l'alpha
   par pixel d'une entrée RGBA (support driver variable). Décision : **si des
   overlays sont présents, le graphe GPU se termine par une queue CPU** —
   `hwdownload,format=nv12,format=yuv420p` après la cascade `overlay_vaapi` des
   vignettes, puis les `overlay` CPU des entrées RGBA. Le scaling et la composition
   des vignettes (le gros du travail) restent GPU. Le tout-GPU avec overlays RGBA
   est une optimisation ultérieure documentée en risque (§8).
6. **Repli robuste** : si `avfilter_graph_config` échoue en mode GPU (filtre absent,
   driver capricieux, mélange de devices), on reconstruit **automatiquement en mode
   CPU** (avec les `hwdownload` du point 3) et on mémorise l'échec (`gpuBroken`)
   pour ne pas retenter à chaque reconfiguration. Trace `Error(...)` unique.
7. La **sortie** suit le mode : composite `Pict` GPU en mode GPU (l'encodeur VAAPI
   de `FfVideoEncoder` le consomme tel quel ; un encodeur CPU fera la redescente —
   politique « le consommateur décide » déjà actée dans avframe.md).

---

## 3. Qui fait quoi : `Mosaic` = cerveau, `MosaicCompositor` = matérialisation

### 3.1 Répartition

- **`Mosaic` et ses dérivées gardent tout le métier** : slots/positions/VAD/élection
  (`SetSlot`, `CalculatePositions`, `SetVADParticipant`…), géométrie
  (`GetLeft/GetTop/GetWidth/GetHeight(pos)` virtuels — **seule obligation restante
  des sous-classes**), aspect (`ComputeAspectRatio`, `ComputeSlotPlacement`),
  overlays (rendu ImageMagick via `Overlay`). Elles **n'écrivent plus un pixel** :
  les `Update(int,BYTE*,int,int)` virtuels, `Clean(pos)` virtuels, `GetFrame()`,
  les membres `mosaic/mosaicBuffer/resizer/under` disparaissent.
- **`MosaicCompositor`** (nouvelle classe, `mcu/include/mosaiccompositor.h` +
  `mcu/src/mosaiccompositor.cpp` — côté mcu comme `VideoRescaler`, libmedikit n'est
  pas touché) : matérialise une **description de graphe** en `AVFilterGraph`
  persistant et gère le cycle push/pull. C'est la généralisation N-entrées de
  `VideoRescaler` — mêmes idiomes (`avfilter_graph_parse_ptr`, reconfiguration
  paresseuse, `AV_BUFFERSRC_FLAG_KEEP_REF` non utilisé ici, voir §6).

### 3.2 Structures de description

```cpp
// mcu/include/mosaiccompositor.h
struct MosaicSlotDesc
{
	int  pos;              // index de slot (traçage/logs)
	int  x, y;             // position de la vignette = GetLeft/GetTop + décalage letterbox
	int  w, h;             // taille effective de la vignette (rzWidth/rzHeight)
	int  inW, inH, inFmt;  // géométrie/format de la trame d'entrée (clé de reconfig)
	AVBufferRef* hwFramesCtx; // ctx trames VAAPI de l'entrée (non possédé), clé de reconfig
	bool hasOverlay;       // overlay participant à empiler sur ce slot
	int  ovW, ovH;         // taille de l'overlay rgba (== taille du slot)
};

struct MosaicGraphDesc
{
	int  width, height;    // taille du composite (mosaicTotalWidth/Height)
	bool wantGPU;          // souhait GPU (le compositor peut replier CPU)
	bool blackBackground;  // fond noir (mosaic1p1) au lieu du gris neutre
	bool hasMosaicOverlay; // overlay plein cadre
	std::vector<MosaicSlotDesc> slots;  // slots ACTIFS uniquement, ordre = ordre Z

	bool operator==(const MosaicGraphDesc&) const; // comparaison structurelle (clé de cache)
};
```

### 3.3 Interface `MosaicCompositor`

```cpp
class MosaicCompositor
{
public:
	MosaicCompositor();
	~MosaicCompositor();                      // Release() : avfilter_graph_free
	MosaicCompositor(const MosaicCompositor&)            = delete;
	MosaicCompositor& operator=(const MosaicCompositor&) = delete;

	// (Re)construit le graphe si desc != description courante (sinon no-op).
	// Applique la politique GPU/CPU (§2) avec repli. false = échec définitif
	// (l'appelant renonce à composer ce tick).
	bool Configure(const MosaicGraphDesc& desc);

	// Un tick de composition : pousse la trame de chaque slot actif, le fond,
	// les overlays (tous au même pts, tick monotone), puis tire le sink une fois.
	// slotFrames[i] / slotOverlays[i] alignés sur desc.slots[i].
	// Retour : composite dans un Pict NEUF (nullptr en cas d'échec).
	PictPtr Compose(const std::vector<PictPtr>& slotFrames,
	                const std::vector<PictPtr>& slotOverlays,
	                const PictPtr& background,
	                const PictPtr& mosaicOverlay);

	void Release();

private:
	bool BuildGraph(bool gpu);                // matérialise cur -> AVFilterGraph
	bool PushInput(AVFilterContext* src, const PictPtr& pic, int64_t pts);

	AVFilterGraph*   graph   = nullptr;
	AVFilterContext* sinkCtx = nullptr;
	AVFilterContext* bgSrc   = nullptr;
	std::vector<AVFilterContext*> slotSrcs;   // 1 par desc.slots
	std::vector<AVFilterContext*> ovrSrcs;    // 1 par slot avec hasOverlay
	AVFilterContext* mosaicOvrSrc = nullptr;

	MosaicGraphDesc  cur;                     // description du graphe courant
	bool             curGPU   = false;        // mode effectif du graphe courant
	bool             gpuBroken = false;       // échec GPU mémorisé (ne pas retenter)
	int64_t          tick     = 0;            // pts monotone (time_base 1/1000)
};
```

### 3.4 Nouvelle surface de `Mosaic` (base)

```cpp
class Mosaic
{
public:
	// API conservée telle quelle pour videomixer.cpp :
	int     Update(int pos, const PictPtr& pic); // NON virtuel : mémorise slotFrames[pos]
	                                             // (+ slotKeepAspect[pos] = keepAspect), SetChanged().
	int     Clean(int pos);                      // NON virtuel : slotFrames[pos].reset(), SetChanged().
	PictPtr GetPict();                           // compose via compositor (avec cache), cf. §4.
	// supprimés : virtual Update(int,BYTE*,int,int), virtual Clean(int),
	//             BYTE* GetFrame(), DrawVUMeter (voir §5), ApplyParticipantOverlay.

protected:
	// géométrie : inchangée, reste LA responsabilité des dérivées
	virtual int GetWidth(int pos)  = 0;
	virtual int GetHeight(int pos) = 0;
	virtual int GetTop(int pos)    = 0;
	virtual int GetLeft(int pos)   = 0;
	virtual bool HasBlackBackground() const;     // false ; true pour mosaic1p1 (AsymmetricMosaic)
	virtual bool StretchSlot(int pos) const;     // false ; true pour PIPMosaic pos==0 (étirement)

	MosaicGraphDesc BuildDesc() const;           // géométrie + ComputeSlotPlacement + overlays
	void ComputeSlotPlacement(int pos, int inW, int inH, bool keepAspect,
	                          int& w, int& h, int& dx, int& dy) const;

private:
	std::vector<PictPtr> slotFrames;             // dernière trame connue par slot (nullptr = vide)
	std::vector<bool>    slotKeepAspect;
	PictPtr              background;             // Pict de fond WxH (généré une fois)
	PictPtr              composite;              // cache du dernier composite
	MosaicCompositor     compositor;
};
```

`PartedMosaic`/`AsymmetricMosaic` se réduisent aux 4 getters de géométrie (+
`HasBlackBackground` pour `mosaic1p1`). `PIPMosaic` garde ses getters, perd
`Update/Clean/GetFrame/underBuffer` (~260 lignes de memcpy supprimées). Les décisions
type `CleanSlot(pos, mosaic, logo)` de `videomixer.cpp:16-36` sont **inchangées** :
pousser le logo dans un slot = `Update(pos, logo)` avec `keepAspect=false`, exactement
comme aujourd'hui.

---

## 4. Ordonnancement / synchronisation (composition événementielle)

La boucle `VideoMixer::MixVideo` (`videomixer.cpp:125-476`) garde **exactement sa
structure** : réveil sur `mixVideoCond` (nouvelle trame, `pipevideooutput` signale),
`forceUpdate` sur timeout 500 ms, logique VAD intacte. Ce qui change :

- `mosaic->Update(pos, output->GetFrame())` devient **une simple mémorisation de
  `PictPtr`** (copie de shared_ptr) — plus aucun coût pixel dans les rafales VAD.
- `input->SetFrame(mosaic->GetPict())` (`videomixer.cpp:156`) déclenche la
  composition **au plus une fois par tick** grâce au cache :

```cpp
PictPtr Mosaic::GetPict()
{
	if (!mosaicChanged && composite)      // rien de neuf depuis le dernier tick
		return composite;                 // plusieurs inputs même mosaïque : 1 seule compo
	MosaicGraphDesc desc = BuildDesc();
	if (!compositor.Configure(desc))      // reconfig UNIQUEMENT si desc a changé
		return composite;                 // échec : on ressert l'ancien composite
	composite = compositor.Compose(/* slotFrames actifs, overlays, background, mosaicOverlay */);
	return composite;
}
```

- **Push par tick** : `Compose` pousse dans **chaque** buffersrc (fond, slots actifs,
  overlays) la dernière trame connue, **toutes au même pts** (`tick++`, time_base
  1/1000). Framesync a ainsi toujours une paire exacte sur chaque `overlay` → le
  sink rend **exactement une trame** par tick, jamais d'EAGAIN en régime établi.
  Réglages : `eof_action=pass:repeatlast=1:shortest=0` sur tous les `overlay` ;
  **aucun filtre `fps`** (pas de cadence imposée — la cadence reste celle des
  réveils du mixer).
- **Amorçage** : à la (re)construction du graphe, tous les buffersrc sont poussés
  immédiatement (premier `Compose`) — pas d'attente framesync du « premier frame
  des deux entrées ».
- **Slot sans trame** : pas de branche (fond visible). Le cas « participant sans
  vidéo → logo » est déjà géré en amont par `CleanSlot(pos, mosaic, logo)` qui
  pousse le logo comme trame de slot.
- **Reconstruction du graphe UNIQUEMENT au changement de topologie** — la
  comparaison `desc == cur` de `Configure` (stratégie identique au
  `VideoRescaler::Configure`, `videorescaler.cpp:41-44`) échoue quand : type/taille
  de mosaïque (`SetCompositionType`), ensemble des slots actifs (arrivée/départ,
  `Clean`), **positions** (reshuffle VAD `CalculatePositions`, `SetSlot`),
  géométrie d'une entrée (`SizeHasChanged`), présence/taille d'un overlay, bascule
  GPU/CPU. Un simple changement de **contenu** (nouvelle trame, nouveau texte
  d'overlay re-rendu à la même taille) ne reconstruit **rien**.
- Le pts repart de 0 à chaque reconstruction (graphe neuf) — sans conséquence,
  aucune horloge externe ne dépend de ces pts.
- **Threading** : `MosaicCompositor` n'est pas thread-safe et n'a pas à l'être —
  tous les accès (Update/Clean/GetPict/Configure) se font sous la protection
  existante de `lstVideosUse` dans le thread MixVideo et les méthodes XML-RPC de
  `VideoMixer`, comme aujourd'hui pour les buffers BYTE\*.

Optimisation notée (non requise en première livraison) : le filtre `overlay` accepte
les commandes runtime `x`/`y` (`avfilter_graph_send_command`) — permettrait de
déplacer une vignette (reshuffle VAD) **sans** reconstruire le graphe. À garder pour
plus tard si le coût de reconstruction se voyait (§8).

---

## 5. Overlays, fond, logo, VU-mètre

| Élément actuel | Devenir |
|---|---|
| Fond gris `memset -128` (`mosaic.cpp:133`) + `Clean` par slot | buffersrc de fond alimenté par un `Pict` gris WxH généré une fois (fabrique à ajouter : `Pict::CreateColor(w,h,y,u,v)` dans `libmedikit/video.cpp`, à côté de `CreateBlack`) ; slot vide = pas de branche |
| Fond noir `mosaic1p1` (`asymmetricmosaic.cpp:13-24`) | même buffersrc, `Pict::CreateBlack(w,h)` (`HasBlackBackground()`) |
| Fond « à trous » PIP (`pipmosaic.cpp:171-254`) | disparaît : slot 0 plein cadre en bas de pile, PIP en `overlay` par-dessus |
| Logo slot vide (`CleanSlot(pos,mosaic,logo)`, `videomixer.cpp:16-30`) | inchangé côté mixer : le logo (`PictPtr` déjà chargé par `Pict::Load`) est poussé comme trame du slot |
| Rendu image/SVG/texte ImageMagick (`Overlay::LoadImage/RenderText`) | **CONSERVÉ** (le rendu). `Overlay` gagne `PictPtr GetPict()` : enveloppe **directement le RGBA natif d'ImageMagick** dans un `AVFrame` `AV_PIX_FMT_RGBA` (Pict caché, régénéré au changement de contenu/taille) — **pas de `ConvertToYUVA`** sur ce chemin, la conversion RGBA→YUV et l'alpha sont faits par le graphe. `ConvertToYUVA` (buffer `overlay` yuva420p) reste tant que `Display` existe, supprimé en Phase 6. |
| Blit alpha manuel (`Overlay::Display`, `overlay.cpp:466-747`, ~280 lignes) + `ApplyParticipantOverlay` (`mosaic.cpp:1057`) | **SUPPRIMÉS** (Phase 6) : remplacés par les entrées `overlay` RGBA du graphe (l'alpha du RGBA est appliqué nativement par le filtre) |
| Overlay mosaïque plein cadre (`Mosaic::GetFrame` + `overlayNeedsUpdate`) | dernière entrée `overlay` du graphe ; `overlayNeedsUpdate` disparaît (le push par tick ressert le Pict caché) |
| VU-mètre (`Mosaic::DrawVUMeter`, `mosaic.cpp:1110`, décl. `mosaic.h:102` — memset dans le composite `BYTE*`, seul appel `videomixer.cpp:452-463` sous `#ifdef MCUDEBUG`) | **SUPPRIMÉ ENTIÈREMENT** (décision 2026-07-16). N'opérait que sur le buffer `BYTE* mosaic` (condamné en Phase 6), uniquement en build `MCUDEBUG`, inopérant sur le chemin GPU. Le réécrire en post-passe AVFrame (`av_frame_make_writable` + memset via linesize) a un coût réel pour un outil de diagnostic marginal. On retire l'appel `videomixer.cpp:452-463`, la méthode `Mosaic::DrawVUMeter` et sa déclaration. Diagnostic VAD éventuel = `Log()` du niveau par slot dans `MixVideo` (2 lignes), pas de VU-mètre pixel. |

**Remplacement éventuel (feature prod, hors périmètre de ce plan)** — si un
indicateur d'activité de parole *visible par les endpoints* est souhaité, ce
n'est plus un portage du code debug supprimé mais une feature à part entière,
côté signalisation plutôt que côté pixels du composite :

- **RFC 6465 — Mixer-to-Client Audio Level Indication** : extension d'en-tête RTP
  (`urn:ietf:params:rtp-hdrext:csrc-audio-level`) portant, dans le flux audio
  mixé envoyé au client, le niveau de chaque source (via la liste CSRC). Le
  client affiche lui-même qui parle. Voie standard, à négocier en SDP ; s'appuie
  sur les niveaux déjà calculés par l'audiomixer/VAD (mêmes valeurs que celles
  qui alimentaient `DrawVUMeter`). Nécessite d'émettre les CSRC + l'extension
  dans `audiostream`/`rtpsession`.
- **Indicateur d'activité en T.140** : repli pour les endpoints sans RFC 6465 —
  émettre un marqueur de « participant actif » sur le canal texte temps réel.
  Solution non standard/ad hoc, à réserver aux cas où l'extension RTP n'est pas
  négociable.

À traiter comme un chantier signalisation distinct (proche de la négo fmtp /
extension API JSR309), pas dans la refonte mosaïques.

---

## 6. Ownership `PictPtr` et intégration `videomixer.cpp`

- **Entrées** : `slotFrames[pos]` détient une copie du `shared_ptr` publié par
  `PipeVideoOutput::GetFrame()` — la trame survit tant que la mosaïque la ressert
  (`repeatlast` implique que le graphe garde aussi sa propre référence interne).
  Les trames sont **immuables** : jamais de `av_frame_make_writable` sur une entrée.
- **Push** : pour poser le pts sans muter la trame partagée, chaque push crée un
  conteneur éphémère :
  `t = av_frame_alloc(); av_frame_ref(t, pic->GetAVFrame()); t->pts = tick;`
  `av_buffersrc_add_frame(srcCtx, t)` (le graphe **prend** la référence, `t` ressort
  vide) puis `av_frame_free(&t)`. Jamais de pointeur nu du `Pict` donné au graphe.
- **Sortie** : `av_buffersink_get_frame` remplit un `AVFrame` neuf →
  `std::make_shared<Pict>(out)` (adoption, patron `videorescaler.cpp:154-166`).
  Le composite est publié aux pipes par `input->SetFrame(composite)` — copie de
  shared_ptr, zéro copie pixel (le pont `GetPict()` copiant `w*h*3/2` octets
  disparaît).
- **Destruction** : `Release()` fait `avfilter_graph_free(&graph)` qui libère les
  références internes (framesync comprises). `MosaicCompositor` est membre de
  `Mosaic` → détruit avec lui (déjà hors verrou grâce à `DeleteMosaic`,
  `videomixer.cpp:1316-1337`). Aucune référence du graphe ne pointe vers des objets
  de durée de vie inférieure au `Mosaic` (le `hwFramesCtx` des buffersrc est
  refcompté par ffmpeg).
- **Suppressions finales** : pont `Mosaic::Update(int, const PictPtr&)` version
  aplatissement + pont `Mosaic::GetPict()` copie (`mosaic.cpp:23-81`), surcharges
  virtuelles `Update(int,BYTE*,int,int)`/`Clean` des 4 classes, `Mosaic::GetFrame()`,
  `mosaic/mosaicBuffer/mosaicSize/resizer`, `PIPMosaic::under/underBuffer/GetFrame`,
  `Overlay::Display/image/imageBuffer`, `FrameScaler` (voir Phase 6 — y compris
  `libmedikit/transcoder.cpp` qui l'utilise encore, dernier appelant).
- `videomixer.cpp` : la boucle ne change **pas de forme** — `Update(pos, pict)` =
  dépôt, `GetPict()` = push+pull caché, `Clean(pos)` = retrait. `Reset()/HasChanged()`
  gardent leur rôle de dirty-flag par tick.

---

## 7. Plan d'implémentation par phases (build vert à chaque phase)

Compilation : `cd /home/ebuu/mediaserver/mcu && make -f Makefile.rpm mcu` ;
`./install.ksh libmedkit` quand libmedikit est touchée. **PIÈGE connu** : le Makefile
ne suit pas les headers → `rm` des `.o` concernés (mcu **et** libmedikit) après tout
changement de header partagé (`medkit/video.h`, `mosaic.h`).

### Phase 0 — Briques passives (aucun comportement changé) — FAIT 2026-07-16
- `Pict::CreateColor(w,h,y,u,v)` **dans `libmedikit/logo.cpp`** (là où vit réellement
  `CreateBlack`, pas `video.cpp`) + déclaration `medkit/video.h` ; `CreateBlack`
  refactorée en `return CreateColor(w,h,0,128,128)` (sortie identique).
- `Overlay::GetPict()` dans `mcu/src/overlay.cpp` : enveloppe **le RGBA natif
  d'ImageMagick** dans un `Pict` `AV_PIX_FMT_RGBA` caché (helper `RGBABlobToPict`),
  **sans `ConvertToYUVA`** — la conversion vers YUV est laissée au graphe. Cache
  invalidé par `LoadImage/RenderSVG/RenderText/Clear/Resize`. `Display` (et le
  `ConvertToYUVA` qui alimente son buffer yuva420p) laissés intacts jusqu'en Phase 6.
- Fichiers : `medkit/video.h`, `libmedikit/logo.cpp`, `mcu/include/overlay.h`,
  `mcu/src/overlay.cpp`. `rm` de `logo.o`/`overlay.o`, rebuild libmedkit + mcu.
- **Validation** : build vert, aucun appelant, non-régression triviale (lancement
  d'une conf de test).

### Phase 1 — `MosaicCompositor` (nouvelle classe, non branchée) — FAIT 2026-07-16
- `mcu/include/mosaiccompositor.h` + `mcu/src/mosaiccompositor.cpp` : structures §3.2,
  interface §3.3, chemin **CPU uniquement** (le GPU vient en Phase 5) ; `BuildGraph`
  génère la chaîne §1.3 par `avfilter_graph_parse_ptr` ; `Compose` push+pull §4/§6.
- Ajouter `mosaiccompositor.o` à `OBJS` dans `mcu/Makefile.rpm` (ligne 90).
- **Détails d'implémentation** : buffersrc créés par `avfilter_graph_create_filter`
  (pour poser plus tard `hw_frames_ctx` par entrée), corps de chaîne (scale + cascade
  overlay) par `avfilter_graph_parse_ptr` ; labels ouverts = buffersrc côté `outputs`,
  sink côté `inputs` (nom = dernier label produit). Overlays participants empilés
  APRÈS toutes les vignettes (fidèle à l'exemple asymétrique §1.3), overlay mosaïque
  en dernier. Sink contraint `AV_PIX_FMT_YUV420P` (`av_opt_set_int_list`). Cas
  dégénéré (0 slot) : `[bg] null`. `MosaicSlotDesc` a gagné `ovX/ovY` (position
  absolue de l'overlay participant = GetLeft/GetTop du slot). `Configure` : structure
  GPU-d'abord/repli-CPU en place (`gpuBroken`), `BuildGraph(true)` renvoie false
  jusqu'en Phase 5.
- **Validation** : build vert (`make -f Makefile.rpm mcu`) ; test autonome
  `scratchpad/compositest.cpp` (façon `negotest`) 4/4 : 2x2 (luma par slot),
  letterbox (fond visible dans les bandes), reconfiguration paresseuse 2x2→no-op→1x1,
  30 ticks consécutifs (1 trame/tick, pas d'EAGAIN).

### Phase 2 — Description de géométrie dans `Mosaic` (chemin actuel conservé) — FAIT 2026-07-16
- Base `Mosaic` : membres `slotFrames/slotKeepAspect/background/composite/compositor` ;
  `ComputeSlotPlacement` factorisé (copie fidèle de l'arithmétique
  `partedmosaic.cpp:144-205` / `asymmetricmosaic.cpp:119-173`) ; `BuildDesc()` ;
  virtuels `HasBlackBackground()`/`StretchSlot()` implémentés dans les dérivées.
- `Mosaic::Update(int, const PictPtr&)` mémorise `slotFrames[pos]` **en plus** de
  déléguer au pont BYTE\* existant (comportement inchangé).
- Fichiers : `mcu/include/mosaic.h`, `mcu/src/mosaic.cpp`, les 3 dérivées (ajout des
  deux petits virtuels seulement). `rm` des `.o` mcu (header mosaic.h).
- **Détails** : `ComputeSlotPlacement` compare `picRatio` au ratio GLOBAL de la
  mosaïque (membre `ratio`, comme l'existant), diff vertical rendu pair ; `StretchSlot`
  court-circuite l'aspect (PIP pos 0). `BuildDesc` = slots ACTIFS uniquement (trame
  mémorisée non nulle), `x=GetLeft+dx`, `y=GetTop+dy` ; `wantGPU=false` et overlays
  laissés à Phase 4/5. **PIÈGE name-hiding** : la surcharge `Update(BYTE*)` des
  dérivées masque `Update(const PictPtr&)` de la base — OK car videomixer appelle via
  `Mosaic*` ; ajouter `using Mosaic::Update;` seulement si appel via type dérivé.
- **Validation** : build vert (mcu complet) ; test autonome `scratchpad/desctest.cpp`
  (sous-classes exposant `BuildDesc`) OK : 2x2 fill+letterbox 16:9 (w352 h198 y44) +
  slot vide exclu, keepAspect=false→plein slot, PIP stretch slot 0 plein cadre, fond
  noir 1p1.

### Phase 3 — Bascule du composite sur le graphe (CPU) — FAIT 2026-07-16
- `Mosaic::GetPict()` → `Configure(BuildDesc()) + Compose(...)` avec **repli** vers
  l'ancien chemin BYTE\* (`GetPictLegacy`) si `Configure` échoue (garde-fou de mise
  au point) ; `Clean(pos)` reset `slotFrames[pos]` ; `Update(PictPtr)` cesse
  d'appeler le pont d'aplatissement.
- Fichiers : `mcu/src/mosaic.cpp`, éventuels ajustements `videomixer.cpp` (aucun
  attendu — API conservée).
- **Détails d'implémentation** : cache intra-tick par un flag interne
  `compositeValid` (PAS `mosaicChanged` : `Reset()` le remet à `true` à chaque tick,
  donc il vaut toujours `true` au moment du `GetPict` en tête de `MixVideo`).
  `compositeValid` est invalidé par `SetChanged()` — point de passage unique des
  `Update(BYTE*)`/`Clean` dérivés + du nouveau `Update(PictPtr)`. La sémantique
  `HasChanged()` vue par `videomixer` est donc **inchangée** (chaque input appelle
  toujours `GetPict`), seule la composition est dédupliquée. Fond généré une fois
  (`GetBackground` : `CreateColor(128,128,128)` ou `CreateBlack` si
  `HasBlackBackground`). `Update(PictPtr)` redescend GPU→CPU (chemin Phase 3 = CPU)
  et mémorise la trame CPU (le stockage de l'original GPU reviendra en Phase 5).
  `Clean` des 3 dérivées appelle le helper base `ClearSlotFrame(pos)` ;
  `PIPMosaic::Clean` (jadis no-op) gagne `ClearSlotFrame`+`SetChanged`. Trames des
  slots ACTIFS reconstruites dans `GetPict` par `slotFrames[desc.slots[i].pos]`
  (alignées sur `desc.slots`). Overlays laissés à Phase 4 (Compose reçoit un vecteur
  vide + `nullptr`). **PIÈGE name-hiding confirmé** : appeler `Update(PictPtr)` via
  le type dérivé échoue (masqué par `Update(BYTE*)`) — videomixer appelle via
  `Mosaic*`, OK ; le test passe par une réf `Mosaic&`.
- **Validation** : build vert (`make -f Makefile.rpm mcu`) ; test autonome
  `scratchpad/mosaictest.cpp` (via `Mosaic&`, lié aux `.o` mosaïque + libmedkit)
  18/18 : remplissage 2x2 (luma par quadrant), cache intra-tick (même `PictPtr`),
  invalidation par `Update`, `Clean`→fond gris, letterbox 16:9 dans slot 4:3 (bandes
  = fond), 30 ticks consécutifs (30 composites, aucun stall/EAGAIN). Recette conf
  live (types de composition, VAD, record.mp4) restant à dérouler sur déploiement.
- **Validation (à dérouler pour CHAQUE phase suivante aussi)** : conférence de test
  et bascule XML-RPC `SetCompositionType` sur **tous les types** — 1x1, 2x2, 3x3,
  4x4, 1p1, 3p4, 1p7, 1p5, 1p4, 2p8, PIP1, PIP3 ; aspect ratio (source 16:9 dans
  slot 4:3 → letterbox, CIF → traité 4:3) ; logo sur slot vide ; VAD Basic et Full
  (reshuffle → vérifier reconstruction du graphe dans les logs, une seule par
  changement) ; enregistrement `record.mp4` pour inspection visuelle ; `mcu.log`
  sans erreurs avfilter ; CPU mixer comparé avant/après (`top -H`).

### Phase 4 — Overlays dans le nouveau chemin + suppression du VU-mètre
- Entrées overlay du graphe : `BuildDesc` renseigne `hasOverlay/ovW/ovH` depuis
  `Mosaic::overlays` (participants, indexés par `mosaicPos[pos]`) et
  `Mosaic::overlay` (mosaïque) ; `Compose` reçoit les `Overlay::GetPict()`.
- Retirer `ApplyParticipantOverlay`/`GetFrame`-overlay du flux.
- **Supprimer entièrement le VU-mètre** (cf. §5) : retrait de l'appel
  `videomixer.cpp:452-463` (`#ifdef MCUDEBUG`), de `Mosaic::DrawVUMeter`
  (`mosaic.cpp:1110`) et de sa déclaration (`mosaic.h:102`). Pas de post-passe de
  remplacement. (Indicateur d'activité éventuel = feature signalisation séparée,
  RFC 6465 / T.140, cf. §5 — hors périmètre.)
- Fichiers : `mosaic.{h,cpp}`, `mosaiccompositor.cpp`, `videomixer.cpp`.
- **Validation** : `SetOverlayImage` (mosaïque et participant), `SetDisplayName`
  (texte, y compris re-render au changement de nom → PAS de reconstruction de
  graphe), `ResetOverlay`, `MoveOverlays` via `SetCompositionType` ; vérifier que
  le build `MCUDEBUG` compile sans le VU-mètre.

### Phase 5 — Chemin GPU (VAAPI)
- `MosaicCompositor::BuildGraph(gpu=true)` : `scale_vaapi`/`overlay_vaapi`,
  `hwupload` des entrées CPU et du fond, `hw_frames_ctx` sur les buffersrc GPU,
  `hw_device_ctx` sur les `hwupload`, queue CPU si overlays (§2.5), repli
  automatique + `gpuBroken`. Politique §2 dans `Configure`.
- Exposer si besoin `GetSharedVAAPIDevice()` de `libmedikit/video.cpp` (aujourd'hui
  `static`) via un accesseur `medkit/video.h` (`Pict::GetVAAPIDevice()` par ex.).
- **Validation** : machine à GPU (comme pour le chemin VAAPI de `FfVideoEncoder`,
  toujours à valider sur GPU réel) : décodeurs VAAPI → mosaïque → encodeur VAAPI
  sans redescente (vérifier `IsGPUPict()` du composite + absence d'appels
  `DownloadToCPU` dans les traces) ; mélange participants GPU/CPU ; machine sans
  GPU → repli CPU silencieux (une seule trace).

### Phase 6 — Suppressions (ponts, BYTE\*, FrameScaler)
- Supprimer : ponts `mosaic.cpp:23-81`, surcharges `Update(BYTE*)`/`Clean` des 4
  classes et leurs ~600 lignes de blits, `Mosaic::GetFrame()`,
  `mosaic/mosaicBuffer/resizer`, `PIPMosaic::under*/GetFrame/GetSlots`,
  `Overlay::Display` + buffers `image*`, includes `framescaler.h` de
  `mosaic.h/partedmosaic.h/asymmetricmosaic.h/pipmosaic.h`.
- **Déplacer `VideoRescaler` dans libmedikit** (`medkit/videorescaler.h` +
  `videorescaler.cpp`, ajout à `OBJS` du Makefile libmedikit ; `mcu/Makefile.rpm`
  retire `videorescaler.o`, `mcu/include/videorescaler.h` devient un simple
  `#include <medkit/videorescaler.h>` ou disparaît) et **porter
  `libmedikit/transcoder.cpp`** (dernier utilisateur de `FrameScaler`,
  `transcoder.cpp:26,251`) sur `VideoRescaler`.
- Supprimer `framescaler.{h,cpp}` **des deux arbres** (`mcu` : Makefile.rpm lignes
  90 et 98 `framescaler.o` ; libmedikit : Makefile ligne 101) — ensemble, pour ne
  jamais laisser une seule des deux copies (risque de double définition/ODR).
- `rm` de **tous** les `.o` (mcu + libmedikit), rebuild complet
  `./install.ksh localcompile`.
- **Validation** : campagne complète Phase 3 + Phase 4 + point-à-point transcodé
  (chemin transcoder) + `grep -rn FrameScaler` vide ; mise à jour `avframe.md`
  (§8 = fait) et de la mémoire projet.

**FAIT 2026-08-03 (branche feat/hw-mcu), avec deux écarts assumés au plan :**
- `libmedikit/transcoder.cpp` n'est PAS porté sur `VideoRescaler` : il n'est
  compilé qu'en `ASTERISK=yes` (en-têtes Asterisk absents ici, port invérifiable).
  En conséquence `framescaler.{h,cpp}` ne sont pas supprimés de libmedikit mais
  `framescaler.o` est déplacé dans `ASTOBJ` (objets Asterisk-only) : plus AUCUNE
  copie compilée dans nos binaires, une seule copie source au total (ODR réglé).
  Le port + la suppression définitive se feront lors d'une campagne Asterisk.
- `Mosaic::GetPictLegacy()` (repli BYTE*) supprimé aussi : sur échec de
  (re)configuration du graphe, `GetPict()` ressert le dernier composite connu.
Le reste est conforme : Update(BYTE*)/Clean virtuels supprimés (Clean devient
concret dans la base : ClearSlotFrame+SetChanged), GetFrame/ApplyParticipantOverlay/
DrawVUMeter/mosaic/mosaicBuffer/resizer/under* retirés, Overlay::Display +
ConvertToYUVA + buffers YUVA retirés (le Pict RGBA en cache est le seul rendu),
VideoRescaler déplacé dans libmedikit (`medkit/videorescaler.h`, shim mcu),
includes framescaler purgés. Bilan : ~1 500 lignes nettes supprimées, 85 tests PASS.

Chaque phase est **additive** (l'ancien chemin reste le repli jusqu'à la Phase 6),
sur le modèle des migrations smart-pointers et VideoRescaler précédentes.

---

## 8. Risques et points d'attention

1. **Coût de reconstruction du graphe** : `avfilter_graph_config` sur ~20 filtres =
   quelques ms. Déclencheurs bornés (changement de topologie, reshuffle VAD ~périodes
   de 5 s `vadDefaultChangePeriod`, changement de résolution d'entrée). Si mesuré
   gênant : commandes runtime `overlay x/y` (`avfilter_graph_send_command`) pour les
   déplacements, reconstruction réservée aux changements structurels.
2. **Latence/stalls framesync** : `overlay` ne sort rien tant qu'une entrée n'a pas
   de trame → règle absolue « **chaque tick pousse TOUTES les entrées au même
   pts** » + amorçage au premier `Compose`. Ne jamais insérer `fps`. Si une entrée
   était omise, le sink rendrait EAGAIN : traiter EAGAIN comme « ressert l'ancien
   composite » (jamais bloquer le thread MixVideo).
3. **Formats non-420** : branches CPU terminées par `format=yuv420p`, buffersink
   contraint — sinon un participant NV12/422 ferait échouer la négociation de
   formats du graphe ou sortirait un composite inattendu pour les encodeurs.
4. **VAAPI mixte** : `overlay_vaapi` exige des entrées sur le même device ; les
   `hw_frames_ctx` diffèrent par décodeur mais dérivent du device partagé — les
   `scale_vaapi` intermédiaires renormalisent. En cas d'échec de config : repli CPU
   automatique (`gpuBroken`). L'alpha per-pixel `overlay_vaapi` n'est PAS supposé
   fonctionner → overlays = queue CPU (§2.5).
5. **Fuites de références AVFrame** : (a) toujours passer au graphe un conteneur
   éphémère (`av_frame_ref` + `av_buffersrc_add_frame` + `av_frame_free`) — jamais
   l'`AVFrame` du `Pict` partagé, sinon le graphe posera son pts/unref sur une trame
   partagée ; (b) sur échec de `Compose` à mi-parcours, `av_frame_free` systématique
   du frame de sortie ; (c) `Release()` avant toute reconstruction (le
   `avfilter_graph_free` libère les refs framesync internes). Valgrind/ASAN sur une
   conf de 10 min en fin de Phase 3 et 6.
6. **Ordre de destruction** : `MosaicCompositor` membre de `Mosaic`, détruit hors
   verrou par le mécanisme existant de `DeleteMosaic` (shared_ptr extrait puis
   relâché hors lock). Ne jamais faire détenir au graphe un pointeur vers `Overlay`
   ou `PipeVideoOutput` — uniquement des refs `AVBufferRef` refcomptées.
7. **Double définition `FrameScaler`** (mcu + libmedikit) : suppression atomique des
   deux copies en Phase 6, `transcoder.cpp` porté d'abord ; jusqu'à cette phase, ne
   toucher à aucune des deux (ABI mcu↔medkit).
8. **Pièges de build** : `rm *.o` (mcu et libmedikit) après modification de
   `medkit/video.h`/`mosaic.h` ; `libmedkit.a` périmée (piège x264 déjà rencontré) →
   en cas de comportement absurde, `./install.ksh libmedkit` puis relink complet.
9. **Comportements historiques à préserver sciemment** : letterbox laissant voir le
   fond (pas de `pad` noir), CIF/SIF traités 4:3 (`ComputeAspectRatio` conservé),
   étirement du logo (`keepAspect=false`) et du plein-cadre PIP, fond noir de
   `mosaic1p1`. Tout écart visuel doit être un choix, pas un accident.
10. **`Update` PIP pos==0 avec image plus petite** : l'actuel copie l'image redimensionnée
   plein cadre puis re-blitte les PIP par-dessus au tick suivant seulement ; le graphe
   compose tout à chaque tick — le rendu devient plus cohérent (pas de « trous » périmés),
   à mentionner en recette comme amélioration attendue et non régression.
