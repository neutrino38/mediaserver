# Portage vers AlmaLinux 9 — état et reste à faire

Branche `feat/alma_linux9`. Ce document a été **révisé** : la majeure partie du
portage (compilateur, ffmpeg, OpenSSL, srtp, VAD, dédoublonnage média via
libmedikit) est **réalisée et le build est vert**. Il ne reste que le packaging,
le service systemd, la CI et quelques nettoyages.

---

## 1. Réalisé (build mcu vert sur AlmaLinux 9 / GCC 11.5)

### Toolchain & langage
- **GCC 11**, `-std=gnu++17 -Werror=return-type` (`mcu/Makefile.rpm`). ~66
  chemins sans `return` (UB) corrigés. Cf. `cpp17-migration.md`.

### Dépendances passées en dynamique système
- **OpenSSL 3.0** : lien dynamique `-lssl -lcrypto` ; init via `OPENSSL_init_ssl`
  (`main.cpp`) ; plus de build openssl source. `spec` : `openssl-libs >= 3.0`,
  `openssl-devel >= 3.0`.
- **ffmpeg 5** (avcodec/avformat/avutil/swscale/swresample/avfilter/avdevice/
  postproc) : dynamique. L'API ffmpeg 5 est portée **dans libmedikit** (branche
  `migration/almalinux_9`), pas dans mcu. Cf. `libmedikit-ffmpeg5-migration.md`.
