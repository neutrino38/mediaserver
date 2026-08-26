# CLAUDE.md

Ce fichier guide Claude Code (claude.ai/code) dans le travail sur ce dépôt.

## De quoi il s'agit

Une unité de conférence multipoint (MCU) / serveur média, dérivée à l'origine de Medooze/Fontventa, maintenue par IVèS. Elle mixe de l'audio, de la vidéo, du texte et du partage de documents pour des conférences, et fait le pont avec Asterisk et des endpoints SIP/WebRTC. Le binaire compilé s'appelle `mediaserver` (la cible de build est `mcu`) et se pilote à distance en XML-RPC ; il parle aussi RTMP, WebSocket, RTP/SRTP, BFCP et (en option) RabbitMQ.

La base de code est essentiellement du C++ (dans `mcu/`), plus trois projets Java compagnons.

## Compiler et lancer

Le build est porté sur **AlmaLinux 9 / GCC 11** (chantier mené sur la branche `feat/alma_linux9` — voir `almalinux9_port_plan.md` pour l'état). La plupart des dépendances sont désormais **liées dynamiquement aux paquets système** (ffmpeg 5, OpenSSL 3, libsrtp2, x264, Magick++ 7, webrtc-audio-processing, usrsctp). Deux bibliothèques seulement sont encore bâties depuis les sources dans `./staticdeps` (mp4v2, g722_1) ; xmlrpc-c est le paquet système dynamique (dépôt CRB) et speex n'est plus bâti (le codec Speex passe par ffmpeg via libmedikit). L'essentiel du code codec/média vient maintenant du **sous-module `libmedikit`** (`third_party/fontventa/libmedikit`), qui porte lui-même le portage ffmpeg 5.

```sh
# 1. Installer les dépendances de build système :
./install.ksh prereq       # dnf/yum: gsm-devel ffmpeg-devel webrtc-audio-processing-devel libsrtp-devel xmlrpc-c-devel usrsctp-devel
#                            usrsctp-devel vient du dépôt EPEL : il doit être activé sur la machine de build.

# 2. Build complet en une commande : initialise les sous-modules (libmedikit +
#    libbfcp), les bâtit in-tree, bâtit les dépendances source dans
#    ./staticdeps, puis le mcu :
./install.ksh localcompile   # sortie : bin/debug/mcu

# (builds individuels des sous-modules in-tree, au besoin :)
./install.ksh libmedkit    # libmedkit.a (codecs), cible 'all', ASTERISK=no
./install.ksh libbfcp      # libbfcp{dbg,rel}.a (BFCP), cible 'all'

# Recompiler seulement le binaire C++, dépendances déjà en place.
# TOUJOURS depuis mcu/ (cf. le piège plus bas) :
cd mcu && make mcu                # sortie : bin/debug/mcu

# Bâtir le RPM (ce que lance la fabrication de release) :
./install.ksh rpm nosign          # sans "nosign" : signature GPG

# Nettoyer :
./install.ksh clean
```

- Le `Makefile` et le `config.mk` de la racine sont **périmés/inutilisés** (ils référencent un répertoire `media/` inexistant). Le vrai build est `mcu/Makefile`, piloté par `install.ksh`.
- **Deux sous-modules in-tree sont obligatoires** : `mcu/Makefile` prend libmedkit (codecs) **par `pkg-config`** et lie `$(BFCPDIR)/lib/libbfcp{dbg,rel}.a` (bibliothèque C BFCP, `-I$(BFCPDIR)/include`) par chemin complet. Plus aucune dépendance à `/opt/ives`. Les `.o` de codecs que libmedkit fournit ont été retirés d'`OBJS`.
- **ffmpeg n'est écrit en dur nulle part.** `third_party/fontventa/libmedikit/Makefile` demande ses flags à `pkg-config` (`libavcodec libswscale libavformat libavutil libswresample`) et **génère `libmedkit.pc`** — chemins absolus vers le clone, motif « uninstalled » des autotools, non versionné, régénéré à chaque build. `mcu/Makefile` le consomme via `PKG_CONFIG_PATH=$(abspath $(MEDKITDIR))` et y ajoute les trois modules qu'il utilise en propre : `libavfilter`, `libavdevice`, `libpostproc`. ffmpeg y est déclaré en `Requires:` (et non `Requires.private:`) parce que la dépendance est publique — `ffvideocodec.h` inclut `<libavcodec/avcodec.h>` —, donc ses `-I` et ses `-l` remontent seuls au consommateur. Conséquence pratique : **bâtir contre un autre ffmpeg ne demande qu'un `PKG_CONFIG_PATH`**, ce dont le portage ffmpeg 9 a besoin (`ffmpeg9_migration_plan.md`). Les deux Makefiles ont un **repli bruyant** sur les chemins historiques si pkg-config ne résout pas : `libmedkit.pc` n'existe qu'une fois le sous-module bâti, ce que `install.ksh` garantit mais pas un `make` lancé à la main sur un arbre neuf.
- L'ordre des `-I` compte : ceux de libmedkit passent **AVANT** `-Iinclude/`, pour que les inclusions `<g722/…>` / `<amr/…>` visent le codec du sous-module et non son homonyme dans `mcu/src`. Idem au lien : `-lmedkit` avant les `-lav*`, l'archive devant précéder ce dont elle dépend.
- Principaux commutateurs de build dans `mcu/Makefile` : `DEBUG=yes` (défaut — `-g -O0`, compile dans `media/build/debug`, lie `libbfcpdbg.a`), `LOG=yes` (`-DLOG_`), `MOTELI=yes` (active le backend moteli RabbitMQ/protobuf — encore bâti depuis les sources, pas encore porté sur el9), `VADWEBRTC=yes` (VAD via l'APM système `webrtc-audio-processing`, `pkg-config`). Le standard C++ est `gnu++17` (avec `-Werror=return-type`).
- **Une seule ligne de lien, AlmaLinux 9, sans alternative.** Les branches `DISTRO` (`fc`/`el5`/`el6`) et `FEWSTATICDEPS` ont été supprimées : la détection ne reconnaissait même pas AlmaLinux — elle tombait dans le `else`, seul chemin réellement emprunté depuis le portage — et le spec est réservé à el9. Les codecs que ces branches liaient en direct (`-lx264`, `-lvpx`, `-lspeex`, `-lfdk-aac`, `-lg722_1`, `-lopencore-amr*`) arrivent tous par libavcodec, via libmedkit. Ne pas les réintroduire : le mediaserver n'appelle plus aucune de ces API directement.
- `mcu.proto` n'est compilé par `staticdeps/bin/protoc` que si `MOTELI=yes`.

- Le spec RPM (`mcumediaserver.spec`) est réservé à AlmaLinux 9 (compatibilité CentOS 6/el5 retirée) : `%prep` initialise les sous-modules git et `%build` lance `install.ksh localcompile`. Il installe une **unité systemd** (`mediaserver.service`, `Type=simple`) via les scriptlets standard `%systemd_post`/`%systemd_preun`/`%systemd_postun_with_restart` — l'ancien script SysV `mediaserver.init` a été supprimé. Les options de ligne de commande vivent dans `/etc/sysconfig/mediaserver` (`OPTIONS=`, sourcé par l'unité) ; le binaire tourne en avant-plan (pas de `-f`), donc systemd possède le PID et le cycle de vie, et l'arrêt est un `SIGTERM` (traité par `signing_handler`).

### Lancement (convention de déploiement IVèS)

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release          # sauvegarder le binaire courant
ln -s /home/user/mediaserver/bin/debug/mcu mediaserver
systemctl restart mediaserver
tail -f /var/log/mcu.log                     # suivre l'exécution (aussi : journalctl -u mediaserver -f)
```

Le RPM installe le binaire dans `/opt/ives/bin/mediaserver`, l'unité systemd dans `%{_unitdir}/mediaserver.service`, ses options dans `/etc/sysconfig/mediaserver`, la configuration dans `/etc/mediaserver/`. stdout/stderr sont ajoutés à `/var/log/mcu.log` par systemd (`StandardOutput`/`StandardError`).

Le binaire `mcu` a sa **propre suite de tests GoogleTest** dans `mcu/tests/`. **Se lancer depuis `mcu/`** — `$(OBJS)` porte des noms d'objets nus, donc un `make -f mcu/Makefile …` depuis la racine échoue sur `No rule to make target 'httpparser.o'` (et laisse un `media/` parasite à la racine). Utiliser `cd mcu && make …`, ou `make -C mcu …` qui change de répertoire pour vous :

```sh
cd mcu
make check       # compile puis exécute
make tests       # compile seulement -> tests/runtests
```

> **Tag `:ipv6` — chantier en cours, la plupart des tests sont désormais ACTIFS.** `mcu/tests/test_ipv6.cpp` porte une suite **adverse** de conformité IPv6 (notations, forme canonique RFC 5952, plages, `::ffff:` v4-mapped, dual-stack, ICE, STUN, URL/SDP, DNS AAAA, écoutes TCP). Écrite en échec volontaire (27 rouges au 2026-08-12), elle est **intégralement verte** depuis les étapes 0 à 8 du chantier (branche `feat/ipv6`) : ces tests ont perdu leur préfixe `DISABLED_` et sont joués par `make check`, où ils servent de garde-fous. Un seul reste désactivé, `IPv6Servers.LeServeurRtmpAccepteUnClientV6` — et il PASSE : il est exclu parce qu'il expose une course de démontage RTMP préexistante qui bloque une fois sur dix, pas parce qu'IPv6 y échoue. GoogleTest n'acceptant pas `:` dans un nom (c'est le séparateur de `--gtest_filter`), le tag est porté par une **double convention** : suites préfixées `IPv6` (sélection) + tests préfixés `DISABLED_` (exclusion par défaut). Conséquences pratiques :
> - `cd mcu && make check` joue tous ceux qui ne portent plus `DISABLED_` : un échec y est une VRAIE régression ;
> - `cd mcu && make check-ipv6` les joue tous (`--gtest_filter='IPv6*' --gtest_also_run_disabled_tests`) : c'est le **tableau de bord** du chantier, à consulter avant/après chaque étape ;
> - retirer le préfixe `DISABLED_` d'un test dès qu'il passe — c'est ainsi qu'il devient un garde-fou ; garder les suites `IPv6*`.
>
> Deux tests de cette suite doivent passer **avant comme après** (garde-fous anti-régression IPv4) : `IPv6DualStack.LaSocketRtpEntendToujoursLIPv4` et `IPv6Url.LAdresseV4NEstPasEncadree`. Elle a remplacé les harnais manuels `rtmptest`/`wstest`. Conception et organisation dans `TEST.md` (racine), mémo de lancement dans `mcu/tests/README.md`. Le **sous-module `libmedikit` a la sienne** dans `third_party/fontventa/libmedikit/tests/` (tests unitaires et adverses du négociateur, des codecs, des parseurs, et lecture/aller-retour MP4) : `make -C third_party/fontventa/libmedikit check` (nécessite le `gtest` système ; bâtit `libmedkit.a` avec `ASTERISK=no`). Voir son `tests/README.md` et `libmedikit_tests_plan.md` à la racine.

## Architecture

### Serveur média C++ (`mcu/`)

Le point d'entrée `mcu/src/main.cpp` démarre plusieurs serveurs qui partagent tous le moteur de conférence :
- **Serveur XML-RPC** (`xmlrpcserver`, `xmlhandler`) — l'API de contrôle principale. Les tables de commandes (`mcuCmdList`, `broadcasterCmdList`, `jsr309CmdList`) associent les noms de méthodes RPC à des handlers dans `xmlrpcmcu.cpp`, `xmlrpcbroadcaster.cpp`, etc.
  **Toute modification de l'API XML-RPC `/mcu` ou `/jsr309` (méthodes, paramètres, valeurs de retour, énumérations, événements) DOIT aussi mettre à jour les schémas protobuf MOTELI v2** du dépôt elixip (`apps/elixip2/priv/proto/moteli_*.proto`) dans le même jeu de changements — ils sont le transport RabbitMQ elixip 2.0 du même contrat (conception : elixip `docs/design/moteli-reboot.md`), et ne doivent jamais prendre du retard sur la version HTTP.
- **Serveur RTMP** (`rtmpserver`) et **serveur WebSocket** (`websocketserver`) pour le transport média Flash/web.
- Backend optionnel **RabbitMQ/moteli** (`src/moteli/`, `-DMOTELI`) portant des messages protobuf (`mcu.proto`) pour le projet MOTELI.

Modèle de conférence, au cœur :
- `MCU` (`mcu.h`/`mcu.cpp`) — gestionnaire de plus haut niveau ; possède les conférences et les files d'événements. `CreateConference`, `GetConferenceRef`. Implémente `MultiConf::Listener` pour recevoir les événements des participants (demandes de FPU, partage de document).
- `MultiConf` (`multiconf.cpp`) — une conférence. Gère participants, mosaïques, sidebars, players. `CreateParticipant`, `CreateMosaic`, `CreateSidebar`, `SetVADMode`, etc.
- Les **participants** sont soit `RTPParticipant`, soit `RTMPParticipant` (transports RTP/SRTP ou RTMP).
- **Mixeurs** : `videomixer`/`audiomixer`/`textmixer` combinent les flux des participants. La disposition vidéo est composée via des **mosaïques** (`mosaic`, `partedmosaic`, `asymmetricmosaic`, `pipmosaic`) et `sidebar`/`overlay`/`logo`.
- **Flux et tuyaux** : `audiostream`/`videostream`/`textstream` enveloppent les sessions RTP ; `pipe{audio,video,text}{input,output}` transportent le média brut entre décodeurs, mixeurs et encodeurs.

Plomberie média :
- `rtpsession`, `rtp`, `RTPSmoother`, `dtls` (clés SRTP), `stunmessage`, `fecdecoder`, `red`/`redcodec` pour la couche transport.
- Les codecs viennent maintenant du **sous-module `libmedikit`**, pas de `mcu/src` : les sous-répertoires par codec (`g711`, `g722`, `gsm`, `speex`, `opus`, `h263`, `h264`, `aac`, `amr`, `vp8`…) ont été supprimés et les classes sont fournies par `medkit/*` (`FfAudioEncoder`/`FfVideoEncoder`, `PCMUEncoder`, etc.), qui enveloppent ffmpeg/x264 derrière l'interface de `include/codecs.h`. Ne restent dans `mcu/src` que les bouts de média que libmedikit ne couvre pas (p.ex. `nelly`, `vp6`, `flv1` là où ils sont encore référencés). **STUN** (`stunmessage`, et `crc32calc` qui le sert) a lui aussi été rapatrié dans le sous-module le 2026-08-15 : le mcu en portait une copie et libmedikit une autre, morte et périmée — il n'y en a plus qu'une, consommée par `medkit/stunmessage.h`. Le rééchantillonnage audio passe par **libswresample** (ffmpeg) ; l'ancien `AudioTransrater` speexdsp a disparu.
- Enregistrement/lecture MP4 : `mp4recorder`/`mp4player`/`mp4streamer` sont de fines coquilles au-dessus des **`mp4writer`/`mp4reader`** de libmedikit (libmp4v2 en dessous). L'enregistrement et la diffusion FLV/RTMP (`FLVEncoder`, `flvrecorder`, `rtmpflvstream`) restent dans `mcu`.
- La VAD utilise l'APM système **`webrtc-audio-processing`** (`VoiceDetection`) ; l'ancien tronc WebRTC vendu avec les sources a été supprimé.
- `src/bfcp/` implémente l'API objet C++ BFCP (contrôle de parole pour le partage de document/écran) au-dessus du `libbfcp` de niveau C, désormais bâti in-tree depuis le **sous-module `third_party/libbfcp`** (en-têtes `-I$(BFCPDIR)/include`, archive `libbfcp{dbg,rel}.a` liée par chemin complet — plus de `/opt/ives`).

Les en-têtes sont dans `mcu/include/`, les sources dans `mcu/src/`. La liste d'objets et les options de lien de `mcu/Makefile` font foi sur ce qui est réellement compilé.

### Projets Java compagnons (NetBeans/Ant — chacun avec `build.xml` + `nbproject/`)

- `jsr309impl/` — implémentation JSR-309 (Media Server Control API) sous `org.murillo.mscontrol`. Pilote le MCU C++ depuis un serveur d'applications Java/JSR-309 ; lui parle via XmlRpcMcuClient.
- `XmlRpcMcuClient/` — bibliothèque cliente Java de l'API de contrôle XML-RPC du serveur C++.
- `sdp/` — bibliothèque d'analyse/manipulation SDP (volumineuse, ~270 fichiers) utilisée par la couche JSR-309.

Se bâtissent avec `ant` dans chaque répertoire (hors build RPM).

## Conventions et pièges

- La base de code est du vieux C++ (mélange `.c`/`.cpp`, bâti aujourd'hui en `gnu++17`), avec un usage massif de threads bruts, de `std::wstring` pour les noms/tags, et de la gestion mémoire manuelle. Se conformer au style environnant. La migration vers `std::thread`/`std::atomic` et les verrous `std::mutex` est en cours.
- **Modèle de propriété mémoire (migration smart pointers, voir `smart_pointers_plan.md`)** — le motif cible des sous-systèmes « un gestionnaire possède une map de sessions » (`MCU`/`MultiConf`, `JSR309Manager`/`MediaSession`, `Broadcaster`/`BroadcastSession`) est désormais : la map propriétaire détient un `shared_ptr` par entrée ; `Get*Ref(id, shared_ptr&)` rend une **copie** du `shared_ptr` (plus de compteurs `numRef`/`enabled`, plus de `Release*Ref`) ; `Delete*` extrait le `shared_ptr` sous le verrou, efface l'entrée de la map (plus aucune nouvelle référence ne peut être distribuée), puis appelle `End()` (idempotent) **hors** du verrou — le dernier `shared_ptr` survivant détruit l'objet, donc les références détenues par des handlers en vol restent sûres. `Connect()` (RTMP) rend le `shared_ptr` de la session (ou un `shared_ptr` aliasé partageant sa propriété) pour que `RTMPConnection::app` maintienne la session en vie le temps de la connexion. Les membres exclusifs sont des `unique_ptr` (`MultiConf::recorder`, `players`, `PublisherInfo::stream/conn`, décodeurs de `MP4Player`, `Mosaic::overlay`/`overlays`, `MediaSession::recorderTimers`) ; les observateurs et pointeurs arrière sont bruts ou `weak_ptr` avec un `.lock()` au point d'usage (`NetStream::part`, `VideoStream::rtpSession`, `RecorderTimer→MediaSession`). Les phases 0 à 5 sont faites ; `use.h` (`Use`/`IncUse`/`WaitUnusedAndLock`) porte encore ~20 fichiers (chemins chauds participants/mixeurs/rtp) et se retire progressivement, pas d'un bloc.
- Les commentaires et les messages de commit sont majoritairement en **français**.
- L'essentiel du code média/codec vit désormais dans le **sous-module `libmedikit`** (`third_party/fontventa/libmedikit`), pas dans `mcu/src` — y regarder avant de supposer qu'un codec est local. Deux dépendances source (mp4v2, g722_1) sont encore vendues en builds statiques dans `staticdeps/` ; tout le reste est un paquet système dynamique.
- **L'adresse IP annoncée dans le SDP a exactement une source** : la table des **profils d'adressage** (`mcu/include/addressprofiles.h`, figée au démarrage par `main()`), lue par la jambe RTP concernée (`RTPSession::SetAddressProfile`, à défaut le profil par défaut). Chaque profil porte **deux adresses distinctes** — celle qu'on **lie** et celle qu'on **annonce** —, ce qui est précisément ce qui rend un déploiement natté descriptible. Les deux API de contrôle lisent cette table — `Endpoint::GetMediaCandidates` (JSR-309) et `StartReceiving` (MCU, `returnVal[1]`), plus `GetNetworkProfiles` pour l'introspection — donc elles ne peuvent pas se contredire, et un contrôleur n'a jamais à porter l'adresse dans sa propre configuration. Ne **pas** redériver une adresse locale ailleurs : c'est cette duplication qui faisait de chaque déploiement derrière NAT une devinette par appelant. `main()` refuse de démarrer si aucun profil n'est disponible, et c'est ce qui permet aux appelants de traiter la valeur comme toujours présente. Doc exploitant, par cas d'usage (adresse publique portée par l'hôte, NAT 1:1, publique + interne) : `NETWORK-CONFIGURATION.md`.
- **Ce que le serveur sait de lui-même, c'est à lui qu'on le demande** — même règle que pour l'adresse annoncée ci-dessus, et pour la même raison : toute liste que le contrôleur écrit de son côté est une copie, et une copie dérive. Les codecs portés en sont le second cas, et il a coûté un appel. `AudioCodecFactory` et `VideoCodecFactory` (`third_party/fontventa/libmedikit/{audio,video}.cpp`) sont la seule source honnête, et **les deux directions n'y coïncident pas** : VP6 se décode sans s'encoder. Or `GetSupportedCodecs` (`mcu/src/xmlrpcmcu.cpp`) est un tableau **écrit à la main** de 8 codecs audio qui **ne contient pas OPUS** — celui de tous les appels réels — et répond *media not supported* pour la vidéo ; et l'API JSR-309 ne l'expose pas du tout. Faute de pouvoir demander, le contrôleur a déclaré : elixip offrait H.264/VP8 alors que le serveur portait AV1 depuis des mois, et un appel AV1 ↔ AV1 est mort en 488 avec un audio parfait des deux côtés (2026-08-12). Conséquence pratique : **une capacité qui existe dans le code mais qu'aucune API ne permet d'interroger est un défaut**, pas un détail — c'est ce qui force le contrôleur à deviner. Plan de reprise : `codec_capabilities_plan.md`.

- **La durée de vie d'une `MediaSession` JSR-309 et d'une conférence MCU est celle de sa file d'événements.** Le long-poll du contrôleur (`/events/jsr309/<queueId>`, `/events/mcu/<queueId>`) est sa **preuve de vie** : `XmlEventQueue::AttachPoller`/`DetachPoller` (posés par `XmlStreamingHandler::ProcessRequest`) horodatent la présence d'un lecteur. La politique vit dans **un seul endroit**, `eventqueuesweeper.h` (`EventQueueSweeper`, bâti sur `worker.h`), dont `JSR309Manager` et `MCU` héritent en fournissant seulement `CollectQueueIds` + `DeleteQueueOwners` — ne pas la réimplémenter par service. Deux signaux, un même délai (`--event-queue-expires`, 60 s par défaut = `XmlEventQueue::DefaultExpiresSecs`, 0 = désarmé) : (1) file existante mais sans lecteur depuis le délai → destruction des objets puis de la file ; (2) file détruite alors que des objets la référencent encore (`EventQueueDelete`, ou `queueId` jamais valide) → **armement** du délai, jamais de destruction immédiate, pour laisser au contrôleur une chance de se reconnecter. Un `queueId` ≤ 0 n'est rattaché à rien : jamais balayé (c'est ce qui protège le chemin MOTELI, où `eventListenerId` vaut 0 par défaut). La portée du nettoyage est celle du découpage des files choisi par le contrôleur — c'est **assumé** (arbitrage 2026-08-11) : une file par appel/conférence isole, une file partagée emporte tout. C'est le SEUL nettoyage automatique de ces objets — ne pas ajouter de ping applicatif ni de compteur d'inactivité XML-RPC en parallèle (`jsr309_session_expiry_plan.md` §7.4). L'équivalent pour le transport RabbitMQ/MOTELI, qui n'a pas de long-poll, reste **à concevoir** (§7.5).
- Aucune chaîne d'intégration continue n'est versionnée (l'ancien `Jenkinsfile` Jenkins, une matrice CentOS 6, a été supprimé). Les builds se lancent à la main via `install.ksh` (voir *Compiler et lancer*).
- La chaîne de version vit dans `install.ksh` (`VERSION=`), `mcu/include/version.h` et `mcumediaserver.spec` — les garder synchronisées lors d'un incrément.
