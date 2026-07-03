# Mendooze media server fork

This software is a fork of the mendooze media server originaly written by Sergio Murillo Garcia (itself derived from the Medooze / Fontventa projects). It is a multipoint conferencing unit (MCU) / media server maintained by IVèS that mixes and bridges audio, video, text and document-sharing media between Asterisk and SIP/WebRTC endpoints. It has been used as

- MCU
- Mediagateway / webrtc gateway.
- Media server

and can be used to provide these functions. It is controlled remotely over XML-RPC and also speaks RTMP, WebSocket, RTP/SRTP, BFCP and (optionally) RabbitMQ. It supports:

- Bitstream : RTP, SRTP, SRTP-DTLS (Webrtc); NACK, REMB, TMMBR, Text over Websocket
- RTMP (flash related protocol) support
- Audio Codecs : GSM, G.711, G.722, OPUS some others
- Video Codecs : H.263, H.263+, H.264, VP8
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

The `nosign` argument produces an **unsigned** package. Omitting it makes
`rpmbuild` GPG-sign the package with the IVèS key (it clones the private
`gnupg` key repository first, so it only works in the IVèS environment):

```sh
./install.ksh rpm            # GPG-signed package (IVèS only)
```

Under the hood `install.ksh rpm`:

1. sets up the `rpmbuild` macros and directory tree (`./rpmbuild/…`);
2. runs `rpmbuild -bb` on `mcumediaserver.spec` — whose `%build` stage calls
   `install.ksh localcompile` after initialising the submodules;
3. moves the resulting `*.rpm` to the project root and cleans the build tree.

The RPM installs the binary to `/opt/ives/bin/mediaserver`, the SysV init
script to `/etc/init.d/mediaserver` and the configuration to
`/etc/mediaserver/`.

## Running

IVèS deployment convention: symlink the freshly built binary over the
installed one.

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release           # back up the current binary
ln -s /home/user/mediaserver/bin/debug/mcu mediaserver
```

Restart the application:

```sh
/etc/init.d/mediaserver restart
```

Follow the execution:

```sh
tail -f /var/log/mcu.log
```

# Modernization

## This mediaserver has been updated and modernized using Claude Code

- base media functions has been gathered into a framework called libmedkit to be able to reuse them in other telco servers
- ffmeg is now used whenether it is possible and I intend to use more of it to take advantage of hardware acceleration
- use of C++17 and progressive replacement of older style C++ with std:: stuff.
- removal of some external media processing libraries in favor of ffmpeg and webrtc-audio-processing