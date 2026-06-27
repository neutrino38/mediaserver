# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A multipoint conferencing unit (MCU) / media server, originally derived from Medooze/Fontventa, maintained by IVèS. It mixes audio, video, text and document-sharing media for conferences and bridges them to Asterisk and SIP/WebRTC endpoints. The compiled binary is named `mediaserver` (the build target is `mcu`) and is controlled remotely over XML-RPC; it also speaks RTMP, WebSocket, RTP/SRTP, BFCP and (optionally) RabbitMQ.

The codebase is mostly C++ (in `mcu/`) plus three Java companion projects.

## Build & run

Builds target **CentOS/RHEL with a custom `/opt/ives` toolchain** and many statically-linked dependencies. There is no quick local build without that environment.

```sh
# Full local compile: builds all static deps (openssl, mp4v2, speex, opus,
# srtp, vpx, g722_1, webrtc VAD, xmlrpc-c...) into ./staticdeps, then the mcu.
./install.ksh localcompile

# Recompile just the C++ binary after deps already exist:
make -f mcu/Makefile.rpm mcu      # output: bin/debug/mcu

# Build the RPM (what Jenkins runs):
./install.ksh prereq              # yum install gsm-devel ffmpeg-devel
./install.ksh rpm nosign          # omit "nosign" to GPG-sign

# Clean:
./install.ksh clean
```

- The top-level `Makefile` and `config.mk` are **stale/unused** (they reference a non-existent `media/` dir). The real build is `mcu/Makefile.rpm`, driven by `install.ksh`.
- Key build switches in `mcu/Makefile.rpm`: `DEBUG=yes` (default — `-g -O0`, builds into `media/build/debug`, links `libbfcpdbg.a`), `LOG=yes` (`-DLOG_`), `MOTELI=yes` (enables the RabbitMQ/protobuf moteli backend), `VADWEBRTC=yes` (WebRTC voice-activity detection). C++ standard is `gnu++0x`.
- `mcu.proto` is compiled by `staticdeps/bin/protoc` only when `MOTELI=yes`.

### Running (IVèS deployment convention)

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release          # back up current binary
ln -s /home/user/mediaserver/bin/debug/mcu mediaserver
/etc/init.d/mediaserver restart
tail -f /var/log/mcu.log                     # follow execution
```

RPM installs the binary to `/opt/ives/bin/mediaserver`, init script to `/etc/init.d/mediaserver`, config to `/etc/mediaserver/`.

There is no automated test suite. `rtmptest` is a standalone manual test target in `mcu/Makefile.rpm`.

## Architecture

### C++ media server (`mcu/`)

Entry point `mcu/src/main.cpp` starts several servers that all share the conference engine:
- **XML-RPC server** (`xmlrpcserver`, `xmlhandler`) — the primary control API. Command tables (`mcuCmdList`, `broadcasterCmdList`, `mediagatewayCmdList`, `jsr309CmdList`) map RPC method names to handlers in `xmlrpcmcu.cpp`, `xmlrpcbroadcaster.cpp`, etc.
- **RTMP server** (`rtmpserver`) and **WebSocket server** (`websocketserver`) for Flash/web media transport.
- Optional **RabbitMQ/moteli** backend (`src/moteli/`, `-DMOTELI`) carrying protobuf messages (`mcu.proto`) for the MOTELI project.

Core conference model:
- `MCU` (`mcu.h`/`mcu.cpp`) — top-level manager; owns conferences and event queues. `CreateConference`, `GetConferenceRef`. Implements `MultiConf::Listener` to receive participant events (FPU requests, doc sharing).
- `MultiConf` (`multiconf.cpp`) — a single conference. Manages participants, mosaics, sidebars, players. `CreateParticipant`, `CreateMosaic`, `CreateSidebar`, `SetVADMode`, etc.
- **Participants** are `RTPParticipant` or `RTMPParticipant` (RTP/SRTP vs RTMP transports).
- **Mixers**: `videomixer`/`audiomixer`/`textmixer` combine participant streams. Video layout is composed via **mosaics** (`mosaic`, `partedmosaic`, `asymmetricmosaic`, `pipmosaic`) and `sidebar`/`overlay`/`logo`.
- **Streams & pipes**: `audiostream`/`videostream`/`textstream` wrap RTP sessions; `pipe{audio,video,text}{input,output}` move raw media between decoders, mixers and encoders.

Media plumbing:
- `rtpsession`, `rtp`, `RTPSmoother`, `dtls` (SRTP keying), `stunmessage`, `fecdecoder`, `red`/`redcodec` for the transport layer.
- Codecs live in per-codec subdirs under `mcu/src/`: `g711`, `g722`, `gsm`, `speex`, `opus`, `nelly` (audio); `h263`, `h264`, `vp6`, `vp8`, `flv1` (video). Each wraps an external lib (ffmpeg/x264/libvpx/...) behind a common codec interface in `include/codecs.h`.
- MP4 record/play (`mp4recorder`, `mp4player`, `mp4streamer`) via libmp4v2; FLV/RTMP recording and streaming (`FLVEncoder`, `flvrecorder`, `rtmpflvstream`).
- `mediagateway`/`mediabridgesession` bridge between transports (the `mediagw` target).
- `src/bfcp/` implements BFCP floor control (binary-floor-control for document/screen sharing), linked against the external `libbfcp`.

Headers are in `mcu/include/`, sources in `mcu/src/`. The object list and link flags in `mcu/Makefile.rpm` are the source of truth for what actually gets compiled.

### Java companion projects (NetBeans/Ant — each has `build.xml` + `nbproject/`)

- `jsr309impl/` — JSR-309 (Media Server Control API) implementation under `org.murillo.mscontrol`. Drives the C++ MCU from a Java/JSR-309 app server; talks to it via the XmlRpcMcuClient.
- `XmlRpcMcuClient/` — Java client library for the C++ server's XML-RPC control API.
- `sdp/` — SDP parsing/manipulation library (large, ~270 files) used by the JSR-309 layer.

Build these with `ant` in each directory (not part of the RPM build).

## Conventions & gotchas

- The codebase is old C++ (mixed `.c`/`.cpp`, `gnu++0x`) with heavy use of raw threads, `std::wstring` for names/tags, and manual memory management. Match the surrounding style.
- Comments and commit messages are predominantly in **French**.
- Many "dependencies" are vendored as static builds produced by `install.ksh` into `staticdeps/`; don't expect system packages to satisfy them.
- CI is Jenkins (`Jenkinsfile`): a CentOS6 matrix that runs `install.ksh prereq` then `install.ksh rpm nosign`, archiving the `.rpm` on tag builds and notifying via Office365 webhook.
- Version string lives in `install.ksh` (`VERSION=`), `mcu/include/version.h`, and `mcumediaserver.spec` — keep them in sync when bumping.
