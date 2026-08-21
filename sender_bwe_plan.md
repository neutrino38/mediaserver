# Estimateur de bande passante côté émetteur (lot 6) — conception v1

> Document de conception du lot 6 de [`rate_control_plan.md`](rate_control_plan.md),
> écrit au GO du portillon (annexe D de [`rate-control.md`](rate-control.md),
> séance du 2026-08-19). Le périmètre v1 et les interfaces y étaient figés ;
> ce document les décline en architecture, décisions et sous-lots.

## 1. Objet, et ce que la v1 n'est pas

Le mcu ne sait pas mesurer le sort de ses propres paquets. Quand il émet vers un
pair, il subit deux signaux pauvres : le REMB/TMMBR que le pair veut bien
renvoyer (une opinion, par flux), et rien d'autre. Le lot 6 construit ce qui
manque : un estimateur **côté émetteur**, nourri par les temps d'arrivée que le
pair rapporte (transport-cc, puis CCFB RFC 8888), qui produit un débit cible
**par patte sortante** et l'applique à l'encodeur par le levier existant.

La v1 se limite volontairement (fiche du lot 6, actée) :

- **cœur** : trendline + AIMD avec `LinkCapacityEstimator`, étage de perte =
  logique historique 2 %/10 % ;
- **hors v1** : sondage actif, détection ALR, `LossBasedBweV2`, fenêtre de
  congestion, unification audio+vidéo (pas de bundle dans le mcu), abandon de
  couche. La montée en découverte est multiplicative ×1,08/s, point.

> **Le jour où le sondage actif arrivera : ses paquets ne passent PAS par
> `RTPEndpoint::onRTPPacket`.** Ce chemin est celui d'un paquet remis par une
> SOURCE attachée (transcodeur, mixeur, player), et il jette désormais tout
> paquet de longueur média nulle — donc toute sonde entièrement en bourrage.
> C'est voulu : une sonde reçue d'un pair n'a aucun sens relayée sur l'autre
> patte, elle y invalide même l'image du destinataire (voir plus bas). Mais une
> sonde que NOUS produisons est un paquet à nous, pas un paquet relayé : elle a
> besoin de son propre chemin d'émission, sur le modèle de
> `RTPSession::SendEmptyPacket` qui fait son `sendto` directement. Sans ça le
> garde l'avalerait en silence, et un sondage qui n'émet rien se lit comme un
> lien qui ne monte pas.
>
> Le garde vient de la seconde moitié de `28970c8`. Ce commit avait retiré le
> bourrage de la charge utile (RFC 3550 §5.1) et réglé le dépaquetiseur LOCAL —
> les sondes de Chrome n'entrent plus dans notre décodeur VP8 comme du média. Il
> restait le relais : après retrait du bourrage la sonde est un paquet de
> longueur nulle, et l'émettre vers le pair lui livre un paquet RTP vide portant
> l'horodatage de l'image en cours, qui invalide l'image entière chez lui.
> Capture du 2026-08-21 20:09, Chrome → Linphone en VP8 relayé : 1356 des 1368
> paquets d'Alice relayés à l'octet près, et les 12 qui portent du bourrage
> arrivés vides. Trois tombaient dans l'intra, d'où une image cassée dès le
> décroché qui ne se rétablissait jamais.
>
> Effet mesuré sur l'estimateur, à connaître mais négligeable : ces octets ne
> sont plus comptés comme émis, soit ~2 kb/s sur l'échantillon. C'est le bon
> sens — ils ne portaient aucun média.

Doctrine inchangée : le chemin REMB côté réception se **répare** (fait, lots
0-3) mais ne se raffine pas. L'estimateur émetteur est du code **neuf** : lui se
construit aligné sur le témoin actuel, pas sur l'ancêtre.

## 2. Témoin et licence

Témoin : `../webrtc`, commit `e12c39e03c` (le même que
[`docs/reference/kalman-vs-webrtc.md`](docs/reference/kalman-vs-webrtc.md)).
Les modules dont la v1 dérive, avec leur taille réelle :

| module témoin | rôle | lignes |
|---|---|---|
| `congestion_controller/rtp/transport_feedback_adapter.cc` | historique d'émission, appariement feedback | 480 |
| `goog_cc/inter_arrival_delta.cc` | groupes d'envoi, deltas | 141 |
| `goog_cc/trendline_estimator.cc` | pente + seuil adaptatif | 326 |
| `remote_bitrate_estimator/aimd_rate_control.cc` + `goog_cc/link_capacity_estimator.cc` | AIMD, mémoire de capacité | ~450 |
| `goog_cc/bitrate_estimator.cc` (+ `acknowledged_bitrate_estimator`) | débit acquitté | 242 |
| `goog_cc/send_side_bandwidth_estimation.cc` (étage de perte seul) | perte 2 %/10 %, plafond délai | ~150 utiles |

