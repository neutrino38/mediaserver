# Plan — dépaquetiser le RTP est un travail de bibliothèque, pas de serveur

> Statut : **étude**. Aucune ligne de code n'a été écrite POUR ce document. Il
> répond à une seule question : peut-on remplacer les dépaquetiseurs RTP maison
> par ceux de ffmpeg, et à quel prix ? La réponse est en §6 ; elle est nuancée,
> et le critère qui la renverserait est écrit. Le travail AV1 des commits
> `254b0a7` et `5134fb8` lui est antérieur et indépendant : il n'en est pas une
> mise en œuvre, mais il fournit deux des faits qui le décident (§1, §6.3).

**Le principe d'abord.** Un format de payload RFC est une spécification publique
qu'un tiers a déjà lue, testée sur du trafic réel et corrigée pendant vingt ans.
Chaque fois que nous la relisons nous-mêmes, nous ne gagnons rien et nous
héritons de la dette. C'est la même règle que celle du `CLAUDE.md` sur les
capacités codec — *ce qui est connu ailleurs, on le demande ailleurs* — appliquée
au sens inverse : ici ce n'est pas le serveur qui est la source de vérité, c'est
la RFC, et la meilleure implémentation disponible de la RFC.

La règle a une limite, et tout ce document consiste à la trouver : ffmpeg ne
publie **pas** ses dépaquetiseurs. Ils existent, ils sont bons, ils sont dans
le binaire que nous lions déjà — et ils sont derrière un en-tête privé.

Ce changement ne touche **aucune API de contrôle** (ni `/mcu`, ni `/jsr309`) :
pas de mise à jour `moteli_*.proto` à prévoir.

---

## 1. Pourquoi la question se pose : chaque format a coûté une panne, une par une

Les faits sont dans l'historique du sous-module `libmedikit` (branche
`geat/av_frame`), pas dans une impression générale.

| commit | date | ce que c'était |
|---|---|---|
| `48e2fd3` | 2018-01-19 | « Trame I parasite ! » — 2 lignes dans `h264depacketizer.cpp` : la détection d'image clé |
| `6e534d0` | 2018-01-19 | « Amelioration des controles dans le depaketizer » — +45 lignes de contrôles de bornes |
| `6e5e47e` | 2023-03-16 | « Corrections pour gerer les modes STAP-X » |
| `e9f146b` | 2023-03-17 | « Corrections pour gerer les modes STAP-x dans le depacketiseur H264 » — **337 insertions / 277 suppressions** dans un fichier de 614 lignes : le chemin STAP-A réécrit onze ans après l'écriture du fichier |
| `d67252e` | 2026-06-30 | le dépaquetiseur RTP entre dans libmedkit sous forme de **monolithe de 230 lignes** dans `ffvideocodec.cpp` |
| `e6883b1` | 2026-06-30 | le même jour, il est éclaté par codec (−217 lignes dans `ffvideocodec.cpp`, +144 `h264decoder`, +95 `vp8decoder`, +59 `h263codec`) |
| `9b5ff89` | 2026-07-30 | « h264: refermer la FU-A même sans bit E (échantillon AVCC corrompu) » |
| `254b0a7` | 2026-08-12 | `av1/av1depacketizer.{h,cpp}` — 445 lignes + 557 lignes de tests, écrites le jour de la panne |
| `5134fb8` | 2026-08-12 | `av1/av1packetizer.{h,cpp}` + `av1obu.{h,cpp}` — le sens émission, et la boîte à outils OBU factorisée |

Deux de ces entrées méritent d'être lues en entier, parce qu'elles sont le coût
mesuré, pas le coût redouté.

**`9b5ff89`.** L'ouverture d'une FU-A réserve, en AVCC, un préfixe de longueur
que seul le fragment `E=1` permettait de renseigner. Chez les émetteurs qui ne
posent pas le bit E — et sur simple perte du dernier fragment — le préfixe
restait à sa valeur d'attente, qui valait **1** : soit les octets
`00 00 00 01`, indistinguables d'un start code Annex-B. L'échantillon MP4 était
définitivement illisible (ffmpeg lui-même se désynchronise dessus) alors que les
données H.264 étaient intactes. Neuf ans après l'écriture du fichier.

**AV1, 2026-08-12.** L'accumulation brute de `FfVideoDecoder::DecodePacket`
donnait à libdav1d l'octet d'agrégation RTP en tête de charge ; lu comme un
`obu_header`, il annonçait des types d'OBU inexistants (« Unknown OBU type 11 »)
et pas une image n'était jamais décodée, sur un appel AV1 ↔ AV1 par ailleurs
parfaitement négocié. Il a fallu écrire 445 lignes de dépaquetiseur **parce que
ffmpeg 5.1 n'en a pas** (§3). C'est le contre-exemple central de ce document :
la panne la plus récente est précisément celle que ffmpeg n'aurait pas évitée.

À ce coût s'ajoute un coût statique, celui-là mesurable à la ligne :

