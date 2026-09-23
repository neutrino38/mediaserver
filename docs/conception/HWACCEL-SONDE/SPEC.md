# Sonde d'accélération matérielle au démarrage

> Statut : **lots 0 à 2 faits** — le mode `mediaserver --hwprobe`, son
> lancement par `main()` à chaque démarrage, et l'application des verdicts
> (§5, §6, §10). Les lots 3 et 4 sont à faire : les compteurs `VideoAccel` et
> `/status/general` ne reflètent pas encore les verdicts.
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
| `device`, `upload` ou `download` en échec | `Pict::DisableVAAPI()` : tout le processus passe en CPU |
| `h264.encode` en échec | les encodeurs H.264 s'ouvrent en logiciel |
| `h264.decode.<profil>` en échec | les flux de ce profil se décodent en logiciel |
| `h264.decode` en échec (aucun profil `ok`) | tout décodage H.264 se fait en logiciel |
| `vp8.decode` en échec | tout décodage VP8 se fait en logiciel |
| `scale` en échec | `VideoRescaler` redescend la trame GPU avant de la retailler |
| `mosaic` en échec | `Mosaic::BuildDesc` ne demande plus le graphe GPU |

`ApplyHwProbe` (mcu) traduit les verdicts. Il éteint le GPU dès qu'un des trois
premiers échoue. Sinon, il appelle `VideoAccel::RefuseHw` pour chaque capacité
en échec. Les capacités `non testee` après une mort de la sonde sont en échec,
donc refusées : c'est le cas prudent.

**Le point de consultation est une liste de refus, pas de preuves.** Ce qui n'a
pas été refusé reste tenté, comme avant la sonde. Trois raisons :

- sans sonde (mode `--hwprobe` lui-même, tests, autre appelant de libmedikit),
  le comportement ne change pas ;
- un profil que la sonde ne teste pas (High 10, par exemple) reste confié à
  ffmpeg, au lieu d'être refusé d'office ;
- un refus ne peut venir que d'un échec observé.

Les clés sont celles de libmedikit, pas celles de la sonde :
`<codec>.encode`, `<codec>.decode`, `<codec>.decode.<profil>` (noms
libavcodec, profil en minuscules et espaces en « _ », ex.
`h264.decode.baseline`), `scale`, `mosaic`. `ApplyHwProbe` traduit
`h264.decode.42801F` en `h264.decode.baseline`.

Où chaque refus est lu :

| Clé | Lue par | Moment |
|---|---|---|
| `<codec>.encode` | `FfVideoEncoder::SelectCodec` | choix de l'encodeur |
| `<codec>.decode` | constructeur de `FfVideoDecoder` | ouverture du décodeur |
| `<codec>.decode.<profil>` | `GetVAAPIFormat`, le `get_format` du décodeur | à la lecture du SPS : c'est le seul moment où le profil est connu |
| `scale` | `VideoRescaler::Run` | avant de configurer le graphe |
| `mosaic` | `Mosaic::BuildDesc` | calcul de `wantGPU` |

Deux conséquences :

- **Un refus n'est pas un repli.** Il ne compte pas dans `hwFallbacks` :
  c'est une décision connue, publiée par la sonde. Sinon le compteur monterait
  à chaque appel.
- **Un codec qui exige le matériel échoue** s'il est refusé
  (`video.hwaccel.required=1`, `H264Decoder(true)`) : l'exigence ne peut pas
  être tenue.

Les compteurs `VideoAccel` changent aussi de définition (lot 3). Un décodeur
n'est compté matériel que lorsqu'il a rendu une surface. Un décodeur qui passe
en logiciel faute de profil compte un repli. Sans cela, le compteur continue de
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

Ce qui est livré (lots 0 à 2) :

- `mediaserver --hwprobe` juge, écrit ses lignes, rend 0 et sort sans démarrer
  de serveur. Il respecte `--no-hwaccel` et la garde ffmpeg 8 : dans les deux
  cas, tout est `absent`.
- `main()` lance la sonde (`RunHwProbeChild`) **avant** d'ouvrir lui-même le
  device. Il ne la lance pas avec `--no-hwaccel`, ni bâti contre libavfilter
  < 11, ni en mode `--hwprobe`.
