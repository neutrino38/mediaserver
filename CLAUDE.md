# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A multipoint conferencing unit (MCU) / media server, originally derived from Medooze/Fontventa, maintained by IVèS. It mixes audio, video, text and document-sharing media for conferences and bridges them to Asterisk and SIP/WebRTC endpoints. The compiled binary is named `mediaserver` (the build target is `mcu`) and is controlled remotely over XML-RPC; it also speaks RTMP, WebSocket, RTP/SRTP, BFCP and (optionally) RabbitMQ.

The codebase is mostly C++ (in `mcu/`) plus three Java companion projects.

## Build & run

The build is being ported to **AlmaLinux 9 / GCC 11** on branch `feat/alma_linux9` (the current branch) — see `almalinux9_port_plan.md` for status. Most dependencies are now **linked dynamically against system packages** (ffmpeg 5, OpenSSL 3, libsrtp2, x264, Magick++ 7, webrtc-audio-processing). Only two libs are still built from source into `./staticdeps` (mp4v2, g722_1); xmlrpc-c is the dynamic system package (CRB repo) and speex is no longer built (the Speex codec goes through ffmpeg via libmedikit). The bulk of the codec/media code now comes from the **`libmedikit` submodule** (`third_party/fontventa/libmedikit`), which itself carries the ffmpeg-5 port.

```sh
# 1. Install system build deps:
./install.ksh prereq       # dnf/yum: gsm-devel ffmpeg-devel webrtc-audio-processing-devel libsrtp-devel xmlrpc-c-devel

# 2. One-shot build: inits submodules (libmedikit + libbfcp), builds them
#    in-tree, builds source deps into ./staticdeps, then the mcu:
./install.ksh localcompile   # output: bin/debug/mcu

# (individual in-tree submodule builds, if needed:)
./install.ksh libmedkit    # libmedkit.a (codecs), cible 'all', ASTERISK=no
./install.ksh libbfcp      # libbfcp{dbg,rel}.a (BFCP), cible 'all'

# Recompile just the C++ binary after deps already exist:
make -f mcu/Makefile.rpm mcu      # output: bin/debug/mcu

# Build the RPM (what Jenkins runs):
./install.ksh rpm nosign          # omit "nosign" to GPG-sign

# Clean:
./install.ksh clean
```

- The top-level `Makefile` and `config.mk` are **stale/unused** (they reference a non-existent `media/` dir). The real build is `mcu/Makefile.rpm`, driven by `install.ksh`.
- **Two in-tree submodules are mandatory**: `mcu/Makefile.rpm` links `$(MEDKITDIR)/libmedkit.a` (codecs, `-I$(MEDKITDIR)` before `-Iinclude/`; the codec `.o` files it provides were removed from `OBJS`) and `$(BFCPDIR)/lib/libbfcp{dbg,rel}.a` (BFCP C lib, `-I$(BFCPDIR)/include`), both by full path. No `/opt/ives` dependency remains.
- Key build switches in `mcu/Makefile.rpm`: `DEBUG=yes` (default — `-g -O0`, builds into `media/build/debug`, links `libbfcpdbg.a`), `LOG=yes` (`-DLOG_`), `MOTELI=yes` (enables the RabbitMQ/protobuf moteli backend — still source-built, not yet ported to el9), `VADWEBRTC=yes` (VAD via the system `webrtc-audio-processing` APM, `pkg-config`). C++ standard is `gnu++17` (with `-Werror=return-type`).
- `DISTRO` in `Makefile.rpm` only detects `fc`/`el5`/`el6`; the **default (`else`) branch is the AlmaLinux 9 dynamic-link mode** (there is no explicit `el9` yet).
- `mcu.proto` is compiled by `staticdeps/bin/protoc` only when `MOTELI=yes`.

- The RPM spec (`mcumediaserver.spec`) is AlmaLinux 9-only (CentOS 6/el5 compat removed): `%prep` inits the git submodules and `%build` runs `install.ksh localcompile`. It installs a **systemd unit** (`mediaserver.service`, `Type=simple`) via the standard `%systemd_post`/`%systemd_preun`/`%systemd_postun_with_restart` scriptlets — the old SysV `mediaserver.init` was removed. Command-line options live in `/etc/sysconfig/mediaserver` (`OPTIONS=`, sourced by the unit); the binary runs in the foreground (no `-f`), so systemd owns the PID and lifecycle, and stop is a `SIGTERM` (handled by `signing_handler`).