- **`h264/h264decoder.cpp:241–468` — 228 lignes de dépaquetiseur H.264 mort**,
  encadrées par un `#if 0` / `#endif`. C'est une **troisième** lecture de la
  RFC 6184 dans l'arbre, jamais compilée, jamais testée, jamais supprimée.
- **Deux lectures vivantes de la RFC 6184 qui ne portent pas la même
  information** (détail en §4.3) : `h264_append_nals`
  (`h264decoder.cpp:103–212`, sortie Annex-B, pour décoder) et
  `H264Depacketizer::AddPayload` (`h264/h264depacketizer.cpp:73–400`, sortie
  AVCC + carte de fragmentation, pour enregistrer).
- `H264Depacketizer::SetUseStartCode` (`h264depacketizer.h:22`) n'a **aucun
  appelant** dans tout l'arbre : un mode de sortie configurable, mort, qui
  traverse pourtant tout le corps de `AddPayload` (`if (useStartCode)`).

## 2. L'inventaire : six lectures de RFC, dont deux du même format

Contrat unique, déclaré en `third_party/fontventa/libmedikit/medkit/video.h:250` :

```cpp
virtual int DecodePacket(BYTE *in, DWORD len, int lost, int last) = 0;
```

| format RFC | fichier | lignes de dépaquetisation | état porté entre paquets | équivalent ffmpeg 5.1 |
|---|---|---|---|---|
| **RFC 6184** (H.264 → Annex-B, décodage) | `libmedikit/h264/h264decoder.cpp:103–239` | ~137 | **aucun** : `buffer`/`bufLen` hérités, pas d'état de fragmentation | `rtpdec_h264.c`, `enc_name = "H264"` — présent |
| **RFC 6184** (H.264 → AVCC + carte RTP, enregistrement) | `libmedikit/h264/h264depacketizer.{cpp,h}` | 406 + 43 | `VideoFrame frame`, `iniFragNALU`, `fragNALUOpen`, `hasSPS`, `hasPPS`, `useStartCode` (mort) | *aucun* : rtpdec ne produit pas d'AVCC et **jette** la carte de fragmentation (§5) |
| **RFC 6184** (mort) | `libmedikit/h264/h264decoder.cpp:241–468` | 228 (`#if 0`) | — | — |
| **RFC 7741** (VP8) | `libmedikit/vp8/vp8decoder.cpp:81–151` | ~71 | aucun (`buffer`/`bufLen`) — le descripteur VP8 n'exige pas d'état | `rtpdec_vp8.c`, `enc_name = "VP8"` — présent |
| **RFC 4629** (H.263+, en-tête RFC 2429) | `libmedikit/h263/h263codec.cpp:56–109` | 54 | aucun (`buffer`/`bufLen`) | `rtpdec_h263.c`, `enc_name = "H263-1998"` / `"H263-2000"` — présents |
| **AOMedia « RTP Payload Format For AV1 » v1.0** | `libmedikit/av1/av1depacketizer.{cpp,h}` + `av1/av1obu.{cpp,h}` (partagé avec le sens émission) | 322 + 123 + 31 | `pending`, `unit`, `seqHdr`, `out`, `fragmentOpen`, `unitHasSeqHdr`, `damaged` | **AUCUN** (§3) |
| *aucune* — accumulation brute (MPEG-4, VP6, SORENSON/FLV1) | `libmedikit/ffvideocodec.cpp:871–896` | 26 | aucun | `rtpdec_mpeg4.c` (`"MP4V-ES"`, `"mpeg4-generic"`) pour MPEG-4 ; rien pour VP6/FLV1 |
| **audio, tous codecs** — aucun retrait d'en-tête de payload | `mcu/src/rtp.cpp:121–166` (`DummyAudioDepacketizer`) | ~46 | `AudioFrame frame` | `rtpdec_amr.c`, `rtpdec_latm.c`, opus… (à vérifier, cf. §8) |
| **RFC 2198** (RED) + **RFC 4103** (T.140) | `libmedikit/red.cpp` (293) / `mcu/src/redcodec.cpp` (243) | 536 | — | *aucun* handler RED dans libavformat |
| **RFC 5109** (ULPFEC) | `mcu/src/fecdecoder.cpp` | 259 | — | *aucun* |
| **RFC 2190** (H.263, **paquetisation**) | `libmedikit/h263packet.cpp` | 947 | tables VLC | `rtpenc_*` — hors sujet ici, mais le problème symétrique existe |

**Total dépaquetisation vidéo maison vivante : ~1 213 lignes**, plus 228 lignes
mortes. Et **six** implémentations là où il y a **quatre** formats de payload
distincts.

Sur les versions ffmpeg d'apparition de chaque handler : **non établissable
depuis cette machine** (pas de sources ffmpeg, pas de réseau). Voir §8.

## 3. Ce que ffmpeg 5.1.9 offre, et ce qu'il n'offre pas — vérifié sur la machine