Licence (arbitrage A5 du plan, confirmé ici) : BSD-3 → GPL est compatible, mais
la v1 est **réécrite en style maison**, comme le lot 1 l'a fait — mêmes
constantes, même comportement, chaque divergence tracée contre le fichier:ligne
du témoin. Aucune recopie de fichier sans instruction formelle du mainteneur.

## 3. Ce que le dépôt offre déjà (inventaire du 2026-08-19)

Ce qui existe et se réutilise :

- **Le RTT est déjà calculé** depuis le DLSR des SR/RR
  (`rtpsession.cpp:3092-3125`, filtré par `sendSSRC`/`sendSR`) et remonte par
  `RTPSession::SetRTT`.
- **Les pertes rapportées par le pair sont déjà décodées… puis jetées** : dans
  les blocs RR reçus, seul `GetDelaySinceLastSRMilis` est lu ; `GetFactionLost`
  et `GetLostCount` (`rtp.h:526-531`) ne sont appelés nulle part. L'étage de
  perte de la v1 a sa donnée, il suffit de la brancher.
- **Le levier d'application existe et il est unique** : REMB reçu et TMMBR reçu
  convergent tous deux vers `VideoStream::SetTemporalBitrateLimit`
  (`videostream.cpp:116`, plafond persistant en kb/s, consommé par la boucle
  d'encodage `videostream.cpp:546-580`) ; côté JSR-309, l'homologue est
  `Joinable::SetREMB` → `VideoEncoderMultiplexerWorker::SetREMB`
  (`VideoEncoderWorker.cpp:272-280`). La consigne ne pilote **que l'encodeur**
  — c'est le bon endroit, et la v1 ne crée pas de second levier.
- **Un embryon d'historique d'émission** : la map `rtxs` de `RTPSession`
  (`rtpsession.h:433`, remplie dans `SendPacket` `rtpsession.cpp:2106-2130`).
  Inutilisable en l'état : conditionnée à `useNACK`, indexée par seq RTP,
  stockant le paquet chiffré complet, purgée au compte (200) et non à la durée.
- **L'extension abs-send-time est écrite à l'émission**
  (`rtpsession.cpp:2051-2074`) : la mécanique d'écriture d'une extension
  one-byte `0xBEDE` existe, il n'y a qu'à la généraliser. La branche est morte
  en pratique (aucun appelant ne pose la propriété) — la plomberie est saine,
  c'est la négociation qui manque.

Ce qui manque entièrement :

- **Aucun transport-wide sequence number** nulle part (émission ou réception).
- **Aucune classe RTCP pour RTPFB fmt 15 ni fmt 11** : l'enum
  (`rtp.h:916-921`) s'arrête à NACK/TMMBR/TMMBN, et un fmt inconnu produit une
  ligne `Error` **par paquet reçu** (`rtp.cpp:946`) — dès que le pair négociera
  transport-cc, le journal sera pollué : la classe de parsing est aussi un
  correctif de bruit. Attention : la boucle de parsing actuelle suppose des
  champs de taille fixe ; transport-cc et CCFB sont à taille variable et
  demandent leur propre chemin de décodage.
- **Aucun pacing au sens du témoin** (cf. §6).

## 4. Architecture v1

```
                     RTP sortant (+ extension transport-wide seq n° 6.1)
   VideoEncoderWorker ──► smoother ──► RTPSession::SendPacket ──► réseau
                                            │
                                            ▼ (à l'envoi)
                                    SentPacketHistory          (6.1)
                                    seq TW → (taille, t_envoi)
                                            │
   RTCP entrant                             ▼ (à l'appariement)
   RTPFB fmt 15 / fmt 11 ──parse──► SenderBWE                  (6.2/6.3)
   RR (fraction lost, DLSR)  ────►    ├─ deltas par groupe d'envoi (5 ms)
                                      ├─ trendline (20 paquets) + seuil adaptatif
                                      ├─ débit acquitté (fenêtres 500/150 ms)
                                      ├─ AIMD + LinkCapacityEstimator (±3σ)
                                      └─ étage de perte 2 %/10 %
                                            │  cible = min(perte, plafond délai)
                                            ▼
                        listener → min(cible BWE, REMB/TMMBR du pair)
                                 → SetTemporalBitrateLimit / SetREMB   (encodeur)
```

### Décisions de conception (avec justification)

**D1 — Un estimateur par patte sortante, membre de `RTPSession`.** Le mcu n'a
pas de bundle : chaque média a sa session, donc son transport — un compteur
transport-wide **par session** est conforme au draft (le « transport », c'est
la session). Le budget par patte du §5.4 tombe naturellement. L'unification
audio+vidéo n'aura de sens qu'avec le bundle, hors v1. Contrairement au
`remoteRateEstimator` (membre de l'Endpoint, partagé entre jambes — et source
des crashs du 2026-08-17), le `SenderBWE` appartient à **sa** session : pas de
notification croisée entre jambes, pas d'ordre de destruction piégeux.

**D2 — Trois modules neufs, style du dépôt (une classe, un fichier).**

| fichier | contenu | homologue témoin |
|---|---|---|
| `mcu/{include,src}/sentpackethistory.{h,cpp}` | historique borné par la durée (60 s), `seq TW → {taille, t_envoi}` ; appariement d'un rapport → liste `(t_envoi, t_arrivée, taille)` triée par arrivée ; compteur de seq TW | `transport_feedback_adapter` |
| `mcu/{include,src}/trendlinedetector.{h,cpp}` | groupes d'envoi (5 ms, rafale ≤ 100 ms, reset après 3 groupes réordonnés), régression sur 20 paquets (lissage 0,9), seuil adaptatif `k_up 0,0087 / k_down 0,039`, bornes [6 ; 600] ms, départ 12,5 ms, excursions > seuil+15 ms ignorées, hypothèse = durée > 10 ms ET 2 échantillons ET pente non décroissante | `inter_arrival_delta` + `trendline_estimator` |
| `mcu/{include,src}/senderbwe.{h,cpp}` | débit acquitté (fenêtre 150 ms, 500 ms au départ), AIMD (`beta` unique 0,85, **descente sur le débit acquitté**), `LinkCapacityEstimator` (α 0,05, bornes ±3σ ; capacité connue → montée additive, sinon ×1,08/s), étage de perte (< 2 % → +8 % du min sur 1 s + 1 kb/s ; 2-10 % → rien ; > 10 % → ×(512−perte)/512 au plus une fois par 300 ms + RTT), plafond délai par `min()`, phase de départ 2 s, orchestration et traces | `bitrate_estimator`, `aimd_rate_control`, `link_capacity_estimator`, `send_side_bandwidth_estimation` |

Les régions (`MaxUnknown`/`NearMax`/…) **n'existent pas** dans ce code neuf :
le témoin les a remplacées par la mémoire de capacité, on ne les réintroduit
pas. Pas non plus de Kalman : la trendline n'a pas d'état à mal initialiser.

**D3 — Le format de fil est un module partagé avec le lot 4.**
`mcu/{include,src}/transportfeedback.{h,cpp}` porte la **construction** (lot 4,
nous rapportons) et le **parsing** (lot 6, nous consommons) du RTPFB fmt 15,
puis du CCFB fmt 11 derrière la même interface (ordre = arbitrage A4). Celui
des deux lots qui s'implémente le premier crée le module ; l'autre le complète.
Le dispatch se branche dans `RTCPRTPFeedback::Parse` (`rtp.cpp:904`) avec un
chemin de décodage à taille variable propre — et fait taire l'`Error` par
paquet au passage.

**D4 — La consigne s'applique par le levier existant, en `min()` avec le pair.**
`SenderBWE` notifie `RTPSession::Listener` (nouveau rappel
`onSenderEstimatedBitrate`). `RTPParticipant` et `RTPEndpoint` le traduisent
vers le même point que le REMB reçu aujourd'hui. `videoBitrateLimit` (et son
homologue JSR-309) est mono-valeur, dernier écrivain gagnant : la composition
devient **deux champs** (limite du pair, limite BWE locale) dont la boucle
d'encodage prend le `min()`. Le lot 5 (propagation amont par le throttler)
consommera la même cible ; rien à prévoir de plus ici.

