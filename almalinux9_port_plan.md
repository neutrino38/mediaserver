# Plan de portage vers AlmaLinux 9

Document de préparation — branche `feat/alma_linux9`.

## Objectifs

1. **Lien dynamique sur les libs natives AlmaLinux 9** : abandonner la compilation
   statique « maison » (`install.ksh localcompile` → `staticdeps/`) au profit des
   paquets `-devel` de la distribution chaque fois que possible (ffmpeg, openssl,
   x264, libvpx, opus, speex, gsm, xmlrpc-c, ImageMagick, protobuf, rabbitmq-c…).
2. **Dédoublonnage du code média** : remplacer les fichiers de `mcu/src`
   redondants avec
   [`fontventa/libmedikit`](https://github.com/InteractiviteVideoEtSystemes/fontventa/tree/migration/almalinux_9/libmedikit)
   et intégrer ce dernier comme **sous-module git**.
3. **Portage compilateur/OS** : GCC 11, OpenSSL 3.0, ffmpeg 5/6, Python 3,
   détection `el9`, init systemd.

> Note : la migration vers libmedikit n'est pas qu'un nettoyage. La branche
> `migration/almalinux_9` de fontventa a **déjà** adapté ces primitives média à
> ffmpeg récent et OpenSSL 3 ; l'adopter résout en même temps une grande partie
> du portage des codecs. C'est le cœur du chantier, pas un à-côté.

---

## 1. État des lieux des dépendances

Aujourd'hui (`install.ksh local_compile` + `mcu/Makefile.rpm`), tout est compilé
en statique dans `staticdeps/` puis lié en `-Wl,-Bstatic`. Le mode
`FEWSTATICDEPS=yes` (prévu pour Fedora 20) montre déjà la voie du lien dynamique.

| Dépendance | Aujourd'hui | AlmaLinux 9 | Action |
|---|---|---|---|
| **OpenSSL** | source 1.1.1d statique | `openssl-devel` 3.0 (base) | Dynamique. **API 1.1.1→3.0 à porter** (cf. §4) |
| **ffmpeg** (avcodec/avformat/avutil/swscale/swresample/avfilter) | `ffmpeg-devel` (système, déjà dyn.) | **EPEL `ffmpeg-free-devel`** (décidé) | Dynamique. **API ffmpeg à porter** (cf. §4). ⚠️ sans codecs propriétaires (cf. §4 H.264) |
| **x264** | dynamique | RPM Fusion `x264-devel` | Dynamique (inchangé) |
| **libvpx** | source statique (`v1.7.0`) | AppStream `libvpx-devel` | Dynamique |
| **opus** | source statique (1.1) | AppStream `opus-devel` | Dynamique |
| **speex / speexdsp** | source statique | EPEL `speex-devel`, `speexdsp-devel` | Dynamique |
| **gsm** | dynamique | EPEL `gsm-devel` | Dynamique (inchangé) |
| **xmlrpc-c** | source statique | CRB/EPEL `xmlrpc-c-devel` | Dynamique |
| **ImageMagick (Magick++ 6)** | dynamique `Magick++-6.Q16` | EPEL `ImageMagick-c++-devel` (6.9) | Dynamique — **vérifier en-têtes `ImageMagick-6`** |
| **mp4v2** | source statique | pas de paquet natif | Repris par **libmedikit** (`mp4format`/`mp4track`) — à confirmer, sinon source |
| **libsrtp** | source statique (fork patché IVèS) | EPEL `libsrtp` existe mais **fork patché** | Garder build source (patch IVèS) ou rebaser le patch |
| **g722_1 (SIREN)** | source statique (fork IVèS) | pas de paquet | Garder build source |
| **fdk-aac** | statique (el5) | RPM Fusion nonfree | Repris par **libmedikit** (dossier `aac`) |
| **WebRTC VAD** | source (`vad`/`signal_processing`) | pas de paquet | Garder build source (Python 3 !) |
| **protobuf** (moteli) | source statique 2.5.0 | AppStream `protobuf-devel` 3.x | Dynamique. proto2 OK avec protoc 3 |
| **rabbitmq-c** (moteli) | source statique 0.3.0 | EPEL `librabbitmq-devel` | Dynamique |
| **libbfcp** | `/opt/ives` (lib interne IVèS) | recompiler pour el9 | Hors périmètre direct, mais prérequis |

**Restent en build source après portage** (pas de paquet natif satisfaisant) :
libsrtp (fork patché), g722_1, WebRTC VAD, et `libbfcp` (interne). Tout le reste
passe en dynamique natif.

Dépôts à activer sur le build : **AppStream + CRB (CodeReady Builder) + EPEL 9**.
ffmpeg via **RPMFUSION `ffmpeg`** (décision §6). RPM Fusion `x264-devel` reste
nécessaire tant que le code lie `-lx264` en direct (cf. §4, point H.264).

---

## 2. Code redondant ↔ libmedikit

`libmedikit` produit `libmedkit.a` (archive statique, mais liée dynamiquement à
ffmpeg/openssl/x264/bz2). Ses en-têtes sont dans `libmedikit/medkit/*.h` et
`libmedikit/astmedkit/*.h` (inclusion via préfixe : `#include "medkit/audio.h"`).

> **Choix retenu : pas d'installation dans `/opt/ives`.** On garde `libmedkit.a`
> et ses en-têtes **dans l'arbre de compilation** (le sous-module) et on lie
> directement dessus. On n'utilise donc **jamais** la cible `install` de
> libmedikit, seulement sa cible `all` (qui produit l'archive sur place sans rien
> installer).

### Fichiers de `mcu/src` couverts par libmedikit (candidats à suppression)

Primitives média :
- `audio.cpp`, `video.cpp` → `medkit/audio.cpp`, `medkit/video.cpp`
- `framescaler.cpp` → `medkit/framescaler.cpp`
- `avcdescriptor.cpp` → `medkit/avcdescriptor.cpp`
- `logo.cpp` → `medkit/logo.cpp`
- `red.cpp` / `redcodec.cpp` → `medkit/red.cpp`
- `stunmessage.cpp` → `medkit/stunmessage.cpp`
- `textencoder.cpp` → `medkit/textencoder.cpp`
- `tools.c` → `medkit/tools.c`
- log (`log.h`) → `medkit/log.c` / `astlog.c`

Codecs (sous-dossiers de `mcu/src`) :
- `g711/` → `libmedikit/g711`
- `g722/` → `libmedikit/g722`
- `h263/` → `libmedikit/h263`
- `h264/` → `libmedikit/h264`
- `aac/` → `libmedikit/aac`

MP4 / transcodage :
- `mp4player.cpp`, `mp4recorder.cpp`, `mp4streamer.cpp` → `medkit/mp4format.cpp`,
  `mp4track.cpp`, `transcoder.cpp`, `ffvideocodec.cpp`, `picturestreamer.cpp`

### Risque d'intégration (à valider fichier par fichier)

Le mediaserver inclut aujourd'hui ses en-têtes « à plat » (`#include "audio.h"`,
`#include "video.h"` depuis `mcu/include/`). Après migration, ces classes viennent
de `medkit/…` (depuis l'arbre du sous-module, pas `/opt/ives`). Il faut s'attendre à :
- **divergences de signatures/classes** entre la copie du mediaserver et celle de
  libmedikit (elles ont évolué séparément) ;
- adaptation des `#include` : `-I$(MEDKITDIR)` puis préfixe `medkit/` (p.ex.
  `#include "medkit/audio.h"`). Attention aux collisions de noms avec les en-têtes
  homonymes restants dans `mcu/include/` (`audio.h`, `video.h`, `tools.h`,
  `codecs.h`, `log.h`, `version.h`…) : supprimer la copie locale en même temps que
  le `.o` correspondant pour éviter qu'un mauvais en-tête soit résolu en premier ;
- les fichiers du mediaserver qui *dérivent* ou *appellent* ces primitives
  (`videomixer`, `audiomixer`, `videostream`, `audiostream`, `mp4recorder`…)
  devront être recompilés contre l'API libmedikit.

**Ne pas supprimer en masse.** Procéder codec par codec / primitive par primitive,
en compilant à chaque étape (cf. §5).

### Constats du build réel (étape concrète, AlmaLinux 9 / GCC 11.5)

- **ffmpeg-free n'embarque pas x264** : `x264.h` est requis par `h264/h264encoder.h`.
  → `x264-devel` doit être installé (compilé/packagé côté IVeS, RPM Fusion absent).
- **Objets couplés à Asterisk** dans cette branche de libmedikit (incluent
  `<asterisk/...>`) : `transcoder.o`, `mp4format.o`, `framebuffer.o`,
  `frameutils.o`, `astlog.o`. Inutilisables sans `asterisk-devel`. Le mediaserver
  n'étant pas un module Asterisk, on **exclut ces objets** d'un `OBJS` réduit
  (cf. `compile_libmedkit` dans `install.ksh`). **Conséquence à arbitrer (§6)** :
  le MP4 « muxer » (`mp4format`) et le `transcoder` de libmedikit sont couplés
  Asterisk → la reprise du MP4 (objectif §2) demandera soit un découplage amont,
  soit de conserver `mp4recorder`/`mp4player`/`mp4streamer` du mediaserver.
- **Dépendance mp4v2** : `log.c`, `mp4track.cpp` incluent `mp4v2/mp4v2.h`. Pas de
  paquet natif → en-têtes pris dans `staticdeps/include` (build source IVeS),
  surchargeable via `MP4V2INC`.
- **Bug du Makefile libmedikit** : `-I$(CUSTOM_ASTPATH)` vide produit un `-I`
  orphelin qui avale le `-DLOG_` suivant → on surcharge `INCLUDE` à l'appel.

---

## 3. Intégration du sous-module

**Décision (§6)** : sous-module du **dépôt fontventa entier**, on ne builde que
`libmedikit/`.

```sh
git submodule add -b migration/almalinux_9 \
    https://github.com/InteractiviteVideoEtSystemes/fontventa.git third_party/fontventa
```

- Seul `third_party/fontventa/libmedikit/` est compilé ; `app_*`, `libh324m`,
  `mp4creator`, etc. sont ignorés par notre build.

Chaîne de build cible (**lien direct in-tree, sans `/opt/ives`**) :
1. `make -C third_party/fontventa/libmedikit all` (cible `all` uniquement →
   produit `third_party/fontventa/libmedikit/libmedkit.a` sur place, **pas
   d'install**). Étape ajoutée dans `install.ksh` (et `local_compile`).
2. `mcu/Makefile.rpm` :
   - en-têtes : `INCLUDE += -I$(MEDKITDIR)` (où
     `MEDKITDIR = ../third_party/fontventa/libmedikit`), ce qui rend disponibles
     `medkit/*.h` et `astmedkit/*.h` ;
   - lien : ajouter l'archive par **chemin complet** dans `LDFLAGS`, p.ex.
     `$(MEDKITDIR)/libmedkit.a` (plus robuste qu'un `-L … -lmedkit` qui pourrait
     attraper une vieille copie de `/opt/ives/lib64`) ;
   - retrait des `.o` redondants de la liste `OBJS` (cf. §2).
3. Adapter le `Makefile` de libmedikit si besoin : son `-I/usr/include/ffmpeg` doit
   pointer vers les en-têtes d'**EPEL `ffmpeg-free-devel`** sur AlmaLinux 9 (chemin
   à confirmer). On ne touche **pas** à ses cibles `install*`.

> Remarque : `libmedkit.a` étant une archive statique, son code finit *dans* le
> binaire `mcu`. Seules ses dépendances **dynamiques** (ffmpeg-free, ssl, crypto,
> x264, bz2) doivent rester sur la ligne de lien finale de `mcu` — elles y sont
> déjà. Rien n'est donc déployé dans `/opt/ives` pour medkit.

---

## 4. Portage technique (les vrais pièges)

### OpenSSL 1.1.1d → 3.0
- `dtls.cpp` et l'extraction de clés SRTP utilisent OpenSSL directement.
- API bas niveau dépréciée/supprimée en 3.0 ; nombreux warnings `-Wdeprecated`.
- Vérifier `SRTP_*`, `EVP_*`, l'init (`OPENSSL_init_ssl` vs `SSL_library_init`).
- Possibilité de `-DOPENSSL_API_COMPAT=0x10100000L` en transition, mais viser une
  vraie compat 3.0.

### ffmpeg (EPEL `ffmpeg-free`, passage à 5/6)
- API channel layout (`AVChannelLayout`) refondue en 5.1, `AVCodec` devenu `const`,
  `avcodec_send_packet`/`receive_frame`, suppression d'API `avcodec_decode_*`.
- C'est précisément ce que la branche `migration/almalinux_9` de libmedikit a déjà
  traité → raison de plus d'adopter le code amont plutôt que de re-porter les
  copies locales.
- ⚠️ **H.264 / codecs propriétaires** : `ffmpeg-free` est compilé **sans** les
  encodeurs externes (pas de `libx264`, pas de `libfdk-aac` dans libavcodec). Or le
  mediaserver lie `-lx264` en direct et fait de l'encodage H.264 (mosaïque,
  transcodage). Deux conséquences à valider tôt (étape 3) :
  - garder `x264-devel` (RPM Fusion) en lien direct `-lx264` — indépendant de
    ffmpeg-free, donc l'encodage H.264 reste possible via le chemin x264 direct ;
  - vérifier qu'aucun chemin ne dépend de l'encodeur `libx264` *interne* à
    libavcodec (sinon il faudrait basculer sur RPM Fusion `ffmpeg` complet).
  - AAC : l'encodeur AAC natif de ffmpeg-free suffit-il, ou faut-il fdk-aac
    (dossier `aac` de libmedikit) ?

