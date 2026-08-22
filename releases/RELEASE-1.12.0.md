# Medooze mediaserver 1.12.0

This release provides a major rework of the MCU function

- Mosaic and image composition is now using ffmpeg instead of in house code
- VAAPI Harware acceleration support (not tested) for encoding, decoding and image composition
- improved conference recording that include realtime text exchanged in the room
- Migration to ImageMagick 7
- improved NAT traversal: RTP latching and natted IP is configurable
- VU meter are removed