Les dépaquetiseurs de ffmpeg vivent dans **libavFORMAT** (`rtpdec_*.c`), pas dans
libavcodec, derrière `RTPDynamicProtocolHandler` déclaré dans l'en-tête **privé**
`libavformat/rtpdec.h`.

```
$ ls /usr/include/ffmpeg/libavformat/
avformat.h  avio.h  version.h  version_major.h        # rtpdec.h : absent

$ nm -D --defined-only /usr/lib64/libavformat.so.59 | grep -c ff_rtp
0                                                     # symboles non exportés
```

La bibliothèque est **déjà liée** — `mcu/Makefile:186` (`-lavformat`) et
`third_party/fontventa/libmedikit/Makefile:138`. Le code est donc dans notre
processus, mais hors d'atteinte : ni en-tête, ni symbole.

Handlers effectivement présents dans `libavformat.so.59`, établis par recherche
d'octets exacts (`\0<enc_name>\0`) dans la bibliothèque installée :

```
H264  H265  VP8  VP9  H261  H263-1998  H263-2000  MP4V-ES  mpeg4-generic
opus  speex  theora  vorbis  X-QT  QCELP  JPEG  SVQ3  L16  X-MP3-draft-00
```

Et l'absence qui décide de tout :

```
AV1 : les deux seules occurrences de "AV1\0" dans libavformat.so.59 sont
      "V_AV1", l'identifiant de codec Matroska. Aucun enc_name RTP.
      Les seules chaînes AV1 du démultiplexage sont
      « AV1 Annex B/low overhead OBU demuxer » — des lecteurs de FICHIER.
```

Licence : `mcumediaserver.spec` déclare `License: GPL` ; les `rtpdec_*.c` sont
LGPL 2.1+. La recopie est donc **licitement possible** dans le sens LGPL → GPL.
La licence n'est pas l'obstacle de l'option (a) ; §5.1 dit ce qui l'est.

## 4. Le contrat que les appelants imposent, et qui n'est pas « du RTP »

### 4.1 Les six appelants

| appelant | ce qu'il passe en `in` | `lost` | `last` |
|---|---|---|---|
| `mcu/src/jsr309/VideoDecoderWorker.cpp:222` | charge RTP (après SRTP, après RED) | `seq - lastSeq - 1` sur numéros de séquence **étendus** (`GetExtSeqNum`, l.114-133) | `packet->GetMark()` |
| `mcu/src/videostream.cpp:859` | idem | idem (l.~780) | `packet->GetMark()` |
| `mcu/src/mediabridgesession.cpp:598` | charge RTP | **`0` en dur** | `packet->GetMark()` |
| `mcu/src/mp4player.cpp:121` | charge reconstruite par `mp4streamer` depuis les **hint tracks** du MP4 | `false` | `packet.GetRTPHeader()->m` |
| `mcu/src/rtmpparticipant.cpp:1000,1004,1039` | **NAL AVCC nus**, découpés d'une trame RTMP `AVCNALU`, plus les SPS/PPS d'un `AVCDescriptor` | `0` | `(size < NALUnitLength)` |
| `libmedikit/transcoder.cpp:158` | un `MediaFrame`, pas un paquet | calculé par le transcodeur | idem |

**Trois de ces six appelants ne voient jamais un paquet RTP.**
`rtmpparticipant` ingère de l'AVCC issu de RTMP ; `mp4player` rejoue un fichier ;
`transcoder` traite une trame déjà assemblée. `DecodePacket` n'est donc pas
« donne-moi un paquet RTP » mais **« ingère un fragment au format de payload du
codec »** — et ça marche pour H.264 parce qu'un NAL nu *est* une charge RTP
« single NAL unit packet » (RFC 6184 §5.6), ce que `h264_append_nals` traite dans
son `default:`.

C'est décisif pour toute migration : le point d'entrée de ffmpeg
(`ff_rtp_parse_packet`) prend un paquet RTP **avec ses 12 octets d'en-tête**. Ces
trois appelants devraient fabriquer de faux en-têtes RTP — ou garder un chemin
maison. Dans les deux cas la migration n'élimine pas le code, elle en ajoute.

### 4.2 L'appel de vidange

`VideoDecoderWorker.cpp:205` et `videostream.cpp:840`, identiques :

```cpp
//Check if we have lost the last packet from the previous frame
if (seq > frameSeqNum)
        videoDecoder->DecodePacket(NULL,0,1,1);   // vidange
```

Traduction : « on n'a jamais vu le bit marqueur de la trame précédente, sors ce
que tu as ». Trois des cinq implémentations le supportent par accident (`inLen`
nul ⇒ rien à accumuler, `last` ⇒ on décode) ; `AV1Depacketizer` le documente
explicitement (`av1depacketizer.cpp:153`) et le teste
(`test_av1_depacketizer.cpp:374`). Un dépaquetiseur ffmpeg n'a pas cette
primitive : la trame partielle est libérée par lui, quand lui le décide.