### Compilateur GCC 11
- `-std=gnu++0x` (brouillon C++11, 2012) → passer à `-std=gnu++11` au minimum,
  idéalement `gnu++17` (défaut GCC 11).
- Le Makefile libmedikit utilise déjà `-Wno-narrowing` (conversions rétrécissantes
  désormais erreurs) — à reporter.
- Attendre des erreurs sur litéraux, `register`, comparaisons signé/non-signé.

### Détection de distribution
- `mcu/Makefile.rpm` détecte `fc`/`el5`/`el6` via `/etc/redhat-release`. AlmaLinux 9
  → ajouter `el9` (et la branche de lien dynamique associée). Réutiliser/renommer
  `FEWSTATICDEPS` en mode `el9` dynamique.

### Python (build WebRTC VAD)
- `install.ksh compile_webrtc*` exige Python 2.6/2.7 → AlmaLinux 9 n'a que Python 3.
  Adapter ou figer la VAD en `.a` pré-compilée archivée (`lib/linux-64bits`).

### Packaging & service
- `mcumediaserver.spec` : mettre à jour `BuildRequires`/`Requires` pour les paquets
  `-devel` natifs ; `Release: 1.ives%{?dist}` donnera `.el9`.
- `mediaserver.init` (SysV) → unité **systemd** (`mediaserver.service`).
- `Jenkinsfile` : nœud `centos6` → nœud `almalinux9` ; activer EPEL/CRB/RPM Fusion.

