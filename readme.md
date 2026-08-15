# Mendooze media server fork

This software is a fork of the mendooze media server originaly written by Sergio Murillo Garcia (itself derived from the Medooze / Fontventa projects). It is a multipoint conferencing unit (MCU) / media server maintained by IVèS that mixes and bridges audio, video, text and document-sharing media between Asterisk and SIP/WebRTC endpoints. It has been used as

- MCU
- Mediagateway / webrtc gateway.
- Media server

and can be used to provide these functions. It is controlled remotely over XML-RPC and also speaks RTMP, WebSocket, RTP/SRTP, BFCP and (optionally) RabbitMQ. It supports:

- Bitstream : RTP, SRTP, SRTP-DTLS (Webrtc); NACK, REMB, TMMBR, Text over Websocket
- RTMP (flash related protocol) support
- Audio Codecs : GSM, G.711, G.722, OPUS, AAC some others
- Video Codecs : H.263, H.263+, H.264, VP8, AV1
- Realtime text as RFC 4103 with RED support
- BFCP floor control for document / screen sharing

Main functions:

- Media playing and recording using local MP4 files.
- Audiomixer, videomixer, textmixer
- Video layout composition through mosaics, sidebars and picture-in-picture
- Logo and overlay

The codebase is mostly C++ (in `mcu/`) around a shared conference engine (`MCU` → `MultiConf` → participants / mixers), plus three Java companion projects (`jsr309impl/`, `XmlRpcMcuClient/`, `sdp/`). Most of the codec / media plumbing now lives in the **libmedkit** submodule (ffmpeg 5, OpenSSL 3, x264, libsrtp2, webrtc-audio-processing).

## XML-RPC interfaces

The mediaserver exposes three XML-RPC interfaces

- a general purpose JSR309 interface that let an external controller connect and activate all mediaserver resources. It is documented in [xmlrpc_jsr309_api.md](xmlrpc_jsr309_api.md).

- a specialized MCU API, oriented around conferences, participants and video mosaics. It is documented in [MCU-API.md](MCU-API.md).

- other APIs are present but unmaintained.


## Building

This version is intended to run on RHEL 9 / AlmaLinux 9 servers.

All build steps are driven by the `install.ksh` script at the root of the
project. It takes a single argument selecting the action to perform.

### 1. Install the build prerequisites

Here are the most of prerequisites:

- ffmpeg (from RPMFUSION)
- ImageMagic 7 - RPM needs to be rebuilt from source
- webrtc-audio-processing
- libsrtp
- xmlrpc-c

The build links dynamically against system packages. Install them once with:

```sh
./install.ksh prereq
```

This installs (via `dnf`/`yum`): `gsm-devel`, `ffmpeg-devel`,
`webrtc-audio-processing-devel`, `libsrtp-devel` and `xmlrpc-c-devel`
(the last one comes from the *crb* repository). `libtool` is also required.

> Note: `ffmpeg-devel` is provided by the RPMFusion (free and non-free)
> repositories.

### 2. Full local build

```sh
./install.ksh localcompile
```

This one-shot command:

1. checks that the required `-devel` packages are installed;
2. builds the few remaining source-only dependencies into `./staticdeps`
   (`libmp4v2`, `speex`, `libg722_1`);
3. initialises the git submodules if needed (`libmedikit` = codecs,
   `libbfcp` = BFCP floor control) and builds their archives in-tree;
4. builds the `mcu` binary.

The resulting binary is `bin/debug/mcu`.

### Incremental rebuild

Once the dependencies and submodule archives already exist, you can rebuild
just the C++ binary with:

```sh
make -C mcu mcu
```

### Building the submodules individually

If you only need to (re)build one of the in-tree submodules:

```sh
./install.ksh libmedkit   # builds libmedkit.a (codecs)
./install.ksh libbfcp     # builds libbfcp{dbg,rel}.a (BFCP)
```

### Cleaning

```sh
./install.ksh clean
```

This removes the RPM build tree and the previously generated packages, and
runs `make clean` for the `mcu` binary **and for both submodules**
(`libmedikit` and `libbfcp`) — objects, static archives and shared objects —
so the tree is left in a pristine state.

## Building the RPM package

