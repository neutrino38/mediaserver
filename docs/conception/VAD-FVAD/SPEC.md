# La VAD sur libfvad, en sous-module

> Statut : **fait**, sauf la recette en appel réel (§7).
> Branche `feat/remove-webrtc-vad`. Arbitrages du 2026-09-21 et du 2026-09-22.
>
> État courant de la VAD, pour qui veut seulement la comprendre :
> `docs/reference/vad.md`. Ce document-ci porte la conception et ce qu'elle a
> coûté.

## 1. Objectif

Remplacer `webrtc-audio-processing` par **libfvad** pour la détection de voix
(VAD, *voice activity detection*).

libfvad est le moteur VAD de WebRTC, extrait en bibliothèque C autonome, publié
sous licence BSD.

Trois gains attendus :

1. Le réglage d'agressivité redevient effectif sur Debian et Ubuntu.
2. Le mediaserver perd une dépendance système, et les deux branches `#ifdef`
   qui vont avec.
3. Le coût CPU baisse : une décision binaire ne demande plus de faire tourner
   toute la chaîne de traitement audio.

## 2. L'état de départ

La VAD sert à **désigner qui parle**, pas à couper de l'audio. `mcu/src/vad.cpp`
était le **seul** consommateur de `webrtc-audio-processing` dans tout le dépôt.

Deux APM coexistaient, et leurs API différaient :

- APM 0.3 sur AlmaLinux 9 : `voice_detection()->set_likelihood()` fonctionnait.
- APM 1.x sur Debian et Ubuntu : le réglage d'agressivité **n'existait plus**.
  `VAD::SetMode()` n'y avait aucun effet.

Sur l'APM 1.x, obtenir un booléen coûtait un `ProcessStream()` complet par trame
de 10 ms et par participant. Le flux traité était écrit dans un tampon, puis
jeté.

## 3. ffmpeg ne répond pas au besoin

C'est le fait qui ferme la porte à l'option la plus tentante. ffmpeg est déjà
lié au mediaserver : s'en servir pour la VAD n'ajouterait aucune dépendance.

Constats faits sur ffmpeg 8 (libavfilter 11.4.100) :

| Candidat | Ce que c'est | Pourquoi il ne convient pas |
|---|---|---|
| `silencedetect` | seuil d'énergie, métadonnées `lavfi.silence_start` / `silence_end` | ne distingue pas la voix du bruit |
| `silenceremove`, `speechnorm`, `astats` | traitement ou mesure d'énergie | même limite |
| `arnndn` (RNNoise) | débruiteur, exige un fichier modèle `.rnnn` externe | n'exporte aucune métadonnée de VAD |
| filtre `whisper` (Silero VAD) | le seul vrai VAD du projet ffmpeg | demande `--enable-whisper`, whisper.cpp et un modèle : aucune distribution ne l'active |
| DTX des codecs (Opus, AMR, G.729) | VAD interne à l'encodeur | non exposé par l'API ffmpeg, et il faudrait encoder pour l'obtenir |

Un seuil d'énergie ne demande d'ailleurs pas ffmpeg : c'est un calcul de RMS en
vingt lignes. Il donnerait la fenêtre principale au participant le plus bruyant,
pas à celui qui parle. C'est une régression fonctionnelle.

**Règle qui en découle : ne pas rouvrir la piste ffmpeg pour la VAD.**

## 4. Les options, et celle qui est retenue

| Option | Verdict |
|---|---|
| Garder l'APM | agressivité perdue sur Debian, deux `#ifdef` à maintenir, coût CPU inutile |
| VAD sur ffmpeg | régression fonctionnelle, voir §3 |
| Lier l'API bas niveau `WebRtcVad_*` | les symboles sont exportés par `libwebrtc-audio-processing-1.so.3`, mais **l'en-tête n'est pas installé** par le paquet `-dev`. Il faudrait déclarer les prototypes soi-même, donc s'appuyer sur un contrat que l'amont ne tient pas |
| Copier libfvad dans l'arbre | perd la frontière de licence |
| **Sous-module `aks-tel/libvad`** | **retenu** |

Le dépôt retenu n'est pas `dpirch/libfvad`, l'amont d'origine, mais
`aks-tel/libvad`, qui l'embarque. Décision du 2026-09-22, **sans fork IVèS**.

Le coût accepté tient en deux points, et ils sont réels :

- `%prep` du spec RPM clone les sous-modules. Une release dépend donc de la
  disponibilité d'un dépôt tiers. Le SHA épingle le contenu, pas la
  disponibilité.
- Le dépôt ne documente aucun chemin de merge amont. Une mise à jour se lit comme
  un changement de SHA en revue, et rien de plus.

## 5. Ce que porte `aks-tel/libvad`

Relevé sur le commit `dcfad77` (tag `v1.0`), et vérifié fichier par fichier
contre `dpirch/libfvad` au commit `532ab66`.

**Le cœur est identique.** Les 12 sources fvad sont octet pour octet celles de
l'amont. Une seule ligne diffère, dans `fvad.c` : `#include "fvad.h"` au lieu de
`"../include/fvad.h"`. `fvad.h` est identique.

Le dépôt **ajoute** `sivr-vad.c` / `sivr-vad.h`, une couche issue de la VAD
FreeSWITCH, et un `Makefile` écrit à la main qui produit `libsivrvad.a`.

