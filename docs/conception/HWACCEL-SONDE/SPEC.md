# Sonde d'accélération matérielle au démarrage

> Statut : **lot 0 fait** — le mode `mediaserver --hwprobe` (§6, §10). Les lots
> 1 à 4 sont à faire : aujourd'hui, la sonde ne décide encore rien.
> Décision d'architecture : [ADR 002](../../architecture/adr-002-sonde-gpu-hors-processus.md).
> Prérequis : ffmpeg 8 au minimum (libavfilter 11). En dessous, `main()`
> éteint le GPU (voir §8).

## 1. Objectif

Le serveur ne doit annoncer « GPU actif » que pour ce qu'il a **vu marcher**.

Aujourd'hui, `hardware.vaapi` vaut `true` dès que le device VAAPI s'ouvre.
Cela ne prouve rien sur l'usage réel. Un device peut s'ouvrir alors que :

- l'encodeur accepte chaque image, ne rend aucun paquet, puis arrête le
  processus sur une assertion de libavcodec ;
- le décodeur refuse le profil du flux et passe en logiciel sans le dire ;
- le graphe de mosaïque GPU échoue à sa configuration et passe en CPU pour de
  bon.

Dans ces trois cas, l'appel marche, le statut annonce le GPU, et le GPU ne sert
pas. La sonde remplace cette déclaration par une **mesure**, capacité par
capacité, au démarrage.

## 2. Ce que la sonde prouve, et ce qu'elle ne prouve pas

Elle prouve qu'une opération donne une **image juste**, sur ce poste, avec ce
driver, à une taille de test.

Elle ne prouve pas :

- que l'opération tient **la charge**. Un driver peut réussir en 320×240 et
  échouer en 1080p, ou échouer à 40 sessions. La capacité en parallèle est
  mesurée par un autre outil (§9).
- qu'elle tiendra **toute la durée** de vie du processus. Le repli pendant
  l'exécution (`FfVideoEncoder::FallbackToSoftware`) reste nécessaire.

## 3. Les capacités sondées

Chaque capacité est une opération élémentaire, avec un verdict propre. Un
échec n'éteint que sa capacité, sauf l'échec du device. La sonde teste le
chemin **que le serveur emprunte**, pas ce que ffmpeg saurait faire en théorie.

| Capacité | Opération | Vérification |
|---|---|---|
| `device` | `Pict::GetVAAPIDevice()` | non nul |
| `upload` | CPU → surface NV12 (`Pict::UploadToGPU`) | la redescente rend la mire à ±2 |
| `download` | surface → CPU (`Pict::DownloadToCPU`) | luma et chroma à ±2 |
| `h264.encode` | `H264Encoder`, `video.hwaccel.required=1` | voir §4, décodage de contrôle en logiciel |
| `h264.decode.<profil>` | `H264Decoder(true)`, flux encodé en logiciel | voir §4 |
| `h264.decode` | synthèse des profils | `ok` dès qu'un profil l'est |
| `vp8.decode` | `FfVideoDecoder` VP8 en VAAPI exigé, flux encodé en logiciel | voir §4 |
| `vp8.encode` | toujours `absent` : `VP8Encoder` ne tente pas le matériel | — |
| `av1.decode` | toujours `absent` : `AV1Decoder` passe par libdav1d, logiciel | — |
| `scale` | `VideoRescaler` sur une surface (`scale_vaapi`), vers 160×120 | mire à ±4, sortie en surface |
| `mosaic` | `MosaicCompositor` 640×360, un slot GPU et un slot CPU | mire du slot GPU, luma du slot CPU, du liseré et du fond, sortie en surface |

`vp8.encode` et `av1.decode` figurent dans la liste pour que leur absence soit
dite, pas devinée : ffmpeg fournit `vp8_vaapi` et un décodeur AV1 VAAPI, mais
libmedikit ne s'en sert pas.

Une capacité a trois états :

- `ok` : l'opération a rendu une image juste ;
- `absent` : le serveur ne l'emprunte pas, ou le poste n'a pas de device VAAPI.
  Sans device, toutes les capacités sont `absent`. Ce n'est pas une panne ;
- `echec` : elle est empruntée, mais elle a échoué, menti, dépassé le délai, ou
  tué le processus de sonde. Le motif est conservé (voir §6).

## 4. La règle de vérification d'un codec

Un codec n'est `ok` que si **l'image qui sort ressemble à celle qui entre**.
Trois règles viennent des pannes que la sonde doit attraper :