To produce the RPM package (this is what the release build runs):

```sh
./install.ksh rpm nosign
```


```sh
./install.ksh rpm            # GPG-signed package (IVèS only)
```


## Running

The RPM installs the server as a **systemd service** (`mediaserver.service`,
replacing the old SysV `/etc/init.d/mediaserver` script). The binary is
`/opt/ives/bin/mediaserver`, the configuration lives in `/etc/mediaserver/`.

```sh
systemctl start mediaserver          # démarrer
systemctl stop mediaserver           # arrêter (SIGTERM → arrêt propre)
systemctl restart mediaserver        # redémarrer
systemctl status mediaserver         # état
systemctl enable  mediaserver        # démarrage au boot
```

The unit runs the binary **in the foreground** (`Type=simple`) — it does *not*
use the `-f` daemon mode: systemd manages the process lifecycle and PID itself,
and a clean stop is done by sending `SIGTERM` (handled by `signing_handler`,
which flushes the event queues and stops the XML-RPC server). A crash triggers
an automatic restart (`Restart=on-failure`).

### Logs

Standard output/error are captured by systemd. The historical convention is
kept: they are appended to `/var/log/mcu.log`, and they are also available
through the journal:

```sh
tail -f /var/log/mcu.log             # convention historique
journalctl -u mediaserver -f         # via le journal systemd
```

### Command-line options / configuration

Command-line options are set through the `OPTIONS` variable in
`/etc/sysconfig/mediaserver` (sourced by the unit) — e.g.:

```sh
OPTIONS="--http-port 9090 --websocket-port 8100"
```

> ⚠️ Do **not** put `-f` in `OPTIONS`: under systemd the process must stay in
> the foreground. `--mcu-pid` is likewise useless (systemd tracks the PID).

> ⚠️ Behind a NAT, `--public-ip <ip>` is **mandatory** — without it the SDP
> announces the private address and no media flows. See *Adressage*
> below.

After editing the unit or the sysconfig file, reload systemd:

```sh
systemctl daemon-reload && systemctl restart mediaserver
```

# Command line options

```
mcu [-h|--help] [-f] [-d]
    [--mcu-log <log_file>] [--mcu-pid <pid_file>]
    [--http-port <control_port>] [--rtmp-port <port>]
    [--websocket-port <ws_port>] [--websocket-host hôte]
    [--websocket-secure] [--websocket-cert <pem>] [--websocket-key <pem>]
    [--min-rtp-port <min_port>] [--max-rtp-port port]
    [--public-ip <ip>] [--nat <ip>|auto] [--stun-server <hôte[:port]>]
    [--internal-ip <ip>] [--default-profile <profil>]
    [--vad-period <m>]
    [--event-queue-expires <s>]
```

### Options générales

| Option | Défaut | Description |
|---|---|---|
| `-h`, `--help` | — | Affiche la version et l'aide, puis quitte. |
| `-f` | désactivé | Lance le serveur en démon « safe mode » : double `fork()`, détachement du terminal (`setsid`), puis un processus superviseur relance automatiquement le serveur s'il meurt sur un signal (crash). La sortie standard est redirigée vers le fichier de log et le PID est écrit dans le fichier PID. **Ne pas utiliser sous systemd** (voir *Running*) : systemd gère lui-même le cycle de vie et le redémarrage. |
| `-d` | désactivé | Active les logs de debug (`Logger::EnableDebug`). |
| `--mcu-log fichier` | `mcu.log` | Fichier de log (utilisé pour rediriger stdout/stderr en mode démon `-f` uniquement ; sans effet en avant-plan / sous systemd, où stdout/stderr vont dans `/var/log/mcu.log` et le journal). |
| `--mcu-pid fichier` | `mcu.pid` | Fichier où le PID du processus serveur est écrit (mode démon `-f` uniquement ; inutile sous systemd). |

### Ports et réseau

