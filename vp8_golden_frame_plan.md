# Chantier RPSI VP8 — acquitter les golden/altref frames (point 7.10 du contrôle de débit)

Branche : `feat/vp8-golden-frame`.

## 1. Problème

Recette 7.9 (2026-08-30) : le throttler TMMBR a réduit nos annonces de 274 à 11,
mais Linphone émet toujours ~930 trames clés en 34 min (1 toutes les 2,9 s).

Cause, lue dans `~/linphone-sdk/mediastreamer2/src/videofilters/vp8.c` :
en AVPF, msvp8 émet une trame de référence toutes les `fps × 3` images (3 s).
Cette trame est une **trame clé** tant que golden ET altref ne sont pas
acquittées (`vp8.c:265-280`). L'acquittement attendu est un **RPSI**
(RTCP PSFB fmt 3, RFC 4585 §6.3.3) portant le PictureID de la trame décodée.
Le mcu n'émet jamais de RPSI. Une trame clé VP8 coûte 5 à 10× une inter :
c'est le vrai poste de dépense que le 7.9 visait.

## 2. Contrat, vérifié dans les sources Linphone SDK 5.5.9

- **Quand acquitter** (modèle : leur décodeur, `vp8.c:1076-1081`) : après un
  décodage RÉUSSI d'une trame qui met à jour golden OU altref, et seulement si
  le PictureID est présent. Eux le savent par libvpx
  (`VP8D_GET_LAST_REF_UPDATES`) ; nous décodons par ffmpeg qui n'expose rien,
  et `vpx-devel` est absent → il faut parser l'en-tête de trame VP8 nous-mêmes
  (RFC 6386, voir lot 1).
- **Quoi envoyer** (`vp8rtpfmt.c:423-439` émission, `vp8.c:821-835` lecture) :
  le bit string RPSI = le PictureID **tel que reçu** dans le payload descriptor
  RTP. 15 bits (M=1) → 2 octets réseau, bit 0x8000 posé ; 7 bits → 1 octet.
  L'encodeur msvp8 compare à l'identique : son `picture_id` interne porte
  0x8000 (`vp8.c:128`). Toute réécriture du PictureID casse l'acquittement.
- **Vite** : `is_reference_sane` (`vp8.c:341`) rejette un ack trop vieux.
  Émettre le RPSI immédiatement après le décodage, pas au prochain SR.
- **Une trame clé acquitte les deux** : golden et altref prennent toutes deux
  le picture_id de la keyframe (`vp8.c:320-330`) — c'est ce qui sort du régime
  « trame clé toutes les 3 s » dès le premier ack.