---

## 5. Découpage en étapes (incrémental, compilable à chaque palier)

1. **Préparation environnement** : VM/conteneur AlmaLinux 9 + dépôts
   (AppStream/CRB/EPEL/RPM Fusion) ; recompiler `libbfcp` el9.
2. **Détection el9 + lien dynamique** : ajouter le mode `el9` dans
   `mcu/Makefile.rpm`, basculer openssl/xmlrpc/vpx/opus/speex en dynamique
   (sans encore toucher au code). Régler les erreurs de link.
3. **Portage OpenSSL 3 + ffmpeg** : faire compiler le mediaserver *tel quel*
   contre les nouvelles libs (avant libmedikit), pour isoler les régressions API.
4. **Sous-module libmedikit** : ajout du sous-module, build de `libmedkit.a` via
   `make … all` (in-tree, sans install), intégration des chemins d'include/link
   directs (`-I$(MEDKITDIR)`, archive par chemin complet dans `LDFLAGS`).
5. **Bascule incrémentale vers libmedikit**, un bloc à la fois (codecs g711/g722,
   puis h263/h264/aac, puis mp4, puis primitives audio/video/framescaler/…),
   en retirant les `.o` correspondants des `OBJS` et en compilant à chaque fois.
6. **Build RPM el9** : `install.ksh`, `.spec`, systemd.
7. **CI** : Jenkins sur nœud AlmaLinux 9.
8. **Tests fonctionnels** : pas de suite auto — valider conférence (mixage
   audio/vidéo mosaïque), enregistrement/lecture MP4, SRTP/DTLS, BFCP, et le
   chemin RTMP/WebSocket. `rtmptest` pour le RTMP.