1. **20 images.** Un encodeur matériel retient sa première image, et un DPB
   (la mémoire d'images de référence du codec) qui se remplit sans jamais rien
   rendre ne se voit qu'après une quinzaine d'images. Un encodeur est en échec
   s'il rend moins de 16 paquets.
2. **Une image décodée se compte par `GetFrame()`, pas par le code retour.**
   `FfVideoDecoder::Decode` rend 1 même quand aucune image ne sort.
3. **Un décodeur n'est matériel que si sa sortie est une surface.**
   `IsHardwareReady()` teste seulement la présence d'un device. Le décodeur
   peut avoir rejeté le flux et décoder en logiciel. Seul `IsGPUPict()` sur
   l'image rendue le prouve. Un décodeur est en échec si plus d'une image sort
   hors GPU.

Chaque sens se juge **seul**. L'encodeur matériel est contrôlé par un décodeur
libavcodec sans device. Le décodeur matériel reçoit un flux de l'encodeur
logiciel. Une panne du décodage GPU ne fait donc pas tomber l'encodage, et
inversement.

La vérification elle-même : une mire 320×240 à quatre bandes verticales de luma
connue (40, 90, 140, 190), chroma non neutre (U = 90, V = 160). On compare au
centre de chaque bande, à ±8 après compression. Une bande juste et une chroma
fausse est un échec : c'est le symptôme d'un mauvais format de surface.

### Les profils H.264 sondés

Le décodage H.264 se sonde **par profil**. VAAPI ne connaît pas le Baseline : il
ne décode que le Constrained Baseline. Le bit `constraint_set1` du 2e octet du
`profile-level-id` fait la différence. Exemple : `42801F` est du Baseline,
`42c01f` et `42e01f` sont du Constrained Baseline.

Deux règles de libmedikit rendent ce cas praticable :

- **À l'émission**, `H264Encoder` pose toujours `constraint_set1` dans le SPS
  d'un flux Baseline : son flux est du Constrained Baseline, en x264 comme en
  VAAPI. Un plid négocié `42801F` donne donc un SPS `42 C0 1F`.
- **À la réception**, `FfVideoDecoder` décode un flux H.264 Baseline sur le GPU
  (`AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH`). C'est juste tant que le flux
  n'utilise pas les outils propres au Baseline : FMO, ASO, tranches
  redondantes. Aucun terminal courant ne les utilise, mais ce n'est pas vérifié.

La sonde teste donc ces profils :

| Profil | Pourquoi |
|---|---|
| `42e01f` | Constrained Baseline, celui des navigateurs WebRTC |
| `42801F` | Baseline, celui de beaucoup de terminaux SIP : prouve la tolérance de profil. La sonde rabat l'octet de contraintes du SPS sur `0x80`, puisque notre encodeur pose `constraint_set1` |
| `4d001f` | Main |
| `64001f` | High |

`h264.decode` est `ok` dès qu'un profil l'est. La liste des profils `ok` est
publiée (§7), et le décodeur l'applique : un flux d'un profil en échec est
décodé en logiciel **et compté comme tel** (§5).

## 5. Ce que le verdict change

La sonde **décide**. Ses résultats sont la seule source consultée par les
codecs et le compositeur. Il n'y a pas de seconde liste de capacités, ni dans
une table écrite à la main, ni côté contrôleur.

| Verdict | Effet |
|---|---|
| `device` en échec | `Pict::DisableVAAPI()` : tout le processus passe en CPU |
| `h264.encode` en échec | les encodeurs H.264 s'ouvrent en logiciel |
| `h264.decode` en échec pour un profil | les flux de ce profil se décodent en logiciel |
| `scale` en échec | `VideoRescaler` refuse le chemin GPU et redescend la trame |
| `mosaic` en échec | `MosaicCompositor` ne tente pas le graphe GPU |
| `upload` ou `download` en échec | équivaut à `device` en échec : aucune autre capacité n'est utilisable |

Côté libmedikit, il faut un point de consultation unique, lu par
`FfVideoEncoder::SelectCodec`, `FfVideoDecoder` (`TryVAAPI`) et
`VideoRescaler`. Côté mcu, `Mosaic::BuildDesc` le lit pour `wantGPU`. Sa forme
exacte (par exemple `VideoAccel::IsProven(capacité)`) se décide à
l'implémentation.

Les compteurs `VideoAccel` changent aussi de définition. Un décodeur n'est
compté matériel que lorsqu'il a rendu une surface. Un décodeur qui passe en
logiciel faute de profil compte un repli. Sans cela, le compteur continue de
compter des décodeurs logiciels comme GPU.

## 6. Déroulement

La sonde tourne dans **un processus séparé**, le binaire lui-même relancé avec
l'option `--hwprobe`. Le choix, et les options écartées, sont dans
l'[ADR 002](../../architecture/adr-002-sonde-gpu-hors-processus.md).

1. `main()` lit ses options. Si `--no-hwaccel` est présent, pas de sonde.
2. Avant de créer le moindre thread ou le moindre codec, `main()` lance
   `/proc/self/exe --hwprobe` et lit sa sortie standard.
3. Le processus de sonde teste les capacités dans l'ordre du §3. Il écrit
   **une ligne par capacité, dès qu'elle est jugée**. Si le processus meurt
   au milieu, les lignes déjà écrites restent valables.
4. Le père attend, avec un délai global (`--hwprobe-timeout`, 10 s par défaut).
5. Le processus de sonde se termine :
   - normalement : les verdicts sont lus tels quels ;
   - par un signal (abort, segfault) : la capacité en cours passe en `echec`,
     avec le signal pour motif. Les capacités non encore testées passent aussi
     en `echec`, motif « non testée ». Le père ne peut pas savoir si elles
     auraient marché ;
   - après le délai : le père tue le processus de sonde. Même traitement,
     motif « délai dépassé ».
6. Le père applique les verdicts (§5), puis démarre ses serveurs.

Le lot 0 livre les étapes 3 et 5 côté processus de sonde : `mediaserver
--hwprobe` juge, écrit ses lignes, rend 0 et sort sans démarrer de serveur. Il
respecte `--no-hwaccel` et la garde ffmpeg 8 : dans les deux cas, tout est
`absent`.

Une ligne s'écrit `hwprobe <capacité> <état> <détail> (<durée> ms)`. Elle est
lisible par un humain comme par le père. Le père ne lit que les lignes qui
commencent par `hwprobe ` : les logs du serveur et de ffmpeg partagent la même
sortie. Sortie réelle sur un Iris Xe (driver iHD, ffmpeg 8) :

```
hwprobe device ok device VAAPI ouvert (0 ms)
hwprobe upload ok mire juste apres aller-retour (0 ms)
hwprobe download ok luma et chroma a +-2 (0 ms)
hwprobe h264.encode ok 19/20 paquets, mire juste (93 ms)
hwprobe h264.decode.42e01f ok 19/20 images sur GPU, mire juste (23 ms)
hwprobe h264.decode.42801F ok 19/20 images sur GPU, mire juste (15 ms)
hwprobe h264.decode.4d001f ok 19/20 images sur GPU, mire juste (15 ms)
hwprobe h264.decode.64001f ok 19/20 images sur GPU, mire juste (13 ms)
hwprobe h264.decode ok profils 42e01f 42801F 4d001f 64001f (0 ms)
hwprobe vp8.decode ok 20/20 images sur GPU, mire juste (22 ms)
hwprobe vp8.encode absent VP8Encoder encode en logiciel (libvpx) (0 ms)
hwprobe av1.decode absent AV1Decoder decode par libdav1d, logiciel (0 ms)
hwprobe scale ok 160x120, mire juste (6 ms)
hwprobe mosaic ok slot GPU, slot CPU, lisere et fond justes (29 ms)
```

`mediaserver --hwprobe` lancé à la main par un exploitant affiche ces mêmes
lignes. C'est l'outil de diagnostic d'un poste suspect.

## 7. Ce qui est publié

**Log de démarrage** : un bloc qui reprend chaque verdict, puis une ligne de
synthèse, par exemple :
« GPU actif pour : h264.encode, h264.decode (42e01f, 4d001f, 64001f), scale,
mosaic ».

**`/status/general`** : `hardware` gagne un objet `probe`. Chaque capacité y
porte son état et son motif.

```json
"hardware": {
  "vaapi": true,
  "probe": {
    "device":      { "state": "ok" },
    "h264.encode": { "state": "ok" },
    "h264.decode": { "state": "ok", "profiles": ["42e01f", "4d001f", "64001f"] },
    "vp8.encode":  { "state": "absent" },
    "mosaic":      { "state": "echec", "reason": "composition retombee sur le CPU" }
  },
  "videoEncoders": 0
}
```

`vaapi` garde son nom et change de sens. Il vaut `true` si le device est `ok`
**et** qu'au moins une capacité média l'est. Un device qui s'ouvre sans rien
savoir faire donne `false`. Les consommateurs actuels de `vaapi` (elixip) lisent
donc un booléen plus honnête, sans changer leur code. `docs/reference/status-http.md`
§3.7 et §3.8 sont à réécrire en conséquence.

## 8. Options et prérequis

| Option | Effet |
|---|---|
| `--no-hwaccel` | pas de sonde, pas de GPU (inchangé) |
| `--hwprobe` | mode sonde : teste, écrit ses lignes, sort. Ne démarre aucun serveur (lot 0, fait) |
| `--hwprobe-timeout <s>` | délai global de la sonde, 10 s par défaut (lot 1) |

**ffmpeg 8 au minimum.** Le graphe de mosaïque GPU a besoin de l'API segment de
libavfilter : `hwupload` exige son device avant son initialisation. Bâti contre
libavfilter < 11, `main()` éteint le GPU et le dit dans le log. La sonde rend
alors `absent` partout.

## 9. Coût et limites

**Coût mesuré** : 0,35 s pour tout le processus de sonde sur un Iris Xe, dont
environ 220 ms de tests. `h264.encode` est le plus lent (93 ms). Sans GPU, la
sonde rend la main tout de suite.

**Un profil que le driver ne décode pas est un `echec`, pas un `absent`.** Le
lot 0 ne sait pas distinguer « le driver ne propose pas ce codec » de « il le
propose et échoue » : dans les deux cas, libavcodec passe en logiciel sans le
dire. Seule une interrogation de libva (`vaQueryConfigProfiles`) ferait la
différence (§11).

**Capacité en charge** : hors sujet de la sonde. Le harnais de parallélisme
(N mixeurs, sources H.264 GPU, mosaïque GPU, retaillage, encodage de sortie,
vérification pixel) répond à cette question. Il a tenu 8 mixeurs (40 encodeurs
et 40 décodeurs GPU) sans erreur sur un Iris Xe, au débit total d'environ
40 compositions par seconde. Il pourrait devenir une cible `make check-hwaccel`,
hors `make check`.

## 10. Découpage en lots

| Lot | Contenu | Livrable vérifiable |
|---|---|---|
| 0 — **fait** | Mode `--hwprobe` : capacités du §3, lignes du §6, sans effet sur le serveur | `mediaserver --hwprobe` sur ce poste, puis `LIBVA_DRIVER_NAME=aucun` ; tests `HwProbe.*` |
| 1 | Lancement par `main()`, délai, signal, lecture partielle | test : processus de sonde forcé à `abort()` en pleine capacité (variable d'environnement de test) |
| 2 | Point de consultation libmedikit + application du §5 | tests : verdict injecté → codec logiciel, sans GPU |
| 3 | Compteurs `VideoAccel` redéfinis (§5) | test : décodeur dont `get_format` échoue → compté logiciel, un repli compté |
| 4 | Publication : log, `/status/general`, réécriture de `status-http.md` | test `test_status.cpp` sur l'objet `probe` |

Chaque test qui demande un GPU est préfixé `DISABLED_` et joué par
`--gtest_also_run_disabled_tests`, comme `H264HwVaapi`. Chaque test qui
simule un verdict tourne partout, dans `make check`.

Tests du lot 0 (`mcu/tests/test_hwprobe.cpp`) :

- `HwProbe.ChaqueCapaciteRecoitUnVerdict` tourne partout : chaque capacité
  reçoit un verdict et une ligne bien formée, et tout est `absent` sans device ;
- `HwProbe.DISABLED_ToutCeQueLeServeurUtiliseEstProuve` exige un GPU : device,
  transferts, encodage H.264, décodage `42e01f` et `42801F`, retaillage et
  mosaïque doivent être `ok`.

Contre-épreuves faites en réintroduisant les défauts corrigés sur la branche :

| Défaut réintroduit | Verdict de la sonde |
|---|---|
| ancien compositeur (device posé après le parse) | `mosaic echec composition retombee sur le CPU` |
| décodeur sans tolérance de profil | `h264.decode.42801F echec 0/20 images sur GPU` |
| surfaces en YUV420P | le processus de sonde meurt sur `pic->nb_dpb_pics < 16` pendant `h264.encode` ; les trois lignes déjà écrites restent lisibles |

La dernière ligne montre pourquoi la sonde doit tourner hors du processus
serveur (ADR 002) : dans le serveur, ce même abort aurait arrêté le service.

## 11. Questions ouvertes

1. **Resonder en cours de vie ?** Après une mise à jour du driver, ou après N
   replis pendant l'exécution. Proposition : non, un redémarrage suffit, et la
   sonde reste un événement de démarrage.
2. **Délai par défaut.** 10 s est une borne prudente pour 0,35 s mesurées.
   Proposition : 5 s, qui laisse de la marge à un poste plus lent.
3. **`absent` ou `echec` pour un codec que le driver ne propose pas ?**
   Interroger libva demanderait de lier `libva` directement. Proposition : s'en
   passer tant qu'aucun poste de production ne le demande.
