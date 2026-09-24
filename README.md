# Medooze media server fork

This software is a fork of the Medooze media server originally written by Sergio Murillo Garcia (itself derived from the Medooze / Fontventa projects). It is a multipoint conferencing unit (MCU) / media server maintained by IVèS that mixes and bridges audio, video, text and document-sharing media between Asterisk and SIP/WebRTC endpoints. It has been used as

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


## XML-RPC interfaces

The mediaserver exposes two maintained XML-RPC interfaces

- a general purpose JSR309 interface that let an external controller connect and activate all mediaserver resources. It is documented in [JSR-309-API.md](docs/JSR-309-API.md).

- a specialized MCU API, oriented around conferences, participants and video mosaics. It is documented in [MCU-API.md](docs/MCU-API.md).

- other APIs are present but unmaintained.

## Documentation

| Document | Content |
|---|---|
| [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md) | **Network configuration, by use case**: public IP carried by the host, public IP behind a 1:1 NAT, two addresses (public + internal). Ports to open, verification, diagnosis. Read it before any deployment. |
| [MCU-API.md](docs/MCU-API.md) | MCU XML-RPC API (conferences, participants, mosaics) |
| [JSR-309-API.md](docs/JSR-309-API.md) | JSR-309 XML-RPC API |
| [RATE-CONTROL.md](docs/RATE-CONTROL.md) | Rate control: estimation, RTCP feedback, smoothing, key frames |
| [CODECS.md](docs/CODECS.md) | Codecs and negotiated `fmtp` parameters |
| [TEST.md](TEST.md) | Unit testing `mcu` |


## Building

The codebase is mostly C++ (in `mcu/`). Most of the codec / media plumbing now lives in the **libmedkit** framework. It
contains what used to be the base classes of the mediaserver and has been turned into some kind of C++ FFMPEG based media
framework. It is managed as a submodule. Voice activity detection comes from the **libvad** submodule (libfvad).

This version is intended to run on RHEL 9 / AlmaLinux 9 servers and runs on Debian / Ubuntu.

All build steps are driven by the `install.ksh` script at the root of the project. It takes a single argument selecting the action to perform. It detects the distribution family (`rpm` or `dpkg`) and picks the package names itself.

### 1. Install the build prerequisites


The build links dynamically against system packages. Install them once with:

```sh
./install.ksh prereq
```

### 2. Full local build

```sh
./install.ksh localcompile
```

This one-shot command:

1. checks that the required `-devel` packages are installed;
2. builds the only remaining source-only dependency into `./staticdeps`
   (`libmp4v2`);
3. initialises the git submodules if needed (`libmedikit` = codecs,
   `libvad` = voice activity detection) and
   builds their archives in-tree;
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
./install.ksh libvad      # builds libfvad.a (voice activity detection)
```

### Cleaning

```sh
./install.ksh clean
```

This removes the RPM build tree and the previously generated packages, and
runs `make clean` for the `mcu` binary **and for both submodules**
(`libmedikit` and `libvad`) — objects, static archives and shared
objects — so the tree is left in a pristine state.


### Building the Debian package

```sh
./install.ksh localcompile      # the package ships bin/debug/mcu
./install.ksh deb               # -> mcumediaserver_<version>_<arch>.deb
```

It installs the same files as the RPM, with two Debian conventions: the
command-line options live in `/etc/default/mediaserver` (the systemd unit reads
both that file and `/etc/sysconfig/mediaserver`, whichever exists) and the unit
goes to `/lib/systemd/system`. The `Depends:` field is **computed from the
binary itself** (its `DT_NEEDED` entries mapped to packages), so it follows the
ffmpeg version you built against instead of a hand-written list going stale.

The package is unsigned, and no APT repository publishes it: install it with
`sudo apt install ./mcumediaserver_<version>_<arch>.deb`.

## Building the RPM package

To produce the RPM package (this is what the release build runs):

```sh
./install.ksh rpm nosign
```

## Running

The RPM installs the server as a **systemd service** (`mediaserver.service`,
replacing the old SysV `/etc/init.d/mediaserver` script). The binary is
`/opt/ives/bin/mediaserver`, the configuration lives in `/etc/mediaserver/`.

```sh
systemctl start mediaserver          # start
systemctl stop mediaserver           # stop (SIGTERM → clean shutdown)
systemctl restart mediaserver        # restart
systemctl status mediaserver         # status
systemctl enable  mediaserver        # start at boot
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
tail -f /var/log/mcu.log             # historical convention
journalctl -u mediaserver -f         # through the systemd journal
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
    [--websocket-port <ws_port>] [--websocket-host host]
    [--websocket-secure] [--websocket-cert <pem>] [--websocket-key <pem>]
    [--websocket-client-insecure] [--websocket-client-ca <pem>]
    [--min-rtp-port <min_port>] [--max-rtp-port port]
    [--public-ip <ip>] [--nat <ip>|auto] [--stun-server <host[:port]>]
    [--internal-ip <ip>] [--default-profile <profile>]
    [--vad-period <m>]
    [--event-queue-expires <s>]
