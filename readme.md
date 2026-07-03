# Mendooze media server fork

This software is a fork of the mendooze media server originaly written by Sergio Murillo Garcia. It has been used as

- MCU
- Mediagateway / webrtc gateway.
- Media server

and can be used to provide these functions. It supports:

- Bitstream : RTP, SRTP, SRTP-DTLS (Webrtc); NACK, REMB, TMMBR, Text over Websocket
- RTMP (flash related protocol) support
- Audio Codecs : GSM, G.711, G.722, OPUS some others
- Video Codecs : H.263, H.263+, H.264, VP8
- Realtime text as RFC 4103 with RED support

Main functions:

- Media playing and recording using local MP4 files.
- Audiomixer, videomixer
- Logo and overlay

## XML-RPC interfaces

The mediaserver exposes three XML-RPC interfaces

- a general purpose JSR309 interface that let an external controller connect and activate all mediaserver resources

- A spcialized MCU API

- other APIs are present but unmaintained.


## Compilation

This version is intended to run on RHEL9 / almalinux 9 servers

Un script est present à la racine du projet 'install.ksh'.

Plusieurs arguments sont possibles, pour une compialtion:

```./install.ksh localcompile```

Pour une simple recompilation:

```make -f mcu/Makefile.rpm mcu```

Le binaire resultat de cette compialtion est dans 'bin/debug/' avec comme nom de fichier 'mcu'

## Execution 


Redemarrer l'application:

```/etc/init.d/mediaserver restart```

Pour suivre l'évolution de l'execution:

```tail -f /var/log/mcu.log```

# Modernization

## This mediaserver has been updated and modernized using Claude Code

- base media functions has been gathered into a framework called libmedkit to be able to reuse them in other telco servers
- ffmeg is now used whenether it is possible and I intend to use more of it to take advantage of hardware acceleration
- use of C++17 and progressive replacement of older style C++ with std:: stuff.
- removal of some external media processing libraries in favor of ffmpeg and webrtc-audio-processing