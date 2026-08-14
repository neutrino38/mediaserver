# Plan — Tests automatisés pour libmedikit (gtest)

## Objectif

Doter le sous-module `libmedikit` (`third_party/fontventa/libmedikit`) d'une
suite de tests automatisés, dans un répertoire **`libmedikit/tests/` bien
séparé**, en reprenant les harnais de test existants et en s'appuyant sur
**GoogleTest**.

## Décisions actées (validées avec le mainteneur)

- **Système de build : Makefile.** Une cible `tests` est ajoutée au
  `libmedikit/Makefile` existant, sur le modèle des cibles `negotest` /
  `ffmp4probe` déjà présentes. Pas de CMake (cohérence avec tout le repo).
- **gtest : paquet système.** gtest 1.11.0 est déjà installé
  (`/usr/include/gtest`, `/usr/lib64/libgtest.so`, `pkg-config gtest`). On lie
  en dynamique avec `-lgtest -lgtest_main` (+ `pkg-config --cflags/--libs
  gtest`). Cohérent avec la politique « paquets système » du port AlmaLinux 9.
- **Périmètre : large d'emblée.** Noyau pur (négociateur, descripteurs,
  parseurs) **plus** tests d'E/S MP4 : lecture d'une **fixture réelle**
  (`Mp4FfReader` sur `record.mp4`) *et* round-trip `mp4writer` → `Mp4FfReader`.
- **Fixture MP4 : `record.mp4`** (~563 Ko), **versionnée dans le dépôt** à
  `tests/fixtures/record.mp4` (sous-module `fontventa`). Contenu de référence
  (via `ffprobe`) pour les assertions :

  | Piste | Codec | Détails | Frames | Durée |
  |---|---|---|---|---|
  | Audio | AAC (mp4a) | 48000 Hz, mono | 796 | 16.772 s |
  | Vidéo | H264 (avc1) | 640×480 | 191 | 16.664 s |
  | Data | rtp (hint) | — | 191 | 16.664 s |
  | Texte | mov_text (tx3g) | 384×60 | 7 | 14.526 s |

## État des lieux (harnais existants)

| Fichier | Cible Makefile | Contenu | Réutilisation |
|---|---|---|---|
| `tools/negotest.cpp` | `negotest` | Négociateur `CodecNegotiator::Negotiate`, **déjà des assertions** `Check()` PASS/FAIL + code retour, 8 cas | → `test_negotiator.cpp` |
| `testsps.cpp` (racine) | commentée | Décodage `H264SeqParameterSet`, sans assertion (juste `Dump`) | → `test_h264_sps.cpp` (avec asserts) |
| `tools/ffmp4probe.cpp` | `ffmp4probe` | `Mp4FfReader` sur un `.mp4` réel (probe CLI) | conservé comme **outil CLI** ; sa logique inspire `test_mp4_roundtrip.cpp` |

Ces cibles restent buildables (outils de debug manuels) ; les tests gtest
viennent en plus, ils ne les remplacent pas.

## Surface testable (headers publics `medkit/`, sans Asterisk)

Testable en pur (aucune E/S) : `negotiator`, `avcdescriptor`, `bitstream` /
H264 SPS, `text.h` (`UTF8Parser`), `text2subtitle` (`Text2Subtitle`), `red`
(`RTPRedundantPayload`), `codecs` (`IsSupported`/`GetSupportedCodecs`).
Testable en E/S round-trip : `mp4writer` + `ffmp4reader`.
**Hors périmètre** (dépendent des en-têtes `<asterisk/...>`) : `transcoder`,
`mp4format`, `framebuffer`, `frameutils`, `astlog` — la lib de test se
construit contre `libmedkit.a` bâtie **`ASTERISK=no`**.

## Arborescence proposée

```
libmedikit/tests/
  README.md                 # comment lancer, conventions
  test_env.cpp              # ::testing::Environment global : SetLogFunctions()
  test_negotiator.cpp       # migration de negotest (8 cas -> TEST())
  test_h264_sps.cpp         # migration de testsps (avec EXPECT_*)
  test_avcdescriptor.cpp    # round-trip Serialize/Parse + getters
  test_utf8parser.cpp       # SetWString<->GetWString, tailles UTF-8 multi-octets
  test_text2subtitle.cpp    # Accumulate + GetSubtitle + défilement/historique
  test_red.cpp              # ParseRed sur paquet RED forgé + getters
  test_mp4_read.cpp         # Mp4FfReader sur la fixture record.mp4 (lecture)
  test_mp4_roundtrip.cpp    # mp4writer -> MP4Close -> Mp4FfReader (sans fixture)
  fixtures/
    record.mp4              # fixture versionnée (AAC/H264/mov_text, ~563 Ko)
```

Artefacts de build (`runtests`, `*.o`, MP4 temporaires du round-trip) : **non
commités** (ajout au `.gitignore` du sous-module). La fixture
`tests/fixtures/record.mp4` est en revanche **versionnée** ; son chemin absolu
est injecté au build (voir ci-dessous). Un `GTEST_SKIP()` défensif reste en
place si le fichier venait à manquer.