```

### General options

| Option | Default | Description |
|---|---|---|
| `-h`, `--help` | — | Prints the version and the help, then exits. |
| `-f` | disabled | Runs the server as a "safe mode" daemon: double `fork()`, detach from the terminal (`setsid`), then a supervisor process restarts the server automatically if it dies on a signal (crash). Standard output is redirected to the log file and the PID is written to the PID file. **Do not use under systemd** (see *Running*): systemd manages the lifecycle and the restarts itself. |
| `-d` | disabled | Enables debug logs (`Logger::EnableDebug`). |
| `--mcu-log file` | `mcu.log` | Log file (used to redirect stdout/stderr in `-f` daemon mode only; no effect in the foreground / under systemd, where stdout/stderr go to `/var/log/mcu.log` and the journal). |
| `--mcu-pid file` | `mcu.pid` | File where the PID of the server process is written (`-f` daemon mode only; useless under systemd). |

### Ports and network

| Option | Default | Description |
|---|---|---|
| `--http-port port` | `8080` | Listening port of the HTTP server carrying the XML-RPC control API (and the HTTP event streams). |
| `--rtmp-port port` | `1935` | Listening port of the RTMP server. |
| `--websocket-port port` | `9090` | Listening port of the WebSocket server. |
| `--websocket-host host` | *(none)* | Host name/address announced in the WebSocket endpoint URLs (`WSEndpoint::SetLocalHost`). Not listed by `--help`. |
| `--min-rtp-port port` | `49152` | Lower bound of the UDP port range allocated to RTP/RTCP sessions. |
| `--max-rtp-port port` | `65535` | Upper bound of the RTP/RTCP port range. |
| `--public-ip ip` | *(auto-detected)* | Address of the **outside**, announced in the SDP. IPv4 **or IPv6**. **Mandatory behind a NAT.** |
| `--nat ip\|auto` | *(none)* | Public address seen from the outside, when `--public-ip` carries the **local** address of a NATed host (IPv4 only). `auto` discovers it through STUN. |
| `--stun-server host[:port]` | `stun.l.google.com:19302` | Server queried by `--nat auto`. |
| `--internal-ip ip` | *(none)* | Address of the **inside** (service network, SBC mode). **Restricts the control API to this address.** |
| `--default-profile name` | `publicv4` | Profile used by a call that requests none: `publicv4`, `publicv6`, `internalv4`, `internalv6`. |

> 📖 **These five options are configured together, and the details are in a
> dedicated document: [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md).**
> It goes by use case — public address carried by the host, public address
> behind a 1:1 NAT, two addresses (public + internal) — and gives the ports to
> open, the check at startup and the diagnosis of media failures.

### Addressing: the principle in ten lines

A media server handles **two addresses**: the one it **binds** (carried by a
network card of the machine, it decides the sending interface) and the one it
**announces** in the SDP (the one the peer will use to send it media). They are
identical on a directly exposed machine, and **different behind a NAT** —
mixing them up is the most frequent configuration failure: the call is set up,
no media flows.

The server therefore describes its addressing as **profiles** — `publicv4`,
`publicv6`, `internalv4`, `internalv6` —, each carrying this pair of addresses.
Most deployments use only one (`publicv4`). The startup logs the table, the
first place to look at when a call has no media:

```
-Profils d'adressage :
publicv4 : bind 192.168.1.10, annoncee 203.0.113.12 (NAT) [defaut]
publicv6 : indisponible
internalv4 : bind 172.16.0.5
internalv6 : indisponible
```

> 📖 **Everything else — the three use cases (public IP carried by the host,
> public IP behind a 1:1 NAT, two addresses public + internal), the ports to
> open, the verification, the diagnosis of media failures and the list of
> blocking checks at startup — is in
> [NETWORK-CONFIGURATION.md](docs/NETWORK-CONFIGURATION.md).**
>
> On the controller side, the `profile` parameter of `StartSending`/`StartReceiving`
> is described in `MCU-API.md` §6.7 bis and `JSR-309-API.md` §6.7 bis.

### Media

| Option | Default | Description |
|---|---|---|
| `--vad-period ms` | `5000` | Period (in milliseconds) of the mosaic changes driven by voice activity detection (VAD). |

### Event queues — expiry of abandoned sessions and conferences

| Option | Default | Description |
|---|---|---|
| `--event-queue-expires s` | `60` | Grace period, in seconds, without any long-poll client on an event queue, before the queue **and the objects that depend on it** are destroyed. `0` disables the cleanup (historical behaviour). Applies to **both control APIs**: the `MediaSession`s of `/jsr309` and the **conferences** of `/mcu`, each bound to a queue by its `queueId`. |

The controller's long-poll on `/events/jsr309/<queueId>` (or
`/events/mcu/<queueId>`) acts as a **proof of life**: it is re-established in
less than a second after a disconnection and the server sends a keep-alive on it
every 30 s. Sixty seconds without a reader therefore means a dead controller —
and without this cleanup its sessions and conferences (endpoints, mixers,
encoding threads, RTP ports) lived until the server restarted.

Two signals, a single delay:

1. **queue still there, but no longer read** → the attached objects are
   destroyed, then the queue;
2. **queue explicitly destroyed** (`EventQueueDelete`) while objects still
   reference it → the delay is **armed**, not executed: the objects only go
   away when it expires, which gives the controller a chance to come back.


### WebRTC — secure WebSocket (wss://)

The WebSocket transport (used in particular for real-time text and the
signalling channel of Web endpoints) can be served over TLS (`wss://`).