---

## 6. Questions ouvertes à trancher

Décidé :
- ✅ **Sous-module** : dépôt `fontventa` entier (on ne builde que `libmedikit/`).
- ✅ **ffmpeg** : EPEL `ffmpeg-free` (⚠️ implique de garder `-lx264` direct via
  RPM Fusion `x264-devel`, cf. §4).

Restent à trancher :
- **MP4 / transcoder couplés Asterisk** : `mp4format` et `transcoder` de libmedikit
  dépendent de `<asterisk/...>` dans cette branche. Découpler côté fontventa, ou
  garder le MP4/transcodage propre au mediaserver ? (cf. constats §2)
- **mp4v2** : reste un build source (pas de paquet natif) ; `libmedkit` *et* le
  mediaserver en dépendent. Conserver via `staticdeps/`.
- **H.264/AAC sous ffmpeg-free** : l'encodage H.264 passe-t-il bien par `-lx264`
  direct (et non par `libx264` interne à libavcodec) ? l'AAC natif suffit-il ou
  faut-il le fdk-aac de libmedikit ? — à valider dès l'étape 3.
- **libsrtp** : rebaser le patch IVèS sur le `libsrtp` d'EPEL, ou conserver le
  fork compilé en source ?
- **ImageMagick** : rester en Magick++ 6.9 (EPEL) ou viser 7 (API C++ différente) ?
- **moteli/RabbitMQ** : portage maintenant (protobuf 3 / librabbitmq EPEL) ou plus
  tard, vu que `MOTELI` est optionnel ?

