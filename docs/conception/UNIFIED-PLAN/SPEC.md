# Flux vidéo multiples par participant (RFC 9429, BUNDLE)

> Statut : **en cours**. Branche `feat/unified-plan`. Le chantier préalable
> (famine de `ChangeStream`, §5) et le lot 0 (extension MID) sont faits ; les
> lots 1 à 5 restent à faire.
>
> Le serveur média ne parle pas SIP. La signalisation et le SDP sont tenus par
> un contrôleur externe (elixip), qui pilote le serveur par XML-RPC ou par
> RabbitMQ (MOTELI). « Contrôleur » désigne cet appelant dans tout le document.
>
> Arbitrages : [ADR 004](../../architecture/adr-004-bundle-session-porteuse.md).

## 1. Objectif

Un participant apporte **plusieurs flux vidéo** dans une même communication :
sa caméra et son écran partagé, ou deux caméras. Chaque flux devient une
**source à part entière du mixeur vidéo**, et prend **un slot dans une
mosaïque** de conférence, comme si c'était un participant de plus.

Le serveur sait déjà faire cela pour **un** cas : le partage de document
BFCP, où un endpoint SIP envoie un second flux vidéo (SLIDES) à côté de sa
caméra. Ce chantier généralise ce montage à N flux, et y ramène le BFCP
lui-même, pour qu'un écran partagé depuis un navigateur et un document
partagé depuis un endpoint SIP soient la même chose vue du mixeur.

Le client visé est un navigateur. Un navigateur suit la RFC 9429 (JSEP) : il
offre **une section `m=` par piste**, et il rassemble toutes ses sections sur
**un seul transport** (BUNDLE, RFC 8843). Le serveur doit accepter cette offre
telle qu'elle est, sans réglage particulier du client.

| Sujet | Norme |
|---|---|
| Offre/réponse d'un navigateur (JSEP, Unified Plan) | **RFC 9429** |
| Plusieurs `m=` sur un transport (BUNDLE) | **RFC 8843** |
| Groupes de `m=` dans le SDP (`a=group`) | RFC 5888 |
| RTP et RTCP sur un même port (`a=rtcp-mux`) | RFC 5761 |
| Extensions d'en-tête RTP (`a=extmap`) | RFC 8285 |
| Identifiant de flux `a=msid` | RFC 8830 |
| Rôle d'un flux vidéo, `a=content:main/slides` | RFC 4796 |
| Contrôle de parole du partage de document | RFC 4582, RFC 8855 (fait : `docs/reference/bfcp.md`) |
| Simulcast (`a=rid`, `a=simulcast`) | RFC 8851, RFC 8853 — **hors périmètre** |

Périmètre :