| Option | Default | Description |
|---|---|---|
| `--websocket-secure` | disabled | Enables the secure WebSocket (`wss://`). Implied as soon as `--websocket-cert` or `--websocket-key` is given. |
| `--websocket-cert file` | *(DTLS certificate)* | PEM certificate presented for `wss://`. Implies `--websocket-secure`. By default, reuses the DTLS certificate (`/etc/mediaserver/mcu.crt`). |
| `--websocket-key file` | *(DTLS key)* | PEM private key for `wss://`. Implies `--websocket-secure`. By default, reuses the DTLS key (`/etc/mediaserver/mcu.key`). |
| `--websocket-host host` | *(none)* | Host name/address announced in the WebSocket endpoint URLs (`WSEndpoint::SetLocalHost`). Useful behind a proxy / with `wss://`. |
| `--websocket-client-ca file` | *(system store)* | Additional PEM certificate authority, accepted for the `wss://` servers the mediaserver **calls**. Added to the system store. |
| `--websocket-client-insecure` | disabled | Do **not** verify the certificate of the `wss://` servers called. For debugging only: the leg becomes open to interception. |


### WebRTC — DTLS-SRTP certificate

WebRTC media (audio/video/text) is encrypted by **DTLS-SRTP**. The certificate
and the key used for the DTLS handshake are **not** configurable on the command
line: the paths are hard-coded to `/etc/mediaserver/mcu.crt` and
`/etc/mediaserver/mcu.key`.

This certificate is also the default for the secure WebSocket (see above). The
RPM generates it automatically if it is missing, through the `%post` script
`certcommunication.sh`: a self-signed **ECDSA P-256** certificate (SHA-256
signed, valid for 10 years), compatible with OpenSSL 3 and WebRTC browsers (the
historical RSA 1024 one was rejected).

# Modernization

## This mediaserver has been updated and modernized using Claude Code

- base media functions have been gathered into a framework called libmedkit to be able to reuse them in other telco servers
- ffmpeg is now used whenever it is possible and I intend to use more of it to take advantage of hardware acceleration
- use of C++17 and progressive replacement of older style C++ with std:: stuff.
- removal of some external media processing libraries in favor of ffmpeg and libfvad
