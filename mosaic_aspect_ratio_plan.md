# Plan de correction — aspect ratio de la mosaïque

Constat du 2026-08-06 : la vidéo affichée dans un slot de mosaïque n'a pas le bon
format. Capture de référence : un appel Linphone 6.2 (VP8) dans une conférence `1+1`,
slot gauche entouré en rouge, logo `ffmpeg.jpg` à droite.

Ce document sépare **ce qui est déjà juste** de **trois défauts distincts**, parce que
deux d'entre eux sont invisibles l'un derrière l'autre et que corriger le mauvais ne
change rien à l'image.

## 0. L'état observé, mesuré et non supposé

Conférence (`kelictl mcu conference.show`) :

```
Video:  bitrate=1024 fps=15 intra_period=300 size=hd720p   <- taille ENCODEUR (1280x720)
Layout: auto=false comp=1+1 size=vga                       <- taille TOILE (640x480)
Logo:   ffmpeg.jpg                                          <- bannière 5000x1340
```

Graphe de composition effectivement construit (`MosaicCompositor: graphe #9`) :

```
CPU 640x480, fond noir, 2 slot(s):
  pos0 640x480(fmt0)->316x237@2,120(b2)+ovr     <- participant : letterbox CORRECT
  pos1 5000x1340(fmt0)->316x476@322,2(b2)       <- logo : ÉTIRÉ plein slot
desc=[[in0] scale=316:237,…,pad=320:241:2:2:black [s0];
      [in1] scale=316:476,…,pad=320:480:2:2:black [s1]; …]
```

Mesure sur la capture : le sous-cadre du participant apparaît en ~1.71 alors que son
image est composée en 4:3 (1.333). Le facteur 1.28-1.33 est exactement le rapport
640x480 → 1280x720. **La distorsion visible ne vient pas du slot, elle vient de la
mise à l'échelle toile → encodeur.**

## 1. Ce qui est déjà juste — à ne pas « corriger »

- **`Mosaic::ComputeSlotPlacement`** (mosaic.cpp:52) calcule un letterbox/pillarbox
  correct : ratio de l'image comparé au ratio du **slot** (et non à celui de la toile,
  ce qui était le bug historique), largeur ou hauteur conservée, décalage centré et
  rendu **pair** pour ne pas décaler le chroma 4:2:0. Le slot participant de la capture
  en est la preuve : 640x480 → 316x237 centré à y=120.
- **La réaction à un changement de format entrant.** `MosaicSlotDesc` porte `inW`,
  `inH`, `inFmt` et `hwFramesCtx`, et `MosaicGraphDesc::operator==` les compare : toute
  variation de la taille (ou du format, ou du passage GPU/CPU) d'une entrée **reconstruit
  le graphe** et repasse donc par `ComputeSlotPlacement`. Un simple changement de contenu
  ne reconstruit rien. C'est le comportement voulu, et il est en place.
- Le liseré (`GetSlotBorder`) et le `pad` qui le rend : cohérents avec les positions
  d'overlay.

## 2. Défaut D1 — la toile est étirée vers la taille de l'encodeur (dominant)

`PipeVideoInput` (pipevideoinput.cpp:191) :

```cpp
last = resizer.Rescale(pic, videoWidth, videoHeight, /* keepRatio = */ false);
```

`videoWidth/Height` viennent de `VideoStream::SetVideoCodec(mode=…)`, c'est-à-dire de
`conf.video.size` côté kelixip. La toile, elle, vient de `SetCompositionType(…, size)`,
c'est-à-dire de `conf.layout.size`. **Deux réglages indépendants, aucun invariant entre
leurs formats**, et le rescale final ne conserve pas le ratio : une toile 4:3 encodée en
16:9 élargit tout de 33 %, letterbox du slot compris.

Trois options, à trancher :

1. **Composer à la géométrie de l'encodeur** (recommandé). La toile suit `video.size` ;
   `layout.size` cesse d'être un réglage et ne garde que la *disposition*. Plus aucun
   rescale toile → encodeur, donc ni perte de netteté ni bandes. C'est aussi ce
   qui rend le chantier « profil vidéo par patte » cohérent : si demain chaque patte
   encode à sa taille, une toile unique devra de toute façon être redimensionnée **par
   patte**, et c'est là que le letterbox devra vivre.