### Running (IVèS deployment convention)

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release          # back up current binary
ln -s /home/user/mediaserver/bin/debug/mcu mediaserver
systemctl restart mediaserver
tail -f /var/log/mcu.log                     # follow execution (also: journalctl -u mediaserver -f)
```

RPM installs the binary to `/opt/ives/bin/mediaserver`, the systemd unit to `%{_unitdir}/mediaserver.service`, its options to `/etc/sysconfig/mediaserver`, config to `/etc/mediaserver/`. stdout/stderr are appended to `/var/log/mcu.log` by systemd (`StandardOutput`/`StandardError`).

The `mcu` binary itself has no automated test suite (`rtmptest` is a standalone manual test target in `mcu/Makefile.rpm`). The **`libmedikit` submodule does** have a GoogleTest suite under `third_party/fontventa/libmedikit/tests/` (unit + adversarial tests for the negotiator, codecs, parsers, and MP4 read/round-trip): run it with `make -C third_party/fontventa/libmedikit check` (needs system `gtest`; builds `libmedkit.a` `ASTERISK=no`). See `tests/README.md` there and `libmedikit_tests_plan.md` at the repo root.

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
- Codecs now come from the **`libmedikit` submodule**, not `mcu/src`: the per-codec subdirs (`g711`, `g722`, `gsm`, `speex`, `opus`, `h263`, `h264`, `aac`, `amr`, `vp8`…) were removed and the classes are provided by `medkit/*` (`FfAudioEncoder`/`FfVideoEncoder`, `PCMUEncoder`, etc.) wrapping ffmpeg/x264 behind the interface in `include/codecs.h`. Only `mcu/src`-local media bits that libmedikit doesn't cover remain (e.g. `nelly`, `vp6`, `flv1` where still referenced). Audio resampling goes through **libswresample** (ffmpeg); the old speexdsp `AudioTransrater` is gone.
- MP4 record/play: `mp4recorder`/`mp4player`/`mp4streamer` are thin shells over libmedikit's **`mp4writer`/`mp4reader`** (libmp4v2 underneath). FLV/RTMP recording and streaming (`FLVEncoder`, `flvrecorder`, `rtmpflvstream`) stay in `mcu`.
- VAD uses the system **`webrtc-audio-processing`** APM (`VoiceDetection`); the old vendored WebRTC trunk was removed.
- `mediagateway`/`mediabridgesession` bridge between transports (the `mediagw` target).
- `src/bfcp/` implements the C++ object BFCP API (floor control for document/screen sharing) on top of the C-level `libbfcp`, now built in-tree from the **`third_party/libbfcp` submodule** (headers `-I$(BFCPDIR)/include`, archive `libbfcp{dbg,rel}.a` linked by full path — no more `/opt/ives`).

Headers are in `mcu/include/`, sources in `mcu/src/`. The object list and link flags in `mcu/Makefile.rpm` are the source of truth for what actually gets compiled.

### Java companion projects (NetBeans/Ant — each has `build.xml` + `nbproject/`)

- `jsr309impl/` — JSR-309 (Media Server Control API) implementation under `org.murillo.mscontrol`. Drives the C++ MCU from a Java/JSR-309 app server; talks to it via the XmlRpcMcuClient.
- `XmlRpcMcuClient/` — Java client library for the C++ server's XML-RPC control API.
- `sdp/` — SDP parsing/manipulation library (large, ~270 files) used by the JSR-309 layer.

Build these with `ant` in each directory (not part of the RPM build).

## Conventions & gotchas

- The codebase is old C++ (mixed `.c`/`.cpp`, now built as `gnu++17`) with heavy use of raw threads, `std::wstring` for names/tags, and manual memory management. Match the surrounding style. Migration to `std::thread`/`std::atomic` and `std::mutex` locks is ongoing.
- **Memory-ownership model (smart-pointer migration, see `smart_pointers_plan.md`)** — the target pattern for the "manager owns a map of sessions" subsystems (`MCU`/`MultiConf`, `JSR309Manager`/`MediaSession`, `MediaGateway`/`MediaBridgeSession`, `Broadcaster`/`BroadcastSession`) is now: the owning map holds a `shared_ptr` per entry; `Get*Ref(id, shared_ptr&)` hands back a **copy** of the `shared_ptr` (no more `numRef`/`enabled` counters, no `Release*Ref`); `Delete*` extracts the `shared_ptr` under the lock, erases the map entry (so no new refs can be handed out), then calls `End()` (idempotent) **outside** the lock — the last surviving `shared_ptr` destroys the object, so in-flight handler refs stay safe. `Connect()` (RTMP) returns the session `shared_ptr` (or an aliasing `shared_ptr` sharing the session's ownership) so `RTMPConnection::app` keeps the session alive for the connection's lifetime. Exclusive members are `unique_ptr` (`MultiConf::recorder`, `players`, `PublisherInfo::stream/conn`, `MP4Player` decoders, `Mosaic::overlay`/`overlays`, `MediaSession::recorderTimers`); observers/back-pointers are raw or `weak_ptr` with a `.lock()` at the use site (`NetStream::part`, `VideoStream::rtpSession`, `RecorderTimer→MediaSession`). Phases 0–5 are done; `use.h` (`Use`/`IncUse`/`WaitUnusedAndLock`) is still load-bearing in ~20 files (participant/mixer/rtp hot paths) and is being retired progressively, not wholesale.
- Comments and commit messages are predominantly in **French**.
- Most media/codec code now lives in the **`libmedikit` submodule** (`third_party/fontventa/libmedikit`), not `mcu/src` — check there before assuming a codec is local. Two source deps (mp4v2, g722_1) are still vendored as static builds in `staticdeps/`; everything else is a dynamic system package.
- No CI pipeline is checked in (the old Jenkins `Jenkinsfile`, a CentOS6 matrix, has been removed). Builds are driven manually via `install.ksh` (see Build & run).
- Version string lives in `install.ksh` (`VERSION=`), `mcu/include/version.h`, and `mcumediaserver.spec` — keep them in sync when bumping.