- Le père complète les verdicts dans l'ordre du §3. Le motif de la capacité en
  cours dit la cause : `sonde tuee par le signal 6 (Aborted)`, `delai depasse
  (10 s)`, `sonde sortie avec le code 127` (le binaire n'a pas pu être relancé)
  ou `verdict manquant`. Les capacités suivantes portent `non testee`.
- Chaque verdict est journalisé (`-hwprobe <capacité> <état> <détail>`), en
  erreur s'il est en échec. Quand la sonde s'arrête avant la fin, le père
  journalise aussi les 5 dernières lignes de l'enfant qui ne sont pas des
  verdicts : c'est là qu'apparaît l'assertion de libavcodec.
- Le père applique ensuite les verdicts (§5), **avant** d'ouvrir lui-même le
  device. Une sonde tuée à l'ouverture du device donne `device` en échec : le
  père éteint le GPU sans jamais l'ouvrir, et démarre en CPU.

Pour les tests, `MCU_HWPROBE_FAULT=abort:<capacité>` fait mourir la sonde sur
`abort()` juste avant de juger cette capacité, et `hang:<capacité>` la bloque.
Le crochet agit même sans GPU : les tests du lot 1 tournent partout.

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
| `--hwprobe-timeout <s>` | délai global de la sonde, 10 s par défaut (lot 1, fait) |

**ffmpeg 8 au minimum.** Le graphe de mosaïque GPU a besoin de l'API segment de
libavfilter : `hwupload` exige son device avant son initialisation. Bâti contre
libavfilter < 11, `main()` éteint le GPU et le dit dans le log. La sonde rend
alors `absent` partout.

## 9. Coût et limites

**Coût mesuré** : 0,35 s pour tout le processus de sonde sur un Iris Xe, dont
environ 220 ms de tests. `h264.encode` est le plus lent (93 ms). Sans GPU, la
sonde rend la main tout de suite. Au démarrage du serveur, la sonde ajoute
environ 0,23 s : le serveur répond en 0,35 s, contre 0,13 s avec `--no-hwaccel`.

**Une sonde tuée coûte une seconde de plus sur Ubuntu.** Le noyau passe le core
à apport (`core_pattern` est un pipe) même quand la limite de core vaut 0.
apport n'écrit rien pour un binaire hors paquet, mais il met environ une
seconde à le décider.

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
| 1 — **fait** | Lancement par `main()`, délai, signal, lecture partielle | tests `HwProbeChild.*`, sonde forcée à `abort()` ou bloquée par `MCU_HWPROBE_FAULT` |
| 2 — **fait** | Point de consultation libmedikit + application du §5 | tests `HwProbeApply.*` (partout), `HwRefusal.*` (libmedikit) et `MosaicCompositorGpu.UneMosaiqueRefuseeNeDemandePlusLeGpu` |
| 3 | Compteurs `VideoAccel` redéfinis (§5) | test : décodeur dont `get_format` échoue → compté logiciel, un repli compté |
| 4 | Publication : log, `/status/general`, réécriture de `status-http.md` | test `test_status.cpp` sur l'objet `probe` |

Chaque test qui demande un GPU est préfixé `DISABLED_` et joué par
`--gtest_also_run_disabled_tests`, comme `H264HwVaapi`. Chaque test qui
simule un verdict tourne partout, dans `make check`.

Tests des lots 0 à 2 (`mcu/tests/test_hwprobe.cpp`). `runtests` accepte
`--hwprobe` comme le binaire serveur, pour que les tests relancent le vrai code
de sonde :

- `HwProbe.ChaqueCapaciteRecoitUnVerdict` : chaque capacité reçoit un verdict
  et une ligne bien formée, et tout est `absent` sans device ;
- `HwProbe.DISABLED_ToutCeQueLeServeurUtiliseEstProuve` exige un GPU : device,
  transferts, encodage H.264, décodage `42e01f` et `42801F`, retaillage et
  mosaïque doivent être `ok` ;
- `HwProbeChild.RendLesMemesVerdictsQueLaSonde` : la sonde relancée rend les
  mêmes états que la sonde dans le processus ;
- `HwProbeChild.UneSondeTueeGardeSesVerdictsEtNommeLaCause` : tuée pendant
  `h264.encode`, la sonde garde les trois verdicts déjà écrits, nomme le signal,
  et marque la suite `non testee` ;
- `HwProbeChild.UneSondeBloqueeEstTueeAuDelai` : bloquée pendant `scale`, elle
  est tuée à 3 s et le père rend la main ;
- `HwProbeApply.*` (partout) : un échec du device ou d'un transfert éteint le
  GPU ; tout autre échec ne refuse que son chemin, et `42801F` devient
  `h264.decode.baseline` ;
- `MosaicCompositorGpu.UneMosaiqueRefuseeNeDemandePlusLeGpu` (sauté sans GPU).

Côté libmedikit, `tests/test_hw_refusal.cpp` : un refus ne vaut que pour son
chemin (partout) ; encodeur, décodeur, profil et retaillage refusés passent en
logiciel (`HwRefusal.DISABLED_*`, GPU exigé). Chaque test GPU vérifie d'abord
que le chemin est matériel sans refus : il échoue aussi sur un poste où le GPU
ne servait pas de toute façon.

**Piège des tests en sous-processus.** Un refus est définitif : ces tests
tournent sous `EXPECT_EXIT`. Ils demandent le style `threadsafe`, qui relance le
binaire. Le style par défaut fait un `fork()` sans `exec`, et l'enfant d'un
processus qui a déjà touché au GPU hérite d'un état libva inutilisable : le
chemin GPU y échoue sans raison apparente.

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