Le retour `0` est l'autre moitié du contrat : « rien de décodable ». L'appelant
en fait une **politique** — demander une image clé, bornée à une par seconde
(`getDifTime(&lastFPURequest) > 1000000`), `j->Update()` /
`listener->onRequestFPU()` + `session->RequestFPU(recSSRC)`. Un `AVERROR` ffmpeg
ne dit pas la même chose : il ne distingue pas « unité amputée, redemande une
clé » de « charge illisible, ignore ».

### 4.3 Les deux lectures vivantes de la RFC 6184 ne coïncident pas

Non pas par négligence : parce qu'elles n'ont pas la même *information* et pas
la même *politique*.

- **`lost` n'atteint pas la voie enregistrement.** La signature est
  `AddPayload(payload, payload_len, mark)` — pas de paramètre de perte. Le
  décodeur sait qu'un paquet manque, l'enregistreur ne le sait pas : un trou au
  milieu d'une FU-A produit un échantillon MP4 vraisemblable et faux.
- **La politique d'image clé diffère.** Le dépaquetiseur d'enregistrement exige
  d'avoir vu SPS **et** PPS avant de poser `frame.SetIntra(true)`
  (`h264depacketizer.cpp`, branche `case 0x05`), et expose ce verdict par
  `MayBeIntra()` que `mp4writer.cpp:245` consulte pour décider du *sync sample*.
  La voie décodage n'a aucune notion équivalente.