**D5 — Verrouillage : les leçons du 2026-08-17 s'appliquent d'emblée.** Le
feedback arrive sur le thread RTCP de la session (celui de
`ProcessRTCPPacket`), l'estimation se met à jour là. La notification du
listener se fait **hors** du verrou écrivain de l'estimateur (le motif
lecteur/`IncUse` du lot 1 n'est même pas nécessaire : pas de partage entre
jambes, cf. D1). Déclaration des membres : l'estimateur avant tout membre qui
le référence.

**D6 — Négociation : deux propriétés, posées par elixip.** Symétrique de
l'existant `abs-send-time` (`rtpsession.cpp:622-626`) : la propriété extmap
`http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01`
arme l'écriture de l'extension sur nos paquets sortants (id pris dans
`extMap`), et `a=rtcp-fb:* transport-cc` accepté par le pair autorise à
espérer du feedback. Sans négociation : extension non écrite, estimateur muet,
comportement actuel inchangé — même doctrine que le lot 2 (A2 : rien de
spontané). La moitié elixip est la même que celle du lot 4 (offrir/accepter
extmap + rtcp-fb) : **un seul chantier SDP côté contrôleur pour les deux lots.**

## 5. Ce que le témoin fait et que la v1 ne fait pas — dit explicitement

| capacité témoin | sort en v1 | conséquence assumée |
|---|---|---|
| sondage 3×/6× au départ | absent | montée initiale au rythme ×1,08/s : ~15 s pour ×3 — la phase de départ 2 s + consigne négociée en atténuent le coût |
| ALR (seau à jetons 65 %/80 %) | absent | une source qui n'a rien à dire pourra faire baisser l'estimation ; acceptable en visio permanente, à revoir avec le partage de document |
| `LossBasedBweV2` | absent | pertes gérées par l'étage 2 %/10 % — celui que le témoin garde en repli |
| fenêtre de congestion (pushback) | absente | pas de contre-pression sur la file d'émission ; le pacing v1 (§6) reste ouvert |
| BWE audio | absente | seules les pattes **vidéo** portent un estimateur ; l'audio est à débit quasi constant |

