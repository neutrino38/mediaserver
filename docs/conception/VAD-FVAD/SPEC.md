# La VAD sur libfvad, en sous-module

> Statut : **à faire**. Rien n'est commencé. Arbitrage du 2026-09-21.
> Aucune branche ouverte.

## 1. Objectif

Remplacer `webrtc-audio-processing` par **libfvad** pour la détection de
voix (VAD, *voice activity detection*).

libfvad est un fork autonome du seul moteur VAD de WebRTC. Il est publié sous
licence BSD, à l'adresse `https://github.com/dpirch/libfvad`.

Trois gains attendus :

1. Le réglage d'agressivité redevient effectif sur Debian et Ubuntu.
2. Le mediaserver perd une dépendance système, et les deux branches `#ifdef`
   qui vont avec.
3. Le coût CPU baisse : une décision binaire ne demande plus de faire tourner
   toute la chaîne de traitement audio.

## 2. L'état actuel

La VAD sert à **désigner qui parle**, pas à couper de l'audio :

| Étape | Fichier | Ce qui se passe |
|---|---|---|
| Décision | `mcu/src/vad.cpp:52` | `CalcVad()` découpe en trames de 10 ms et rend 0 ou 1 |
| Accumulation | `mcu/src/pipeaudiooutput.cpp:79` | le 0/1 est multiplié par le nombre d'échantillons, puis cumulé |
| Usage | `mcu/src/audiomixer.cpp:152`, `mcu/src/videomixer.cpp:193` | le cumul choisit le locuteur affiché en mosaïque |

`mcu/src/vad.cpp` est le **seul** consommateur de `webrtc-audio-processing`
dans tout le dépôt.

Deux APM coexistent, et leurs API diffèrent (`mcu/Makefile:109-119`) :

- APM 0.3 sur AlmaLinux 9 : `voice_detection()->set_likelihood()` fonctionne.
- APM 1.x sur Debian et Ubuntu : le réglage d'agressivité **n'existe plus**.
  `VAD::SetMode()` n'y a aucun effet (`mcu/src/vad.cpp:117`).

Sur l'APM 1.x, obtenir un booléen coûte un `ProcessStream()` complet par
trame de 10 ms et par participant. Le flux traité est écrit dans un tampon,
puis jeté (`mcu/src/vad.cpp:80`).

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

Un seuil d'énergie ne demande d'ailleurs pas ffmpeg : c'est un calcul de
RMS en vingt lignes. Il donnerait la fenêtre principale au participant le
plus bruyant, pas à celui qui parle. C'est une régression fonctionnelle.

**Règle qui en découle : ne pas rouvrir la piste ffmpeg pour la VAD.**

## 4. Les options, et celle qui est retenue

| Option | Verdict |
|---|---|
| Garder l'APM | agressivité perdue sur Debian, deux `#ifdef` à maintenir, coût CPU inutile |
| VAD sur ffmpeg | régression fonctionnelle, voir §3 |
| Lier l'API bas niveau `WebRtcVad_*` | les symboles sont exportés par `libwebrtc-audio-processing-1.so.3`, mais **l'en-tête n'est pas installé** par le paquet `-dev`. Il faudrait déclarer les prototypes soi-même, donc s'appuyer sur un contrat que l'amont ne tient pas |
| Copier libfvad dans l'arbre | perd la frontière de licence et le chemin de merge upstream |
| **Sous-module libfvad** | **retenu** |

Ce qui tranche pour le sous-module n'est pas la taille du code, qui plaiderait
plutôt pour une copie. C'est la **provenance**. libfvad est du code WebRTC
dérivé, avec sa licence BSD et son fichier `PATENTS` propres. Il documente un
chemin de merge amont (branches `upstream-import` et `upstream-renamed`,
script `tools/import.sh`) qui n'existe que dans un clone git. Une copie
brouille ces trois choses. Un sous-module les garde nettes, et une mise à jour
devient un changement de SHA, visible en revue.

Le coût accepté : une étape réseau de plus au build, et une cible de nettoyage
de plus.

## 5. Ce que porte libfvad

Relevé sur le commit `532ab66` du 2024-02-07, dernier à ce jour :

- 12 fichiers `.c`, 2562 lignes en tout.
- autotools et CMake, plus un `libfvad.pc.in` : le projet sait produire un
  fichier pkg-config.