- N jambes vidéo par participant de conférence, N sources dans le mixeur,
  chacune avec une entrée (la mosaïque qu'elle reçoit) et une sortie (ce
  qu'elle apporte aux mosaïques) ;
- le partage de document BFCP ramené sur ce modèle ;
- un transport partagé par participant (BUNDLE), audio, vidéo, texte et data
  channel compris ;
- l'extension d'en-tête RTP `sdes:mid`, qui sert à démultiplexer ;
- l'API de contrôle MCU, puis JSR-309, et les schémas MOTELI ;
- ce que le contrôleur doit faire (§4.7), sans le coder ici.

Hors périmètre : §8.

## 2. Le fait structurant : sans BUNDLE, la deuxième vidéo n'est pas négociable

C'est le point qui fixe la forme du chantier.

Le serveur n'a jamais fait de BUNDLE. Chaque jambe a son port, son ICE et son
DTLS, et le contrôleur ne répond jamais `a=group:BUNDLE`. Un navigateur s'en
accommode **pour une piste de chaque type**. Il ne s'en accommode pas pour la
deuxième vidéo.

La RFC 9429 §4.1.1 définit la politique `balanced`, qui est celle par défaut
de toutes les `RTCPeerConnection` :

> The first "m=" section of each type (audio, video, or application) will
> contain transport parameters, which will allow an answerer to unbundle that
> section. The second and any subsequent "m=" sections of each type will be
> marked as bundle-only.

Une section `bundle-only` est offerte avec le **port 0** (§5.2.1). Si le
serveur ne prend pas le groupe BUNDLE, la RFC 8843 §7.2 range cette section
avec les sections rejetées : la deuxième vidéo **n'existe pas** dans l'appel.
Ni le contrôleur ni le serveur ne peuvent y remédier après coup.

Seule la politique `max-compat` donne un transport à chaque section. Elle se
règle dans le code JavaScript du client, pas dans le SDP. Elle vaut donc pour
un client que nous écrivons, jamais pour un client tiers.

Conséquences :

1. **Le BUNDLE est un prérequis** de la fonction, pas une amélioration.
2. Les deux moitiés du chantier sont indépendantes : les **N sources du
   mixeur** (lot 1) se font et se recettent sans BUNDLE, avec un client réglé
   en `max-compat` ou avec un endpoint SIP BFCP ; le **transport partagé**
   (lot 2) se fait à côté.
3. Le BUNDLE impose `a=rtcp-mux` sur toutes les sections du groupe (RFC 8843
   §9.3). Le serveur le sait déjà faire.

## 3. Ce qui existe (inventaire vérifié)

| Élément | Emplacement | État |
|---|---|---|
| Deux jambes vidéo au plus, indexées par le rôle | `MAX_VIDEO_STREAM 2`, `video[]` `mcu/include/rtpparticipant.h:22,145` | **à ouvrir, §4.2** |
| Énumération du rôle | `MediaRole {VIDEO_MAIN=0, VIDEO_SLIDES=1}` `libmedikit/medkit/media.h:52` | **à ouvrir, §4.2** |
| Second flux vidéo BFCP : un `VideoStream` SLIDES, ses deux pipes | §4.1 | **précédent du modèle** |
| Relais des slides du présentateur vers la source 2 | `SharedDocMixer::NextFrame` `mcu/src/shareddocmixer.cpp:219` | **à retirer, §4.3** |
| Source d'émission des slides, `partId+100000` | `MultiConf::SetAppCodec` `mcu/src/multiconf.cpp:1880`, `SetDocSharingMosaic` `:1293` | **à remplacer, §4.3** |
| Paramètre `role` sur l'API MCU, avec repli de signature | `mcu/src/xmlrpcmcu.cpp`, `docs/MCU-API.md` §4 | OK, réutilisé tel quel |
| Paramètre `role` absent des RPC JSR-309 `Endpoint*` | `mcu/src/jsr309/xmlrpcjsr309.cpp`, `MediaSession.cpp:1089` | **à ajouter, §4.6** |
| Une `RTPSession` = une paire de sockets + ICE + DTLS + SRTP | `mcu/src/rtpsession.cpp:1869` (`Init`), `:3149` (`GetPollFds`) | **à ouvrir, §4.4** |
| Réacteur `RtpSessionSet`, deux groupes par participant | `mcu/include/rtpsessionset.h`, `docs/reference/threads-rtp.md` | OK |
| `rtcp-mux` | propriété `rtcp-mux`, `rtpsession.cpp:572`, auto-détection `:2752` | OK |
| SRTP sans SSRC fixé | `ssrc_any_outbound` `rtpsession.cpp:513`, `ssrc_any_inbound` `:835` | OK : plusieurs SSRC par contexte |
| Plusieurs SSRC reçus sur une session | `RTPStream`, `AddStream`, `GetPacket(ssrc)` `rtpsession.h:190-266` | OK, réutilisé |
| Un décodeur lit un SSRC donné d'une session tierce | `VideoStream::SetRTPSession(weak_ptr, ssrc)` `mcu/include/videostream.h:74` | OK, réutilisé |
| SSRC inconnu = **remplacement** de l'ancien | `RTPSession::Listener::onNewStream` → `ChangeStream` `rtpsession.cpp:4406` | **à encadrer, §5** |
| Extensions d'en-tête connues | `HeaderExtension::Type` `mcu/include/rtp.h` : audio level, toffset, abs-send-time, transport-cc, MID | lot 0 livré, §4.5 |
| Lecture des extensions | `ProcessExtensions` `mcu/src/rtp.cpp:9`, appelée seulement sous `useTransportCC` `rtpsession.cpp:3000` | **à rendre inconditionnelle, §4.5** |
| Propriété `ssrc` = SSRC **d'émission** ; `cname` | `rtpsession.cpp:599-602` | OK ; il manque le SSRC distant |
| Source du mixeur vidéo = un `int id > 0`, une entrée + une sortie | `VideoMixer::CreateMixer` `mcu/src/videomixer.cpp:660`, `Mosaic` `mcu/src/mosaic.cpp` | OK, réutilisé tel quel |
| Sources réservées | `AppMixerId=1`, `SharedDocMixerId=2`, `RecorderId=10` `mcu/include/multiconf.h:36-38` ; `maxId` part de 500 | OK ; la 2 disparaît, §4.3 |
| VAD d'une source sans audio | `AudioMixer::GetVAD` `audiomixer.cpp:822` rend 0 | OK, §4.2 |
| Endpoint JSR-309 : deux ports vidéo | `ports[4]`, `ports2[4]` `src/jsr309/Endpoint.h:298`, `GetPort` `:253` | **à ouvrir, §4.6** |
| Ports du mixeur JSR-309, anonymes, N par endpoint possible | `VideoMixerResource::CreatePort` `src/jsr309/VideoMixerResource.cpp:36` | OK ; rôle en dur à l'attache, §4.6 |
| Contrôleur : une section par type, pas de BUNDLE, pas de BFCP | elixip `MediaServerMendoozeSdp.ex:459-463`, `adapter/conn.ex:559-580`, `docs/kelixip/modules/mcu_module_guide.md:292` | **§4.7** |
| MOTELI : `MediaRole` énuméré, `EndpointCreate` à trois booléens | elixip `moteli_common.proto:53`, `moteli_jsr309.proto:131` | **§4.5** |
| Tests BFCP | `mcu/tests/test_bfcp_{codec,server,tcp,udp}.cpp` | garde-fous du lot 1b |

## 4. Conception

### 4.1 Le précédent : comment le partage de document BFCP est monté

Le serveur porte déjà un second flux vidéo par participant. Le lire en détail
dit tout le modèle, parce que c'en est déjà une instance.

**Deux `VideoStream` par participant, dès la construction.** `video[0]` est la
caméra (MAIN), `video[1]` est construit avec le rôle `VIDEO_SLIDES`
(`rtpparticipant.cpp:31-32`). Chacun a sa `RTPSession`, donc son port, son
décodeur, son encodeur, ses threads. Les deux sessions battent dans le même
réacteur `videoGroup` (`rtpparticipant.cpp:143`).

**Sens montant : les slides du présentateur entrent dans les mosaïques sous
un identifiant unique par conférence.**

1. Le contenu arrive comme un **SSRC inconnu**. `RTPParticipant::onNewStream`
   (`rtpparticipant.cpp:623-647`) le traite selon le mode du participant : en
   BFCP, il **ajoute** le SSRC à la session qui l'a reçu (`AddStream`), puis
   rebranche le décodeur de SLIDES dessus : `video[VIDEO_SLIDES]->
   SetRTPSession(shared_ptr(this, session), newSsrc)`. Peu importe que le
   SSRC soit arrivé sur le port de MAIN ou sur celui de SLIDES : SLIDES lit
   ce SSRC-là, sur cette session-là. Hors BFCP, le SSRC inconnu **remplace**
   l'ancien (`ChangeStream`).
2. Le thread `RecVideo` de SLIDES tire `GetPacket(recSSRC)`
   (`videostream.cpp:804`), décode, et pousse chaque image dans son
   `videoOutput`.
3. Ce `videoOutput` n'est **pas** un pipe du mixeur. Quand la parole BFCP est
   accordée, `SharedDocMixer::ShareSecondaryStream` fait
   `part->SetVideoOutput(this, VIDEO_SLIDES)` (`shareddocmixer.cpp:79`) :
   `SharedDocMixer` est lui-même un `VideoOutput`, et `NextFrame` **relaie**
   l'image vers `output` (`shareddocmixer.cpp:219`), qui est le
   `PipeVideoOutput` de la source réservée `SharedDocMixerId = 2`
   (`multiconf.cpp:109-121`).
4. Le contrôleur a placé l'identifiant 2 dans une mosaïque une fois pour
   toutes (`SetMosaicSlot(mosaic, n, 2)`). Le changement de présentateur se
   fait derrière ce relais, sans qu'il touche à la mosaïque. À la libération,
   le relais pousse le logo (`TearDownSharing`, `shareddocmixer.cpp:181`).

**Sens descendant : chaque participant BFCP reçoit la « mosaïque partagée »
sur sa jambe SLIDES.**

1. `SetAppCodec(BFCP)` (`multiconf.cpp:1880-1882`) crée une source du mixeur
   sous l'identifiant `partId+100000`, sans mosaïque, et branche l'encodeur
   de SLIDES sur son entrée : `part->SetVideoInput(GetSharedInput(id+100000),
   VIDEO_SLIDES)`. Seule l'**entrée** de cette source sert ; sa sortie n'est
   jamais câblée, et l'identifiant n'est jamais mis dans une mosaïque.
2. `SetDocSharingMosaic(mosaicId)` (`multiconf.cpp:1253`) donne cette
   mosaïque en entrée à chaque `partId+100000` (`InitMixer`) et lance
   `StartSending(Video, VIDEO_SLIDES)` : la jambe SLIDES émet le composite.
   `StartSending` sur le rôle 1 y rappelle (`multiconf.cpp:1433`).
3. `DeleteParticipant` détruit `partId+100000` (`multiconf.cpp:779,792`).

Le modèle qui se dégage : **un flux vidéo, c'est une jambe RTP, un décodeur
qui nourrit la sortie d'une source du mixeur, et un encodeur qui lit l'entrée
de cette source.** Le BFCP l'a réalisé avec trois entorses, parce qu'il n'y
avait qu'un cas : la source montante est unique par conférence (le 2) et
alimentée par un relais ; la source descendante est un identifiant calculé
(`+100000`) dont la moitié est morte ; et le tout n'est disponible que pour
le rôle 1.

### 4.2 Le modèle : un flux vidéo = une jambe RTP + une source du mixeur

Le mixeur et les mosaïques n'ont **pas** à changer. Ils ne connaissent que des
identifiants de source, entiers strictement positifs, chacun avec une entrée
et une sortie. Un participant en a aujourd'hui un seul de plein droit : son
`partId`. Il suffit d'en donner un à chaque flux.

**Le rôle devient l'index du flux vidéo dans le participant.** Le paramètre
garde son nom `role` dans les API, pour ne rien casser :

| `role` | Sens | Création | Source du mixeur | Entrée de la source | Sortie de la source |
|---|---|---|---|---|---|
| 0 | MAIN : la caméra | `CreateParticipant` | `partId` | la mosaïque du participant | la caméra |
| 1 | SLIDES : le document BFCP | `CreateParticipant` (jambe), `SetAppCodec(BFCP)` (source) | alloué | la mosaïque partagée | le document, si le participant a la parole |
| 2, 3, … | flux supplémentaire | `CreateVideoStream` | alloué | aucune par défaut | la piste |

Dans le code :

- `MediaRole` devient `enum MediaRole : int { VIDEO_MAIN=0, VIDEO_SLIDES=1,
  VIDEO_EXTRA=2 }`. Le type sous-jacent fixé rend toute valeur entière
  valide : aujourd'hui, convertir 2 dans l'énumération est un comportement
  indéfini. `VIDEO_EXTRA` signifie « 2 et au-delà ».
- `RTPParticipant::video[MAX_VIDEO_STREAM]` devient une map `role →
  unique_ptr<VideoStream>`. Les rôles 0 et 1 sont créés comme aujourd'hui. Un
  rôle ≥ 2 est créé à la demande, dans le groupe de réacteur vidéo du
  participant (ou joint au transport partagé, §4.4). Un `VideoStream` de rôle
  ≥ 2 est un flux MAIN ordinaire : il ne reçoit **pas** le réglage propre à
  SLIDES (`h264.qpel=6`, `videostream.cpp:90`).
- `MultiConf` tient `partId → {role → sourceId}`. Le `sourceId` vient de
  `maxId++`, le même compteur que les `partId` : il est unique, positif, et ne
  peut pas entrer en collision, ce que `partId+100000` ne garantit pas.
- Câblage d'un flux : `videoMixer.CreateMixer(sourceId)`,
  `InitMixer(sourceId, mosaicId ou -1)`,
  `part->SetVideoOutput(GetSharedOutput(sourceId), role)`,
  `part->SetVideoInput(GetSharedInput(sourceId), role)`. Les deux sens sont
  toujours câblés ; c'est la mosaïque d'entrée (`-1` = aucune) qui dit si la
  jambe a quelque chose à émettre.
- **`SetVideoStreamMosaic(partId, role, mosaicId)`** pose la mosaïque
  d'entrée d'un flux (`SetMixerMosaic`). `SetParticipantMosaic` en est le cas
  `role = 0`, `SetDocSharingMosaic` en est le cas `role = 1` pour tous les
  participants BFCP (§4.3). Les deux restent, comme raccourcis.
- Suppression : `EndMixer(sourceId)` retire la source de toutes les mosaïques,
  puis `DeleteMixer(sourceId)`. `DeleteParticipant` fait de même pour chaque
  flux du participant. Un rôle supprimé n'est **jamais réattribué** pendant la
  vie du participant : le contrôleur ne peut pas confondre deux flux.

Toutes les méthodes de mosaïque restent telles quelles. Là où elles prennent
un `partId`, elles prennent le `sourceId` du flux : `AddMosaicParticipant`,
`SetMosaicSlot`, `RemoveMosaicParticipant`, `SetParticipantDisplayName`,
`SetOverlayImage`. `GetMosaicPositions` rend des identifiants de source,
comme aujourd'hui ; le contrôleur sait lesquels sont des `sourceId` puisqu'il
les a reçus de `CreateVideoStream`.

**Direction.** Un flux ≥ 2 n'a pas de mosaïque d'entrée par défaut : il est
**reçu seulement**, et la mosaïque part sur MAIN. Une offre `sendrecv` sur
cette section reçoit une réponse `recvonly` ; `sendonly` reçoit `recvonly` ;
`inactive` reste `inactive`. Le modèle permet d'émettre sur un flux ≥ 2
(`SetVideoStreamMosaic` puis `StartSending`, comme SLIDES le fait) ; ce
chantier ne le recette pas (§8).

**VAD.** `GetVAD(sourceId)` rend 0 : un flux autre que MAIN n'est jamais élu
locuteur. En mode VAD, un slot non fixé est donc « kickable ». Pour qu'un
écran partagé reste visible, le contrôleur **fixe son slot**
(`SetMosaicSlot(mosaicId, num, sourceId)`). C'est le comportement voulu pour
un écran ; le faire suivre la parole du participant est hors périmètre (§8).

**L'écran partagé d'un navigateur est un flux ≥ 2**, pas le rôle 1. Le rôle 1
reste réservé au participant qui parle BFCP : c'est le contrôle de parole qui
le distingue, pas le contenu. Si l'offre porte `a=content:slides` (RFC 4796)
ou un `a=msid` parlant, le contrôleur s'en sert pour nommer la source
(`SetParticipantDisplayName`), et pour rien d'autre : le serveur n'a pas de
sémantique du contenu.

### 4.3 Le BFCP sur le modèle

Le partage de document garde son contrôle de parole (`docs/reference/bfcp.md`,
`SharedDocMixer` et ses endpoints TCP/UDP) et ses événements
(`ParticipantRequestDocSharing`). Ce qui change, c'est le chemin des pixels :
il devient celui de n'importe quel flux.

**Sens montant.** La sortie de SLIDES est câblée **en permanence** sur la
sortie de sa propre source (`GetSharedOutput(sourceId)`), comme MAIN. Le
relais `SharedDocMixer::NextFrame` disparaît ; `SharedDocMixer` n'est plus un
`VideoOutput`. La parole BFCP décide seulement **quelle source est la
présentation** :

- à `onFloorGranted` → `ShareSecondaryStream(part)` : `MultiConf` marque le
  `sourceId` de SLIDES de ce participant comme présentation courante ;
- à la libération ou à la révocation → `TearDownSharing` : plus de
  présentation courante.

**La parole place la source dans la mosaïque partagée.** `SetDocSharingMosaic`
dit déjà au serveur quelle mosaïque est partagée. À `onFloorGranted`,
`MultiConf` y ajoute le `sourceId` de SLIDES du présentateur
(`AddMosaicParticipant`) ; à la libération ou à la révocation, il l'en retire
(`RemoveMosaicParticipant`). Le chemin BFCP reste autonome : aucun appel du
contrôleur n'est nécessaire, comme aujourd'hui. La source réservée
`SharedDocMixerId = 2`, son pipe, son logo et son cas particulier dans
`RemoveMosaicParticipant` (`multiconf.cpp:109-121`, `:470`) disparaissent.
Le contrôleur qui posait l'identifiant 2 n'est plus en service (décision du
2026-09-29) : il n'y a pas de contrat à garder.

Pour montrer le document ailleurs que dans la mosaïque partagée, par exemple
dans la mosaïque des navigateurs, le contrôleur place lui-même le `sourceId`.
Il le reçoit dans l'événement `ParticipantRequestDocSharing`, qui gagne ce
champ, ou par `GetVideoStreamSource(partId, 1)`.

**Sens descendant.** `SetAppCodec(BFCP)` alloue la source de SLIDES et la
câble dans les deux sens (§4.2) ; `partId+100000` disparaît.
`SetDocSharingMosaic(mosaicId)` devient : pour chaque participant BFCP,
`SetVideoStreamMosaic(partId, 1, mosaicId)` puis `StartSending(Video, 1)` ;
avec `-1`, `StopSending`. Rien ne change pour l'endpoint en face.

**Le SSRC inconnu reste la règle d'entrée du contenu BFCP.** Un endpoint SIP
BFCP ne fait pas de BUNDLE ; son contenu arrive comme aujourd'hui, second
SSRC sur MAIN ou premier SSRC sur SLIDES, et `onNewStream` le route sur le
décodeur de SLIDES. Cette règle vaut pour un participant **non joint**
(§4.4). Sur une session jointe, un SSRC inconnu est jeté : les deux règles ne
se rencontrent jamais sur la même session.

**Ce que la conférence y gagne.** La mosaïque partagée est une mosaïque comme
une autre, et la présentation une source comme une autre :

- l'écran d'un navigateur (flux ≥ 2) placé dans la mosaïque partagée est vu
  par les endpoints SIP BFCP sur leur jambe SLIDES ;
- le document d'un endpoint BFCP (son `sourceId`) placé dans la mosaïque des
  navigateurs est vu par eux dans un slot ;
- un enregistrement qui capte une mosaïque capte l'un comme l'autre.

Aucun de ces trois cas n'existe aujourd'hui.

### 4.4 Le transport partagé : session porteuse et sessions jointes

Une `RTPSession` porte aujourd'hui deux choses fondues : le **transport**
(sockets, ICE, DTLS, SRTP, latch d'adresse, `PollHandler`) et la **session**
(rtpMap, SSRC reçus, SSRC émis, RTCP, estimateurs). Le BUNDLE demande N
sessions sur un transport.

Trois formes possibles, une retenue. L'[ADR 004](../../architecture/adr-004-bundle-session-porteuse.md) les compare.

- **Scinder la classe** en `RtpTransport` + `RTPSession`. Propre, mais
  4 700 lignes de chemin chaud, avec les chantiers ICE, DTLS client, SCTP,
  watchdog et réacteur posés dessus. Le risque de régression est le plus élevé.
- **Plusieurs SSRC sur la session vidéo MAIN**, un `VideoStream` par SSRC.
  C'est le précédent BFCP du §4.1. Mais il partage la rtpMap et le RTCP entre
  sections, ne couvre pas l'audio ni le texte sur le même transport, et casse
  le « un port = une session » de `RTPEndpoint` en JSR-309.
- **Retenu : une session porteuse, des sessions jointes.** Une `RTPSession`
  garde le transport. Les autres sessions du participant s'y joignent : elles
  n'ouvrent pas de socket, ne sont pas des `PollHandler`, et passent par la
  porteuse pour émettre et recevoir. Tout le reste d'une session jointe est
  inchangé : rtpMap, `RTPStream`, `GetPacket(ssrc)`, RTCP, événements,
  watchdog.

**La porteuse est la session audio du participant, toujours.** Elle existe
même si l'appel n'a pas d'audio : le contrôleur ne voit qu'un port, celui de
la porteuse, et l'écrit dans toutes les sections. C'est conforme à la RFC 8843
§7.3.1 : toutes les sections du groupe partagent les paramètres de transport
de la section marquée.

**Ce que fait une session jointe.**

| Chemin | Aujourd'hui | Jointe |
|---|---|---|
| `Init` | ouvre deux sockets, s'inscrit au réacteur | ne fait ni l'un ni l'autre |
| `SendPacket(RTP)` `rtpsession.cpp:2088` | chiffre SRTP, `sendto` | chiffre avec le contexte SRTP **de la porteuse**, `porteuse.SendOnWire()` |
| `SendPacket(RTCP)` `:2025` | idem | idem ; chaque session émet son propre paquet composé |
| ICE, DTLS, profil d'adressage, candidats, `StartSending(ip, port)` | configurent le transport | **acceptés et ignorés**, journalisés en debug (§4.5) |
| `StartReceiving` | rend son port | rend le port et l'adresse annoncée **de la porteuse** |
| `ReadRTP` / `ReadRTCP` | tout | rien : la porteuse lui livre les paquets déjà déchiffrés |
| Watchdog RTP, événement « connecté » | sur ses paquets | sur les paquets que la porteuse lui route |
| Extensions abs-send-time, transport-cc | compteur par session | compteur **de la porteuse** : transport-wide veut dire par transport |

**Démultiplexage en réception**, dans la porteuse, après SRTP, dans l'ordre de
la RFC 8843 §9.2 :

1. l'extension d'en-tête **MID** désigne une section → sa session ; le couple
   SSRC → session est mémorisé (et mis à jour si le MID change, avec la règle
   du numéro de séquence étendu) ;
2. sinon, le **SSRC** est dans la table (déclaré par le contrôleur depuis les
   `a=ssrc` de l'offre, ou appris au point 1) → sa session ;
3. sinon, le **payload type** n'appartient qu'à une session → celle-ci ;
4. sinon, le paquet est **jeté**, avec une trace cadencée. Jamais deviné : un
   SSRC inconnu sur une section vidéo passerait sinon par `ChangeStream` et
   remplacerait la caméra par l'écran (§5).

Le RTCP se route par SSRC : un SR par le SSRC de son émetteur, un RR ou un
message de retour (PLI, FIR, NACK, TMMBR) par le SSRC de média qu'il vise,
qui est le SSRC d'émission d'une session. REMB et transport-wide-cc portent
sur le transport : la porteuse les garde, et son estimateur pilote le seul
émetteur vidéo, MAIN. Un paquet RTCP qu'aucune session ne réclame est jeté.

**Déclaration du BUNDLE.** Le transport partagé est une propriété du
participant, posée **à la création** : `CreateParticipant(…, bundle=1)`. Les
sockets sont ouvertes à la création (`rtpparticipant.cpp:111`), donc joindre
une jambe déjà initialisée coûterait une fermeture puis une réouverture, pour
un cas qu'aucun navigateur ne produit. Avec `bundle=1`, la session audio
porte, et les sessions vidéo MAIN, SLIDES, texte et data channel sont jointes.
Un flux ≥ 2 créé ensuite se joint à la création. Les participants créés sans
le drapeau ne changent pas d'un octet.

Un groupe qui ne couvre pas toutes les sections RTP de l'offre n'est pas pris
en charge : le contrôleur rejette les sections hors groupe. Aucun navigateur
n'en produit.

**Data channel.** La SPEC [T140-DC](../T140-DC/SPEC.md) §13.1 prévoyait
exactement ce cas : le `T140DataChannel` se pose sur la DTLS de la session
audio. Avec `bundle=1`, la jambe application est jointe, et la pile SCTP lit
et écrit par la DTLS de la porteuse. Les quatre couches au-dessus ne changent
pas.

**Réacteur.** La porteuse reste dans son groupe `{audio, texte}`. Les sessions
vidéo jointes n'y sont plus : toute la réception RTP du participant passe par
**un** thread. Ce qui bloquait le seul groupe vidéo bloque désormais aussi
l'audio du participant. Voir §9.1.

### 4.5 L'extension d'en-tête MID, et l'API de contrôle MCU

**MID.**

- `RTPPacket::HeaderExtension::Type` gagne `MID = 5`
  (`urn:ietf:params:rtp-hdrext:sdes:mid`, RFC 8843 §15). La valeur est une
  chaîne courte, gardée dans `HeaderExtension`.
- `RTPSession::SetProperties` accepte cette URN (`rtpsession.cpp:644`), comme
  abs-send-time. L'id négocié est mémorisé pour l'écriture (`midExtId`, sur
  le modèle d'`absSendTimeExtId`).
- `ProcessExtensions` est appelée sur **tout** paquet reçu, plus seulement
  sous `useTransportCC` (`rtpsession.cpp:3000`). Elle ne lit que l'en-tête à
  un octet (RFC 8285 §4.2) ; le format à deux octets reste ignoré, sans
  crash : la vérification de longueur existante suffit.
- En émission, une session **porteuse ou jointe** écrit le MID sur chaque
  paquet, 3 à 4 octets, comme abs-send-time. La RFC demande « au début et à
  chaque changement de SSRC » ; l'écrire toujours est plus simple, et le
  navigateur en face démultiplexe alors sans dépendre des `a=ssrc` de la
  réponse.

Deux propriétés RTP nouvelles, par jambe : `mid` (la valeur de `a=mid` de la
section) et `remote-ssrc` (le SSRC annoncé par `a=ssrc` de l'offre, s'il y en
a un). La propriété `ssrc` garde son sens : SSRC d'émission.

**API `/mcu`, nouveautés :**

| Méthode | Signature | Rend |
|---|---|---|
| `CreateVideoStream` | `(ii)` confId, partId | `(ii)` role, sourceId |
| `DeleteVideoStream` | `(iii)` confId, partId, role (≥ 2) | `(i)` |
| `SetVideoStreamMosaic` | `(iiii)` confId, partId, role, mosaicId (`-1` = aucune) | `(i)` |
| `GetVideoStreamSource` | `(iii)` confId, partId, role | `(i)` sourceId (rôle 0 : `partId`) |
| `CreateParticipant` | `(isiiii)` … + **bundle** (0 ou 1, défaut 0), repli sur `(isiii)` | inchangé |

Inchangées, mais élargies :

- toute méthode à `(media, role)` accepte un rôle ≥ 2 pour la vidéo :
  `StartReceiving`, `StopReceiving`, `StartSending`, `StopSending`,
  `SetRTPProperties`, `SetVideoCodec`, `SetLocalCryptoSDES`,
  `SetRemoteCryptoSDES`, `SetRemoteCryptoDTLS`, `SetLocalSTUNCredentials`,
  `SetRemoteSTUNCredentials`, `StartRTPTimeout`, `SetAddressProfile`. Un rôle
  inconnu du participant est une erreur.
- avec `bundle=1`, `StartReceiving` rend le même port et la même adresse pour
  toutes les jambes. Les appels de transport sur une jambe jointe sont acceptés
  et ignorés : le contrôleur peut continuer à les faire section par section.
- `SetParticipantMosaic` et `SetDocSharingMosaic` restent, raccourcis de
  `SetVideoStreamMosaic` (§4.2, §4.3).
- les événements `ParticipantMediaTimeout` et `ParticipantMediaConnected`
  portent déjà `(media, role)` : un rôle ≥ 2 y passe tel quel.
  `ParticipantRequestDocSharing` gagne `sourceId` en dernier champ.
- propriétés : `mid`, `remote-ssrc`, extmap `urn:ietf:params:rtp-hdrext:sdes:mid`.

`docs/MCU-API.md` §4 (MediaRole), §5 (événements), §6.3, §6.5 et §6.10 sont à
réécrire en entier, règle de la documentation oblige.

**MOTELI, dans le même jeu de changements** (règle du `CLAUDE.md`) :

- les champs `role` de `moteli_mcu.proto` et `moteli_jsr309.proto` passent en
  `int32` (**décidé** 2026-09-29) : un index n'est plus une énumération.
  `MediaRole` de `moteli_common.proto` reste pour nommer 0 et 1 ;
- `CreateParticipantReq` et `EndpointCreateReq` gagnent `bool bundle` ;
- `CreateVideoStreamReq/Resp`, `DeleteVideoStreamReq`,
  `SetVideoStreamMosaicReq`, `GetVideoStreamSourceReq/Resp`, et leurs
  pendants JSR-309 ; `source_id` dans l'événement de partage de document.

### 4.6 API JSR-309 (`/jsr309`)

Le transport (§4.4) vaut tel quel : `RTPEndpoint` **est** une `RTPSession`.

- `EndpointCreate(…, bundle)` : la session audio porte, les autres ports sont
  joints.
- `EndpointCreateVideoStream(sessId, epId)` rend un rôle ≥ 2 ;
  `EndpointDeleteVideoStream(sessId, epId, role)`.
- `Endpoint::ports[4]` et `ports2[4]` deviennent une map `(media, role) →
  Port`. `GetPort(media, role)` la lit.
- **Tous** les RPC `Endpoint*` qui visent une jambe gagnent le paramètre
  `role`, facultatif, 0 par défaut, avec le repli de signature déjà en usage
  sur `/mcu` (**décidé** 2026-09-29 : une seule règle, aucune exception) : `EndpointStartReceiving`, `EndpointStartSending`,
  `EndpointStopSending`, `EndpointStopReceiving`, `EndpointSetRTPProperties`,
  les quatre `Set*Crypto*`, les deux `Set*STUNCredentials`,
  `EndpointAddICECandidate`, `EndpointStartRTPTimeout`, `GetMediaCandidates`,
  `EndpointRequestUpdate`. Aujourd'hui le rôle existe dans `Endpoint.h:159-186`
  mais aucun RPC RTP ne l'atteint (`MediaSession.cpp:1089` force MAIN).
- Attaches avec rôle : `EndpointAttachToEndpoint(sessId, epId, srcEpId, media,
  role)` pour relayer N flux entre deux endpoints (B2BUA) ;
  `EndpointAttachToVideoMixerPort(…, role)` et
  `VideoMixerPortAttachToEndpoint(…, role)`. Un port du mixeur par flux, ce que
  `VideoMixerResource::CreatePort` permet déjà.
- À corriger au passage : `Endpoint.cpp:848` recrée un port sans son rôle. Un
  port SLIDES recréé se déclare MAIN dans ses événements.

`docs/JSR-309-API.md` est à réécrire aux sections concernées.

### 4.7 Ce que le contrôleur doit faire

Rien de ceci n'est codé dans ce dépôt. C'est la liste que le lot 4 doit
couvrir dans elixip, adaptateurs MCU et JSR-309.

1. **Lire `a=group:BUNDLE`** de l'offre. S'il est présent, créer le
   participant ou l'endpoint avec `bundle=1`. Répondre `a=group:BUNDLE` avec
   les `mid` de toutes les sections acceptées, dans l'ordre de l'offre ; la
   section marquée est la première acceptée. Mettre le **même port**, les
   mêmes `a=ice-ufrag/pwd`, la même `a=fingerprint`, les mêmes candidats, et
   `a=rtcp-mux` sur chaque section du groupe. Les sections `bundle-only`
   (port 0 + `a=bundle-only`) de l'offre sont des sections normales du groupe.
2. **Indexer par `(type, role)`** et non plus par type : `local_ports`,
   `negs`, `proposed_recv`, `accepted`. Une section `m=video` de rang k ≥ 2
   dans l'offre appelle d'abord `CreateVideoStream`, puis `StartReceiving(…,
   role)`. Aujourd'hui, une offre à deux `m=video` reçoit deux réponses au même
   port, par écrasement (`MediaServerMendoozeConn.ex:2122`, `adapter/conn.ex:559`)
   — constaté par lecture, non exécuté.
3. **Pousser `mid` et `remote-ssrc`** par `SetRTPProperties` avant
   `StartReceiving`, comme le fmtp distant. Renvoyer `a=extmap` pour
   `sdes:mid` si l'offre le porte, et poser la propriété.
4. **Placer la source** : `AddMosaicParticipant(mosaic, sourceId)`, et fixer le
   slot si la mosaïque est en VAD. Nommer la source d'après `a=content` ou
   `a=msid` si l'offre en donne. Pour que les endpoints BFCP voient cet
   écran : le placer aussi dans la mosaïque partagée.
5. **Répondre `recvonly`** sur un flux ≥ 2, ne pas appeler `StartSending`.
6. **Re-négociation.** Une nouvelle offre avec une section vidéo de plus
   (`addTrack`) → `CreateVideoStream` + `StartReceiving`. Une section passée
   à `port 0` ou `inactive` (`removeTrack`) → `DeleteVideoStream`. Avec
   BUNDLE, ni ICE ni DTLS ne redémarrent.
7. **Écrire `a=ssrc` et `a=msid`** sur nos sections émises, avec la propriété
   `ssrc` que le contrôleur pose déjà. Le MID sur le fil suffit au navigateur ;
   l'`a=ssrc` reste utile aux clients qui démultiplexent par le SDP.
8. Rejeter en port 0 toute section RTP hors du groupe BUNDLE.
9. **SIP avec `a=content:slides`** (RFC 4796) : la section est la jambe de
   rôle 1 (`StartReceiving(…, role=1)`), et le participant est en mode BFCP
   (`SetAppCodec(BFCP)`). Le contrôleur ne le fait pas aujourd'hui
   (`mcu_module_guide.md:292`) ; c'est ce qui rend le lot 1b recettable
   seulement par script, ou par un contrôleur patché.

Le B2BUA JSR-309 relaie chaque flux par `EndpointAttachToEndpoint(…, role)`,
section par section, dans les deux sens.

## 5. Pièges relevés dans le code actuel

- **Un SSRC inconnu remplace le flux courant.** `Listener::onNewStream` par
  défaut fait `ChangeStream(old, new)`, et la branche BFCP de
  `RTPParticipant::onNewStream` route au contraire vers SLIDES. Sur une session
  jointe, ni l'un ni l'autre ne doit être atteint **avant** le routage par
  MID, SSRC ou PT : sinon l'écran remplace la caméra. Le point 4 du
  démultiplexage (jeter) est là pour ça.
- **`ChangeStream` s'exécute sur le thread du réacteur, sous le verrou
  écrivain de la map des flux.** Ce verrou est borné à 500 ms, et aucun
  consommateur ne le tient plus qu'une recherche : `GetPacket` attend son
  paquet hors du verrou, sur une référence partagée du flux (chantier
  préalable, **fait** ; garde-fou `mcu/tests/test_rtp_stream_lock.cpp`).
  Tenir le verrou pendant l'attente faisait attendre le réacteur jusqu'à
  200 ms à chaque changement de SSRC, et deux consommateurs sur une même
  session suffisaient à faire échouer le renommage à chaque paquet. Avec le
  BUNDLE, l'audio du participant l'aurait subi. Règle pour le lot 2 : le
  démultiplexage de la porteuse ne prend jamais un verrou qu'un consommateur
  peut tenir pendant une attente (`docs/reference/threads-rtp.md` §7).
- **`ProcessExtensions` n'était appelée que sous transport-cc.** Sans
  transport-cc négocié, un MID reçu était invisible. Réglé au lot 0 : elle
  tourne sur tout paquet reçu (garde-fou
  `RtpMid.LaSessionLitLeMidSansTransportCC`).
- **Le parseur d'extensions ne vérifiait pas le profil** (`0xBEDE` contre
  `0x100x`) : un en-tête à deux octets était lu comme un en-tête à un octet.
  Réglé au lot 0 : seul le profil one-byte est lu (garde-fou
  `RtpMid.UnEnTeteTwoByteNEstPasPrisPourDuOneByte`).
- **`partId+100000`** n'est protégé par rien, et seul son sens descendant est
  câblé. Le lot 1b le retire.
- **`SharedDocMixer` est à la fois serveur BFCP et `VideoOutput`.** Le lot 1b
  lui retire le second rôle ; le premier ne bouge pas, et ses tests
  (`test_bfcp_server.cpp`) doivent rester verts sans modification.
- **`Endpoint.cpp:848`** perd le rôle à la recréation d'un port.
- **La socket RTCP reste ouverte en rtcp-mux** (`GetPollFds` la déclare
  toujours). Sans conséquence ; une session jointe n'ouvre rien.
- **Un participant ouvre 8 ports à la création.** Avec `bundle=1` il en ouvre
  2, effet secondaire bienvenu sur la plage de ports.

## 6. Lots

Chaque lot compile, passe `cd mcu && make check` et `make check-ipv6`, et
porte sa preuve. Les lots 1 et 2 sont indépendants et peuvent aller de front.
Le lot 1b suit le lot 1. Le correctif de la famine de `ChangeStream` (§5),
préalable au lot 2, est livré ; sa recette en appel réel reste à faire. Le lot
4 attend que l'API des lots 1 à 3 soit figée.

### Lot 0 — L'extension MID (livré)

- `rtp.h` / `rtp.cpp` : type `MID`, lecture, écriture ; vérification du profil
  de l'en-tête d'extension.
- `rtpsession.cpp` : propriété `sdes:mid`, `midExtId`, `ProcessExtensions`
  inconditionnelle ; propriétés `mid` et `remote-ssrc` mémorisées.
- Tests gtest (`mcu/tests/test_rtp_mid.cpp`) : un paquet forgé avec MID à un
  octet est lu ; un en-tête à deux octets n'est pas pris pour un en-tête à un
  octet ; une session lit le MID reçu sans transport-cc ; un paquet émis porte
  le MID annoncé.

### Lot 1 — N sources du mixeur, sans BUNDLE

- `media.h` : `MediaRole : int`, `VIDEO_EXTRA`.
- `rtpparticipant.*` : map de `VideoStream`, création et destruction d'un
  rôle ≥ 2, callbacks par rôle.
- `multiconf.*` : `CreateVideoStream`, `DeleteVideoStream`,
  `SetVideoStreamMosaic`, `GetVideoStreamSource`, table des `sourceId`,
  nettoyage dans `DeleteParticipant`.
- `xmlrpcmcu.cpp` : les RPC, `bundle` accepté et **refusé** tant que le
  lot 2 n'est pas là, rôles ≥ 2 partout.
- `docs/MCU-API.md`, protos MOTELI.
- Tests gtest : créer un participant, deux flux, vérifier deux sources dans
  le mixeur et deux slots occupés ; supprimer un flux, le slot se libère ;
  supprimer le participant, plus aucune source. Les deux garde-fous IPv4
  restent verts.
- Recette : page web `bundlePolicy: 'max-compat'`, caméra + écran, contrôleur
  patché à la main ou script XML-RPC : deux slots dans la mosaïque.

### Lot 1b — Le BFCP sur le modèle

- `multiconf.*` : source allouée pour le rôle 1 dans `SetAppCodec`, câblée
  dans les deux sens ; `SetDocSharingMosaic` par `SetVideoStreamMosaic` ;
  pose et retrait de la source du présentateur dans la mosaïque partagée ;
  retrait de `partId+100000` et de la source réservée 2.
- `shareddocmixer.*` : plus de `VideoOutput`, plus de relais ni de logo ;
  `ShareSecondaryStream` et `TearDownSharing` posent et retirent la
  source du présentateur. Le serveur BFCP ne change pas.
- `mcu.cpp` : `sourceId` dans `ParticipantRequestDocSharing`.
- `rtpparticipant.cpp` : la branche BFCP d'`onNewStream` est gardée, et
  réservée aux sessions non jointes.
- Tests gtest : parole accordée à A → la source de SLIDES de A est dans la
  mosaïque partagée ; parole passée à B → A en sort, B y entre ; libération
  → plus aucune source de présentation ; `SetDocSharingMosaic` → l'entrée de SLIDES de chaque
  participant BFCP est la mosaïque partagée. Les tests BFCP existants restent
  verts sans modification.
- Recette : un endpoint SIP BFCP qui partage, un navigateur qui voit le
  document dans sa mosaïque ; un navigateur qui partage son écran (lot 1),
  l'endpoint SIP le voit sur sa jambe SLIDES.

### Lot 2 — Le transport partagé

- `rtpsession.*` : porteuse et jointes, `SendOnWire`, démultiplexage RTP et
  RTCP, transport-wide et REMB à la porteuse, ignorance journalisée des
  réglages de transport sur une jointe.
- `rtpparticipant.*` et `Endpoint.*` : `bundle` à la création, jonction des
  jambes, data channel sur la porteuse.
- Tests gtest en boucle locale : un participant `bundle=1` en face d'un autre,
  audio + deux vidéos + texte sur un port de chaque côté ; routage par MID,
  puis par SSRC déclaré, puis par PT ; SSRC inconnu jeté ; RR et PLI reçus par
  la bonne session ; DTLS-SRTP par la porteuse ; watchdog d'une jointe qui se
  tait alors que l'audio passe. La suite IPv6 reste verte.

### Lot 3 — JSR-309

- `Endpoint.*`, `MediaSession.*`, `xmlrpcjsr309.cpp` : §4.6 en entier, dont
  la correction de `Endpoint.cpp:848`.
- `XmlRpcMcuClient` et `jsr309impl` : signatures avec rôle.
- `docs/JSR-309-API.md`, protos MOTELI.
- Tests gtest : deux endpoints `bundle=1` attachés flux à flux, la vidéo de
  rang 2 arrive sur le rang 2 de l'autre.

### Lot 4 — Contrôleur (elixip)

§4.7, avec ses tests. Hors de ce dépôt.

### Lot 5 — Recette réelle

§7.

## 7. Recette de bout en bout

| # | Scénario | Attendu |
|---|---|---|
| 1 | Chrome, politique par défaut, caméra + `getDisplayMedia`, rejoint une conférence MCU | l'offre porte deux `m=video` dont une `bundle-only` ; la réponse porte `a=group:BUNDLE` ; la mosaïque montre la caméra et l'écran dans deux slots |
| 2 | Même appel, arrêt du partage puis reprise | le slot se libère, puis se remplit ; aucun redémarrage ICE ni DTLS ; l'audio ne coupe pas |
| 3 | Firefox, même scénario | idem 1 |
| 4 | Linphone (SIP, sans BUNDLE), audio + vidéo | inchangé : garde-fou anti-régression |
| 5 | Chrome en `max-compat` | idem 1, sans BUNDLE : prouve que les deux moitiés tiennent séparément |
| 6 | B2BUA JSR-309 Chrome ↔ Chrome, caméra + écran de chaque côté | chaque côté voit les deux flux de l'autre |
| 7 | Texte T.140 sur data channel dans un appel `bundle=1` | le texte passe par la DTLS de la porteuse |
| 8 | Conférence à 4 participants dont 2 partagent leur écran, mosaïque VAD, slots des écrans fixés | les écrans restent, les caméras suivent la parole |
| 9 | Endpoint SIP BFCP partage un document ; un navigateur est dans la conférence | l'endpoint reçoit la mosaïque partagée sur SLIDES comme avant ; le navigateur voit le document dans un slot |
| 10 | Navigateur partage son écran ; un endpoint SIP BFCP est dans la conférence, écran placé dans la mosaïque partagée | l'endpoint voit l'écran sur sa jambe SLIDES |

Pour chacun : `make check`, `make check-ipv6`, et `/status/general` avant et
après.

## 8. Ce que le chantier ne fait pas

- **Recetter l'émission sur un flux ≥ 2** (une seconde mosaïque vers un
  navigateur). Le modèle le permet, l'API ne l'interdit pas, aucun scénario
  du §7 ne le joue.
- **Simulcast** (`a=rid`, `a=simulcast`, extensions `rtp-stream-id`).
- **Groupe BUNDLE partiel**, et jonction d'une jambe après sa création.
- **VAD** d'un flux autre que MAIN qui suivrait la parole du participant.
- **Plusieurs présentations BFCP à la fois** : le contrôle de parole n'a qu'un
  floor (`FloorId = 1`).
- **Offre à N vidéos depuis notre côté** (UAC). Le serveur ne produit pas de
  SDP ; le jour venu, c'est un `CreateVideoStream` avant l'offre.
- **`a=content` et `a=msid`** n'ont aucune sémantique côté serveur.
- **Redémarrage ICE** sur un groupe BUNDLE : pas plus qu'aujourd'hui sur une
  jambe.
- **B2BUA SIP ↔ SIP avec contenu BFCP chiffré en SDES** : abandonné avec la
  passerelle Java `mediagw-b2bua` (décision du 2026-09-29). L'API l'accepte,
  parce que le rôle est partout ; aucun scénario du §7 ne le joue.

## 9. Risques

### 9.1 Un thread pour toute la réception d'un participant

Avec `bundle=1`, la porteuse bat seule l'audio, le texte et N vidéos du
participant. Un callback qui bloque bloque tout. Les règles de
`docs/reference/threads-rtp.md` deviennent vitales, et la famine de
`ChangeStream` (§5) est réglée. La livraison reste tirée (`GetPacket`),
donc le décodage vidéo reste hors du thread de la porteuse : le coût ajouté au
`poll` est celui du démultiplexage, une recherche de table par paquet.

### 9.2 Le démultiplexage rate

Un client qui n'envoie ni MID, ni `a=ssrc`, et réutilise le même PT sur deux
sections vidéo, est indémultiplexable. Le paquet est jeté et tracé. Aucun
navigateur n'est dans ce cas : tous envoient le MID ; Chrome et Firefox
mettent aussi `a=ssrc`. Un test adverse joue ce cas pour vérifier qu'il ne
casse rien d'autre.

### 9.3 Le contrôleur écrit un port par type

Tant que le lot 4 n'est pas là, elixip ne sait ni répondre `a=group:BUNDLE`,
ni traiter un second `m=video`, ni piloter le BFCP. Le serveur seul ne change
donc rien pour les appels réels. C'est assumé : le serveur avance en premier,
avec ses tests.

### 9.4 Le slot du document est vide quand personne ne partage

Aujourd'hui, quand le présentateur rend la parole, `TearDownSharing`
(`shareddocmixer.cpp:181`) envoie le logo de la conférence dans la source 2.
Le slot du document reste donc occupé par une image fixe tant que personne
ne partage.

Avec le nouveau chemin, la source du présentateur est simplement retirée de
la mosaïque partagée, et son slot se vide. C'est le comportement retenu
(décision du 2026-09-29). Si un exploitant veut une image à cet endroit, le
contrôleur pose une image de fond sur la mosaïque avec
`SetMosaicOverlayImage`, qui existe déjà.

## 10. Arbitrages

### Décidés et proposés

| Sujet | Proposé | Alternatives écartées |
|---|---|---|
| Transport | BUNDLE par session porteuse (ADR 004) | refuser BUNDLE et imposer `max-compat` aux clients IVèS : ne sert pas les clients tiers ; scinder `RTPSession` : trop de chemin chaud |
| Identifiant d'un flux | `role` = index, 0 et 1 réservés, ≥ 2 alloués ; `sourceId` distinct pour le mixeur | un `mid` chaîne dans l'API : rien dans le serveur n'en a besoin, et toute l'API est en entiers ; `role` = `sourceId` : un seul entier, mais deux sens dans un nom |
| Présentation BFCP | la parole pose la source du présentateur dans la mosaïque partagée, et l'en retire (**décidé** 2026-09-29) | garder le relais de pixels vers la source 2 : deux chemins pour une même chose, et le document BFCP invisible aux navigateurs ; un alias 2 résolu par `MultiConf` : préservait un contrat dont plus aucun contrôleur ne dépend |
| Porteuse | la session audio, toujours | la première section acceptée de l'offre : le serveur ne voit pas l'offre |
| Déclaration | drapeau à la création | jonction jambe par jambe : coûte une fermeture de sockets pour un cas que rien ne produit |
| Réglages de transport sur une jointe | acceptés et ignorés | refusés : forcerait le contrôleur à distinguer la section marquée, ce que RFC 8843 rend inutile |
| MID en émission | sur chaque paquet | au début seulement : plus de code, gain de 4 octets par paquet |
| Direction d'un flux ≥ 2 | reçu seulement par défaut ; l'émission est permise, non recettée | l'interdire : c'est le chemin même de SLIDES |
| Famine de `ChangeStream` | chantier préalable au lot 2, **fait** : `GetPacket` attend hors verrou (**décidé** 2026-09-29) | la régler dans le lot 2 : mélangerait un correctif de verrou et une refonte de transport |
| `role` dans MOTELI | `int32` (**décidé** 2026-09-29) | énumération ouverte : dépend du comportement de chaque bibliothèque protobuf sur une valeur inconnue |
| Rôle sur les RPC JSR-309 | sur tous les RPC qui visent une jambe (**décidé** 2026-09-29) | seulement média et attaches : ferme le SIP SRTP hors BUNDLE ; média, attaches et chiffrement : cinq handlers de moins, une table d'exceptions à retenir |
| Slot du document sans présentation | vide (**décidé** 2026-09-29) | logo poussé par le code BFCP : plus de pipe où le pousser |

### À trancher

Rien à ce jour. Les décisions ci-dessus sont celles du mainteneur, prises
le 2026-09-29.
