# Tests automatisés du mediaserver (mcu) — GoogleTest

Suite de tests unitaires et d'intégration pour le binaire **mcu**, bâtie sur
**GoogleTest**. Elle **remplace les harnais manuels historiques** `mcu/src/rtmptest.cpp`
et `mcu/src/wstest.cpp` par des tests autonomes, déterministes et assertifs.

📖 **Conception, organisation détaillée et défauts découverts : voir
[`TEST.md`](../../TEST.md) à la racine du dépôt.**

## Lancer les tests

Depuis le répertoire `mcu/` (là où `install.ksh` invoque `make`) :

```sh
make check     # compile puis exécute toute la suite
make tests     # compile seulement (produit tests/runtests)
./tests/runtests               # relancer sans recompiler
```

Filtrer une suite ou un test précis :

```sh
./tests/runtests --gtest_filter='RtmpChunk.*'
./tests/runtests --gtest_filter='WebSocketEcho.HandshakeAndTextEcho'
```

Tracer les `Debug()` du mcu pendant les tests (silencieux par défaut) :

```sh
GTEST_MCU_DEBUG=1 ./tests/runtests
```

### Prérequis

- **GoogleTest système** : `gtest` (`pkg-config gtest`). Sur AlmaLinux 9 :
  `dnf install gtest-devel`.
- La cible se lie contre **tous les objets du mcu** (`$(OBJS)` de `mcu/Makefile`),
  qui doivent avoir été bâtis au préalable : lancer d'abord `./install.ksh localcompile`.

> **Piège du `main` parasite.** La suite fournit son **propre `main()`**
> (`test_env.cpp`) et **n'utilise pas `-lgtest_main`** : `libwebrtc_audio_processing.so`
> exporte un symbole `main` qui, `.so` contre `.so`, l'emportait sur celui de
> `libgtest_main`. Détails dans [`TEST.md`](../../TEST.md).

## Fichiers

| Fichier | Suite(s) | Remplace |
|---|---|---|
| `test_env.cpp` | `Smoke` | — |
| `test_amf.cpp` | `Amf` | rtmptest |
| `test_rtmp_media.cpp` | `RtmpAudio`, `RtmpVideo` | rtmptest |
| `test_rtmp_chunk.cpp` | `RtmpChunk` | **rtmptest** |
| `test_websocket_frame.cpp` | `WebSocketFrame` | wstest |
| `test_websocket_echo.cpp` | `WebSocketEcho` | **wstest** |
| `test_rtp_rtcp.cpp` | `Rtp`, `RtpRtcp`, `RtpAdversarial`, `RtcpAdversarial` | — (round-trips RTP + capture `fixtures/rtp_rtcp.pcap` + paquets cassés) |
| `test_mosaic_composition.cpp` | `MosaicGeometry`, `MosaicComposition` | — (géométrie des 12 dispositions + composition avfilter vérifiée pixel à pixel) |
| `test_codec_type.cpp` | `CodecType` | — (le membre `type` d'un codec doit être lisible via un pointeur de base) |
| `test_rtp_latching.cpp` | `RtpLatching` | — (latching RTP symétrique : où le média atterrit réellement, via un socket sonde en loopback) |
