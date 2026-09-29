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

> **Pas de `-lgtest_main`.** La suite fournit son **propre `main()`**
> (`test_env.cpp`), qui installe l'environnement global de la suite.
> Détails dans [`TEST.md`](../../TEST.md).

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
| `test_rtp_renegotiation.cpp` | `RtpRenegotiation` | — (le trou de l'offre/réponse : un payload type renuméroté par un re-INVITE est rattrapé, un codec retiré ne l'est pas) |
| `test_rtcp_hardening.cpp` | `Rtcp*` (11 suites) | — (suite ADVERSE RTCP : longueurs et compteurs menteurs, page de garde) |
| `test_rtp_header_hardening.cpp` | `RtpHeader` | — (suite ADVERSE de l'en-tête RTP : CSRC et extension qui ne tiennent pas dans le datagramme) |
| `test_red_fec_hardening.cpp` | `RedPayload`, `FecData`, `FecDecoder` | — (suite ADVERSE RED/ULPFEC : blocs sans fin, longueurs de protection mensongères) |
| `test_rtmp_hardening.cpp` | `RtmpMessage`, `RtmpMediaFrame`, `RtmpChunkInput` | — (suite ADVERSE RTMP : message de longueur nulle, trame vide, flux sans message ouvert) |
| `test_websocket_http_hardening.cpp` | `WebSocketHandshake` | — (poignée de main envoyée octet par octet : URL et en-têtes doivent être réassemblés) |
| `test_ws_client_seams.cpp` | `WsClientSeams` | — (coutures du mode client WebSocket : code de statut, reliquat, sérialisation de la requête, transport prêt) |
| `test_ws_client_handshake.cpp` | `WsClientHandshake` | — (connexion WebSocket **sortante** : poignée de main contre le vrai serveur, trame collée au 101, échec avant le 101, masquage sur le fil) |
| `test_ws_client_connect.cpp` | `WsClientConnect` | — (`WebSocketServer::Connect` : une URL `ws://` devient une jambe ouverte, en v4 et en v6 entre crochets ; chemin et query sur le fil ; échec synchrone sans notification, port fermé notifié) |
| `test_ws_client_tls.cpp` | `WsClientTls` | — (jambe `wss://` **sortante** : ouverture et écho dans le tunnel, vérification du pair refusée sans son autorité et acceptée avec, sur une adresse littérale ; certificat et autorité jetables de `wstlsfixture.h`) |
| `test_ws_client_endpoint.cpp` | `WsClientEndpoint` | — (la jambe texte JSR-309 en mode **client** : pontage RTP ↔ WebSocket dans les deux sens, coupure annoncée par U+FFFD des deux côtés et texte de la coupure perdu, reprise indéfinie arrêtée par `End()`, URL inutilisable refusée tout de suite, cible publiée au lieu de l'écoute, ouverture muette abandonnée. **Deux tests durent 5 à 7 s** : ils attendent le backoff de reprise, qui est la chose à prouver) |
| `test_ws_client_xmlrpc.cpp` | `WsClientXmlRpc` | — (la jambe sortante vue du CONTRÔLEUR : `ConnectMediaConnection` recensée dans `jsr309CmdList`, ordre et typage des paramètres, jambe armée qui atteint le pair, média non texte et URL inutilisable refusés, identifiants inconnus, appel mal typé qui ne tue pas le serveur) |
| `test_rate_control.cpp` | `RateControlEstimator`, `RateControlDetector`, `RateControlThrottler`, `RateControlRemb` | — (contrôle de débit, chantier rate-control : les 7 caractérisations du lot 0 ont été levées par le lot 1, les 20 tests sont des garde-fous joués par `make check` ; `make check-ratecontrol` reste le raccourci de la suite) |

## `tools/` — ce qui n'est pas un test

`tools/` ne contient aucun test unitaire et n'est pas compilé (`$(TESTSRCS)` ne
ramasse que `tests/*.cpp`) : c'est l'outillage de la **séance de mesure** du lot 3
du contrôle de débit — injection `tc netem` et dépouillement des traces `BWE:`
d'un appel réel. Voir [`tools/README.md`](tools/README.md).