## Cible Makefile (esquisse)

```make
# Variables gtest via pkg-config (paquet système)
GTEST_CFLAGS := $(shell pkg-config --cflags gtest)
GTEST_LIBS   := $(shell pkg-config --libs gtest) -lgtest_main

TEST_SRCS := $(wildcard tests/*.cpp)

# Chemin de la fixture MP4 (versionnée dans tests/fixtures/), surchargeable :
#   make check TEST_MP4=/autre/chemin/record.mp4
TEST_MP4 ?= $(abspath tests/fixtures/record.mp4)

# Lie contre libmedkit.a (bâtie ASTERISK=no) + ffmpeg + mp4v2/gsm, comme negotest.
tests/runtests: $(TEST_SRCS) libmedkit.a
	$(CXX) $(CXXFLAGS) $(GTEST_CFLAGS) -DTEST_MP4_FILE='"$(TEST_MP4)"' \
		$(TEST_SRCS) libmedkit.a -o $@ \
		-L../../../staticdeps/lib $(LDFLAGS) -lmp4v2 -lgsm $(GTEST_LIBS)

# Cible pratique : build + exécution
check: tests/runtests
	./tests/runtests
```

`all:` reste inchangé (`libmedkit.a` uniquement) ; `tests`/`check` sont
opt-in. `clean` retire aussi `tests/runtests` et `tests/*.o`.

## Phasage

- **Phase 0 — squelette & chaîne de build.**
  `tests/test_env.cpp` (Environment global appelant `SetLogFunctions`) + un
  `TEST(Smoke, Compiles)` trivial. Cible `tests/runtests` + `check` dans le
  Makefile. But : valider compile/link/exécution gtest contre `libmedkit.a`.

- **Phase 1 — migration des harnais existants.**
  - `negotest.cpp` → `test_negotiator.cpp` : les 8 `Check()` deviennent des
    `TEST()` avec `EXPECT_EQ`/`EXPECT_TRUE` (contrat `fmtpByPt` : présence =
    accepté, `""` = sans fmtp, absence = filtré, `telephone-event`, `T140RED`).
  - `testsps.cpp` → `test_h264_sps.cpp` : asserts sur profil/niveau/dimensions
    décodés au lieu de `Dump()`.

- **Phase 2 — tests unitaires purs (nouveaux).**
  - `avcdescriptor` : `AddSequenceParameterSet`/`AddPictureParameterSet` →
    `Serialize` → `Parse` d'un nouvel objet → égalité des SPS/PPS et des
    champs profil/niveau (round-trip).
  - `utf8parser` : `SetWString` → `GetUTF8Size`/`GetWString`, caractères
    multi-octets (accents, emoji) ; cohérence taille annoncée / réelle.
  - `text2subtitle` : `Accumulate` de lignes → `GetSubtitle`/`GetCurrentLine`,
    défilement et passage en historique.
  - `red` : construire un payload RED (primary + N redondances) octet par
    octet, `ParseRed`, vérifier `GetPrimary*` et `GetRedundant*`.

- **Phase 3 — E/S MP4.**
  - `test_mp4_read.cpp` (**fixture réelle `record.mp4`**, cœur de la phase, sur
    le modèle de `ffmp4probe.cpp`) : `Mp4FfReader("...record.mp4")`,
    `IsOpen()`, `OpenTrack` audio/vidéo/texte, puis asserts sur le contenu de
    référence — `HasAudioTrack`/`HasVideoTrack`/`HasTextTrack`,
    `GetDuration()` ≈ 16.77 s (tolérance), vidéo H264 640×480, audio AAC 48 kHz
    (via `OpenAudioTranscoded`/liste incluant AAC), piste texte `mov_text`
    présente ; boucle `GetNextFrame` jusqu'à `Eof()` puis `Rewind()`. Le test
    `GTEST_SKIP()` si `TEST_MP4_FILE` est absent.
  - `test_mp4_roundtrip.cpp` (**sans fixture externe**, complémentaire) :
    `MP4Create` (mp4v2, fichier temporaire) → `mp4writer` `AddTrack(PCMU)` +
    `AddTrack(H264)` → `ProcessFrame` sur des `MediaFrame` synthétiques →
    `MP4Close` → `Mp4FfReader` relit → asserts pistes/durée/codecs/nb frames.
    Fichier temp créé/supprimé par le test.

- **Phase 4 — intégration & doc.**
  `tests/README.md`, `.gitignore`, mention de `make check` dans `CLAUDE.md`
  (section Build & run / « There is no automated test suite » à réviser),
  éventuel appui de `install.ksh` (cible optionnelle, non bloquante pour le
  build de prod).

## Points d'attention / pièges