---

## 7. Fichiers à modifier (récapitulatif)

- `mcu/Makefile.rpm` — détection `el9`, flags de lien dynamique, retrait des `.o`
  repris par libmedikit, `-I$(MEDKITDIR)` (sous-module) + archive `libmedkit.a`
  par chemin complet dans `LDFLAGS` (**pas** `/opt/ives`).
- `install.ksh` — supprimer/court-circuiter les builds source devenus natifs,
  ajouter `make -C third_party/fontventa/libmedikit all` (in-tree, sans install),
  `prereq` el9, Python 3.
- `third_party/fontventa/libmedikit/Makefile` (dans le sous-module) — adapter au
  besoin le `-I/usr/include/ffmpeg` vers les en-têtes `ffmpeg-free` d'el9. Idéalement
  via une variable surchargée à l'appel plutôt qu'un patch commité dans le sous-module.
- `mcumediaserver.spec` — `BuildRequires`/`Requires` natifs, dépendance medkit.
- `mediaserver.init` → `mediaserver.service` (systemd).
- `Jenkinsfile` — nœud et dépôts el9.
- `.gitmodules` — sous-module fontventa/libmedikit.
- Suppression progressive des sources redondantes de `mcu/src` et `mcu/include`.
- `CLAUDE.md` — mettre à jour la section build une fois la bascule faite.