2. ~~**`keepRatio = true`**~~ — **faux, et c'était une erreur de ce plan** :
   `VideoRescaler::Rescale(…, keepRatio=true)` ne letterboxe pas, il conserve la
   *largeur* demandée et **recalcule la hauteur** d'après le ratio source. La sortie n'a
   donc pas la taille imposée par l'encodeur, ce qui ne convient pas ici. Un letterbox
   véritable (ratio conservé **et** taille imposée, bandes noires centrées) n'existait
   pas dans le rescaler : il a fallu l'écrire.
3. **Contraindre côté kelixip** : refuser (ou aligner) une paire `layout.size` /
   `video.size` de formats différents. Nécessaire de toute façon comme garde-fou, quelle
   que soit l'option retenue, avec un log nommant les deux tailles.

Étapes pour l'option 1 :

- `VideoMixer::SetCompositionType(mosaicId, comp, size)` : conserver l'argument `size`
  pour compatibilité, mais faire dériver `mosaicTotalWidth/Height` de la taille
  d'encodage de la conférence ; tracer quand les deux diffèrent.
- Vérifier les consommateurs de la toile qui supposent `layout.size` :
  `Mosaic::GetWidth/GetHeight(pos)`, `Overlay::Resize(mosaicTotalWidth, …)`,
  `MosaicCompositor` (taille du `bg`).
- Côté kelixip : `SetCompositionType` cesse d'envoyer une taille propre, ou l'aligne sur
  `video.size` ; `conference.update` refuse un couple incohérent.

### D1 — ce qui a atterri le 2026-08-06

Les options 1 et 3 ensemble, plus le letterbox manquant :

- **kelixip** : `align_canvas/3` fait suivre la toile à `video.size`. Une conférence dont
  le `layout` réclame une autre taille est **avertie et alignée** (le log nomme les deux
  tailles en clair : `mosaic canvas size vga ignored — … ENCODED size hd720p`), et
  déplacer `video.size` repousse `SetCompositionType` même si l'appelant n'a pas parlé de
  disposition — sinon la toile resterait en arrière et le composite repartirait à
  l'échelle. Quatre tests qui pinnaient l'ancien réglage indépendant ont été réécrits.
- **serveur** : `VideoRescaler::Letterbox(in, w, h)` — ratio conservé, taille imposée,
  bandes noires centrées, offsets forcés pairs (un offset impair serait arrondi par le
  chroma 4:2:0 et décalerait l'image d'un pixel). `PipeVideoInput` l'utilise à la place
  de `Rescale(…, false)`. Dans le cas normal — toile == taille encodée — le rescaler rend
  la trame **par référence** : zéro copie, et c'est bien le graphe de la mosaïque qui
  fait tout le travail.

Deux points relevés en le faisant, à connaître :

- **Pas de `pad_vaapi` en ffmpeg 5** (vérifié sur 5.1.9 : `scale_vaapi` a bien
  `force_original_aspect_ratio`, mais aucun filtre de remplissage VAAPI n'existe). Un
  letterbox sur entrée GPU redescend donc la trame et se fait au CPU, avec un log qui le
  dit ; l'encodeur matériel la remonte lui-même (`av_hwframe_transfer_data` dans
  `FfVideoEncoder::EncodeFrame`), donc c'est correct au prix d'un aller-retour. La
  version nativement GPU demanderait `overlay_vaapi` sur une surface noire générée —
  faisable, non testable sur le poste de dev, et jamais emprunté tant que le contrôleur
  compose à la taille encodée.
- **`videopipe.cpp:214` fait encore `Rescale(…, false)`** : même saut, même étirement,
  mais sur le chemin point-à-point (JSR-309) et non sur la mosaïque. À basculer sur
  `Letterbox` avec l'alignement du §19.3 (P8b) plutôt qu'isolément.

