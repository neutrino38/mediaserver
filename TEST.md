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
make check     # compile puis exécute toute la suite
make tests     # compile seulement (produit tests/runtests)
./tests/runtests               # relancer sans recompiler
./tests/runtests --gtest_filter='RtmpChunk.*'   # filtrer une suite
GTEST_MCU_DEBUG=1 ./tests/runtests              # tracer les Debug() du mcu
```

### Prérequis

- **GoogleTest système** : `gtest` (`pkg-config gtest`). Sur AlmaLinux 9 :
  `dnf install gtest-devel`.
- La cible se lie contre **tous les objets du mcu** (`$(OBJS)` de `mcu/Makefile`,
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
| `test_mosaic_composition.cpp` | `MosaicFactory`, `MosaicGeometry`, `MosaicComposition` | **`MosaicFactory`** : robustesse de `Mosaic::CreateMosaic` face à un type de composition invalide (les 12 types documentés acceptés, tout le reste refusé sans lever) et refus propre côté `VideoMixer`, mixer restant utilisable ensuite — la vérification passe par un thread borné dans le temps, car un verrou fuité ferait *pendre* la suite au lieu de l'échouer. **`MosaicGeometry`** : la géométrie décidée par `Mosaic::BuildDesc()` (quelles vignettes, quelle taille, quelle position), sans produire un pixel — 1+1 pleine hauteur (4:3 → 640x480) et son invariance pour une source 16:9, placements historiques des grilles 1x1/2x2/3x3/1p7, étirement du slot principal PIP, `keepAspect=false`, exclusion des slots vides, **invariant de non-débordement sur les 12 dispositions × 4 ratios de source**, stabilité de la description à contenu changeant (clé de reconstruction du graphe). **`MosaicComposition`** : la composition réelle par `MosaicCompositor` (graphe avfilter), vérifiée **pixel à pixel** — quadrants d'un 2x2, bandes de letterbox laissant voir le fond, cache intra-tick, `Clean()` qui rend le fond, 30 ticks consécutifs sans blocage du framesync, changement de résolution d'entrée, et composition effective des 12 types | — (nouveau) |
| `test_codec_type.cpp` | `CodecType` | Le membre `type` d'un codec doit être lisible **à travers un pointeur de base** (`VideoDecoder*`/`VideoEncoder*`/`AudioDecoder*`/`AudioEncoder*`), pour tous les codecs que la machine déclare supportés, + reproduction du prédicat de recréation de `VideoStream::RecVideo` | — (nouveau) |
| `test_rtp_latching.cpp` | `RtpLatching` | Latching RTP symétrique (NAT « comedia ») de `RTPSession`, testé **uniquement par le comportement observable** : un socket sonde en loopback joue le pair NATé, émet du média puis vérifie si celui de la session lui revient. Couvre l'annonce `0.0.0.0`, l'annonce privée autorisée (rattrapage), l'absence d'autorisation, l'annonce publique (pas de rattrapage), la réouverture du droit par un nouveau `SetRemotePort`, la demande d'image clé sur changement de source, **les deux faces de la clause ICE** — ICE réellement en jeu (le pair a répondu ses credentials) écarte le rattrapage ; ICE offert et **décliné** par le pair ne l'écarte pas, puisque plus personne ne posera la cible. Une dernière paire tient le **chemin ICE** lui-même, distinct du rattrapage (`natLatch` éteint, la sonde émet un vrai binding request) : la cible qu'ICE a validée survit à une renégociation qui repose le `c=` d'origine, et un **redémarrage ICE** (mot de passe distant différent) la rend au plan de contrôle. **+ 2 tests de caractérisation** de limites/défauts (voir plus bas) | — (nouveau) |
| `test_rtp_renegotiation.cpp` | `RtpRenegotiation` | **Le trou de l'offre/réponse en réception** (RFC 3264 §8) : nous appliquons la nouvelle map de réception en RÉPONDANT, l'offreur ne bascule qu'en RECEVANT cette réponse, et entre les deux ses paquets portent encore l'ancien payload type. Linphone renumérote à chaque re-INVITE (OPUS 96→98, VP8 96→107) ; ces paquets étaient jetés — inaudible en audio, **image figée en vidéo** jusqu'à l'intra suivante (traffic du 2026-08-13, ~450 ms perdues par renégociation). `RTPSession` garde donc la map précédente quelques secondes (`RTP_MAP_FALLBACK_MS`) et s'y rattrape, **sous la garde du codec** : un paquet n'est accepté que si son ancien codec est encore négocié sous un autre numéro. Deux tests sont les deux faces de cette garde — VP8 renuméroté 96→107 : rattrapé ; VP8 remplacé par H.264 : jeté. Deux autres tiennent sa **fermeture** : le repli est retiré dès qu'arrive un numéro que SEULE la nouvelle map porte (le pair a lu la réponse, inutile d'attendre la borne), et un numéro **commun aux deux** maps — PCMU sur 0 pendant qu'OPUS passe de 96 à 98 — ne le ferme pas, puisque le pair l'émettait déjà avant. Observation par `onNewStream`, le seul signal public émis APRÈS la résolution du codec, donc jamais pour un paquet jeté (même parti pris que `test_rtp_latching.cpp` : rien d'interne n'est inspecté). L'expiration du repli n'est pas testée — elle demanderait cinq secondes d'attente ou une horloge injectable | — (nouveau) |
| `test_rtp_rtcp.cpp` | `Rtp`, `RtpRtcp`, `RtpAdversarial`, `RtcpAdversarial` | **`Rtp`** : round-trips autonomes de l'en-tête `RTPPacket` (build → GetData → re-parse : seq/ts/ssrc/pt/marker/payload, bouclages 16/32 bits). **`RtpRtcp`** : parsing sur **capture réelle** (`fixtures/rtp_rtcp.pcap`) via `RTPPacket` + `RTCPCompoundPacket::Parse` — par paquet (version=2, pt<128, en-tête sain) et par flux (SSRC dominant : payload type constant, séquences en avant ≥90 %, timestamps non décroissants ≥90 %) ; côté RTCP, rapports SR/RR décodés. **`RtpAdversarial`/`RtcpAdversarial`** : paquets volontairement cassés (extension/CC surdimensionnés, mauvaise version, RTCP trop court / pt hors plage / length débordante) → parseurs qui ne crashent pas et détectent la malformation | — (nouveau) |
| `test_rtcp_hardening.cpp` | `RtcpCompound`, `RtcpSenderReport`, `RtcpReceiverReport`, `RtcpBye`, `RtcpExtendedJitter`, `RtcpApp`, `RtcpRtpFeedback`, `RtcpPayloadFeedback`, `RtcpFullIntraRequest`, `RtcpNack`, `RtcpSdes` | **Suite adverse RTCP** : chaque paquet MENT sur sa taille — en-tête annonçant plus long que le datagramme, compteur de rapports/SSRC/gigues/descriptions plus grand que la place disponible, longueur d'APP qui rend la soustraction négative. Les parseurs doivent refuser proprement **et** ne pas lire un octet de trop, ce que prouve la page de garde (cf. plus bas). Trois garde-fous de non-régression : compound valide, PLI valide, SDES/CNAME valide, plus un aller-retour NACK qui a mis au jour un décalage de champ | — (nouveau) |
| `test_rtp_header_hardening.cpp` | `RtpHeader` | **Suite adverse de l'en-tête RTP** : un en-tête décrit sa propre longueur (CSRC, extension jusqu'à 262 140 octets) sans que le datagramme la garantisse. Rejet des en-têtes menteurs (`IsValid()`), absence de longueur de média absurde, et parcours d'extension exécuté dans un paquet construit **au ras d'une page interdite**. Garde-fou : une extension audio-level honnête reste décodée | — (nouveau) |
| `test_red_fec_hardening.cpp` | `RedPayload`, `FecData`, `FecDecoder` | **Suite adverse RED (RFC 2198) / ULPFEC (RFC 5109)** : suite de blocs sans bloc final, longueurs de blocs qui dépassent le paquet, charge utile plus grande que le tampon de la FEC, longueur de protection mensongère, et premier paquet sur un décodeur vide (itérateur de fin déréférencé) | — (nouveau) |
| `test_rtmp_hardening.cpp` | `RtmpMessage`, `RtmpMediaFrame`, `RtmpChunkInput` | **Suite adverse RTMP** : message AMF3 de longueur nulle (dont l'octet de saut était lu hors du tampon), trame vidéo vide, flux de chunks dont le message n'a pas été ouvert. Garde-fous : commande AMF0 et trame AVC ordinaires | — (nouveau) |
| `test_websocket_http_hardening.cpp` | `WebSocketHandshake` | **La poignée de main face à un client qui fragmente** : la même requête que `test_websocket_echo.cpp`, écrite **octet par octet**, doit produire le `Sec-WebSocket-Accept` de l'exemple de la RFC 6455 — ce qui n'est vrai que si URL et en-têtes sont réassemblés à travers les appels du parseur HTTP. La même requête d'un seul tenant sert de point de comparaison | — (nouveau) |
| `test_jsr309_session_expiry.cpp` | `JSR309QueueLiveness`, `JSR309IdleQueues`, `JSR309SessionExpiry` | Expiration des `MediaSession` JSR-309 par inactivité du contrôleur (« vitalité par event queue », `jsr309_session_expiry_plan.md` §7 ; politique commune dans `eventqueuesweeper.h`). **`JSR309QueueLiveness`** : la preuve de vie elle-même sur `XmlEventQueue` — file neuve dans son délai de grâce, file jamais pollée qui expire (les « files nues »), poller attaché qui protège indéfiniment (le long-poll dort 30 s entre deux keep-alive), détachement qui relance le compte à rebours, reconnexion qui l'annule, et deux pollers dont seul le départ du dernier compte. **`JSR309IdleQueues`** : `GetIdleQueues` ne recense que les files abandonnées et oublie celles détruites. **`JSR309SessionExpiry`** : le balayeur détruit les sessions d'une file non lue puis la file, une référence « en vol » (`shared_ptr` détenu par un handler concurrent) survit à l'expiration, `EventQueueDelete` **arme** le délai sans rien détruire puis la session part à l'échéance, une session à `queueId` 0 n'est jamais balayée, un délai de 0 désarme tout, et `End()` joint le balayeur sans interblocage | — (nouveau) |
| `test_mcu_conference_expiry.cpp` | `McuConferenceExpiry` | Mêmes propriétés pour les **conférences de l'API MCU** (le `queueId` y est porté par la conférence) : destruction sur file non lue **avec libération du `tag`**, survie d'une référence en vol, `EventQueueDelete` qui arme au lieu de détruire puis destruction à l'échéance, conférence à `queueId` 0 (cas MOTELI) jamais balayée, délai 0 = désarmé, et `End()` qui joint le balayeur en libérant les tags. La couche « vitalité de la file » n'est pas retestée ici : elle est commune aux deux API | — (nouveau) |
| `test_transcoder_bridging.cpp` | `VideoBridgingTest`, `AudioBridgingTest` | **Mode pont dynamique** des transcodeurs JSR-309, observé *uniquement* par les paquets qui atteignent le puits (l'état `state`/`recCodec` est privé, et le mode est une conséquence, pas une valeur à consulter). Un puits instrumenté joue le contrat qu'un `RTPEndpoint` remplit en production : il répond `TryCheckCodec` d'après la liste de codecs qu'il porte. Couvre, pour la vidéo **et** pour l'audio : relais **octet pour octet** quand le puits porte le codec entrant, passage par le décodeur quand il ne le porte pas, et opt-in du drapeau (sans lui, le comportement historique — toujours décoder — reste intact, pour ne pas changer la sémantique du mixage ou de l'enregistrement sans le demander). Côté vidéo, un test de plus : le mode est **rejugé à chaque changement de codec entrant, dans les deux sens**, ce qui est tout l'intérêt de décider par paquet | — (nouveau) |

### Fixtures

`fixtures/rtp_rtcp.pcap` (pcap classique, link-type Ethernet) est extraite de la
capture `record.pcap` du dépôt, filtrée pour ne conserver que le trafic RTP/RTCP
(≈ 1690 paquets RTP + 49 paquets composés RTCP, 4 SSRC). Son chemin absolu est
injecté au test via `-DTEST_PCAP_FILE` (surchargeable : `make check TEST_PCAP=…`).
Le test lit le pcap avec un petit walker Ethernet/IPv4/UDP intégré (pas de
dépendance libpcap) et classe chaque datagramme via `RTCPCompoundPacket::IsRTCP`
puis la version RTP ; le bruit résiduel est ignoré. Fixture absente → test SKIPPÉ.

## Conception

Le principe directeur est le **round-trip** : produire une structure (trame,
message, chunk, en-tête) avec le code de sérialisation du mcu, puis la reparser
avec le code de désérialisation et asserter l'égalité. Cela exerce les deux
sens du même contrat sans dépendre de captures externes. Le seul test qui sort
de ce cadre est l'écho WebSocket, volontairement conservé comme test
d'intégration (sockets + threads réels) pour couvrir fidèlement ce que faisait
`wstest.cpp` + le client Python.

### Tests adverses de parseurs : la page de garde

Les suites `*_hardening.cpp` posent un problème que le round-trip ne résout pas : un parseur qui lit dix
octets de trop ne se voit pas. Le tampon voisin, dans le tas ou sur la pile,
contient presque toujours *quelque chose*, et le test passe.

`tests/guardedbuffer.h` rend donc le débordement bruyant. `GuardedBuffer` mappe
N pages accessibles suivies d'**une page `PROT_NONE`**, et place le paquet de
sorte que son dernier octet touche la frontière : lire un seul octet au-delà de
la fin du paquet déclenche un SIGSEGV. Comme celui-ci emporterait toute la
suite, le parsing tourne dans un `EXPECT_EXIT` — gtest fork, le fils meurt, et
seul *ce* test échoue.

Le même outil sert pour les objets, pas seulement pour les tampons d'entrée :
`RTPPacket` recopie le datagramme dans son propre tampon interne, donc c'est
l'**objet** qu'on construit au ras de la page interdite (placement `new` dans le
`GuardedBuffer`, taille arrondie à 8 pour rester aligné) — c'est ainsi qu'un
parcours d'extension de 256 Ko se voit.

Chaque suite adverse porte aussi ses **garde-fous de non-régression** (un paquet
honnête du même type, décodé correctement) : un durcissement qui jetterait le
trafic réel serait un défaut, pas une protection.

### Tests de composition vidéo : deux étages séparés

`test_mosaic_composition.cpp` sépare volontairement la **décision** de géométrie
et la **production** de pixels, parce que les deux échouent différemment :

- la géométrie (`BuildDesc`) est pure arithmétique : on peut donc couvrir les
  **12 types de composition × plusieurs ratios de source** pour quelques
  millisecondes, là où composer autant de graphes coûte une seconde ;
- la composition (`MosaicCompositor`) exige de vrais pixels. On pousse des trames
  de **luma uniforme** (`Pict::CreateColor`) : la mise à l'échelle d'une surface
  unie laisse la luma inchangée, donc un simple relevé de pixel au centre d'une
  vignette suffit à prouver que la bonne entrée a atterri au bon endroit — sans
  fixture d'image ni comparaison approximative.

`BuildDesc()` étant `protected`, il est atteint via l'**idiome du pointeur sur
membre** pris dans le contexte d'une classe dérivée (`MosaicProbe`), bien défini
par le standard, plutôt qu'un cast de type. `MosaicProbe` reste abstraite et
n'est jamais instanciée.

### Vérification par mutation des garde-fous

Les suites `MosaicGeometry`/`MosaicComposition` gardent deux corrections
apportées après des tests en appel réel. Leur
pouvoir de détection a été vérifié en **réintroduisant temporairement** chaque
bug et en constatant l'échec :

| Bug réintroduit | Tests qui échouent |
|---|---|
| `mosaic1p1` bridé à la moitié de la hauteur (`GetHeight` `rows=4,size=2`, `GetTop` `mosaicTotalHeight/4`) | `MosaicGeometry.Mosaic1p1UsesFullHeightFor4x3Source` |
| letterbox calculé sur le ratio GLOBAL de la mosaïque au lieu du ratio du slot | `Mosaic1p1UsesFullHeightFor4x3Source`, `Mosaic1p1UnchangedFor16x9Source`, `SlotsAlwaysFitInsideTheComposite`, `MosaicComposition.LetterboxBandsShowTheBackground` |
| `throw new std::runtime_error` rétabli dans `Mosaic::CreateMosaic` | `MosaicFactory.RejectsUnknownTypeWithoutThrowing`, `MosaicFactory.MixerRejectsUnknownCompositionAndStaysUsable` (« Unknown C++ exception thrown in the test body ») |
| `RTPSession::NatCorrectable` neutralisé (`return false`) | 6 des 11 `RtpLatching.*` ; survivent exactement les 5 qui doivent survivre — le chemin `0.0.0.0` (qui ne passe pas par la politique) et les 4 tests attendant justement l'absence de rattrapage |
| veto ICE rétabli sur nos SEULS credentials (`if (iceRemotePwd \|\| iceLocalPwd)`) | `RtpLatching.ReAimsWhenWeOfferedIceAndThePeerDeclinedIt`, et lui seul — les 12 autres passent, ce qui est le propre du bug : il ne se voit que sur la jambe où ICE est offert et décliné |
| `SetRemotePort` écrase de nouveau la paire validée (branche `iceOwnsSendAddr` neutralisée) | `RtpLatching.TheIceValidatedTargetSurvivesARenegotiation`, et lui seul |
| redémarrage ICE ignoré (`iceRestart` neutralisé dans `SetRemoteSTUNCredentials`) | `RtpLatching.AnIceRestartGivesTheTargetBackToTheControlPlane`, et lui seul — c'est le garde-fou qui empêche le correctif ci-dessus d'enfermer la session sur un pair parti |

Un test qui n'échoue pas quand le bug revient ne garde rien : refaire cette
vérification pour tout nouveau garde-fou ajouté ici.

### Tests tolérants à l'environnement

`WebSocketEcho.HandshakeAndTextEcho` ouvre un socket d'écoute loopback et s'y
connecte. En sandbox réseau restreinte (pas de bind/connect loopback possible),
il est **SKIPPÉ** (`GTEST_SKIP`) plutôt qu'échoué.

## Défauts mis au jour par la suite

Les round-trips ont révélé trois bugs latents (deux corrigés, un caractérisé),
plus une fuite mémoire trouvée par les tests adverses et corrigée (point 4).

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

4. **`RTCPCompoundPacket::Parse` — fuite mémoire sur entrée malformée (CORRIGÉE dans
   `rtp.cpp`).** Sur le chemin d'erreur « Wrong rtcp packet size » (champ *length*
   débordant, cf. `RtcpAdversarial.ParseRejectsLengthOverflow`), `Parse` faisait
   `return NULL` sans libérer le `RTCPCompoundPacket` déjà alloué ni le sous-paquet
   en cours — fuite à chaque paquet RTCP malformé reçu. Un second cas fuyait aussi le
   sous-paquet lorsque `packet->Parse` échouait (type connu mais corps invalide).
   Correctif : `delete packet; delete rtcp;` avant le `return NULL`, et `delete packet`
   dans la branche « non ajouté au compound » (delete nullptr est sans effet).

## Défauts trouvés en appel réel, désormais gardés par la suite

Ceux-ci n'ont **pas** été mis au jour par les tests : ils ont été diagnostiqués sur
des conférences réelles, et la suite a été étendue ensuite pour qu'ils ne puissent
pas revenir.

5. **Membre `type` masqué dans les codecs vidéo ffmpeg (CORRIGÉ dans
   `libmedikit/ffvideocodec.h`).** `FfVideoEncoder` et `FfVideoDecoder` redéclaraient
   chacun un `VideoCodec::Type type`, masquant celui, public, de leurs bases
   `VideoEncoder`/`VideoDecoder`. Les constructeurs renseignaient le membre **dérivé**,
   celui de la base restait **non initialisé** — or tout le mcu lit `->type` via un
   pointeur de base (`videostream.cpp:810`, `jsr309/VideoDecoderWorker.cpp:190`,
   `rtmpparticipant.cpp:949/961/973`,
   `FLVEncoder.cpp:404`). Le prédicat « recréer le codec si le type a changé » était
   donc toujours vrai : **le décodeur H264 était détruit et recréé à chaque paquet
   RTP**, le tampon de dépaquetisation ne survivait jamais jusqu'à une trame complète
   et aucune image n'était décodée (mosaïque vide → surface unie chez les endpoints).
   L'audio était épargné, `ffaudiocodec.h` ne masquant pas `type` — c'est l'indice qui
   a orienté le diagnostic. Garde-fou : suite `CodecType`.
   *Limite assumée* : un membre non initialisé peut fortuitement contenir la bonne
   valeur, donc ce garde-fou détecte la régression avec une probabilité élevée mais
   pas certaine ; il ne remplace pas l'interdiction de redéclarer `type`, rappelée par
   un commentaire à l'endroit exact des anciennes déclarations.

6. **`mosaic1p1` bridé à la moitié de la hauteur + letterbox calculé sur le ratio
   global (CORRIGÉS dans `asymmetricmosaic.cpp` / `mosaic.cpp`).** Voir « Vérification
   par mutation des garde-fous » ci-dessus pour le détail et les tests concernés.

9. **Type de composition inconnu = mort du mediaserver entier (CORRIGÉ dans
   `mosaic.cpp` / `videomixer.cpp`).** Les deux API de contrôle (XML-RPC
   `SetCompositionType`/`CreateMosaic`, JSR-309 `VideoMixerMosaicCreate`) castent un
   entier brut venu du réseau en `Mosaic::Type` **sans le valider**.
   `Mosaic::CreateMosaic` levait alors `throw new std::runtime_error` — un
   **pointeur**, qu'aucun `catch (const std::exception&)` ne peut intercepter — depuis
   un code exécuté sous `lstVideosUse.WaitUnusedAndLock()`. Résultat : `std::terminate`,
   donc **toutes les conférences tombaient** sur une seule valeur erronée du
   contrôleur ; et même attrapée, l'exception aurait laissé le verrou du mixer pris.
   Correctif en trois points : `CreateMosaic` journalise et rend `NULL` (style d'erreur
   du reste du code, et rien ne traverse plus le verrou) ; `VideoMixer::SetCompositionType`
   teste le retour, **relâche le verrou** et sort en erreur, la mosaïque en place restant
   valide ; `VideoMixer::CreateMosaic` propage l'échec par **-1** (et non 0, qui est l'id
   légitime de la mosaïque par défaut) et `VideoMixer::Init` refuse de démarrer sur un
   type invalide au lieu d'installer une `defaultMosaic` nulle. Le handler XML-RPC
   `CreateMosaic` teste désormais `< 0`, ce que le handler JSR-309 faisait déjà.
   Garde-fou : suite `MosaicFactory`. Le `throw new` jumeau de `PartedMosaic` (devenu
   inatteignable) lève au moins **par valeur**.

## Défauts mis au jour par les tests de latching (NON corrigés, caractérisés)

Ces deux-là ont été trouvés en écrivant `test_rtp_latching.cpp` : le test attendu
échouait, et l'analyse a montré que le code — non le test — était en tort. Ils sont
**épinglés par des tests de caractérisation** qui décrivent le comportement actuel,
sur le modèle d'`Amf.NumberZeroDecodeQuirk`. Chacun réussit **tant que le défaut
existe** ; il devient rouge dès qu'on le corrige, ce qui est le signal d'inverser
ses attentes.

7. **Un changement de PORT source seul n'est pas relevé**
   (`RtpLatching.PortOnlyMappingChangeIsNotFollowed`). La source observée n'est
   relevée que si l'ADRESSE change : `rtpsession.cpp:2097` teste `recIP` seul, donc
   `recPort` n'est jamais rafraîchi quand le pair garde son IP et rebinde son
   mapping — ce qu'un NAT symétrique fait couramment. Le média continue vers
   l'ancien port. Le commentaire de `SendPacket` (« recIP est recalé sur *chaque*
   paquet de source différente ») décrit l'intention, pas le code.

8. **L'observation périmée brûle le one-shot après un re-INVITE**
   (`RtpLatching.StaleObservationBurnsTheOneShotWhenSendingContinues`).
   `SetRemotePort` remet `natCorrected` à false — l'intention explicite étant
   qu'« un pair qui change de mapping ne reste pas coincé sur l'ancien » — mais
   laisse `recIP`/`recPort` pointer sur l'observation précédente. Or le média coule
   en continu : le premier paquet sortant après le re-INVITE précède la première
   trame du pair déplacé, `SendPacket` ré-aiguille donc sur l'observation périmée et
   **reconsomme le one-shot**. Quand le nouveau pair se manifeste, `natCorrected`
   vaut déjà true : plus aucune correction n'est possible, la session reste bloquée
   sur l'ancien pair en journalisant en boucle « WARNING Trying to send packet from
   different ip address than receiving one ». En production la course est presque
   toujours perdue (audio émis toutes les 20 ms contre un pair qui ne parle qu'après
   son answer). Le mécanisme de réouverture ne fonctionne que si le pair déplacé se
   manifeste avant toute ré-émission (`NewRemotePortReopensTheRightToReAim`, cas
   d'un émetteur au repos). Correction probable : oublier l'observation
   (`recIP`/`recPort`) quand le plan de contrôle pose une nouvelle cible.