## 6. Pacing — évaluation de `RTPSmoother` (prérequis exigé par la fiche)

Verdict : **base utilisable, insuffisante telle quelle, suffisante pour la v1
après un réglage.** Constat (inventaire 2026-08-19) :

- `RTPSmoother` (chemin legacy `videostream.cpp:667`) et
  `RTPMultiplexerSmoother` (chemin JSR-309 transcodé,
  `VideoEncoderWorker.cpp:595`) font la même chose : répartir les paquets
  d'**une** image proportionnellement aux octets sur une fenêtre `duration`
  fournie par l'appelant (`SetSendingTime(current*duration/frameLength)`).
  Aucun débit cible, aucun budget inter-images, aucune dette.
- La fenêtre vaut déjà `bits de l'image / débit cible`, plafonnée à la période
  d'image (`videostream.cpp:660-666`, `VideoEncoderWorker.cpp:586-593`) : c'est
  un pacing par image au débit cible, sans mémoire.
- Le chemin **relayé** (mode pont) court-circuite tout :
  `VideoTranscoder.cpp:214` fait `Multiplex(packet)` direct, et
  `RTPEndpoint::onRTPPacket` émet dans le thread appelant. L'audio et le texte
  ne sont jamais lissés.

Décision v1 : sur les chemins **encodés**, la fenêtre de lissage se calcule au
**débit de pacing = 1,1 × la cible** (le facteur du témoin dès que l'estimation
dépend des temps d'arrivée), au lieu du débit cible nu — un changement de
formule, pas d'architecture. Sur le chemin **relayé**, pas de pacing v1 : les
paquets relayés arrivent déjà espacés par l'émetteur d'origine, les re-lisser
n'apporterait rien et le fil direct préserve la latence. Un pacer à budget
(dette inter-images, priorités, audio compris) reste le chantier de suite si la
mesure du 6.5 montre que les rafales résiduelles polluent la trendline.

## 7. Sous-lots

### 6.1 — Plomberie (extension + format de fil + historique)

1. Extension transport-wide seq à l'émission (`SendPacket`, à côté de
   l'abs-send-time ; compteur par session ; reprise à l'identique dans le
   chemin RTX `rtpsession.cpp:3565`) + lecture à la réception (servira au
   lot 4).
2. `transportfeedback.{h,cpp}` : construction + parsing fmt 15 (base time,
   deltas, chunks run-length/status-vector, wrap 16 bits) ; CCFB fmt 11 ensuite.