La duplication de fond reste : la géométrie voyage par **deux** RPC (`SetCompositionType`
pour la toile, `SetVideoCodec` pour l'encodeur), donc deux valeurs pour une seule
décision. Aujourd'hui kelixip garantit qu'elles coïncident ; le correctif structurel
serait que `SetCompositionType` cesse de prendre une taille. Il n'est vrai que tant que
toutes les pattes encodent à la même taille — le chantier « profil par patte » le
réouvre, et c'est alors que le `Letterbox` ci-dessus devient porteur.

## 3. Défaut D2 — le logo est étiré délibérément, et il pollue un drapeau global

`videomixer.cpp:16` et `:48` :

```cpp
inline void CleanSlot(int pos, Mosaic *mosaic, const PictPtr &p_logo)
{
	if ( p_logo ) {
		mosaic->KeepAspectRatio(false);   // « strech it »
		mosaic->Update(pos, p_logo);
	}
	…
}
// LoadLogo : //Set logo and strech it
itMosaic->second->KeepAspectRatio(false);
```

Deux problèmes :

- **L'étirement volontaire** n'a de sens que pour un logo taillé au slot. Une bannière
  (ici 5000x1340, ratio 3.73) écrasée dans un slot `1+1` de 316x476 (ratio 0.66) donne
  exactement ce que montre la capture. Le logo doit passer par le même letterbox que
  n'importe quelle entrée.
- **Le drapeau est global à la mosaïque** (`Mosaic::keepAspect`) et il est recopié par
  slot au moment du `Update` (`slotKeepAspect[index]`). Le mettre à `false` pour un logo
  laisse le mixeur devoir le remettre à `true` avant la prochaine trame participante
  (videomixer.cpp:388, 421, 443, 1261 le font depuis `output->IsAspectRatioKept()`).
  C'est la « course bénigne » notée dans `mosaic.h` — bénigne aujourd'hui, mais c'est
  une variable partagée qui décide d'une géométrie.

Correction :

- supprimer les deux `KeepAspectRatio(false)` du chemin logo, et laisser
  `ComputeSlotPlacement` letterboxer le logo comme le reste ;
- passer l'aspect **en argument de `Update`** (`Update(index, pic, keepAspect)`) plutôt
  que par le drapeau global, celui-ci ne restant que pour la compatibilité des appelants
  historiques. Le champ `slotKeepAspect` existe déjà : il devient alimenté explicitement ;
- si un opérateur veut vraiment un logo plein slot, cela devient une propriété du logo
  (ou de la conférence), pas un effet de bord d'un appel de nettoyage.

## 4. Défaut D3 — la géométrie des slots de `1+1` est hostile aux formats paysage

`1+1` sur une toile 640x480 donne deux cellules de **320x480**, soit un ratio 0.66 :
portrait. Une caméra 4:3 ou 16:9 correctement letterboxée y occupe une bande centrale et
laisse deux grosses bandes noires — l'image est *juste* mais le rendu reste mauvais. Ce
n'est pas un bug de calcul, c'est un choix de disposition.

À trancher avec D1 : si la toile devient 16:9 (hd720p), `1+1` donne deux cellules de
640x720 (0.89), toujours portrait. Une disposition « deux vignettes côte à côte » qui
respecte le paysage veut des cellules **640x360** centrées verticalement, c'est-à-dire
que le layout doit connaître le format cible. C'est le même sujet que D1 : la disposition
et la taille de la toile ne peuvent plus être choisies séparément.

## 5. Ordre de traitement proposé

1. ~~**D1**~~ — fait le 2026-08-06 (options 1 et 3, plus le letterbox du rescaler).
2. **D2** : letterbox du logo + aspect passé par argument. Contenu, testable seul
   (`tests/` de libmedikit n'a pas de test de mosaïque : en ajouter un sur
   `ComputeSlotPlacement` et un sur la description de graphe produite pour un logo
   bannière).
3. **D3** : à discuter avec le chantier « profil vidéo par patte » — les deux décident de
   la même chose, la géométrie que voit chaque participant.
4. La duplication de la géométrie sur deux RPC, si l'on veut la rendre impossible plutôt
   qu'improbable.

## 6. Comment vérifier

- `MosaicCompositor: graphe #N construit` donne, pour chaque slot, `inWxinH(fmt)->wxh@x,y`.
  Un letterbox correct se lit directement : `w/h` doit valoir le ratio de `inW/inH` dès
  que celui-ci diffère du ratio du slot.
- Ajouter à ce log la taille d'encodage cible, pour que la comparaison toile/encodeur
  n'exige pas de croiser deux lignes.
- Refaire l'appel Linphone de référence avec le logo bannière : le logo doit apparaître
  en bande centrée, et le participant à son vrai format.