| Option | Défaut | Description |
|---|---|---|
| `--http-port port` | `8080` | Port d'écoute du serveur HTTP portant l'API de contrôle XML-RPC (et les flux d'événements HTTP). |
| `--rtmp-port port` | `1935` | Port d'écoute du serveur RTMP. |
| `--websocket-port port` | `9090` | Port d'écoute du serveur WebSocket. |
| `--websocket-host hôte` | *(aucun)* | Nom d'hôte/adresse annoncé dans les URL des endpoints WebSocket (`WSEndpoint::SetLocalHost`). Non listé dans l'aide `--help`. |
| `--min-rtp-port port` | `49152` | Borne basse de la plage de ports UDP allouée aux sessions RTP/RTCP. |
| `--max-rtp-port port` | `65535` | Borne haute de la plage de ports RTP/RTCP. |
| `--public-ip ip` | *(auto-détectée)* | Adresse du côté **extérieur** : liée si elle est attachée à l'hôte, annoncée dans le SDP (ligne `c=`, candidats ICE) pour les deux API de contrôle. IPv4 **ou IPv6**. **Obligatoire derrière un NAT.** Voir *Adressage* ci-dessous. |
| `--nat ip\|auto` | *(aucun)* | Adresse publique vue de l'extérieur, quand `--public-ip` porte l'adresse **locale** d'un hôte natté (IPv4 seulement). `auto` la découvre par STUN et vérifie que le NAT est **1:1**. |
| `--stun-server hôte[:port]` | `stun.l.google.com:19302` | Serveur interrogé par `--nat auto`. À poser sur son propre serveur en production. |
| `--internal-ip ip` | *(aucune)* | Adresse du côté **interne** (réseau de service, mode SBC). Répétable, au plus une par famille ; en IPv4 elle doit être **RFC 1918**. |
| `--default-profile nom` | `publicv4` | Profil employé par un appel qui n'en demande aucun : `publicv4`, `publicv6`, `internalv4`, `internalv6`. |

### Adressage : les quatre profils

Le serveur peut porter jusqu'à **quatre adresses**, croisement de deux axes — le
côté (**publique**, vers l'extérieur ; **interne**, réseau de service) et la
famille (**IPv4**, **IPv6**) :

| Profil | Option | Contrainte |
|---|---|---|
| `publicv4` | `--public-ip <v4>` | peut être **nattée** (`--nat`) |
| `publicv6` | `--public-ip <v6>` | jamais nattée — pas de NAT IPv6, par choix |
| `internalv4` | `--internal-ip <v4>` | **RFC 1918 exigée**, et attachée à l'hôte |
| `internalv6` | `--internal-ip <v6>` | ULA ou unicast global, attachée à l'hôte |

Chaque profil porte **deux adresses distinctes**, et c'est tout l'intérêt :

- l'adresse **liée** — réellement attachée à une interface. C'est elle que la
  socket média lie, donc elle qui décide de l'interface empruntée ;
- l'adresse **annoncée** — celle que le pair verra dans le SDP. Égale à la
  précédente, **sauf** pour `publicv4` derrière NAT.

Confondre les deux rend un déploiement natté indescriptible : on ne peut pas
annoncer une adresse qu'on ne peut pas lier. C'est ce que faisait l'unique
réglage global d'avant.

**Le contrôleur choisit, appel par appel.** `StartSending`/`StartReceiving` (MCU)
et `EndpointStartSending`/`EndpointStartReceiving` (JSR-309) acceptent un dernier
paramètre facultatif `profile`. Absent, c'est le profil par défaut — donc le
comportement d'un contrôleur qui ignore cette notion. Un profil inconnu ou
indisponible est un **échec explicite**, jamais un repli silencieux : voir
`MCU-API.md` §6.7 bis et `xmlrpc_jsr309_api.md` §6.7 bis.

#### Sans aucune option : auto-détection

Le serveur prend la première adresse annonçable de son nom d'hôte (`/etc/hosts`
puis DNS, enregistrements **A et AAAA**, IPv4 préférée) et en fait son profil
`publicv4`. Cette adresse **peut être une RFC 1918** : « publique » désigne ici le
côté extérieur du serveur, pas la classe de l'adresse. **Aucune détection de NAT
dans ce cas** — rien ne dit qu'il y en a un.

L'auto-détection n'a lieu que si **ni `--public-ip` ni `--internal-ip`** n'est
donné : dès que l'exploitant décrit son adressage, le serveur s'en tient à ce
qu'il a dit. Si rien n'est déterminable, il **refuse de démarrer** — mieux vaut ne
pas démarrer que servir des SDP injoignables appel après appel.

#### Derrière un NAT

```sh
mediaserver --public-ip 192.168.1.10 --nat 203.0.113.12     # adresse publique connue
mediaserver --public-ip 192.168.1.10 --nat auto             # découverte par STUN
```

`--nat auto` interroge un serveur STUN et **vérifie que le NAT est 1:1**. Ce n'est
pas un luxe : le serveur annonce des **ports** RTP, et un NAT qui les translate
rend faux tout ce qu'il publie — le pair émet vers un port que le routeur n'a
jamais ouvert, et l'appel est muet. La sonde est faite **deux fois, depuis deux
ports locaux différents** (une seule ne prouverait rien), et le démarrage échoue
si les ports ne sont pas conservés, avec le détail observé.

Réservé au cas qu'il sert : `--public-ip` doit porter une adresse **RFC 1918
attachée à l'hôte**. Sur une adresse publique il n'y a rien à découvrir.

#### Où le serveur écoute

| Plan | Écoute | Famille |
|---|---|---|
| Média RTP/RTCP | `::` (toutes interfaces), ou l'adresse du profil demandé | dual-stack, ou celle du profil |
| RTMP, WebSocket, TCPEndpoint | `::` (toutes interfaces) | dual-stack |
| **API de contrôle XML-RPC** | **l'adresse interne si `--internal-ip` est donnée**, sinon `::` | dual-stack, ou celle de l'adresse interne |

Toutes les écoutes sont **dual-stack** : une seule socket `AF_INET6` avec
`IPV6_V6ONLY=0`, où un client IPv4 arrive en `::ffff:a.b.c.d`.

> ⚠️ **`--internal-ip` restreint l'API de contrôle.** Elle pilote entièrement le
> serveur média : dès qu'un réseau interne est déclaré, elle ne doit pas rester
> exposée sur une interface publique. Deux conséquences : si les deux profils
> internes sont configurés, la socket ne peut porter qu'une famille et **l'IPv4
> l'emporte** (le démarrage le journalise) ; et la **loopback n'est plus une
> porte d'entrée** — un script local qui tapait `http://127.0.0.1:8080/mcu` doit
> viser l'adresse interne.

#### Contrôles au démarrage

Tous **bloquants**, avec un message destiné à l'exploitant : adresse non
annonçable (loopback, multicast, link-local), adresse interne hors RFC 1918,
adresse interne attachée à aucune interface, deux adresses pour un même profil,
`--nat` en IPv6 ou sans `--public-ip` v4, profil par défaut indisponible. Mieux
vaut un serveur qui ne démarre pas qu'un serveur qui annonce une adresse fausse
pendant six mois.

Le démarrage journalise la table — premier endroit à regarder devant un appel
sans média :

```
-Profils d'adressage :
publicv4 : bind 192.168.1.10, annoncee 203.0.113.12 (NAT) [defaut]
publicv6 : indisponible
internalv4 : bind 172.16.0.5
internalv6 : indisponible
```

> Détail complet du modèle, des arbitrages et de ce que la sonde STUN ne prouve
> **pas** : `ipv6.md` §14.

### Média

| Option | Défaut | Description |
|---|---|---|
| `--vad-period ms` | `5000` | Période (en millisecondes) de changement de la mosaïque pilotée par la détection d'activité vocale (VAD). |

### Files d'événements — expiration des sessions et conférences abandonnées

| Option | Défaut | Description |
|---|---|---|
| `--event-queue-expires s` | `60` | Délai de grâce, en secondes, sans aucun client en long-poll sur une file d'événements, avant destruction de la file **et des objets qui en dépendent**. `0` désactive le nettoyage (comportement historique). S'applique aux **deux API de contrôle** : les `MediaSession` de `/jsr309` et les **conférences** de `/mcu`, chacune liée à une file par son `queueId`. |

Le long-poll du contrôleur sur `/events/jsr309/<queueId>` (ou
`/events/mcu/<queueId>`) sert de **preuve de vie** : il est rétabli en moins
d'une seconde après une coupure et le serveur y émet un keep-alive toutes les
30 s. Soixante secondes sans lecteur, c'est donc un contrôleur mort — et sans ce
nettoyage ses sessions et conférences (endpoints, mixers, threads d'encodage,
ports RTP) vivaient jusqu'au redémarrage du serveur.

Deux signaux, un seul délai :

1. **file toujours là, mais plus lue** → les objets rattachés sont détruits,
   puis la file ;
2. **file détruite explicitement** (`EventQueueDelete`) alors que des objets la
   référencent encore → le délai est **armé**, pas exécuté : les objets ne
   partent qu'à l'échéance, ce qui laisse au contrôleur une chance de revenir.

Trace dans `/var/log/mcu.log` :

```
-JSR309Manager: expiration par event queue armee [grace:60000ms,balayage:10000ms]
-MCU: expiration par event queue armee [grace:60000ms,balayage:10000ms]
-JSR309Manager: suppression de la session 12 [tag:call-42,queue:7] : controleur absent du long-poll
-JSR309Manager: file d'evenements 7 sans poller depuis plus de 60s, destruction [objets:1]
-MCU: file d'evenements 9 detruite mais encore referencee, armement du delai de grace de 60s
-MCU: delai de grace ecoule pour la file d'evenements 9, destruction [objets:1]
```

**La portée du nettoyage est celle du découpage des files choisi par le
contrôleur** : une file par appel/conférence isole les objets entre eux ; une
file partagée (cas du client Java `jsr309impl`, et du montage historique décrit
dans `MCU-API.md` §7.2) les emporte *tous ensemble*. Détail du contrat dans
`xmlrpc_jsr309_api.md` §5 et `MCU-API.md` §5.


### WebRTC — WebSocket sécurisé (wss://)

Le transport WebSocket (utilisé notamment pour le texte temps réel et le canal
de signalisation des endpoints Web) peut être servi en TLS (`wss://`). Ces
options n'affectent **que** le serveur WebSocket ; le média WebRTC (SRTP) est
sécurisé séparément par DTLS (voir ci-dessous).

| Option | Défaut | Description |
|---|---|---|
| `--websocket-secure` | désactivé | Active le WebSocket sécurisé (`wss://`). Implicite dès que `--websocket-cert` ou `--websocket-key` est fourni. |
| `--websocket-cert fichier` | *(certificat DTLS)* | Certificat PEM présenté pour `wss://`. Implique `--websocket-secure`. À défaut, réutilise le certificat DTLS (`/etc/mediaserver/mcu.crt`). |
| `--websocket-key fichier` | *(clé DTLS)* | Clé privée PEM pour `wss://`. Implique `--websocket-secure`. À défaut, réutilise la clé DTLS (`/etc/mediaserver/mcu.key`). |
| `--websocket-host hôte` | *(aucun)* | Nom d'hôte/adresse annoncé dans les URL des endpoints WebSocket (`WSEndpoint::SetLocalHost`). Utile derrière un proxy / en `wss://`. |


### WebRTC — Certificat DTLS-SRTP

Le média WebRTC (audio/vidéo/texte) est chiffré par **DTLS-SRTP**. Le certificat
et la clé utilisés pour la poignée de main DTLS ne sont **pas** configurables en
ligne de commande : les chemins sont fixés en dur à `/etc/mediaserver/mcu.crt` et
`/etc/mediaserver/mcu.key`.

Ce certificat est aussi la valeur par défaut du WebSocket sécurisé (voir plus
haut). Le RPM le génère automatiquement s'il est absent, via le script
`%post` `certcommunication.sh` : un certificat auto-signé **ECDSA P-256**
(signé SHA-256, valable 10 ans), compatible OpenSSL 3 et les navigateurs WebRTC
(le RSA 1024 historique était refusé).

# Modernization

## This mediaserver has been updated and modernized using Claude Code

- base media functions has been gathered into a framework called libmedkit to be able to reuse them in other telco servers
- ffmeg is now used whenether it is possible and I intend to use more of it to take advantage of hardware acceleration
- use of C++17 and progressive replacement of older style C++ with std:: stuff.
- removal of some external media processing libraries in favor of ffmpeg and webrtc-audio-processing