3. `sentpackethistory.{h,cpp}` : fenêtre 60 s, appariement rapport → résultats.
4. Tests : aller-retour construction/parsing (trous, wrap, deltas négatifs,
   rapport plein), hardening du parseur (modèle `test_rtcp_hardening.cpp`,
   page de garde `PROT_NONE`), historique (purge, appariement partiel,
   doublons de feedback).

### 6.2 — Cœur (sans réseau, sous fake clock)

`trendlinedetector` + `senderbwe`, portés par une suite
`mcu/tests/test_sender_bwe.cpp` sur le modèle de `test_rate_control.cpp`
(cible `make check-senderbwe`) : nominal (convergence sur lien stable),
adverses (rafales, réordonnancement, feedback perdu/dupliqué, pertes 2/10 %,
excursion > seuil+15 ignorée pour l'adaptation), et les gardes-fous que le
lot 3 a payés (un seul retour au calme relance la montée ; la descente porte
sur le débit **acquitté** ; pas de gel au plafond). Les constantes se vérifient
contre le témoin fichier:ligne, comme au lot 1.

### 6.3 — Intégration

Membre `RTPSession`, dispatch RTCP branché, RR (fraction lost + RTT) branchés,
notification listener, composition `min()` dans `videostream` et
`VideoEncoderWorker`, traces `BWE-TX: estimation stream=… state=… target=…
acked=…` (mêmes conventions que `BWE:` pour que l'outillage du lot 3 se
généralise).

### 6.4 — Pacing v1

La formule 1,1× sur les deux smoothers (§6), et rien d'autre.

### 6.5 — Négociation, recette et mesure (portillon interne)

La négociation ne se coupe pas en deux : l'`a=rtcp-fb transport-cc` de la ligne
média engage les deux sens. Il n'existe pas de montage où l'on consomme le
feedback du pair sans lui devoir le sien — activer le bouton sans générateur de
rapports fait reculer le pair jusqu'à son plancher (mesure du 2026-08-19,
`rate_control_plan.md` lot 4). Le lot 4 est livré, donc la recette peut avoir
lieu : elle éprouve les deux sens à la fois.

- elixip : extmap + `a=rtcp-fb:* transport-cc` (chantier SDP commun avec le
  lot 4). **Fait** ; le bouton `[mediaserver] transport_cc` reste à `false`.
- Recette : face à un Chrome **récepteur**, `webrtc-internals` montre nos
  rapports consommés ; pcap de nos paquets portant l'extension.
- Mesure : mêmes scénarios netem que le lot 3 mais sur notre lien **sortant**
  (egress natif — plus simple que `--ingress`), `bwe_report.py` étendu aux
  traces `BWE-TX:`, mêmes critères que l'annexe D. C'est la séance qui décide
  si le pacer à budget (§6) est nécessaire.

### 6.6 — L'estimateur vit sans transport-cc (prérequis du lot 7)

Constat vérifié le 2026-08-20 : face à un pair qui n'offre pas
`a=rtcp-fb:* transport-cc`, l'estimateur est **muet à jamais**, y compris son
étage de perte. `SetStartBitrate` et `SetMinMaxBitrate` n'ont aucun appelant en
production (seuls les tests les appellent) ; `delayInitialized` ne devient vrai
que dans `UpdateDelayEstimate`, atteint par le seul `ProcessFeedback` (fmt 15) ;
et `GetEstimatedBitrate` rend 0 tant que `lossBasedTarget` est nul
(`senderbwe.cpp:140-149`), lui-même amorcé sur `delayCurrentBitrate`
(`senderbwe.cpp:540`). Or les RR et les SR sont parsés quel que soit le dialecte
(`rtpsession.cpp:3169` et `:3195`) : `UpdateFractionLost` est bien appelé, il
tourne à vide.

C'est la moitié « perte » du témoin, qui chez lui existe **sans** signal de
délai. La rendre autonome :

1. amorcer la patte sur sa consigne négociée — `SetMinMaxBitrate(16000, consigne)`
   puis `SetStartBitrate(consigne)` là où la consigne devient connue côté session ;
2. `lossBasedTarget` part de cette valeur d'amorçage au lieu d'exiger
   `delayInitialized` ; la borne de délai reste conditionnelle, le `min()` de
   `GetEstimatedBitrate` l'est déjà.

Effet attendu, et c'est tout le contrôle d'émission vers un pair SIP : la cible
descend sur les pertes rapportées et remonte quand elles cessent, au lieu de
rester à la valeur signalée quoi qu'il arrive.

