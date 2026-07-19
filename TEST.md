# Tests automatisés du mediaserver (mcu) — GoogleTest

Documentation de conception et de référence de la suite de tests du binaire **mcu**
(`mcu/tests/`), bâtie sur **GoogleTest** (paquet système). Cette suite **remplace les
harnais manuels historiques** `mcu/src/rtmptest.cpp` (rejeu d'une capture RTMP) et
`mcu/src/wstest.cpp` (serveur d'écho WebSocket piloté par un client Python externe)
par des tests autonomes, déterministes et assertifs.

Pour lancer les tests, voir le mémo dans `mcu/tests/README.md`. Le présent fichier
couvre la conception, l'organisation et les défauts mis au jour.

## Lancer les tests (rappel)

Depuis le répertoire `mcu/` (là où `install.ksh` invoque `make`) :

```sh
make -f Makefile.rpm check     # compile puis exécute toute la suite
make -f Makefile.rpm tests     # compile seulement (produit tests/runtests)
./tests/runtests               # relancer sans recompiler
./tests/runtests --gtest_filter='RtmpChunk.*'   # filtrer une suite
GTEST_MCU_DEBUG=1 ./tests/runtests              # tracer les Debug() du mcu
```

### Prérequis

- **GoogleTest système** : `gtest` (`pkg-config gtest`). Sur AlmaLinux 9 :
  `dnf install gtest-devel`.
- La cible se lie contre **tous les objets du mcu** (`$(OBJS)` de `Makefile.rpm`,
  soit tout sauf `main.o`/`rtmptest.o`/`wstest.o`), qui doivent donc avoir été
  bâtis au préalable — le plus simple est de lancer d'abord un
  `./install.ksh localcompile` (qui bâtit aussi les sous-modules libmedikit et
  libbfcp). `make check` (re)compile ensuite juste les sources de test et relie.

> **Piège du `main` parasite.** La suite fournit son **propre `main()`**
> (`test_env.cpp`) et **n'utilise pas `-lgtest_main`** : `libwebrtc_audio_processing.so`
> (VAD) exporte un symbole `main` (un outil interne « RTP timing file ») qui,
> `.so` contre `.so`, l'emportait sur celui de `libgtest_main` dans la résolution
> dynamique — la suite ne lançait alors aucun test. Un `main()` défini dans
> l'exécutable est une définition forte et l'emporte toujours.

## Organisation

| Fichier | Suite(s) | Objet | Remplace |
|---|---|---|---|
| `test_env.cpp` | `Smoke` | Environment gtest global (silence des `Debug()`) + `main()` + smoke test | — |
| `test_amf.cpp` | `Amf` | Round-trips AMF0 `Serialize`/`AMFParser::Parse` (Number ±/entiers, Boolean, String/UTF-8, Object, Null) + `U16Parser`/`U32Parser` ; **+ 1 test de caractérisation** du quirk zéro d'`AMFNumber` | rtmptest |
| `test_rtmp_media.cpp` | `RtmpAudio`, `RtmpVideo` | Round-trips `RTMPAudioFrame`/`RTMPVideoFrame` (en-têtes FLV, AAC, AVC) | rtmptest |
| `test_rtmp_chunk.cpp` | `RtmpChunk` | Round-trip de la **couche chunk** : `RTMPChunkOutputStream` → machine à états de dé-chunking (reprise de `rtmptest.cpp`) → `RTMPMessage` réassemblés (découpage multi-chunks, messages consécutifs, commande AMF) | **rtmptest** |
| `test_websocket_frame.cpp` | `WebSocketFrame` | Round-trip de l'en-tête `WebSocketFrameHeader` (longueurs 7/16/64 bits, masque, opcodes de contrôle, parsing fragmenté) | wstest |
| `test_websocket_echo.cpp` | `WebSocketEcho` | **Intégration en-processus** : `WebSocketServer` + `TextEchoWebsocketHandler`, client loopback interne → handshake HTTP Upgrade (101) + écho d'une trame texte masquée | **wstest** |

## Conception

Le principe directeur est le **round-trip** : produire une structure (trame,
message, chunk, en-tête) avec le code de sérialisation du mcu, puis la reparser
avec le code de désérialisation et asserter l'égalité. Cela exerce les deux
sens du même contrat sans dépendre de captures externes. Le seul test qui sort
de ce cadre est l'écho WebSocket, volontairement conservé comme test
d'intégration (sockets + threads réels) pour couvrir fidèlement ce que faisait
`wstest.cpp` + le client Python.

### Tests tolérants à l'environnement

`WebSocketEcho.HandshakeAndTextEcho` ouvre un socket d'écoute loopback et s'y
connecte. En sandbox réseau restreinte (pas de bind/connect loopback possible),
il est **SKIPPÉ** (`GTEST_SKIP`) plutôt qu'échoué.

## Défauts mis au jour par la suite

Les round-trips ont révélé trois bugs latents. **Deux ont été corrigés**, le
troisième est documenté par un test de caractérisation.

1. **`AMFNumber::GetNumber` — signe (CORRIGÉ dans `amf.cpp`).** Le facteur de signe
   s'écrivait `(value>>63|1)`, ce qui suppose un décalage *arithmétique*. Or `value`
   est un `QWORD` (`uint64_t`) : le décalage est **logique**, donc `value>>63 ∈ {0,1}`
   et `|1` vaut toujours 1 → le signe était perdu et **tout AMFNumber négatif était
   décodé positif**. Correctif : porter la mantisse dans un `int64_t` signé puis la
   négativer si le bit de signe est posé. Non-régression : `Amf.NumberNegativeRoundTrip`
   et les valeurs négatives d'`Amf.NumberIntegralValues`.

2. **Machine à états de dé-chunking RTMP (CORRIGÉE dans le helper de test).** La
   machine à états héritée de `rtmptest.cpp` n'avait **pas de transition en fin de
   chunk** → **boucle infinie** sur tout message plus grand que `maxChunkSize` (le
   harnais d'origine ne l'exerçait jamais). Le helper `ParseChunkStream` de
   `test_rtmp_chunk.cpp` ajoute la transition manquante (retour à la lecture d'un
   en-tête de base fmt 3 pour le chunk de continuation) + un garde-fou anti-boucle.

3. **`AMFNumber::GetNumber` — zéro (NON corrigé, caractérisé).** `SetNumber(0)` stocke
   bien des bits nuls, mais `GetNumber()` n'a pas de cas spécial et reconstruit
   `ldexp(2^52, -1075) ≈ 2⁻¹⁰²³` (un dénormal minuscule) au lieu de `0.0`. Un
   AMFNumber valant 0 ne fait donc **pas** un aller-retour exact. Test
   `Amf.NumberZeroDecodeQuirk` : il réussit tant que le bug est présent ; s'il se met
   à échouer, c'est que le décodage de zéro a été corrigé (remplacer alors par un
   `EXPECT_DOUBLE_EQ(decoded, 0.0)`).
