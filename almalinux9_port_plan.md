# Portage vers AlmaLinux 9 — état et reste à faire

Branche `feat/alma_linux9`. Ce document a été **révisé** : la majeure partie du
portage (compilateur, ffmpeg, OpenSSL, srtp, VAD, dédoublonnage média via
libmedikit, chaîne RPM/`install.ksh` autosuffisante) est **réalisée et le build
est vert**. Il ne reste que le service systemd, la CI et quelques nettoyages.

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
- **mp4v2** (fork IVèS), **g722_1/SIREN** (fork IVèS). **xmlrpc-c est passé en
  paquet système** (`xmlrpc-c-devel`, dépôt crb, backend libxml2 → `-lxml2`,
  lien dynamique — plus de build source). `install.ksh prereq` installe :
  `gsm-devel ffmpeg-devel webrtc-audio-processing-devel libsrtp-devel
  xmlrpc-c-devel` (l'ABI srtp2 est fournie par `libsrtp-devel`, pas
  `libsrtp2-devel`).

### `install.ksh localcompile` autosuffisant — **fait**
- `local_compile` vérifie les paquets système (gsm, ffmpeg, libtool,
  webrtc-audio-processing, libsrtp, xmlrpc-c), bâtit les staticdeps restantes,
  **initialise les sous-modules au besoin** (`git submodule update --init
  --recursive`) puis enchaîne **`compile_libmedkit` + `compile_libbfcp`** avant
  le `make mcu`. Un seul `./install.ksh localcompile` produit le binaire.
- L'ancienne cible `install.ksh webrtc` (build WebRTC VAD) a disparu — la VAD
  est sur le paquet système webrtc-audio-processing (simple contrôle `rpm -q`).
- Les cibles séparées `install.ksh libmedkit` / `libbfcp` restent disponibles
  pour un rebuild individuel ; `install.ksh clean` nettoie aussi les objets/
  archives des deux sous-modules.

### Chaîne RPM (`mcumediaserver.spec`) — **fait** (ex-§A)
- `%prep` : `git submodule update --init --recursive` (plus d'appel à la cible
  `webrtc` supprimée) ; `%build` : `./install.ksh localcompile` qui construit
  désormais tout (sous-modules compris, cf. ci-dessus).
- `Requires`/`BuildRequires` à jour : `libsrtp2`/`libsrtp-devel`,
  `ImageMagick-c++ >= 7`, `xmlrpc-c`/`xmlrpc-c-devel`, `openssl >= 3.0`,
  webrtc-audio-processing.
- Reste au `.spec` : la migration systemd (§B) et une validation de bout en
  bout de `install.ksh rpm` en environnement propre.

---

## 2. Reste à faire

### B. Service systemd — **non fait**
- Toujours `mediaserver.init` (SysV) copié dans `/etc/init.d/` par le `.spec`.
- Créer `mediaserver.service`, l'installer sous `%{_unitdir}`, ajouter les
  scriptlets `%post/%preun/%postun` (`systemctl`), retirer l'init SysV.

### C. `install.ksh local_compile` — **fait** (build speex mort retiré)
- Le bloc `staticdeps` speex a été supprimé de `local_compile` : le codec Speex
  est fourni par libmedikit **au-dessus de ffmpeg** (`AV_CODEC_ID_SPEEX`,
  `speex/speexcodec.cpp` — aucun usage direct de l'API libspeex) et la ligne de
  lien el9 par défaut ne référence plus `-lspeex`. Vérifié : relink vert, pas de
  `NEEDED libspeex` dans le binaire (seule reste la dépendance *transitive* de
  `libavcodec` système, normale). Seules les branches legacy `FEWSTATICDEPS`/
  `el5` du `Makefile.rpm` mentionnent encore `-lspeex` (nettoyage §D).

### D. Détection de distribution
- `DISTRO` (`Makefile.rpm`) ne reconnaît que `fc`/`el5`/`el6`. Le **mode par
  défaut (`else`) est de fait le mode AlmaLinux 9 dynamique** ; il fonctionne mais
  aucun `el9` n'est détecté explicitement. Nettoyer/renommer (ex. brancher `el9`
  explicitement, clarifier le rôle de `FEWSTATICDEPS`).

### E. CI — **à recréer**
- L'ancien `Jenkinsfile` (matrice CentOS6) a été **supprimé**. Aucune CI n'est
  versionnée. Recréer une CI ciblant AlmaLinux 9 : dépôts (AppStream/CRB/EPEL,
  RPMFusion pour ffmpeg/x264), `install.ksh prereq` puis `install.ksh rpm nosign`
  (le `.spec` et `localcompile` gèrent désormais les sous-modules — §1).

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

### H. Documentation — **fait**
- `CLAUDE.md` décrit le flux `localcompile` autosuffisant et la liste des
  staticdeps est corrigée : **mp4v2 + g722_1 seulement** (xmlrpc-c est un paquet
  système, speex n'est plus bâti).

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

- `mediaserver.service` (à créer) + scriptlets `.spec` (`%post/%preun/%postun`),
  retrait de `mediaserver.init` (§B).
- `mcu/Makefile.rpm` — détection/nommage `el9` explicite + purge des branches
  legacy `FEWSTATICDEPS`/`el5` (`-lspeex`, `-lvpx`, `-lfdk-aac`…) (cosmétique, §D).
- CI (à recréer) — cible + dépôts AlmaLinux 9 (l'ancien `Jenkinsfile` a été
  supprimé) (§E).
- (optionnel MOTELI) `install.ksh` — protobuf 3 / librabbitmq EPEL (§G).

> Fait depuis la dernière révision : `mcumediaserver.spec` (`%prep` submodules,
> `%build` localcompile, Requires srtp2/Magick++ 7/xmlrpc-c),
> `install.ksh localcompile` autosuffisant (submodules + libmedkit + libbfcp),
> retrait du build speex mort (§C) et mise à jour `CLAUDE.md` (§H).

> Note : libbfcp est désormais un sous-module bâti in-tree (§F fait), au même
> titre que libmedikit — plus de dépendance `/opt/ives` pour BFCP.