Le dépôt **retire** `LICENSE`, `PATENTS`, `AUTHORS`, les autotools, CMake,
`libfvad.pc.in`, les tests et les exemples.

L'API correspond terme à terme à la classe `VAD` existante :

| `mcu/include/vad.h` | libfvad | Remarque |
|---|---|---|
| constructeur, destructeur | `fvad_new()`, `fvad_free()` | — |
| `SetMode(Mode)` | `fvad_set_mode(inst, 0..3)` | mêmes valeurs **et** mêmes noms que l'enum `VAD::Mode` |
| `IsRateSupported(rate)` | `fvad_set_sample_rate()` | accepte 8000, 16000, 32000 et 48000 Hz |
| `CalcVad(frame, size, rate)` | `fvad_process(inst, frame, length)` | trames de 10, 20 ou 30 ms : la boucle de 10 ms existante convient |

### Deux obstacles, et ce qu'on en a fait

**`sivr-vad.c` ne compile pas hors FreeSWITCH.** Il emploie `int16_t`, `malloc`,
`free`, `memset`, `strcmp` et `abs` sans inclure leurs en-têtes : c'est
`switch.h` qui les fournissait. Sans fork, on ne peut pas le corriger.

On ne le bâtit donc pas — et ce n'est pas qu'un contournement. Sa machine à
états rend un *start/stop talking* à hystérésis, pas le 0/1 par trame que
`PipeAudioOutput` cumule, et elle fige la fréquence à l'init alors que
`CalcVad()` la reçoit à chaque appel. Le mediaserver appelle `fvad_*` en direct,
comme le fait `sivr-vad` elle-même.

**Il manque la notice BSD.** Les en-têtes renvoient à `LICENSE`, `PATENTS` et
`AUTHORS`, absents du dépôt. Distribuer le binaire oblige à reproduire la notice.
Elle est donc reprise dans `LICENSE.libfvad`, à la racine, et installée par les
deux paquets.

## 6. Ce qui a été fait

**Lot 1 — le sous-module.** `third_party/libvad`, épinglé sur `dcfad77`.
`compile_libvad` dans `install.ksh`, sur le modèle de `compile_libbfcp` :
`install.ksh` initialise les sous-modules, bâtit l'archive in-tree, et la
nettoie. `%prep` du spec les initialise déjà tous.

Le `Makefile` amont est appelé avec trois variables surchargées en ligne de
commande, donc **sans écrire un fichier dans le sous-module**. `LIB_SRC` vaut
`$(FVAD_SRC)` : la variable amont, réévaluée chez lui, pas une liste recopiée.

**Lot 2 — le code.** `mcu/src/vad.cpp` et `mcu/include/vad.h` réécrits sur
libfvad. Une seule implémentation, les `#ifdef WEBRTC_APM_1` ont disparu, et
`SetMode()` redevient effectif partout. La fréquence est réécrite sur l'instance
quand elle change.

**Lot 3 — le retrait.** `webrtc-audio-processing` est sorti de `RPM_PREREQ`, de
`DEB_PREREQ`, du spec (`Requires` et `BuildRequires`) et de `mcu/Makefile`. Le
`.deb` calcule son `Depends:` depuis le binaire : rien à y faire.

Le contournement `-lgtest_main` a perdu sa cause : la suite évitait cette
bibliothèque parce que `libwebrtc_audio_processing.so` exportait un symbole
`main` parasite. `tests/test_env.cpp` garde son `main()`, qui installe
l'environnement global de la suite, mais la justification a été corrigée.

**Lot 4 — les tests.** `mcu/tests/test_vad.cpp`, 7 tests, aucun n'existait avant.
Deux d'entre eux ont été vérifiés par mutation du code de production : rendre
`SetMode()` sans effet fait tomber
`Vad.LeModeChangeLaDecisionSurUnSignalLimite` ; ne plus réécrire la fréquence en
fait tomber trois.

## 7. Ce qui reste

- **La recette en appel réel** : une conférence à trois, pour vérifier que la
  mosaïque suit bien le locuteur.
- **Le build complet** n'a pas pu être joué de bout en bout le 2026-09-22 : le
  sous-module `third_party/fontventa` était sorti sur un commit qui exige
  ffmpeg ≥ 6.1 (`AV_FRAME_FLAG_KEY`), alors que la machine porte ffmpeg 5.
  L'échec est antérieur et étranger à la VAD. Ont été vérifiés : la construction
  de `libfvad.a`, la compilation de `vad.o` par le vrai `Makefile`, et les 7
  tests joués contre `src/vad.cpp` et l'archive.
- **Signaler les deux défauts à `aks-tel`** : les en-têtes manquants de
  `sivr-vad.c`, et l'absence de `LICENSE` / `PATENTS` / `AUTHORS`.

## 8. Le piège traité au passage

`mcu/include/vad.h` déclarait 48 kHz non supporté ; `mcu/src/vad.cpp`
l'acceptait. Les deux se contredisaient, et la conséquence était silencieuse : à
48 kHz, `mcu/src/pipeaudiooutput.cpp` n'appelait jamais la VAD.

libfvad accepte les quatre fréquences. Les deux fonctions sont désormais alignées
sur la même liste, et `Vad.IsRateSupportedEtCalcVadSAccordent` les compare.