- **SDP** : Linphone déclare `a=rtcp-fb:<pt> ack rpsi` (`sal_stream_description.cpp:1675`,
  variante historique `nack rpsi`). Leur chemin de réception RTCP
  (`videostream.c:444`) ne vérifie pas la feature par PT, mais l'AVPF doit être
  actif (il l'est sur nos appels : ils nous envoient déjà FIR/TMMBR).
  À vérifier au lot 0 : ce que kelixip répond aujourd'hui à cette ligne.

## 3. État de notre code (points de couture)

| Point | Fichier | État |
|---|---|---|
| Parse en-tête VP8 (refresh flags) | — | N'existe pas. RFC 6386 : keyframe → golden+altref implicites ; inter → 2 bits dans le premier header compressé (décodeur booléen ~100 lignes) |
| PictureID | `libmedikit/vp8/vp8depacketizer.cpp` | Le descripteur est parsé (`VP8DescriptorLen`) mais le PictureID est sauté, pas exposé |
| Champ RPSI | `mcu/include/rtp.h:1233` | `ReferencePictureSelectionField` : Parse OK, `Serialize` FAUX (`set2(data,6,length)` écrase le bit string ; PB traité en octets alors que la RFC le compte en bits) |
| Émission | `mcu/src/rtpsession.cpp` | Aucun `SendRPSI`. Modèle : `SendFIR` (rtpsession.cpp:3489) |
| Réception (pont) | `mcu/src/rtpsession.cpp:3427` | RPSI entrant reconnu et journalisé, rien relayé |
| Décodage conférence | `mcu/src/videostream.cpp` RecVideo (~l.907) | `videoDecoder->DecodePacket(...)`, aucun retour « référence à acquitter » |
| Décodage JSR-309 | `mcu/src/jsr309/VideoDecoderWorker.cpp` + transcodeur inline | Même couture à faire |

## 4. Lots

### Lot 0 — caractérisation et harnais

1. SDP réel : relire l'offre Linphone et l'answer kelixip d'un appel témoin
   (log kelixip ou pcap). Noter si l'answer écho `a=rtcp-fb ack rpsi`.
   Si non : item elixip (écho de la ligne), à trancher au lot 5 s'il est
   bloquant — la lecture du code (§2) dit que non.
2. Fixtures : extraire d'un pcap Linphone des trames VP8 réassemblées
   (1 keyframe, 1 inter qui rafraîchit golden, 1 inter sans refresh) pour les
   tests du lot 1. Complément : trames produites par notre propre encodeur VP8.
3. Mesure de référence chiffrée : `grep -c 'Got Intra'` par minute sur un appel
   témoin de 10 min, binaire actuel (attendu ~20/min).

### Lot 1 — libmedikit : parser l'en-tête VP8 et exposer l'acquittement

1. `vp8/vp8frameheader.{h,cpp}` : décodeur booléen RFC 6386 minimal + lecture
   du premier header compressé jusqu'aux drapeaux. Chemin inter : segmentation
   (§9.3), loop filter (§9.4), partitions de tokens (§9.5), quantizer (§9.6),
   puis `refresh_golden_frame`, `refresh_alternate_frame` et les 2×2 bits
   `copy_buffer_to_*` (§9.7) — une copie met aussi la référence à jour,
   l'acquitter comme libvpx le fait. Keyframe : les deux implicites.
   Octets purs, sans ffmpeg, testable seul (même esprit que le depacketizer).
2. `VP8Depacketizer` : capturer le PictureID **tel que reçu** sur le paquet de
   tête, sous forme de `WORD` : id 15 bits → les 2 octets réseau tels quels
   (bit 15 = M, posé) ; id 7 bits → l'octet seul (< 0x80). Sans réécriture :
   c'est ce que l'encodeur du pair compare à l'identique.
3. `VP8Decoder` : après `Decode()` réussi, rendre « cette trame met à jour une
   référence et porte cet identifiant ». Interface générique dans `codecs.h` :
   `VideoDecoder::GetReferencePictureId(WORD &pictureId)`, défaut `false` —
   pas d'acronyme RTCP dans l'interface codec, et le contrat (PictureID de
   trame de référence) est exactement celui que VP9 partagerait (RFC 9628
   §5.1). ABI mcu↔medkit modifiée → `make clean`.
4. Tests libmedikit : parseur (keyframe / inter avec et sans refresh / copies /
   tronquée / adverses), PictureID 7 et 15 bits, bout-en-bout sur les fixtures.

### Lot 2 — mcu : format de fil et émission

1. `rtp.h` : corriger `ReferencePictureSelectionField::Serialize` (retirer le
   `set2` parasite, PB = bits de bourrage jusqu'au mot de 32 bits ; cas 16 bits
   → PB=0, cas 8 bits → PB=8 + 1 octet nul) et aligner `Parse` sur la même
   sémantique.
2. `RTPSession::SendReferencePictureSelectionIndication(WORD pictureId)` sur le
   modèle de `SendFIR` — nom épelé comme les autres messages RTCP de la classe
   (`SendTempMaxMediaStreamBitrateRequest`, `SendReceiverEstimatedMaxBitrate`)
   et aligné sur l'enum `ReferencePictureSelectionIndication` existant.
   Rien de VP8 dedans : PT = celui du codec vidéo reçu (rtpMapIn), media SSRC =
   SSRC de l'émetteur, et encodage du bit string déterministe et partagé
   VP8/VP9 : `pictureId & 0x8000` → 2 octets réseau (PB=0), sinon 1 octet
   (PB=8) — le miroir exact de `vp8rtpfmt_send_rpsi`.
   Trace `Debug` par envoi + compteur dans les stats.
3. Tests : aller-retour Parse/Serialize + vérification externe **tshark**
   (méthode text2pcap du lot 4 du contrôle de débit — l'aller-retour interne ne
   prouve rien, on a la même main des deux côtés).

### Lot 3 — branchement aux deux chemins de décodage

1. Conférence : `videostream.cpp` RecVideo — après `DecodePacket` réussi en fin
   de trame, `GetReferencePictureId` → `SendReferencePictureSelectionIndication`.
   Le site d'appel ne contient aucune logique codec : pure plomberie.
2. JSR-309 : `VideoDecoderWorker` et le chemin transcodeur inline — même appel.
3. Garde : émettre dès qu'on décode du VP8 avec PictureID (reco, cf. §5.1).

### Lot 4 — relais en pont (différable, arbitrage §5.2)

Pont sans transcodage Linphone↔Linphone : le RPSI du récepteur aval
(rtpsession.cpp:3427, aujourd'hui log seul) doit remonter à la jambe amont,
bit string inchangé (le payload n'est pas réécrit en passthrough). Même motif
que le relais FIR/TMMBR du chantier feedback transcodeur.
Cas Linphone→WebRTC en passthrough : le navigateur n'émet pas de RPSI (WebRTC
l'a retiré) → le régime 3 s persiste là, c'est assumé.

### Lot 5 — recette

Appel réel Linphone (conférence, puis jambe JSR-309 transcodée) :
- critère principal : trames clés reçues de ~20/min à quelques-unes/min
  (`grep -ac 'Got Intra'` rapporté à la durée, contre la mesure du lot 0) ;
- contrôle positif : nos envois RPSI dans les stats/traces ; côté Linphone si
  accessible : `VP8: receiving RPSI for picture_id` ;
- non-régression : throttler TMMBR du 7.9 (≈11 annonces/34 min), qualité image.

## 5. Questions ouvertes (arbitrage mainteneur)

1. **Garde d'émission.** (a) Émettre le RPSI dès qu'on décode du VP8 à
   PictureID — un pair qui ne l'a pas offert l'ignore (Chrome l'ignore) ;
   (b) le conditionner à une propriété SDP poussée par le contrôleur.
   **Reco : (a)** — ack positif inoffensif, zéro couplage elixip pour la v1.
2. **Lot 4 maintenant ou après ?** Le cas réel (mendooze transcodé) est couvert
   par le lot 3. **Reco : recetter le lot 3 d'abord**, ouvrir le lot 4 si un
   pont VP8 passthrough Linphone↔Linphone existe en production.
3. **Sens inverse** (leur décodeur acquitte NOTRE encodeur VP8) : exploiter les
   RPSI reçus pour piloter le motif golden/altref de notre libvpx est un autre
   chantier — hors périmètre, noté ici pour ne pas le confondre avec le lot 4.

## 6. Portée codecs : le chantier reste VP8 seul (vérifié 2026-08-30)

- **AV1** (supporté) : la spec RTP AV1 (AOMedia §8) ne définit aucun RPSI ni
  ack positif — feedback = FIR, PLI, LRR, NACK. Et l'encodeur AV1 de Linphone
  fait l'inverse de msvp8 : en AVPF, `kf_mode = AOM_KF_DISABLED`
  (`av1-encoder.cpp:93-94`) — trame clé à la 1re image et sur PLI/FIR
  seulement, aucun régime périodique à casser. Rien à faire.
- **VP9** (éventuel) : la RFC 9628 §5.1 définit bien le MÊME acquittement RPSI
  par PictureID (VP9 garde des buffers golden/altref). Mais aucun pair réel ne
  l'implémente : mediastreamer2 n'a pas de filtre VP9 (le pair VP9 serait un
  navigateur) et libwebrtc a retiré le RPSI (~2017) — les navigateurs ne
  l'émettent ni ne l'honorent, même pour VP8.
- Conséquence : l'API est codec-neutre de bout en bout —
  `VideoDecoder::GetReferencePictureId` (contrat = PictureID d'une trame de
  référence, identique en VP8 et VP9) et
  `RTPSession::SendReferencePictureSelectionIndication(WORD)` (PT et encodage
  8/16 bits résolus dedans). Un VP9 futur n'ajouterait qu'un parseur
  d'en-tête dans libmedikit, sans toucher au mcu. Écarté : un bit string
  opaque (`BYTE*, len`) traversant l'ABI — généralité spéculative pour un
  codec hypothétique (H.263 RFC 2429, jamais émis), au prix d'un contrat
  d'API plus fragile.

## 7. Suivi (2026-08-30)

**Lots 0 à 3 FAITS, builds verts, 625 tests mcu (621 verts, 4 skips
préexistants) + 187 tests libmedikit.** Binaire `bin/debug/mcu` lié.

- Lot 0 : l'offre Linphone porte bien `a=rtcp-fb:97 ack rpsi` (fixture elixip
  `SDP-linphone-620-srtp-offer.txt:40`). **L'answer kelixip ne l'écho PAS**
  (`@supported_rtcp_fb` de `MediaServerMendoozeConn.ex:143` ne connaît pas
  rpsi). La réception RTCP de Linphone ne vérifie pas cette feature
  (`videostream.c:444` dispatche sans garde) : nos RPSI devraient être honorés
  quand même — à confirmer en recette ; sinon, item elixip = écho de la ligne.
  Mesure de référence : 930 trames clés/34 min (recette 7.9). Fixtures pcap
  remplacées par des en-têtes fabriqués (encodeur booléen §7.2) + des trames
  réelles libvpx.
- Lot 1 : `vp8/vp8frameheader.{h,cpp}` (ordre §9.7 vérifié sur le texte de la
  RFC : les DEUX drapeaux refresh, PUIS les copies conditionnelles) ;
  `VP8DescriptorPictureId` ; `VP8Decoder::GetReferencePictureId` (armé
  seulement si trame DÉCODÉE + PictureID + mise à jour de référence) ;
  `VideoDecoder::GetReferencePictureId` en fin de vtable de `medkit/video.h`
  (une seule copie : `mcu/include/video.h` est une redirection). 13 tests.
- Lot 2 : champ RPSI de `rtp.h` corrigé (le `set2(data,6,…)` écrasait le bit
  string ; PB désormais en BITS, Parse durci) ;
  `RTPSession::SendReferencePictureSelectionIndication(ssrc, pictureId)`
  (PT retrouvé par rtpMapIn depuis le codec reçu du flux). 7 tests dont
  câblage sonde↔session aux octets près ; **tshark** dissèque nos octets :
  PSFB fmt 3 RPSI, FCI `00 60 81 23`, longueur OK.
- Lot 3 : conférence (`videostream.cpp` RecVideo → session) et JSR-309
  (`VideoDecoderWorker` → `Joinable::AcknowledgeReferencePicture`, no-op par
  défaut, relayé par `RTPEndpoint` vers sa session). Le transcodeur passe par
  `decoder.onRTPPacket` (VideoTranscoder.cpp:174/240) : couvert.
- Lot 4 : différé (arbitrage §5.2).
- **RESTE : lot 5, la recette en appel réel Linphone** — critère : `Got Intra`
  de ~27/min à quelques-unes/min, trace `-SendReferencePictureSelectionIndication`
  (Debug, binaire en `-d`), non-régression du throttler TMMBR 7.9.

### Recette n°1 (2026-08-31, 06:25-06:42, ~17 min) : nos RPSI partent, les clés ne baissent PAS

Notre bord est HORS DE CAUSE, chaque maillon vérifié :
- 523 RPSI émis, un par trame de référence décodée, dans les ms qui suivent le
  décodage (pictureId à bit M, PT 96, SSRC média = le SSRC d'émission du pair) ;
- le calcul de longueur d'oRTP sur notre FCI rend exactement 16 bits ;
- le transport RTCP nous→pair FONCTIONNE : 35 TMMBN reçus en réponse à nos
  29 TMMBR (le pair déchiffre et traite notre SRTCP).

Et pourtant : 495 `Got Intra` en 17 min, cadence PARFAITEMENT stable à 2,0 s
(322 intervalles pile à 2,0 s) = le tick msvp8 `vconf.fps × 3` trames en mode
« jamais acquitté », du début à la fin. Nuance : pendant la PREMIÈRE minute,
~23 refresh golden/altref INTER (acquittés par nous) s'intercalent, puis plus
aucun après 06:26:10 — soit des ticks partiellement débloqués, soit du refresh
spontané libvpx (flags à 0 hors tick) ; indécidable de notre bord.

Côté sources Linphone (SDK 5.5.9), AUCUN garde n'explique le rejet :
`media_stream_process_rtcp` itère chaque paquet du compound vers
`video_stream_process_rtcp`, qui ne vérifie que le media SSRC (et journalise
« was ignored. Our SSRC is » en cas d'échec) ; `enc_notify_rpsi` acquitte sans
condition ; `enc_reset_frames_state` n'est appelé qu'à la création de
l'encodeur. MAIS le pair réel est peut-être un Linphone 6 (ms2 plus récent que
ces sources) — et son estimateur spamme : 1435 TMMBR reçus (~1,4/s) malgré nos
1435 TMMBN, un poste d'anomalie en soi.

**PROCHAINE ÉTAPE — le log CLIENT Linphone tranche en une minute** :
- `VP8: receiving RPSI for picture_id` présent → l'ack arrive à msvp8, le
  blocage est dans sa machine frames_state (ou une version divergente) ;
- `was ignored. Our SSRC is` → désaccord de media SSRC, correctif chez nous ;
- rien des deux → perte entre oRTP et videostream (garde propre à la version).
Relever aussi la VERSION exacte du client. Sans log client : pcap côté client
(le RTCP y est lisible si l'appel n'est pas SRTP, sinon logs seulement).

### Recette n°2 (2026-08-31, 07:00-07:02, ~2,3 min, client redémarré avec traces) : **VALIDÉE**

Client Linphone **6.2.1** (AppImage), traces détaillées fournies. Chaîne
complète prouvée :
- client : `VP8: receiving RPSI for picture_id 44577` puis compteur qui monte
  (~1 RPSI/3 s reçu), zéro `was ignored` ;
- client : 3 `Forcing vp8 key frame` dans les 7 premières secondes (démarrage
  + un PLI), puis PLUS AUCUNE — relayées par `Forcing independant altref
  frame.` toutes les ~18 s = le régime AVPF nominal acquitté (vp8.c:423,
  altref indépendant toutes les 5×interval) ;
- mcu : **5 `Got Intra` sur tout l'appel, toutes avant t+20 s** (référence
  avant chantier : ~27/min), 49 RPSI émis. **Critère du lot 5 atteint.**

Deux points consignés, à suivre hors chantier :
1. **L'échec de la recette n°1 (06:25, même binaire, même processus, même
   version client annoncée) reste inexpliqué côté client** — nos émissions
   étaient identiques et arrivaient (TMMBN en réponse à nos TMMBR). Seule
   différence connue : le client a été redémarré entre les deux. S'il
   récidive : reprendre ses traces (elles montrent tout) ; piste à garder,
   `video-conference.cpp:208` IGNORE les RPSI quand le client est en mode
   packet-router/conférence locale.
2. **Le client spamme des TMMBR** (~1,4/s : 1435/17 min puis 187/2,3 min)
   malgré nos TMMBN immédiats, dans les DEUX essais — son estimateur de notre
   flux oscille. Poste distinct (candidat 7.11 du contrôle de débit) ; nos
   ripostes sont bornées (TMMBN systématique, notre encodeur amorti).
3. Correction du constat du lot 0 : le SDP kelixip de CET appel PORTE
   `a=rtcp-fb:96 ack rpsi` (answer et re-INVITE) — l'écho existe donc par un
   autre chemin que la table `@supported_rtcp_fb` lue au lot 0 ; à retracer
   si un jour l'écho compte.

## 8. Pièges connus, hérités des chantiers voisins

- Vérifier les fins de ligne avant d'éditer `rtp.h` (des fichiers du dépôt sont
  en CRLF ; `rtpsession.cpp` est LF depuis 2026-08-19).
- `make clean` obligatoire après changement d'interface libmedikit (ABI
  mcu↔medkit, l'auto-dépendance ne traverse pas le sous-module).
- Recette : vider `/var/log/mcu.log` avant l'appel, `grep -a` (NUL dans le
  log), comparer l'heure du processus à celle du binaire (`systemctl restart`
  oublié = séance perdue).
- Le SRTCP n'est pas lisible au pcap : la preuve d'émission est la trace/stat,
  la preuve de réception est le log Linphone ou la chute des trames clés.