- **FU-B est mal réassemblé des deux côtés, de la même façon.** `case 28: case
  29:` dans `h264_append_nals` (`h264decoder.cpp:158-159`) comme dans
  `H264Depacketizer::AddPayload` (`h264depacketizer.cpp:226-227`) : le type 29
  est traité exactement comme le 28, sans sauter les **2 octets de DON** que la
  RFC 6184 §5.8 place après l'en-tête FU. C'est un écart réel à la RFC, présent
  en double, corrigible en un seul endroit uniquement si les deux lectures
  fusionnent. (STAP-B (25) et MTAP16/24 (26/27) sont refusés proprement des deux
  côtés — c'est correct et assumé.)

## 5. Ce que la couche transport possède, et ce que le second usage exige

### 5.1 Ce que ffmpeg voudrait posséder : quantification

`mcu/src/rtpsession.cpp` fait **3 564 lignes**, dont ~490 pour la seule voie de
réception (`ReadRTP`, l.1839-2329). Ce qui s'y passe **avant** que la charge
n'arrive au dépaquetiseur :

| fonction | où | ce qui serait perdu si ffmpeg ouvrait la socket |
|---|---|---|
| STUN / ICE | `ReadRTP:1864+`, `SendICEBindingRequest:1040`, `DriveICEChecks:1074`, `OnICEConnectivityConfirmed:1106`, `AddICECandidate:1132` | tout WebRTC |
| DTLS-SRTP | `SetRemoteCryptoDTLS:603`, `DriveDTLSClientHandshake:991`, `FlushDTLS:964`, `onDTLSSetup:3494` | tout WebRTC |
| SRTP **à cinq contextes** | `ReadRTP:2149-2185` (`recvSRTPSession`, `_secondary`, `RTX`, `RTX_secondary`) | le changement de clé et le RTX chiffré |
| natLatch symétrique | `l.229-233`, `SetProperties:492-499`, `NatCorrectable:843`, `SetRemotePort:861-896`, `ReadRTP:1486-1510` | Asterisk `nat=yes` et tout pair derrière NAT |
| amorçage NAT | `SendNATPrimingPacket:1221`, `ArmNATPriming:1248` | le pair qui n'émet qu'après avoir reçu |
| RTX (désencapsulation OSN) | `ReadRTP:2189-2201`, `ReSendPacket:2892` | la retransmission |
| RTP map PT→codec, RED/T140RED | `ReadRTP:2205-2270` | le mapping dynamique et RFC 2198 |
| SSRC multiples, flux par défaut | `ReadRTP:2276-2320`, `AddStream:3028`, `ChangeStream:3116` | le changement de SSRC en cours d'appel |
| cycles de séquence → `GetExtSeqNum` | `RTPStream::Add:3271-3286`, `rtp.h:247` | le calcul de `lost` lui-même |
| jitter, NACK, statistiques | `RTPStream::Add:3300-3390`, `GetStatistics:3518` | le RTCP sortant |
| ULPFEC | `RTPStream::Add:3396-3430` + `fecdecoder.cpp` (259 l.) | la récupération de pertes |
| IP annoncée, source unique | `SetAnnouncedIp:156` | la règle du `CLAUDE.md` |
| chien de garde d'inactivité | `ArmRTPTimeout:929`, notification `onRTPPacketReceived` (P5) | l'événement *media connected* JSR-309 |

Verrouillé par `mcu/tests/test_rtp_latching.cpp` (8 tests) et
`mcu/tests/test_rtp_rtcp.cpp` (12 tests).

**Une URL `rtp://` donnée à `avformat_open_input` transfère cette propriété à
ffmpeg.** Ce n'est pas discutable au cas par cas : c'est la fin du WebRTC sur ce
serveur. Toute option qui suppose que ffmpeg ouvre la socket est éliminée ici,
définitivement.

### 5.2 Le second usage : le dépaquetiseur H.264 alimente l'enregistrement MP4

`H264Depacketizer` ne produit pas seulement des octets. Il produit **trois
choses**, et les deux dernières n'ont aucun équivalent ffmpeg :

1. l'échantillon en **AVCC** (préfixes de longueur sur 4 octets), écrit tel quel
   par `MP4WriteSample` (`libmedikit/mp4track.cpp:650`) ;
2. le verdict *sync sample* via `MayBeIntra()` (`mp4writer.cpp:245`) ;
3. la **carte de fragmentation RTP** : `frame.AddRtpPacket(pos, nalu_size, …)`
   aux lignes 214, 323 et 395 de `h264depacketizer.cpp`.

Cette carte a cinq consommateurs :

- `libmedikit/mp4track.cpp:653-690` — écrit les **RTP hint tracks** du MP4
  (`MP4AddRtpHint`) et **y décode le SPS** pour renseigner la piste ;
- `libmedikit/mp4writer.cpp:35` — `GetSizeFromSps` en tire largeur, hauteur et
  `profile-level-id` de la trame (et le commentaire documente une panne réelle :
  deux SPS de niveaux différents cohabitant, `42801F` du prologue contre
  `42c016` négocié) ;
- `mcu/src/mp4streamer.cpp:453-472` — reconstitue les paquets RTP à la lecture ;
- `mcu/src/RTPSmoother.cpp:62-73` et
  `mcu/src/jsr309/RTPMultiplexerSmoother.cpp:55-60` — ré-émission lissée.

Or **un dépaquetiseur ffmpeg jette délibérément cette information** : son contrat
est de produire un `AVPacket` en flux de bits (Annex-B pour H.264), pas de dire
de quels paquets il venait. Conclusion nette : **aucune** des options (a) ou (b)
ne supprime `h264depacketizer.cpp`. Au mieux elles suppriment
`h264_append_nals`, soit 137 des 1 213 lignes.

## 6. Les trois options

### 6.1 (a) Recopier les `rtpdec_*.c` dans libmedikit

**Coût de licence : nul** (LGPL 2.1+ → GPL, §3).

**Coût réel : les `rtpdec_*.c` ne sont pas de la manipulation d'octets
autonome.** La signature du handler est (de mémoire de l'en-tête amont, **à
vérifier** sur les sources 5.1) :

```c
int (*parse_packet)(AVFormatContext *ctx, PayloadContext *s, AVStream *st,
                    AVPacket *pkt, uint32_t *timestamp,
                    const uint8_t *buf, int len, uint16_t seq, int flags);
```

Un handler écrit dans `AVPacket`, lit et écrit `st->codecpar->extradata`,
construit sa trame dans un buffer dynamique `avio_open_dyn_buf` /
`avio_close_dyn_buf`, et appelle des utilitaires internes
(`ff_h264_parse_sprop_parameter_sets`, `ff_parse_fmtp`, `av_base64_decode`). Une
recopie a donc deux issues, toutes deux mauvaises :

- **recopier fidèlement** ⇒ importer `AVFormatContext`/`AVStream` dans
  libmedikit et fabriquer de faux `AVStream` pour chaque patte. On garde la
  possibilité de rejouer les correctifs amont, au prix d'une couche d'adaptation
  qui n'a pas de sens dans notre modèle.
- **réécrire en l'adaptant** ⇒ ce n'est plus une copie ; les correctifs amont ne
  s'appliquent plus ; nous possédons le code **et** la divergence.

Et l'option ne répond ni pour AV1 (rien à copier), ni pour l'enregistrement MP4
(§5.2). Elle apporte, au mieux, une meilleure lecture de la RFC 6184 pour le
décodage, contre un mécanisme de synchronisation amont à maintenir à la main.

**Verdict : à écarter.** C'est le pire des deux mondes.

### 6.2 (b) Piloter le démultiplexeur public par un `AVIOContext` maison

La seule voie publique est `avformat_open_input`. Deux sous-variantes.

**(b1) URL `rtp://` — éliminée en §5.1.** ffmpeg ouvre la socket ⇒ plus de
SRTP/DTLS/ICE, plus de natLatch, plus de RTX/RED/FEC. Sans objet.

**(b2) démultiplexeur `sdp` + `sdp_flags=custom_io`.** libavformat expose une
`AVOption` publique `sdp_flags` dont la valeur `custom_io` (« use custom I/O »)
existe précisément pour qu'une application fournisse elle-même le transport :
combinée à `AVFMT_FLAG_CUSTOM_IO` et à un `avio_alloc_context` dont nous
possédons le `read_packet`, libavformat lit ses octets RTP depuis `s->pb` au lieu
d'une socket et les démultiplexe par SSRC / payload type. L'architecture serait :

```
RTPSession (socket, SRTP, ICE, natLatch, RED/FEC, cycles de séquence)
   → RTPPacket déchiffré
      → notre AVIOContext (read_packet = 1 appel = 1 paquet RTP)
         → démultiplexeur sdp de libavformat → rtpdec_<codec>
            → AVPacket = une trame → notre décodeur
```

C'est **honnêtement faisable en principe** — le mécanisme existe et n'est pas un
détournement. Mais il porte cinq réserves, dont trois sont des questions
ouvertes que seul un *spike* tranche :

1. **`AVIOContext` est un flux d'octets ; RTP est un flux de messages.** Que
   « un appel à notre `read_packet` = un paquet RTP » survive au tampon interne
   d'avio est un **invariant d'implémentation**, pas une garantie d'API. Ça
   marchera très probablement (le lecteur demande de gros blocs et vide le tampon
   à chaque fois), et ça peut se casser **silencieusement** à une montée de
   version. Un paquet RTP recollé au suivant n'est pas une erreur détectable :
   c'est une trame corrompue.
2. **Le même `s->pb` doit d'abord rendre le texte SDP** (le démultiplexeur le lit
   à l'ouverture) **puis les paquets RTP.** Exprimer ce basculement sans un EOF
   qui ferme le flux est la première chose à tester. **Question ouverte.**
3. **Un `AVFormatContext` par sens, par patte, par média.** Coût en descripteurs :
   nul. En threads : nul. En mémoire : les tampons. Mais surtout : libavformat
   **réordonne** les paquets avant de les livrer (`max_delay`, dont le défaut sur
   entrée SDP/RTSP est de l'ordre de 100 ms). Sur un pont média c'est de la
   latence bouche-à-oreille ajoutée, et c'est un **doublon** de
   `RTPSession::RTPStream` / `RTPBuffer`. Il faut donc le mettre à 0 — et on
   demande alors à ffmpeg de faire moins que ce qu'il veut faire.
4. **RTCP.** Le contexte de parsing RTP de ffmpeg émet des *receiver reports* sur
   son propre canal RTCP. En mode custom I/O il n'y en a pas. Soit c'est inerte,
   soit c'est une **seconde source RTCP** qui contredit `SendSenderReport()` et
   le générateur de NACK de `RTPStream::Add`. **À vérifier.**
5. **La sortie ne répond pas au second usage.** Annex-B pour H.264, aucune carte
   de fragmentation (§5.2). `h264depacketizer.cpp` reste, entier. Et AV1 reste
   maison.

**Ce que (b2) achète réellement** : la voie *décodage* de H.264, H.265, VP8, VP9,
H.263+ et MPEG-4 — soit ~288 lignes maison remplacées (137 + 71 + 54 + 26) — et
un accès gratuit à H.265, VP9 et H.261 que nous n'avons pas.
**Ce qu'elle laisse en place** : les 449 lignes de l'enregistrement, les 476 de
l'AV1, les 536 de RED/T.140, les 259 de l'ULPFEC.

### 6.3 (c) Rester maison, mais n'écrire le squelette qu'une fois

Le constat de §2 mérite d'être relu : ce qui est dupliqué n'est **pas** les RFC.
Elles sont réellement différentes (VP8 est sans état, AV1 est à état de
fragmentation, H.264 a trois modes d'agrégation). Ce qui est dupliqué est le
**squelette** — et il l'est cinq fois :

```
ffvideocodec.cpp:871-896     h264decoder.cpp:219-239     vp8decoder.cpp:121-151
h263codec.cpp:56-109         av1codec.cpp:452-482
```

Chacune de ces cinq fonctions rejoue la même séquence : vérifier
`bufLen + inLen + AV_INPUT_BUFFER_PADDING_SIZE > bufSize`, journaliser
« buffer size error, reseting », remettre `bufLen = 0`, accumuler, et sur `last`
faire `memset` du padding puis `Decode`. La variante H.263 ajoute `+2` à la
borne (`h263codec.cpp:62`) — un besoin réel de la RFC 4629, exprimé dans l'idiome
partagé. Le jour où la borne devient fausse, il y a cinq endroits à corriger, et
c'est exactement ce qui s'est produit pour AV1 : la borne héritée
(`bufSize` ≈ 1,1 Mo) n'était pas celle du format, d'où le contrôle **séparé** de
`AV1Depacketizer::MaxUnitSize`.

Le chantier :

1. **Une interface `RtpDepacketizer` dans libmedikit**, dont le contrat est
   énoncé une fois : `AddPayload(payload, len, lost)` → `bool`,
   `GetUnit(DWORD& len)` → `const BYTE*`, `ResetFrame()`, `IsDamaged()`. C'est
   **exactement la forme que `AV1Depacketizer` a déjà** — et son en-tête dit
   pourquoi : « Ne dépend PAS de ffmpeg : c'est de la manipulation d'octets,
   testable seule ». C'est le seul des six qui est testable sans décodeur, et
   c'est le modèle.
2. **`FfVideoDecoder::DecodePacket` devient `final`** : borne, délégation au
   membre dépaquetiseur, vidange sur `last`, padding, `Decode()`. Plus aucune
   classe dérivée ne redéfinit `DecodePacket` — elles fournissent un
   dépaquetiseur. Les cinq squelettes deviennent un.
3. **Les deux lectures de la RFC 6184 fusionnent.** L'AVCC + carte de
   fragmentation devient un **mode de sortie** du dépaquetiseur H.264, pas une
   seconde classe : une seule lecture, deux sorties (Annex-B pour décoder, AVCC +
   carte pour enregistrer). C'est la seule façon que FU-B, la politique d'intra
   et le signal `lost` cessent de diverger (§4.3). `SetUseStartCode`, mort,
   devient le point d'entrée honnête de ce choix — ou disparaît.
4. **Suppression de `h264decoder.cpp:241-468`** (228 lignes mortes).
5. **Une suite de tests adverse par RFC**, sur le modèle des deux qui existent :
   `test_h264_depacketizer.cpp` (6 tests, 243 l.) et
   `test_av1_depacketizer.cpp` (18 tests, 557 l.). Manquent : VP8 (RFC 7741),
   H.263+ (RFC 4629), et la sortie Annex-B de H.264 — qui n'est aujourd'hui
   testée par rien.

Le vrai travail est l'étape 3 ; le reste est mécanique. Coût estimé : quelques
jours, sans risque de régression sur le transport, avec un filet de tests qui
grandit au lieu de se déplacer.

**Un premier acompte a déjà été versé, sans l'avoir prévu.** L'ajout du sens
émission AV1 (`5134fb8`) a rendu la factorisation immédiatement nécessaire :
`av1obu.{h,cpp}` porte désormais le leb128 et le parcours du flux low-overhead
(`AV1ParseObuStream`, en offsets, sans recopie), employés par les deux sens. Ce
n'était pas de l'élégance : les deux sens avaient chacun leur lecture du leb128,
et une divergence entre elles ne se serait vue que sur les tailles ≥ 128 — donc
jamais sur un flux de test. C'est la forme que l'étape 1 généralise, et le fait
qu'elle se soit imposée d'elle-même au deuxième usage est l'argument le plus
concret en faveur de (c).

Cet acompte apporte aussi, par symétrie, la mesure qui manquait pour juger
l'étape 5 : le test le plus solide du couple n'est aucun des deux sens pris
séparément mais **l'aller-retour** — ce que le paquetiseur produit, le
dépaquetiseur doit le rendre octet pour octet. Les deux sens ayant été écrits
séparément, un aller-retour qui referme ne vient pas d'une lecture commune de la
spec. C'est reproductible pour H.264 (`h263packet.cpp` porte déjà le sens
émission RFC 2190) et cela vaut mieux qu'une suite adverse par sens.

## 7. Recommandation

**(c) — mutualiser le maison — et un *spike* borné sur (b2), dans cet ordre.**

Quatre faits la portent, tous vérifiables dans l'arbre :

1. **Trois des six appelants de `DecodePacket` ne voient jamais un paquet RTP**
   (`rtmpparticipant.cpp:1000-1039`, `mp4player.cpp:121`,
   `transcoder.cpp:158`). Le point d'entrée de ffmpeg exige un paquet RTP
   complet. Aucune migration ne les couvre : un chemin maison survit de toute
   façon, et un chemin maison à côté d'un chemin ffmpeg est *deux* lectures, soit
   le problème de §4.3 reproduit à plus grande échelle.
2. **Le format qui a coûté un appel en production est celui que ffmpeg n'a pas.**
   Le 2026-08-12, AV1 ↔ AV1 est mort ; ffmpeg 5.1 n'a aucun handler RTP AV1
   (vérifié, §3). Migrer vers ffmpeg n'aurait pas évité la panne — écrire le
   dépaquetiseur l'a évitée.
3. **Le second usage n'a aucun équivalent ffmpeg.** La carte de fragmentation
   RTP, les hint tracks, l'extraction du SPS, le verdict `MayBeIntra` : rtpdec
   jette précisément cette information (§5.2). Les 449 lignes de
   `h264depacketizer.cpp` restent dans tous les scénarios.
4. **Le transport est à nous et doit le rester.** ~3 564 lignes de
   `rtpsession.cpp` + 259 de FEC + 536 de RED, verrouillées par 20 tests, portant
   SRTP à cinq contextes, DTLS, ICE, natLatch, RTX et les cycles de séquence dont
   `lost` est dérivé (§5.1). La voie supportée par ffmpeg veut cette propriété.

Autrement dit : la duplication réelle n'est pas « nous réécrivons ffmpeg », elle
est « nous écrivons cinq fois le même squelette et deux fois la même RFC ». C'est
un problème interne, et (c) le règle sans toucher au transport.

**Le critère qui renverse la recommandation** — et il faut le dire précisément,
sinon ce n'est pas un critère :

> Si un *spike* de (b2) établit, **cumulativement**, que (i) « un appel à
> `read_packet` = un paquet RTP » tient avec `max_delay=0` et sans effet de bord
> RTCP, (ii) le basculement SDP-puis-média sur le même `AVIOContext` est
> exprimable, et (iii) le surcoût par flux reste sous ~1 ms/trame et quelques
> centaines de kio — **alors la voie décodage vaut le déplacement**, en gardant
> le dépaquetiseur H.264 maison pour l'enregistrement. (i) et (iii) se mesurent
> en une journée ; (ii) est la vraie inconnue.

**Le second critère, dans l'autre sens** : le jour où il faut **recevoir** un
format qui a un handler ffmpeg et pas de dépaquetiseur maison, (b2) devient
rentable pour ce format seul et le *spike* cesse d'être optionnel. Le cas
réaliste est **H.265** : `H265` est dans `libavformat.so.59`, `VideoCodecFactory`
n'a pas de décodeur HEVC, et personne n'a écrit de dépaquetiseur RFC 7798.
Écrire un dépaquetiseur HEVC à la main quand ffmpeg en a un serait, là, le
mauvais choix.

## 8. Ce qui doit déclencher une réévaluation

- **`rtpdec.h` installé, ou symboles `ff_rtp*` exportés.** Test, deux lignes :
  ```sh
  ls /usr/include/ffmpeg/libavformat/rtpdec.h
  nm -D --defined-only /usr/lib64/libavformat.so.* | grep -c ff_rtp   # ≠ 0 ?
  ```
  Si l'un des deux change, l'option (a) devient un *lien* au lieu d'une copie, et
  tout le raisonnement de §6.1 tombe. À revérifier à chaque montée de version du
  paquet `ffmpeg-libs`.
- **Apparition d'un handler RTP AV1 amont** (un `rtpdec_av1.c`). Test :
  `\0AV1\0` dans `libavformat.so.*`, ou `git log -- libavformat/rtpdec_av1.c`
  sur un clone amont. C'est l'argument n° 2 de §7 qui disparaît alors.
- **Besoin de recevoir H.265, VP9 ou un format à handler existant** (§7, second
  critère).
- **Changement de stratégie d'écriture MP4.** Si les *RTP hint tracks* cessent
  d'être écrites, l'argument n° 3 de §7 s'affaiblit beaucoup. Question ouverte :
  **qui lit encore les hint tracks que nous produisons**, hors `mp4streamer` ?
- **`rtpsession_sans_thread.md`.** Ce chantier touche la voie de réception. La
  mutualisation (c) ne doit pas entrer en collision avec lui : (c) est en aval du
  point de livraison de la charge, donc les deux sont séparables — à confirmer
  avant de commencer.

## 9. Questions ouvertes, à ne pas confondre avec des conclusions

1. **Dans quelle version de ffmpeg chaque handler est-il apparu ?** Non
   établissable ici : ni sources ffmpeg, ni réseau sur cette machine. Méthode :
   `git log --oneline -- libavformat/rtpdec_vp9.c` (etc.) sur un clone amont, ou
   la table `Changelog`. Le tableau de §2 ne l'affirme donc pas.
2. **ffmpeg 5.1 a-t-il un handler RTP AMR ?** La recherche d'octets dans
   `libavformat.so.59` est **inconclusive** : `AMR\0` et `AMR-WB\0` sont présents
   mais jamais précédés d'un NUL, donc ce sont peut-être les queues de
   `raw AMR\0` / `raw AMR-WB\0` (fusion de suffixes de chaînes par l'éditeur de
   liens). Sans portée pratique : la voie audio ne dépaquetise rien aujourd'hui
   (`DummyAudioDepacketizer`, `rtp.cpp:121-166`, passe la charge telle quelle) —
   ce qui est d'ailleurs un défaut latent pour AMR/AMR-WB (RFC 4867 a un en-tête
   de payload) **à instruire séparément**.
3. **Le démultiplexeur `sdp` en mode `custom_io` lit-il vraiment le SDP puis le
   média sur le même `AVIOContext` ?** §6.2, réserve 2. C'est la question qui
   décide de la faisabilité de (b2), et elle ne se tranche que sur les sources ou
   par un *spike*.
4. **`H264Depacketizer::SetUseStartCode` : mort ou point d'extension ?** Aucun
   appelant dans tout l'arbre (`grep -rn SetUseStartCode`). S'il est mort, sa
   suppression simplifie l'étape 3 de (c) ; s'il était l'amorce du mode Annex-B,
   c'est au contraire là qu'il faut brancher la fusion.
5. **La signature exacte de `RTPDynamicProtocolHandler::parse_packet` en 5.1**
   (citée de mémoire en §6.1) est **à vérifier** sur les sources. Elle ne change
   pas la conclusion de §6.1, qui tient au seul fait que le handler écrit dans un
   `AVPacket` et lit un `AVStream`.