- Aucune source n'inclut de `config.h` généré. Les 12 `.c` se compilent sans
  `configure`.

L'API correspond terme à terme à la classe `VAD` existante :

| `mcu/include/vad.h` | libfvad | Remarque |
|---|---|---|
| constructeur, destructeur | `fvad_new()`, `fvad_free()` | — |
| `SetMode(Mode)` | `fvad_set_mode(inst, 0..3)` | mêmes valeurs **et** mêmes noms que l'enum `VAD::Mode` : 0 quality, 1 low bitrate, 2 aggressive, 3 very aggressive |
| `IsRateSupported(rate)` | `fvad_set_sample_rate()` | accepte 8000, 16000, 32000 et 48000 Hz |
| `CalcVad(frame, size, rate)` | `fvad_process(inst, frame, length)` | trames de 10, 20 ou 30 ms : la boucle de 10 ms existante convient |

Le fork IVèS doit être créé avant tout le reste, et épinglé sur `532ab66`.
Les deux sous-modules actuels pointent sur l'organisation IVèS de GitHub, et
non sur un amont tiers. La raison est concrète : `%prep` du spec RPM clone les
sous-modules, donc une release dépendrait sinon de la disponibilité du dépôt
d'un tiers.

## 6. Le plan, par lots

**Lot 0 — le fork.** Créer `InteractiviteVideoEtSystemes/libfvad` à partir de
`dpirch/libfvad`, épinglé sur `532ab66`.

**Lot 1 — le sous-module.** L'ajouter en `third_party/libfvad`. Le bâtir
in-tree, sur le modèle de `compile_libbfcp` (`install.ksh:482`) :

- `install.ksh:391` initialise déjà les sous-modules : y ajouter le test de
  présence de libfvad ;
- une fonction `compile_libfvad` : `autoreconf -i` si `configure` est absent,
  puis `./configure --disable-shared`, puis `make` ;
- le nettoyage de `install.ksh:256` ;
- `%prep` du spec.

Le `configure` amont installe `libfvad.pc`. Le consommer par
`PKG_CONFIG_PATH`, comme `mcu/Makefile` le fait déjà pour `libmedkit.pc`.
Ne pas ajouter de Makefile maison au fork : ce serait une divergence à
reporter à chaque merge amont.

**Lot 2 — le code.** Réécrire `mcu/src/vad.cpp` et `mcu/include/vad.h` sur
libfvad. Une seule implémentation. Les `#ifdef WEBRTC_APM_1` disparaissent.
`SetMode()` redevient effectif partout.

**Lot 3 — le retrait.** Sortir `webrtc-audio-processing` de :

- `install.ksh:302` (`RPM_PREREQ`) et `install.ksh:303` (`DEB_PREREQ`) ;
- `mcumediaserver.spec:12` et `mcumediaserver.spec:18` ;
- `mcu/Makefile:109-119`.

Rien à faire côté `.deb` : son `Depends:` est calculé depuis le binaire
(`install.ksh:150`).

Supprimer aussi le contournement documenté en `mcu/Makefile:346`. La suite de
tests évite `-lgtest_main` parce que `libwebrtc_audio_processing.so` exporte
un symbole `main` parasite. Retirer la bibliothèque retire la cause. Vérifier
d'abord que `tests/test_env.cpp` reste nécessaire pour d'autres raisons.

**Lot 4 — la recette.** Aucun test ne couvre la VAD aujourd'hui. Ajouter un
`mcu/tests/test_vad.cpp` : silence numérique rendu non voisé, une salve de
bruit blanc, l'effet réel de `SetMode()` sur un signal limite, et le
comportement à chacune des quatre fréquences. Puis une conférence à trois en
appel réel, pour vérifier que la mosaïque suit bien le locuteur.

## 7. Le piège à traiter au passage

`mcu/include/vad.h:47` déclare 48 kHz non supporté. `mcu/src/vad.cpp:60`
l'accepte. Les deux se contredisent.

Conséquence actuelle : à 48 kHz, `mcu/src/pipeaudiooutput.cpp:77` n'appelle
jamais la VAD. libfvad accepte les quatre fréquences. Aligner les deux
fonctions sur la même liste, et couvrir 48 kHz par un test.
