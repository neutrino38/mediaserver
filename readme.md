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
make -f mcu/Makefile.rpm mcu
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
> announces the private address and no media flows. See *Adresse média annoncée*
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
    [--public-ip <ip>]
    [--vad-period <m>]
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
| `--public-ip ip` | *(auto-détectée)* | Adresse IPv4 **annoncée** dans le SDP : ligne `c=` et candidats ICE, pour les deux API de contrôle. **Obligatoire derrière un NAT.** Voir *Adresse média annoncée* ci-dessous. |

### Adresse média annoncée (`--public-ip`)

Les sockets RTP/RTCP sont bindées sur `0.0.0.0` : le serveur n'écoute pas sur une
interface particulière, seul le *port* est choisi (dans la plage `--min/max-rtp-port`).
Il n'y a donc pas d'« IP RTP » à configurer côté écoute — il y a l'adresse que le
serveur **annonce** au pair, et c'est le seul choix à faire.

Cette adresse est un réglage **global** (`RTPSession::SetAnnouncedIp`), partagé par
les deux API de contrôle, qui ne peuvent donc pas annoncer des adresses différentes :

| Où elle sort | API |
|---|---|
| `GetMediaCandidates` → `"rtp://<ip>:<port>"` | JSR-309 (`xmlrpc_jsr309_api.md` §6) |
| `StartReceiving` → `returnVal[1]` | MCU (`MCU-API.md` §4) |

Sans `--public-ip`, elle est **auto-détectée** au démarrage : `gethostname()`, puis
`gethostbyname()` sur ce nom, puis la **première** adresse rendue qui n'est pas
exactement `127.0.0.1`. C'est donc l'adresse du **nom d'hôte** (`/etc/hosts` puis
DNS), pas celle d'une interface. Trois conséquences :

- **derrière un NAT c'est faux par construction** : le nom résout vers l'adresse
  privée, injoignable par le pair. Comme la socket écoute sur `0.0.0.0`, il suffit
  d'annoncer l'adresse publique avec `--public-ip` — rien à re-binder ;
- seule `127.0.0.1` est écartée, pas `127.0.0.0/8` : un nom d'hôte mappé sur
  `127.0.1.1` (courant en conteneur) serait annoncé tel quel ;
- sur un hôte multi-adressé, c'est l'ordre du résolveur qui décide — donc à fixer
  explicitement.

Une adresse fournie qui n'est pas une IPv4 littérale est **refusée** (log d'erreur)
et l'auto-détection s'applique. Si aucune adresse ne peut être déterminée, le
serveur **refuse de démarrer** avec un message indiquant le nom d'hôte en cause et
les deux corrections possibles : mieux vaut ne pas démarrer que servir des SDP
injoignables appel après appel.

L'adresse retenue est écrite dans le log de démarrage, ce qui est le premier
endroit à regarder devant un appel sans média :

```
-RTPSession announced IP set to "203.0.113.12"              # via --public-ip
-RTPSession announced IP auto-detected as "172.21.105.71"   # sans l'argument
```

### Média

| Option | Défaut | Description |
|---|---|---|
| `--vad-period ms` | `5000` | Période (en millisecondes) de changement de la mosaïque pilotée par la détection d'activité vocale (VAD). |


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