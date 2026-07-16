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

- A spcialized MCU API

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

# Command line options

```
mcu [-h|--help] [-f] [-d]
    [--mcu-log <log_file>] [--mcu-pid <pid_file>]
    [--http-port <control_port>] [--rtmp-port <port>]
    [--websocket-port <ws_port>] [--websocket-host hôte]
    [--min-rtp-port <min_port>] [--max-rtp-port port]
    [--vad-period <m>]
```

### Options générales

| Option | Défaut | Description |
|---|---|---|
| `-h`, `--help` | — | Affiche la version et l'aide, puis quitte. |
| `-f` | désactivé | Lance le serveur en démon « safe mode » : double `fork()`, détachement du terminal (`setsid`), puis un processus superviseur relance automatiquement le serveur s'il meurt sur un signal (crash). La sortie standard est redirigée vers le fichier de log et le PID est écrit dans le fichier PID. |
| `-d` | désactivé | Active les logs de debug (`Logger::EnableDebug`). |
| `--mcu-log fichier` | `mcu.log` | Fichier de log (utilisé pour rediriger stdout/stderr en mode démon `-f`). |
| `--mcu-pid fichier` | `mcu.pid` | Fichier où le PID du processus serveur est écrit (mode démon `-f` uniquement). |

### Ports et réseau

| Option | Défaut | Description |
|---|---|---|
| `--http-port port` | `8080` | Port d'écoute du serveur HTTP portant l'API de contrôle XML-RPC (et les flux d'événements HTTP). |
| `--rtmp-port port` | `1935` | Port d'écoute du serveur RTMP. |
| `--websocket-port port` | `9090` | Port d'écoute du serveur WebSocket. |
| `--websocket-host hôte` | *(aucun)* | Nom d'hôte/adresse annoncé dans les URL des endpoints WebSocket (`WSEndpoint::SetLocalHost`). Non listé dans l'aide `--help`. |
| `--min-rtp-port port` | `49152` | Borne basse de la plage de ports UDP allouée aux sessions RTP/RTCP. |
| `--max-rtp-port port` | `65535` | Borne haute de la plage de ports RTP/RTCP. |

### Média

| Option | Défaut | Description |
|---|---|---|
| `--vad-period ms` | `5000` | Période (en millisecondes) de changement de la mosaïque pilotée par la détection d'activité vocale (VAD). |


### Certificat DTLS

Le certificat utilisé pour DTLS-SRTP (WebRTC) n'est **pas** configurable en ligne de commande : les chemins sont fixés en dur à `/etc/mediaserver/mcu.crt` et `/etc/mediaserver/mcu.key`.

# Modernization

## This mediaserver has been updated and modernized using Claude Code

- base media functions has been gathered into a framework called libmedkit to be able to reuse them in other telco servers
- ffmeg is now used whenether it is possible and I intend to use more of it to take advantage of hardware acceleration
- use of C++17 and progressive replacement of older style C++ with std:: stuff.
- removal of some external media processing libraries in favor of ffmpeg and webrtc-audio-processing