- **libsrtp2** : `-lsrtp2` système (le fork patché IVèS est abandonné ; GCM
  cosmétique aujourd'hui). Cf. `libsrtp2-migration.md`.
- **x264** : `-lx264` direct (encodage H.264 mosaïque/transcodage) — indépendant
  de ffmpeg.
- **ImageMagick** : **Magick++ 7** via `pkg-config --cflags/--libs Magick++`
  (décision tranchée : on ne reste pas en 6).
- **AMR-NB/WB** : via libavcodec dynamique (plus de `-lopencore-amr*` en direct).
- **gsm** : via libmedikit/libavcodec.
- **VAD** : réécrite sur **webrtc-audio-processing** (module APM `VoiceDetection`),
  `pkg-config`. L'ancien WebRTC trunk (et son build Python 2) est **supprimé** —
  ce qui règle le point Python 3. Cf. `vad-webrtc-audio-processing-migration.md`.

### Dédoublonnage média → libmedikit (sous-module)
- Sous-module `third_party/fontventa` (`.gitmodules`, branche
  `migration/almalinux_9`) ; seul `libmedikit/` est bâti, **in-tree** (cible
  `all`, `ASTERISK=no`, pas d'install dans `/opt/ives`). Cible
  `install.ksh libmedkit` → `compile_libmedkit`.
- Liaison **inconditionnelle** de `libmedkit.a` par chemin complet
  (`MEDKITLIB`), `-I$(MEDKITDIR)` placé avant `-Iinclude/`. Le mode « sans
  medkit » a été supprimé. Cf. `integration_libmedkit.md`.
- Codecs retirés de `mcu/src` (fournis par medkit) : **g711, g722, h263/h263+/
  mpeg4, h264, aac, speex**. AMR, VP8, GSM, OPUS également portés côté medkit.
  ABI mcu↔medkit synchronisée.
- **MP4** : `mp4recorder` réécrit en coquille sur **`mp4writer`/`mp4reader`** de
  libmedikit ; `Logo` unifié (mcu `logo.cpp/.h` supprimés). Le couplage Asterisk
  de `mp4format`/`transcoder` est contourné par `ASTERISK=no` (on n'utilise que
  `mp4reader`/`mp4writer`). Cf. `mp4recorder-mp4writer-migration.md`.
- **Rééchantillonnage audio** : ex-`AudioTransrater` (speexdsp) remplacé par
  **libswresample** ; **speexdsp éliminé**. Cf.
  `audiotransrater-swresample-migration.md`.

### Restent en build source (`staticdeps/`, pas de paquet natif satisfaisant)
- **mp4v2** (fork IVèS), **g722_1/SIREN** (fork IVèS), **xmlrpc-c** (lié en `.a`,
  backend libxml2 → `-lxml2`). `install.ksh prereq` installe désormais :
  `gsm-devel ffmpeg-devel webrtc-audio-processing-devel libsrtp2-devel`.

---

## 2. Reste à faire

### A. Chaîne RPM cohérente avec le sous-module — **prioritaire, cassé**
`mcumediaserver.spec` n'a pas été mis à jour pour libmedikit ni pour la nouvelle
`install.ksh` :
- `%prep` appelle **`./install.ksh webrtc`** → **cible inexistante** (l'ancienne
  compilation WebRTC VAD a disparu). À remplacer par l'init du sous-module et le
  build medkit.
- `%build` → `install.ksh localcompile`, mais **`local_compile` ne construit pas
  `libmedkit.a`** (c'est une cible séparée `install.ksh libmedkit`). Le build RPM
  échouera au link faute d'archive medkit.
- Ajouter dans le flux RPM : `git submodule update --init --recursive` puis
  `./install.ksh libmedkit` avant `localcompile`.
- `BuildRequires`/`Requires` à compléter : **`libsrtp2` / `libsrtp2-devel`**
  manquants ; `ImageMagick-c++ >= 6.7.0` incohérent avec Magick++ 7 (viser
  `ImageMagick-c++ >= 7`).

### B. Service systemd — **non fait**
- Toujours `mediaserver.init` (SysV) copié dans `/etc/init.d/` par le `.spec`.
- Créer `mediaserver.service`, l'installer sous `%{_unitdir}`, ajouter les
  scriptlets `%post/%preun/%postun` (`systemctl`), retirer l'init SysV.

### C. `install.ksh local_compile` — nettoyage
- N'initialise pas le sous-module ni n'appelle `compile_libmedkit` : documenter/
  enchaîner l'ordre (`git submodule update --init` → `libmedkit` → `localcompile`)
  ou l'automatiser.
- **speex source probablement mort** : la ligne de lien el9 par défaut ne
  référence plus `-lspeex` (seul `FEWSTATICDEPS`/`el5` le gardent). Confirmer puis
  retirer le build speex de `local_compile`.
- `xmlrpc-c` reste en `.a` staticdeps (l'option paquet dynamique CRB/EPEL n'a pas
  été retenue) : à garder tel quel, sauf décision contraire.

### D. Détection de distribution
- `DISTRO` (`Makefile.rpm`) ne reconnaît que `fc`/`el5`/`el6`. Le **mode par
  défaut (`else`) est de fait le mode AlmaLinux 9 dynamique** ; il fonctionne mais
  aucun `el9` n'est détecté explicitement. Nettoyer/renommer (ex. brancher `el9`
  explicitement, clarifier le rôle de `FEWSTATICDEPS`).

### E. CI — **à recréer**
- L'ancien `Jenkinsfile` (matrice CentOS6) a été **supprimé**. Aucune CI n'est
  versionnée. Recréer une CI ciblant AlmaLinux 9 : dépôts (AppStream/CRB/EPEL,
  RPMFusion pour ffmpeg/x264), init du sous-module, `install.ksh prereq` puis le
  build RPM une fois le `.spec` corrigé (§A).

### F. libbfcp — **fait**
- Converti en **sous-module** (`third_party/libbfcp`, même schéma que libmedikit),
  bâti in-tree via `install.ksh libbfcp` / `compile_libbfcp` (cible `make all`,
  variantes dbg+rel), lié par chemin complet (`BFCPDIR=../third_party/libbfcp`,
  `-I$(BFCPDIR)/include`, `$(BFCPDIR)/lib/libbfcp{dbg,rel}.a`). Plus aucune
  dépendance `/opt/ives` ni `BuildRequires: libbfcp`. Build mcu vérifié vert.

### G. moteli / RabbitMQ (optionnel, `MOTELI=yes`)
- Non porté : `protobuf` 2.5.0 et `rabbitmq-c` 0.3.0 toujours bâtis en source
  statique (`compile_rabbitmq`, `compile_protobuf`). À porter (protobuf 3 /
  `librabbitmq` EPEL) **seulement si** on réactive MOTELI.

### H. Documentation
- Mettre à jour la section « Build & run » de `CLAUDE.md` (elle décrit encore
  `install.ksh localcompile` bâtissant openssl/vpx/opus/srtp/vad en statique).

---

## 3. Questions ouvertes — tranchées

- ✅ Sous-module : dépôt `fontventa` entier, on ne bâtit que `libmedikit/`.
- ✅ ffmpeg : paquet système dynamique (ffmpeg RPMFusion / ffmpeg-free selon dépôt),
  API portée dans libmedikit.
- ✅ H.264 / AAC : H.264 via `-lx264` direct ; AAC via ffmpeg (plus de fdk-aac en
  ligne de lien par défaut).
- ✅ libsrtp : `libsrtp2` système (fork patché abandonné).
- ✅ ImageMagick : Magick++ 7.
- ✅ MP4 : `mp4reader`/`mp4writer` de libmedikit (couplage Asterisk contourné par
  `ASTERISK=no`).
- ✅ VAD/Python : webrtc-audio-processing (plus de build Python).
- 🕐 moteli/RabbitMQ : reporté (optionnel).

---

## 4. Récapitulatif des fichiers à toucher (reste à faire)

- `mcumediaserver.spec` — corriger `%prep`/`%build` (submodule + `libmedkit`),
  `Requires`/`BuildRequires` (srtp2, Magick++ 7), passage systemd.
- `mediaserver.service` (à créer) + scriptlets `.spec`, retrait de
  `mediaserver.init`.
- `install.ksh` — enchaînement submodule→libmedkit→localcompile, retrait du build
  speex mort.
- `mcu/Makefile.rpm` — détection/nommage `el9` explicite (cosmétique).
- CI (à recréer) — cible + dépôts AlmaLinux 9 (l'ancien `Jenkinsfile` a été supprimé).
- `CLAUDE.md` — section build.
- (optionnel MOTELI) `install.ksh` — protobuf 3 / librabbitmq EPEL.

> Note : libbfcp est désormais un sous-module bâti in-tree (§F fait), au même
> titre que libmedikit — plus de dépendance `/opt/ives` pour BFCP.
