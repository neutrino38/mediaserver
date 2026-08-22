# Medooze mediaserver 1.12.3

C++ refactor, safeguard for dangling confs and media sessions, proper AV1 Video Codec support and Makefile renovation

## Threading and tests

- Wait / WaitQueue rewritten on the stdlib, plus a new Worker base class for active objects; the whole code base was ported onto them
- residual pthread calls entirely removed, replaced with std:: equivalents
- automated / unit tests considerably augmented (199 for the mediaserver, 121 for libmedikit)

## Session lifetime

- Conference and JSR309 session autodestrya conference or a JSR-309 session with no live event queue is destroyed after a configurable timeout, set by `--event-queue-expires` at startup (60 s by default, 0 disables it)
- documented in MCU-API.md
## Speex codec tested

Corrected sample rate size for SPEEK at 16 kHz

## AV1 calls and media relay

- video transcoder gains the dynamic bridging mode audio already had (`allowBridging`),
- AV1 RTP depacketizer added in libmedikit
- the video encoder is now created on its first frame instead of at thread start
- fix: `AudioTranscoder` and `VideoTranscoder` now remove themselves from their source's listeners on `Attach`/`Dettach`/`End`
- an endpoint no longer emits packets it cannot label

## Build and packaging

- single link line for AlmaLinux 9: the dead `DISTRO` / `FEWSTATICDEPS` branches are gone
- ffmpeg is no longer hardcoded anywhere: libmedikit publishes `libmedkit.pc` and the mcu consumes it through `PKG_CONFIG_PATH`
- WebRTC VAD is no longer optional
- `make mcu` and `make check` no longer relink / rebuild everything on every invocation
