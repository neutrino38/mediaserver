# Mendooze media server fork

This software is a fork of the mendooze media server originaly written by Sergio Murillo Garcia (itself derived from the Medooze / Fontventa projects). It is a multipoint conferencing unit (MCU) / media server maintained by IVèS that mixes and bridges audio, video, text and document-sharing media between Asterisk and SIP/WebRTC endpoints. It has been used as

- MCU
- Mediagateway / webrtc gateway.
- Media server

and can be used to provide these functions. It is controlled remotely over XML-RPC and also speaks RTMP, WebSocket, RTP/SRTP, BFCP and (optionally) RabbitMQ. It supports:

- Bitstream : RTP, SRTP, SRTP-DTLS (Webrtc); NACK, REMB, TMMBR, Text over Websocket, `transport_cc`
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

- a general purpose JSR309 interface that let an external controller connect and activate all mediaserver resources. It is documented in [JSR-309-API.md](docs/JSR-309-API.md).

- a specialized MCU API, oriented around conferences, participants and video mosaics. It is documented in [MCU-API.md](docs/MCU-API.md).

- other APIs are present but unmaintained.

## Documentation

| Document | Contenu |
|---|---|
| [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md) | **Configuration réseau, par cas d'usage** : IP publique portée par l'hôte, IP publique nattée 1:1, deux adresses (publique + interne). Ports à ouvrir, vérification, diagnostic. À lire avant tout déploiement. |
| [MCU-API.md](docs/MCU-API.md) | API XML-RPC MCU (conférences, participants, mosaïques) |
| [JSR-309-API.md](docs/JSR-309-API.md) | API XML-RPC JSR-309 |
| [RATE-CONTROL.md](docs/RATE-CONTROL.md) | Contrôle de débit : estimation, feedback RTCP, lissage, images clés |
| [CODECS.md](docs/CODECS.md) | Codecs et paramètres `fmtp` négociés |
| [TEST.md](TEST.md) | Suite de tests du binaire `mcu` |


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
> announces the private address and no media flows. See
> [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md).

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
| `--public-ip ip` | *(auto-détectée)* | Adresse du côté **extérieur**, annoncée dans le SDP. IPv4 **ou IPv6**. **Obligatoire derrière un NAT.** |
| `--nat ip\|auto` | *(aucun)* | Adresse publique vue de l'extérieur, quand `--public-ip` porte l'adresse **locale** d'un hôte natté (IPv4 seulement). `auto` la découvre par STUN. |
| `--stun-server hôte[:port]` | `stun.l.google.com:19302` | Serveur interrogé par `--nat auto`. |
| `--internal-ip ip` | *(aucune)* | Adresse du côté **interne** (réseau de service, mode SBC). **Restreint l'API de contrôle à cette adresse.** |
| `--default-profile nom` | `publicv4` | Profil employé par un appel qui n'en demande aucun : `publicv4`, `publicv6`, `internalv4`, `internalv6`. |

> 📖 **Ces cinq options se configurent ensemble, et le détail est dans un document
> dédié : [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md).** Il procède par
> cas d'usage — adresse publique portée par l'hôte, adresse publique nattée 1:1,
> deux adresses (publique + interne) — et donne les ports à ouvrir, la
> vérification au démarrage et le diagnostic des pannes de média.

### Adressage : le principe en dix lignes

Un serveur média manipule **deux adresses** : celle qu'il **lie** (portée par une
carte de la machine, elle décide de l'interface d'émission) et celle qu'il
**annonce** dans le SDP (celle que le correspondant utilisera pour lui envoyer le
média). Elles sont identiques sur une machine directement exposée, et
**différentes derrière un NAT** — confondre les deux est la panne de
configuration la plus fréquente : l'appel s'établit, aucun média ne circule.

Le serveur décrit donc son adressage sous forme de **profils** — `publicv4`,
`publicv6`, `internalv4`, `internalv6` —, chacun portant ce couple d'adresses. La
plupart des déploiements n'en utilisent qu'un (`publicv4`). Le démarrage
journalise la table, premier endroit à regarder devant un appel sans média :

```
-Profils d'adressage :
publicv4 : bind 192.168.1.10, annoncee 203.0.113.12 (NAT) [defaut]
publicv6 : indisponible
internalv4 : bind 172.16.0.5
internalv6 : indisponible
```

> 📖 **Tout le reste — les trois cas d'usage (IP publique portée par l'hôte, IP
> publique nattée 1:1, deux adresses publique + interne), les ports à ouvrir, la
> vérification, le diagnostic des pannes de média et la liste des contrôles
> bloquants au démarrage — est dans
> [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md).**
>
> Côté contrôleur, le paramètre `profile` de `StartSending`/`StartReceiving` est
> décrit dans `MCU-API.md` §6.7 bis et `JSR-309-API.md` §6.7 bis.

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
`JSR-309-API.md` §5 et `MCU-API.md` §5.


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