- **`SetLogFunctions` obligatoire** : `libmedkit` appelle `Log()`/`Error()` ;
  sans initialisation des callbacks (comme le font `negotest`/`testsps`), risque
  de segfault. On l'installe une fois via un `::testing::Environment` global
  (`test_env.cpp`), pas dans chaque test.
- **`gtest_main` fournit `main()`** → aucun `main()` à écrire ; l'Environment
  global est enregistré via `AddGlobalTestEnvironment` dans un initialiseur
  statique.
- **`ASTERISK=no`** : la lib de test se lie contre `libmedkit.a` sans les objets
  Asterisk. `install.ksh localcompile` bâtit déjà `libmedkit` en `ASTERISK=no`.
- **Le Makefile ne suit pas les dépendances d'en-têtes** → `rm *.o` (ou
  `make clean`) avant rebuild si un header change (piège connu du repo).
- **Fixture `record.mp4` versionnée** (`tests/fixtures/record.mp4`, sous-module
  `fontventa`, ~563 Ko) : chemin absolu injecté à la compilation
  (`-DTEST_MP4_FILE`, défaut `$(abspath tests/fixtures/record.mp4)`,
  surchargeable via `make check TEST_MP4=...`) ; `GTEST_SKIP()` défensif si
  absente. La fixture du round-trip est **générée à la volée**, jamais
  commitée ; nettoyage en fin de test (`TearDown`).
- **AAC en lecture** : la liste de codecs audio de `ffmp4probe` n'inclut pas
  AAC ; le test de lecture doit ajouter `AudioCodec::AAC` à la liste
  `OpenTrack` (ou utiliser `OpenAudioTranscoded`) pour la piste audio de
  `record.mp4`.
- **Fins de ligne CRLF** : garder LF (piège documenté du repo).

## Anomalie fixture record.mp4 (lecture vidéo) — INVESTIGUÉE

`Mp4FfReader::GetNextFrame` sur `record.mp4` échoue (`errcode=-5`) dès la 1ʳᵉ
trame vidéo : `VideoFrame::PacketizeH264` refuse (`false`). Investigation
(instrumentation temporaire de `PacketizeH264`, retirée depuis) :
- le flux H264 de `record.mp4` est **incohérent** — SPS/PPS dupliqués (préfixe
  `videoParamsAvcc` + SPS/PPS déjà présents dans le paquet), puis une taille de
  NAL aberrante (`00 00 00 01` lu comme longueur AVCC = 1, puis `naluSz` géant) ;
- **ffmpeg lui-même rejette ce flux** à l'ouverture (« Invalid NAL unit size »,
  « Error splitting the input into NAL units ») ;
- les autres MP4 (`titi.mp4`, `toto.mp4`, `OLD_*.mp4`) se lisent correctement
  (1ʳᵉ intra OK) → le reader et `Packetize` fonctionnent ; `Packetize` **protège
  correctement** en refusant une taille de NAL invalide.

**Conclusion : ce n'est pas un bug du reader — `record.mp4` a un flux vidéo
défectueux.** `test_mp4_read.cpp` teste donc les métadonnées (lisibles malgré le
flux) et le contrat audio AAC, pas la boucle de lecture vidéo. Le round-trip
(Phase 3) valide la lecture vidéo sur des trames H264 **bien formées** produites
par la lib elle-même.

## Dette / nettoyage

- **[FAIT 2026-07-17] Doublon mort `mcu/src/red.cpp` + `mcu/include/red.h`
  supprimé** (`git rm`). Vérifié : `red.o` absent des `OBJS` de
  `mcu/Makefile` (seul `redcodec.o` compilé) ; `mcu/include/red.h` n'était
  qu'un shim `#include "medkit/red.h"` inclus par le seul `red.cpp` non compilé.
  Le mcu consomme la version libmedikit (`medkit/red.h`). mcu recompilé vert via
  `./install.ksh localcompile` (piège : `make -f mcu/Makefile` doit tourner
  depuis `mcu/`, pas depuis la racine).
- **[FAIT 2026-07-17] Cible Makefile `ffmp4probe`** : ajout de
  `-L../../../staticdeps/lib` (comme `negotest`) → compile de nouveau.
- **[FAIT 2026-07-17] Assert mp4v2 `AddDescendantAtoms (mp4file.cpp,705)`** :
  causé par l'ordre `MP4Close(mp4)` **avant** destruction du `mp4writer` (dont le
  destructeur écrit encore via `MP4TagsStore`). Corrigé dans
  `test_mp4_roundtrip.cpp` en encadrant le `mp4writer` dans un bloc pour qu'il
  soit détruit avant `MP4Close`. Plus d'assert.

## Hors périmètre (assumé)

- Modules couplés Asterisk (`transcoder`, `mp4format`, `framebuffer`).
- `framescaler` / `logo` (traitement d'image lourd) — candidats à une itération
  ultérieure si besoin.
- CI : aucun pipeline n'est versionné dans le repo ; `make check` reste manuel.
```