**Tests** (horloge simulée, aucun fmt 15 injecté) : consigne posée → cible égale à
la consigne ; deux rapports à 12 % → cible en baisse, une seule fois par 300 ms +
RTT ; pertes < 2 % → remontée +8 % par seconde ; jamais sous `minConfiguredBitrate`.

### 6.7 — La consigne descend dans le chemin JSR-309 (prérequis du lot 7)

`Joinable::SetSenderEstimate` est un no-op par défaut (`Joinable.h:53`) et
`VideoTranscoder` ne le redéfinit pas, alors que c'est lui le joinable attaché à
un `RTPEndpoint` (`RTPEndpoint.cpp:468`). En 1:1 — donc dans tous les appels
elixip — l'estimation d'émission n'atteint **rien** : ni l'encodeur en
transcodage, ni la source en pont. Seul le chemin conférence est complet
(`videostream.cpp:133`).

Implémenter `VideoTranscoder::SetSenderEstimate` sur le modèle de
`VideoTranscoder::SetREMB` (`VideoTranscoder.cpp:117`) : state 1 →
`encoder.SetSenderEstimate` (la composition par `min()` avec la limite du pair
existe déjà, `VideoEncoderWorker.cpp:525`) ; state 2 → `j->SetREMB` vers la
source, borné par la consigne négociée ; state 0 → rien.

**Tests** : les trois états, et la borne « consigne négociée » en pont.

## 8. Ordre et dépendances

6.6 et 6.7 sont indépendants l'un de l'autre, se testent sous horloge simulée, et
sont tous deux **prérequis du lot 7** (`rate_control_plan.md`) : sans 6.7 la
consigne d'émission n'atteint aucun organe en 1:1, sans 6.6 elle n'existe même pas
face à un pair sans transport-cc — soit, probablement, Linphone.

6.1 → 6.2 → 6.3 → 6.4 sont séquentiels côté mcu et ne dépendent **pas**
d'elixip : tout se teste sous fake clock et en pcap rejoué. 6.5 exige la moitié
elixip (commune au lot 4 — la planifier une fois pour les deux). Le lot 4
(générateur côté réception) reste indépendant : il partage `transportfeedback`
et l'extension, c'est tout.

## Suivi

- [x] 6.0 — ce document
- [x] 6.1 — extension + `transportfeedback` + `sentpackethistory` (2026-08-19,
      15 tests : aller-retours, troncatures, enroulements)
- [x] 6.2 — `trendlinedetector` + `senderbwe` + `test_sender_bwe.cpp`
      (2026-08-19, 14 tests sous horloge simulée, `make check-senderbwe`)
- [x] 6.3 — intégration `RTPSession` (historique à l'envoi, fmt 15 apparié,
      pertes RR/SR branchées, RTT partagé, `onSenderEstimatedBitrate` composé
      par `min()` participant + JSR-309, no-op en relais) (2026-08-19)
- [x] 6.4 — pacing 1,1× sur les deux smoothers (2026-08-19)
- [ ] 6.5 — elixip SDP (extmap + `a=rtcp-fb:* transport-cc`, chantier commun
      lot 4) FAIT, lot 4 FAIT, recette Chrome VALIDÉE (2026-08-20). La première
      séance egress a rendu le verdict du portillon : la re-montée était
      prisonnière du plafond 1,5 x l'acquitté, qui se refermait sur notre
      propre encodeur dès que la source émettait moins que la cible (cible
      gelée à 318 kb/s sur un lien revenu à 2000). Corrigé (commit 97fee86) :
      l'estimateur mesure le débit émis, et le plafond ne s'applique qu'en
      régime limité par le réseau — la moitié « détection » de l'ALR, toujours
      sans sondes ni padding. **Reste** à rejouer la séance egress avec une
      source animée en continu (contrôle : `sent=` > 1000 dans les traces
      BWE-TX pendant toute la séance) pour remplir l'annexe D côté émetteur
- [ ] 6.6 — amorçage hors transport-cc (l'étage de perte vit seul) ; prérequis
      du lot 7, cas Linphone
- [ ] 6.7 — `VideoTranscoder::SetSenderEstimate` (states 1 et 2) ; prérequis du
      lot 7, tous les cas 1:1. Précision au passage sur le « no-op en relais »
      noté en 6.3 : dans le chemin JSR-309 la consigne est aujourd'hui no-op
      dans les DEUX modes, pas seulement en relais
