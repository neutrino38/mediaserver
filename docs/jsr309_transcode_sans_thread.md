# Transcodeurs JSR-309 sans thread

> Statut : **lots 0 à 4 bis faits** — plus aucun thread de codec dans un
> transcodeur. Reste le lot 5 (recette en appel réel).
> Premier lot de la « chasse aux threads inutiles ». Branche de travail :
> `fix/transcoder-threading`.
>
> Périmètre : `AudioTranscoder`, `VideoTranscoder` et les quatre workers qu'ils
> composent (`AudioDecoderJoinableWorker`, `AudioEncoderMultiplexerWorker`,
> `VideoDecoderJoinableWorker`, `VideoEncoderMultiplexerWorker`), dans
> `mcu/src/jsr309/`. Les ports de mixeur (`AudioMixerResource`,
> `VideoMixerResource`) partagent ces workers : le plan dit ce qui change pour
> eux et ce qui ne change pas.

## 1. Ce que l'on constate

### 1.1 Qui appelle `onRTPPacket`, et sur quel thread

Un `RTPEndpoint` (une jambe RTP, un média) porte **deux threads à lui** :

| Thread | Rôle | Où |
|---|---|---|
| `RTPSession::Run` (Worker) | `poll()` des sockets, déchiffrement SRTP, dépôt dans le jitter buffer `RTPBuffer` | `rtpsession.cpp` |
| `RTPEndpoint::MultiplexLoop` (pthread brut) | tire les paquets du jitter buffer (`GetPacket`) et les **pousse** aux listeners : `Multiplex(*packet)` | `RTPEndpoint.cpp:382` |

`RTPMultiplexer::Multiplex` tient `mutex` pendant TOUT l'appel aux listeners.
`onRTPPacket` d'un transcodeur s'exécute donc **sur le thread de
démultiplexage de la jambe source, sous le verrou du port source**.

C'est déjà un chemin « poussé ». Les workers le transforment en chemin
« tiré » : ils copient le paquet (`packet.Clone()`) dans une `WaitQueue`, et
un thread à eux le reprend.

### 1.2 Threads d'un transcodeur aujourd'hui

| Composant | Thread | Ce qui le cadence | Réactif ou cadencé ? |
|---|---|---|---|
| `AudioDecoderJoinableWorker` | `Decode()` | `packets.Pop(5000)` : un paquet | **réactif** |
| `VideoDecoderJoinableWorker` | `Decode()` | `packets.Wait(0)` : un paquet | **réactif** |
| `AudioEncoderMultiplexerWorker` | `Encode()` | `input->RecFrame(1000)` : une trame décodée dans `PipeAudioInput` | **réactif** dans le transcodeur (voir 1.4 pour le mixeur) |
| `VideoEncoderMultiplexerWorker` | `Encode()` (pthread brut) | `GrabFrame(frameTime)` + `pacer.WaitSignal` : **le temps** (1/fps) | **cadencé** |
| `RTPMultiplexerSmoother` (base de l'encodeur vidéo) | `Run()` | `nextSendUs` : **le temps** (étalement des paquets d'une image) | **cadencé** |

Un appel audio + vidéo transcodé dans les deux sens compte donc :
2 jambes × 2 médias × 2 threads (session + démux) = 8, plus 2 transcodeurs
audio × 2 threads = 4, plus 2 transcodeurs vidéo × 3 threads = 6.
**18 threads**, dont **8 ne font que déplacer une donnée d'une file à une
autre** : les quatre décodeurs et les deux encodeurs audio réagissent à
l'arrivée d'un paquet, rien d'autre ; les deux threads de démux pourraient
faire ce travail eux-mêmes. Ce chiffre ne compte pas les threads internes de
ffmpeg, x264 et SVT-AV1.

### 1.3 Une preuve déjà dans le code — SUPPRIMÉE (lot 2)

`VideoDecoderJoinableWorker` avait un constructeur `useThread = true`. À `false`,
`onRTPPacket` appelle `DecodePacket(&packet)` directement, sans copie ni file.
**Aucun appelant ne passe `false`** (vérifié par grep : le paramètre n'apparaît
que dans la classe elle-même). C'est un prototype mort, et il a deux défauts que
le plan corrige (voir 4.3) :

- `Stop()` en mode sans thread détruit `videoDecoder` **sans barrière** : le
  thread de démux peut être dans `DecodePacket` à cet instant ;
- `Decode()` (le corps du thread) déclare un `videoDecoder` **local** qui masque
  le membre et reste toujours nul — le `delete` de sortie ne libère rien, le
  membre est libéré par `Stop()`. Sans conséquence aujourd'hui, mais c'est le
  signe que les deux modes n'ont jamais tourné ensemble.

### 1.4 Les workers servent aussi les ports de mixeur

`AudioMixerResource::Port` et `VideoMixerResource::Port` composent les mêmes
`encoder` + `decoder`. Là, l'encodeur **n'est pas réactif** : `RecFrame` /
`GrabFrame` lisent la sortie du mixeur, qui produit à sa propre cadence, sur son
propre thread. Le thread d'encodeur y est la pompe de sortie du port. Le
décodeur, lui, y est aussi réactif que dans le transcodeur : il dépose des
trames dans un pipe (`PipeAudioOutput` / sortie vidéo du mixeur) que le mixeur
consomme à son rythme.

### 1.5 Ce qui rend l'inline possible sans réécrire les codecs

- Audio : `FfAudioEncoder` porte une **fifo d'accumulation** (`av_audio_fifo`,
  `ffaudiocodec.cpp:233`). On peut lui donner une trame de n'importe quelle
  taille ; il rend zéro, une ou plusieurs trames codées. La boucle
  `for (frame = EncodeFrame(samples); frame; frame = EncodeFrame(NULL))` existe
  déjà dans `Encode()`. Le rééchantillonnage vit dans `PipeAudioInput::Resample`
  (libswresample) — c'est ce qu'il faut garder, pas la file qui l'entoure.
- Vidéo : `VideoDecoderJoinableWorker::DecodePacket(RTPPacket*)` est déjà
  extrait de la boucle. `VideoPipe::NextFrame` fait déjà le redimensionnement
  (`resizer.Rescale`) au moment du dépôt ; `GrabFrame` ne fait que rendre
  `last`.
- `Recorder` et `MP4Player` (`Player::onRTPPacket` → `Multiplex` direct)
  consomment déjà les paquets **inline** sur le thread de la source. Le motif
  n'est pas nouveau dans ce dépôt.

## 2. Objectif

Dans un transcodeur, un paquet RTP entrant doit traverser
décodage → rééchantillonnage/redimensionnement → encodage → `Multiplex` vers le
puits **sur le thread qui l'a livré**, sans copie du paquet ni file
intermédiaire. Les threads supprimés : les deux décodeurs, l'encodeur audio
(dans le transcodeur seulement). Le thread de l'encodeur vidéo est traité à
part (voir 3.3). Le `RTPMultiplexerSmoother` reste : c'est un pacer, il mesure
du temps.

Ce que l'on gagne, et pourquoi c'est aussi une question de stabilité :

- moins de threads et de changements de contexte par paquet (3 réveils au lieu
  de 1 pour un paquet audio aujourd'hui) ;
- plus **aucun `join`** dans les transcodeurs : `Stop()` ne joint rien. Le gel
  du 2026-08-13 (join du thread d'encodage sous le verrou de `MediaSession`,
  deinit SVT-AV1 qui ne revient pas) appartient à une classe de défauts qui
  disparaît de ce chemin, au lieu d'être contournée par `negotiatedDirty` ;
- plus d'auto-join possible : `Worker::StopThread()` appelé depuis
  `onEndStream` sur le thread de la source tombait dans le garde-fou « join()
  ignoré ». Sans thread, le cas n'existe plus ;
- plus de `packet.Clone()` par paquet et par listener sur le chemin chaud.

## 3. Analyse composant par composant

### 3.1 Décodeur audio — thread supprimé — FAIT (lot 1)

Le corps de `Decode()` devient `DecodePacket(RTPPacket&)` : création paresseuse
du décodeur au changement de codec, `Decode` + boucle `GetFrame`, dépôt dans
`output->PlayFrame` **ou** `input->PutFrame`. `onRTPPacket` l'appelle
directement. La `WaitQueue` disparaît. Vaut pour le transcodeur **et** pour le
port de mixeur : `PlayFrame` d'un `PipeAudioOutput` est une écriture sous verrou
dans un tampon, exactement ce que faisait le thread décodeur.

### 3.2 Décodeur vidéo — thread supprimé — FAIT (lot 2)

Le chemin `useThread == false` est devenu l'unique chemin ; le paramètre, la
`WaitQueue` et la boucle `Decode()` ont disparu, avec le membre `videoDecoder`
qu'elle masquait. `DecodePacket` prend une `RTPPacket&` : plus rien à
supprimer, donc plus de `delete(packet)` conditionnel. La destruction du codec
est couverte par la barrière de retrait (4.3). Vaut pour le mixeur aussi.

### 3.3 Encodeur vidéo — deux modes assumés, pas un thread de moins partout — FAIT (lot 4)

C'est le seul composant réellement **cadencé**. Sa boucle fait trois choses :

1. `GrabFrame(frameTime)` rend la **dernière** image quand rien de neuf
   n'arrive avant l'échéance : l'encodeur ré-émet une image gelée à `fps`
   constant, même si la source s'est tue (sémantique figée par
   `test_wait_sites.cpp` pour `VideoPipe`) ;
2. il régule le débit par seconde glissante et applique la consigne
   (`bitrate`, TMMBR/REMB, BWE émetteur) ;
3. il étale l'image dans le `RTPMultiplexerSmoother`.

Dans le **transcodeur**, la cadence utile est celle de la source : une image
décodée = une image à encoder. Le thread ne sert qu'à imposer un `fps` de
sortie et à dupliquer les images quand la source ralentit ou s'arrête. En
**pont** (state 2) cette duplication n'existe pas : le puits reçoit ce que la
source envoie, rien de plus. Supprimer le thread ramène le mode transcodage à
la même règle que le pont.

Décision proposée : extraire le corps de la boucle en `EncodePicture(PictPtr)`
(ouverture paresseuse de l'encodeur, `negotiatedDirty`, `useInputSize`,
`sendFPU`, calcul de `target`, `EncodeFrame`, `SmoothFrame`), garder la boucle
cadencée **pour le mixeur** (`Run()` = `GrabFrame` + `EncodePicture`), et
offrir au transcodeur un chemin poussé : `VideoTranscoder` reçoit l'image du
décodeur (il devient le `VideoOutput` du décodeur à la place de `VideoPipe`),
la redimensionne comme `VideoPipe::NextFrame` le fait aujourd'hui, **écarte**
celles qui arrivent plus vite que `1/fps` (le thread faisait la même chose : une
image qui écrase `last` avant le `GrabFrame` est perdue), et appelle
`EncodePicture`.

**Changement de comportement, arbitré le 2026-08-28 : accepté.** Plus
d'images dupliquées quand la source se tait ou ralentit. Le puits ne reçoit
plus rien tant que la source ne produit rien, comme en mode pont et comme un
relais. Conséquence à traiter : l'encodeur perd la cadence constante sur
laquelle son contrôle de débit s'appuie — voir 3.6.

### 3.4 Encodeur audio — thread supprimé dans le transcodeur, gardé pour le mixeur — FAIT (lot 3)

Même découpage : `EncodeSamples(SamplesPtr)` (ouverture paresseuse, SSRC neuf
par run, `frameTime`, boucle `EncodeFrame`, `Multiplex`). Le thread reste pour
le port de mixeur (`Run()` = `RecFrame` + `EncodeSamples`), parce que là c'est
le mixeur qui cadence.

Dans le transcodeur, `PipeAudioInput` disparaît **comme file** mais son
`Resample` reste nécessaire : l'encodeur travaille à `encoder->GetRate()`, le
décodeur rend à la fréquence native du flux. Deux façons de le garder :

- (a) garder `PipeAudioInput` et remplacer `RecFrame` par un appel synchrone :
  `PutFrame` rééchantillonne puis appelle un rappel au lieu d'empiler. La classe
  a alors deux modes ;
- (b) sortir `Resample` dans un petit objet `AudioResampler` (ouverture
  paresseuse sur la fréquence d'entrée observée, comme aujourd'hui) utilisé par
  `PipeAudioInput` **et** par le transcodeur.

Recommandation : **(b)**. Un objet à responsabilité unique, testable seul, et
`PipeAudioInput` garde son rôle de file pour le mixeur et le pont RTMP sans
gagner un mode.


### 3.6 Cadence réelle reçue → reconfiguration de l'encodeur — FAIT (lot 4 bis)

#### Le problème, vérifié dans libmedikit

`FfVideoEncoder::SetFrameRate` **mémorise** `fps` mais aucun encodeur ne
l'applique à chaud : `fps` n'est lu qu'à `OpenCodec` (`ctx->time_base =
{1,fps}`, `rc_buffer_size = bitrate/fps`, `bit_rate_tolerance`), et la
politique de réouverture `ShouldReopenForBitrate` ne regarde **que le débit**.
`x264_encoder_reconfig` (chemin libx264) ne couvre pas la cadence non plus. Et
`pts` est un compteur (`frameToSend->pts = pts++`, `ffvideocodec.cpp:625`) :
l'encodeur vit dans un temps virtuel où **une image vaut 1/fps configuré**.

Aujourd'hui ce temps virtuel est vrai par construction : `GrabFrame` duplique
la dernière image jusqu'à atteindre `fps`. Après 3.3 il devient faux dès que la
source est plus lente que la consigne : une source à 15 im/s dans un encodeur
ouvert à 30 im/s reçoit un budget de `bitrate/30` par image et sort à **la
moitié** du débit négocié, avec une qualité moitié moindre, et une période
intra (`gop_size`, en images) deux fois plus longue en secondes. Le débit RTP,
lui, reste juste : les timestamps RTP viennent du temps mur
(`getDifTime(&first)`, ×90 dans le smoother), pas de `pts`.

#### Mesurer

Mesurer la cadence **des images décodées offertes à l'encodeur**, pas des
paquets : une image perdue au décodage ne sera pas encodée. Base de temps :
les **timestamps RTP** de la source (90 kHz), pas l'heure d'arrivée — la
gigue réseau et la latence du thread de démux n'y entrent pas.

Plomberie : `VideoDecoderJoinableWorker::DecodePacket` pose
`frame->GetAVFrame()->pts = packet->GetTimestamp()` sur l'image qu'il livre
(`FfVideoDecoder::GetFrame` ne le renseigne pas aujourd'hui : vérifier qu'il
survit au partage zéro-copie de la surface, VAAPI comprise). Le transcodeur,
qui reçoit l'image dans `NextFrame`, garde les **écarts** entre images
consécutives (`pts[n] − pts[n−1]`, en unités 90 kHz) sur les **30 dernières
images**, et estime `fpsMesure = 90000 × (nombre d'écarts) / (somme des
écarts)`. Trente images valent 1 s à 30 im/s et 2 s à 15 im/s : assez pour
lisser le rendu irrégulier d'un navigateur (Chrome oscille entre 24 et 30),
assez court pour suivre une vraie bascule (30 → 15 d'un encodeur pair qui
s'adapte). Un saut de `pts` non monotone (changement de SSRC,
`onResetStream`) vide la fenêtre.

**Une pause n'est pas une cadence.** Un mute vidéo (Linphone en pause n'émet
plus rien ; un navigateur dont la piste est désactivée peut émettre du noir à
cadence réduite) fait passer plusieurs secondes sans image. Compter ce temps
ferait tomber la mesure vers 0, rouvrir l'encodeur à 1 im/s, et la reprise
serait encodée à 1 im/s jusqu'à la remesure suivante — une image toutes les
secondes pendant plusieurs secondes après un simple « unmute ». Règles :

1. **Seuil de pause** : un écart supérieur à **1 s** (90 000 unités) est une
   pause, pas un écart de cadence. Il n'entre pas dans la somme, et il **vide
   la fenêtre** : ce qui précède la pause ne dit rien de ce qui la suit.
2. **Pendant la pause, rien ne bouge** : aucune image n'arrive, donc
   `EncodePicture` n'est pas appelé, donc ni mesure ni réouverture. L'encodeur
   garde son `fps` d'avant la pause — c'est la seule valeur connue.
3. **Reprise = remesure complète avant tout effet** : après une pause, la
   fenêtre est vide ; une nouvelle valeur ne peut être appliquée que quand la
   fenêtre est **pleine** (30 écarts, tous postérieurs à la pause). Une reprise
   à la même cadence ne provoque donc rien (mesure égale, sous l'hystérésis) ;
   une reprise à une autre cadence n'est appliquée qu'après 30 images à cette
   cadence. Entre-temps l'encodeur tourne au `fps` d'avant la pause.
4. Un client qui émet réellement du noir à 1 ou 2 im/s pendant le mute est
   mesuré comme tel, et l'encodeur suit : c'est une cadence réelle, le
   comportement est juste (débit par image plein sur des images noires,
   négligeable).

Le même seuil sert la première mesure : à l'attachement la fenêtre est vide,
l'encodeur s'ouvre à `configuredFps` et ne change qu'après 30 écarts.

Servir la mesure sur `/status` n'est pas prévu ici, mais la valeur est celle
qu'un exploitant voudra voir : la journaliser à chaque changement appliqué.

#### Appliquer

`fpsEffectif = min(configuredFps, arrondi(fpsMesure))`. Jamais au-dessus de la
consigne : `configuredFps` est une borne du contrôleur (et du niveau AV1 via
`ClampToLevel`), la mesure ne peut que la baisser. Plancher à 1.

Hystérésis avant d'appliquer : on ne change `fps` que si la fenêtre est
**pleine**, si la mesure s'écarte de la valeur en vigueur de **plus de 25 %**,
et **au plus une fois toutes les 5 s** — chaque application coûte une trame
clé (ci-dessous). Une source qui passe de 30 à 28 ne déclenche rien ; 15 → 30
déclenche après 30 images à 30, soit 1 s.

Une **baisse** n'est appliquée qu'après **20 s de flux** passées sans
interruption sous la bande (`FpsDropHoldTicks`, compté sur les pts). Le
compteur repart à zéro dès que la mesure revient dans la bande. Raison : une
source qui creuse à 11 im/s trois secondes puis revient à 15 coûtait deux
trames clés, et chez le pair 0,5 à 1 s de gigue à chaque aller-retour, jusqu'à
une fausse congestion sur réseau sain (séance du 2026-09-02, journal Linphone :
`jitter buffer rls stats … max_ts_deviation`). Une baisse qui dure, elle, doit
passer : l'encodeur ouvert à 30 im/s qui n'en reçoit que 10 n'émet qu'un tiers
de son budget. Une hausse s'applique tout de suite pour la même raison.

Trois façons d'appliquer, une seule juste (**décision 2026-08-28 : (i),
réouvrir**) :

| Option | Mécanisme | Verdict |
|---|---|---|
| (i) Réouvrir l'encodeur avec le nouveau `fps` | `ReopenCodec()` après `SetFrameRate` quand `fps` bouge (nouvelle politique `ShouldReopenForFps` à côté de `ShouldReopenForBitrate`, dans libmedikit) | **Retenue (arbitrée).** Seule voie où `time_base`, `rc_buffer_size`, `gop_size` et le budget par image redeviennent cohérents ensemble. Coûte une trame clé — bornée par l'hystérésis |
| (ii) Donner à l'encodeur des `pts` réels (unités de `time_base` dérivées du RTP) | l'encodeur verrait le vrai temps | Ne marche pas : le wrapper libx264 de ffmpeg force `b_vfr_input = 0` (de mémoire de `libx264.c`, à confirmer sur la 5.1.10 installée), SVT-AV1 et libvpx règlent leur budget sur `frame_rate` — tous ignorent la durée réelle entre images |
| (iii) Compenser dans le mcu : `bitrateEncodeur = bitrate × configuredFps / fpsMesure` | rien à changer dans libmedikit | Le budget par image redevient juste, mais `rc_buffer_size` et `gop_size` restent faux ; c'est un mensonge à l'encodeur, pas une reconfiguration |

Tout ce qui lit `fps` dans `EncodePicture` lit `fpsEffectif` : `time_base` à
l'ouverture (via `SetFrameRate` puis réouverture), le pas de remontée du débit
(`target*0.08/fps`), l'écart minimal entre deux images encodées (`1/fps` :
avec `fpsEffectif ≤ configuredFps`, l'écart n'écarte que ce qui dépasse la
consigne), et `ClampToLevel` pour AV1.

**Période intra — décidé le 2026-08-28 : constante en secondes.**
`intraPeriod` est **en images** dans l'API (`VideoTranscoderSetCodec`), et le
mediaserver la pose telle quelle en `ctx->gop_size` à l'ouverture, sans valeur
par défaut ni conversion. À cadence divisée par deux, la garder en images
double l'intervalle en secondes — et c'est la reprise après perte qui en
souffre, là où les FIR/PLI passent par le transcodeur. Règle :
`intraEffectif = max(1, intraPeriod × fpsEffectif / configuredFps)`, calculé
dans `EncodePicture` à côté de `fpsEffectif` et passé au `SetFrameRate` qui
rouvre : **une seule trame clé** pour les deux changements. Le contrat XML-RPC
ne change pas (rien côté elixip, MOTELI ni jsr309impl) ; libmedikit non plus
(`gop_size` reste un nombre d'images, l'arithmétique est dans le mcu). Une
phrase à ajouter dans `JSR-309-API.md` : « si la source est plus
lente que `fps`, le transcodeur garde la durée `intraPeriod/fps` en secondes ».
Valeurs effectives journalisées à chaque réouverture.

#### Ce que ça change ailleurs

- Les ports de mixeur ne sont pas concernés : leur encodeur est cadencé par
  le mixeur, `fps` y reste vrai.
- Le mode pont n'est pas concerné : pas d'encodeur dans le chemin.
- `PushSourceBitrateLimit`/`SetREMB` : inchangés, le débit visé ne change
  pas, seule sa répartition par image change.
- libmedikit : une réouverture sur changement de `fps` dans `SetFrameRate`
  (VP8, AV1, H264 VAAPI **et** libx264, puisque `reconfig` ne couvre pas la
  cadence) ; test unitaire côté sous-module : « `SetFrameRate` avec un `fps`
  différent rouvre et la trame suivante est une clé ».

### 3.5 `RTPMultiplexerSmoother` — reste

Il étale les paquets d'une image sur `sendingTime` : c'est du temps, pas de la
réaction. Hors périmètre. Remarque pour un lot ultérieur : `RTPSession` porte
déjà un pacer à budget (`RTPSmoother`, lot 6 du contrôle de débit) ; un seul
étaleur par jambe émettrice serait la cible logique, mais ce n'est pas le sujet
de ce lot.

## 4. Ce qui menace la stabilité, et la réponse du plan

Chaque point ci-dessous a été vérifié dans le code, pas supposé.

### 4.1 Le verrou du port source est tenu pendant toute la chaîne

Inline, la chaîne complète tourne **sous `RTPMultiplexer::mutex` du port
source** : décodage, encodage, `encoder.Multiplex` (qui prend le mutex de
l'encodeur), `RTPEndpoint::onRTPPacket` du puits, `RTPSession::SendPacket`.
Ce verrou est ce qui fait de `RemoveListener` une **barrière** : quand il rend
la main, aucun `onRTPPacket` n'est plus en vol. Le plan s'appuie dessus (4.3) ;
il faut donc le garder.

Ordre de verrouillage à respecter, documenté dans
`docs/reference/jsr309-ordre-verrous.md` : `Port(source).mutex` →
`encoder.mutex` du transcodeur → verrous d'émission de `RTPSession(puits)`.
Jamais l'inverse. Vérifié : `SendPacket` ne prend aucun
verrou de multiplexeur ; un appel A→B et un appel B→A ne forment pas de cycle.

### 4.2 Interblocage latent avec le verrou de `MediaSession` — PRÉEXISTANT

Le thread XML-RPC tient `MediaSession::mutex` pendant `Attach`/`Dettach`/
`SetCodec`, et y appelle `AddListener`/`RemoveListener`/`TryCodec` → attend
`Port.mutex`. Si, sur le chemin des paquets, sous `Port.mutex`, on appelle
`Joinable::Update()` sur un `RTPEndpoint` dont `useExtFIR` est vrai, alors
`RTPEndpoint::Update` → `PostEvent` → `JSR309Manager::PostEvent` →
`sess->GetEventContext()` → **`MediaSession::mutex`**. Cycle.

Ce chemin existe **déjà** : `VideoTranscoder::onRTPPacket` (sous `Port.mutex`)
appelle `RequestSourceFPU()` → `j->Update()` au basculement de mode.
`useExtFIR` est faux par défaut (`rtpsession.cpp:373`) et posé par propriété,
ce qui explique qu'on ne l'ait pas vu. Le plan l'aggrave : les demandes de FPU
du décodeur (`DecodePacket`, perte ou erreur de décodage) passent aussi sous le
verrou.

Réponse : `MediaSession::PostEvent` (ligne ~128) résout déjà le contexte
**sans** reprendre le mutex pour l'appelant qui le tient. Il faut que le chemin
`JSR309Manager::PostEvent` fasse de même depuis le chemin des paquets : soit
`GetEventContext` devient sans verrou de session (la map des contextes ne
change qu'à la création/destruction d'objets ; un `shared_ptr` copié sous un
verrou dédié plus fin suffit), soit `Joinable` porte directement le
`shared_ptr<JSR309EventContext>` à la création.

**Fait (lot 0)** : la table des contextes a son propre verrou,
`MediaSession::eventContextsMutex`, verrou feuille que rien ne prend avant un
autre. `GetEventContext` et `MediaSession::PostEvent` ne prennent plus le mutex
de session. Test : `mcu/tests/test_jsr309_event_deadlock.cpp` — il provoque un
`Update()` sous `Port.mutex` pendant un `Dettach` concurrent, et TUE le
processus si les deux threads ne rendent pas la main en 5 s (vérifié : il
interbloque bien sur le code d'avant).

### 4.3 Destruction d'un codec pendant qu'il travaille

Sans thread, `Stop()` ne joint plus rien : il détruit un codec que le thread de
démux peut être en train d'utiliser. Règle : **retirer le listener de la source
avant** de toucher au codec, jamais après. `RemoveListener` rend la main
seulement quand le `Multiplex` en cours est terminé (4.1).

À corriger dans l'ordre des appels :
- `AudioDecoderJoinableWorker::Attach` et `VideoDecoderJoinableWorker::Attach`
  faisaient `Stop()` **puis** `RemoveListener` : **inversé (lot 0)** ;
- `Dettach()` faisait le même mauvais ordre, contrairement à ce que ce plan
  affirmait : **inversé côté audio (lot 1) et côté vidéo (lot 2)** ;
- `AudioTranscoder::End`/`VideoTranscoder::End` font déjà `UnlistenSource()`
  en premier (commit `bad033e`) : bon ordre.

En mode pont, le transcodeur alimente son décodeur **à la main** depuis son
propre `onRTPPacket`, et `VideoTranscoder::Attach` appelle `decoder.Start()`
sans l'inscrire : le retrait du **transcodeur** (`UnlistenSource`) est la
barrière pour le décodeur aussi. Rien à changer, à documenter.

### 4.4 Plan de contrôle contre plan de données : plus de séparation par le join

Aujourd'hui `SetCodec` (thread XML-RPC) fait `Stop()` — qui **joint** — puis
`Start()` : l'arrêt du thread sépare l'ancien paramétrage du nouveau. Inline,
`SetCodec` peut écrire `codec`/`fps`/`bitrate` pendant qu'`EncodePicture`
les lit sur le thread de démux.

Réponse : généraliser le motif **déjà en place** pour les bornes négociées
(`negotiated` + `negotiatedLock` + `negotiatedDirty`) : le plan de contrôle
écrit les paramètres sous un verrou court et lève un drapeau atomique ; le chemin
des paquets consomme le drapeau au paquet suivant, jette son encodeur et le
recrée. Un seul motif pour `SetCodec` et `SetNegotiatedCodecProperties`, au lieu
de deux (Stop/Start pour l'un, drapeau pour l'autre). `AudioEncoderWorker` a
le même Stop/Start dans `SetNegotiatedCodecProperties` : même traitement.

`Start()`/`Stop()` gardent un sens : « le chemin est ouvert / fermé ». Ils
deviennent un booléen consulté par `onRTPPacket`, plus un thread.

### 4.5 Le thread de démux devient le thread de calcul

Un encodage vidéo logiciel (x264, SVT-AV1) prend plusieurs ms par image ; il
s'exécutait sur un thread dédié, il s'exécutera sur le thread de démux de la
jambe source. Pendant ce temps, les paquets suivants attendent dans le
`RTPBuffer` (jitter buffer) entre `RTPSession::Run` et `MultiplexLoop`.

Vérifié : ce tampon **n'est pas borné** (`rtpbuffer.h::Add` empile sans
limite). Aujourd'hui la file du décodeur ne l'était pas non plus ; le risque
n'est pas créé par le plan mais il se déplace vers un tampon commun. Réponse :
mesurer avant/après (voir 6), et **borner `RTPBuffer`** (en durée, comme
`PipeAudioInput::MaxQueuedMs`, en jetant les plus anciens) — utile
indépendamment du plan.

**Fait (lot 0)** : `RTPBuffer::MaxQueuedMs` = 500 ms d'arrivées. À l'insertion,
la tête est lâchée tant qu'elle est plus vieille que ça, et la file se
resynchronise (`next = -1`) pour ne pas attendre un trou que rien ne comblera.
Tests dans `test_wait_primitives.cpp`, section « borne de profondeur ».

Point d'attention : les deux médias d'une jambe ont chacun leur `RTPEndpoint`
et donc leur thread de démux (`Endpoint.cpp:28-39`). L'audio ne subit pas la
latence de l'encodage vidéo.

Le deinit d'un encodeur (jusqu'ici sur le thread d'encodage) se fait désormais
sur le thread de démux, **sous `Port.mutex`**. Si un deinit se bloque (cas
SVT-AV1 0.9.0 du 2026-08-13), le thread XML-RPC se bloquera derrière
`Port.mutex` au prochain `RemoveListener` : la même gêne, par un autre chemin.
Le contournement de ce bogue vit dans libmedikit (`medkit/ffcodeclock.h`) ; le
plan n'y change rien, il faut le dire.

### 4.6 `onEndStream` et réentrance sur le même thread

`onEndStream` appelle `Stop()` depuis le thread de la source. Aujourd'hui c'est
l'auto-join que `Worker::StopThread` refuse et journalise. Sans thread, `Stop()`
depuis le chemin des paquets ne fait plus que baisser un booléen et détruire un
codec — ce qui est autorisé : on est sous `Port.mutex`, donc aucun autre
`onRTPPacket` de cette source n'est en vol.

### 4.7 Ce qui ne change pas

- `VideoTranscoder`/`AudioTranscoder::onRTPPacket` : l'arbitrage pont/transcodage,
  `TryCodec`, `RequestSourceFPU`, `PushSourceBitrateLimit` — inchangés.
- Les événements RTCP (FIR/TMMBR/REMB), `SetREMB`, `Update` — inchangés.
- Les ports de mixeur gardent leur thread d'encodeur.
- Le `RTPMultiplexerSmoother` et le SSRC neuf par run d'encodage.

## 5. Options et recommandation

| Option | Description | Pour | Contre |
|---|---|---|---|
| A. Drapeau `useThread` dans chaque worker | Généraliser le prototype de `VideoDecoderJoinableWorker` aux quatre classes | Diff minimal | Deux modes dans chaque classe, l'un d'eux ne tourne que dans les tests d'un des deux appelants ; c'est ce que le code mort actuel a produit |
| B. Extraire le cœur synchrone, garder la boucle là où elle cadence | `DecodePacket`/`EncodeSamples`/`EncodePicture` deviennent l'API ; les décodeurs perdent leur thread partout ; les encodeurs gardent `Run()` pour le mixeur seulement ; `AudioResampler` sorti de `PipeAudioInput` | Un seul corps de traitement par classe, testé seul ; les deux appelants (transcodeur, mixeur) le partagent | Touche les six classes ; demande le lot 0 (4.2, 4.5) |
| C. Tout inline, mixeurs compris | Le mixeur pousse ses trames dans les encodeurs | Zéro thread d'encodeur | Réécriture des mixeurs, hors sujet |

**Recommandation : B.** Elle supprime réellement les threads (6 sur 18 par
appel), sans double mode, et fait disparaître les joins des transcodeurs. A
laisse des chemins morts ; C dépasse le lot.

## 6. Plan de travail

Chaque lot compile, passe `cd mcu && make check`, et se commite seul.

**Lot 0 — préalables de sûreté (aucun thread supprimé) — FAIT**
1. FAIT. `JSR309Manager::PostEvent` ne prend plus `MediaSession::mutex` depuis
   le chemin des paquets (4.2) : verrou dédié aux contextes d'événement, plus
   `test_jsr309_event_deadlock.cpp` (un `Update()` sous `Port.mutex` pendant un
   `Dettach`). Ordre des verrous écrit dans
   `docs/reference/jsr309-ordre-verrous.md`.
2. FAIT. Borne du `RTPBuffer` (4.5) + 3 tests dans `test_wait_primitives.cpp`.
3. FAIT. `RemoveListener` avant `Stop()` dans les deux `Attach` (4.3).
4. FAIT. Tests de caractérisation, `test_transcoder_characterization.cpp` :
   audio PCMU→Opus (nombre de paquets, code codec, SSRC unique par run, pas
   d'horodatage constant, SSRC neuf au run suivant) et vidéo VP8→H264 (code
   codec, SSRC unique, fin d'image marquée, FPU sur perte). Pas de fixture à
   versionner : la source VP8 est encodée à la volée par le vrai encodeur.
   Le NOMBRE d'images vidéo n'est volontairement pas figé — c'est ce que 3.3
   change. Ces tests doivent rendre le même résultat après les lots 1 à 4.
5. RESTE À FAIRE. Mesure de référence : l'outil est livré
   (`mcu/tests/tools/thread_census.sh` : threads par nom, total, CPU sur une
   fenêtre) ; le relevé demande un appel réel, à prendre avant le lot 1.

**Lot 1 — décodeur audio — FAIT** : `DecodePacket(RTPPacket&)` appelé
directement par `onRTPPacket` ; plus de `WaitQueue`, plus d'héritage de
`Worker`, plus de `packet.Clone()`. Le décodeur est un membre créé
paresseusement au codec reçu et détruit par `Stop()`, qui se réduit à un
booléen plus la fermeture du puits (`StopPlaying` / `PipeAudioInput::End`).
Un décodeur introuvable referme le chemin, comme le faisait la sortie de
boucle. `Dettach()` retire le listener AVANT d'arrêter. Vaut pour le port de
mixeur aussi. Tests : `mcu/tests/test_audio_decoder_inline.cpp` (la trame est
là au retour de `Multiplex`, côté `AudioOutput` et côté `PipeAudioInput` ;
aucun thread créé, compté dans `/proc/self/task` ; `Dettach` concurrent d'un
producteur). Les trois premiers échouent sur le code d'avant.

**Lot 2 — décodeur vidéo — FAIT** : chemin unique, `useThread`, `Decode()`,
`WaitQueue` et membre masqué supprimés ; `DecodePacket(RTPPacket&)` ; plus
d'héritage de `Worker` ; `Stop()` détruit le décodeur (le chemin threadé le
fuyait, il ne le libérait que dans la branche sans thread) ; `Dettach()` retire
le listener avant d'arrêter. Tests :
`mcu/tests/test_video_decoder_inline.cpp` (l'image est là au retour de
`Multiplex` ; aucun thread créé ; `Dettach` concurrent). Les deux premiers
échouent sur le code d'avant.

**Lot 3 — encodeur audio — FAIT** : `EncodeSamples(SamplesPtr)` est le corps
unique ; `Encode()` s'y réduit à `RecFrame` + `EncodeSamples`, et ne tourne que
pour le port de mixeur (`Init(AudioInput*)`). Le transcodeur ouvre le mode
**poussé** (`Init()` sans argument) : aucun thread, aucune file, et plus de
`PipeAudioInput` — `AudioTranscoder` est devenu l'`AudioOutput` de son décodeur.
Le rééchantillonnage est sorti dans `AudioResampler`
(`mcu/{include,src}/audioresampler.{h,cpp}`, option (b) de 3.4), utilisé par
`PipeAudioInput` **et** par l'encodeur. `SetCodec` et
`SetNegotiatedCodecProperties` écrivent sous `configLock` et lèvent
`configDirty` : plus aucun Stop/Start depuis le thread XML-RPC (4.4). Tests :
`mcu/tests/test_audio_encoder_inline.cpp`.

**Lot 4 — encodeur vidéo — FAIT** : `EncodePicture(PictPtr)` est le corps
unique ; `Encode()` s'y réduit à `GrabFrame` + attente d'échéance +
`EncodePicture`, et ne tourne que pour le port de mixeur. Le transcodeur ouvre
le mode **poussé** : `VideoTranscoder` est devenu le `VideoOutput` de son
décodeur, il écarte les images plus rapprochées que la CONSIGNE (voir la
nuance ci-dessous) et le redimensionnement a suivi dans `EncodePicture`
(`VideoRescaler`, partage zéro-copie quand la taille coïncide). Le pthread brut
est devenu un `Worker` **composé** (`EncodeLoop`) : `RTPMultiplexerSmoother` est
déjà un `Worker` et occupe l'héritage. `SetCodec` passe par le même drapeau que
les bornes négociées, et la réouverture tire un SSRC neuf (`RenewSSRC`), ce que
faisait le Stop/Start du lisseur. Tests :
`mcu/tests/test_video_encoder_inline.cpp`.

> **Écart assumé avec 3.6.** Le plan dit que l'écart minimal entre deux images
> encodées suit `fpsEffectif`. C'est le seul point où l'implémentation diverge :
> elle suit `configuredFps`. Raison : `fpsEffectif` est la MESURE de ce que la
> source envoie ; s'en servir comme seuil de rejet jetterait, à la moindre gigue,
> les images mêmes qui l'ont produite — une source à 15 im/s avec ±3 ms de gigue
> perdrait la moitié de ses images. Le rôle du seuil est de ne jamais dépasser la
> consigne négociée, et `configuredFps` le remplit exactement. Une tolérance de
> 10 % absorbe la gigue d'une source qui émet déjà à la consigne.

**Lot 4 bis — cadence réelle (3.6) — FAIT** : `VideoDecoderJoinableWorker`
pose `frame->GetAVFrame()->pts = packet.GetTimestamp()` sur les deux chemins de
livraison. `VideoTranscoder` tient la fenêtre de 30 écarts, le seuil de pause de
1 s, l'hystérésis de 25 %, la tenue de 20 s d'une baisse et la borne de 5 s,
puis pousse la valeur par `SetMeasuredFrameRate`. `EncodePicture` calcule `fpsEffectif` et `intraEffectif`
(`RecomputeFrameRate`) et n'appelle `SetFrameRate` qu'au changement : une seule
trame clé pour les deux. Côté libmedikit, `FfVideoEncoder::ShouldReopenForFps`
(seuil 20 %, symétrique) et sa prise en compte dans `VP8Encoder`, `AV1Encoder` et
`H264Encoder` — pour H264 la réouverture précède les deux chemins existants
(VAAPI et reconfig libx264), puisque ni l'un ni l'autre ne change la cadence.
Tests du sous-module : trois de plus dans
`tests/test_video_encoder_reconfig.cpp`. `GetEffectiveFps` /
`GetEffectiveIntraPeriod` exposent les valeurs appliquées ; elles sont
journalisées à chaque changement. Contrat écrit dans
`JSR-309-API.md` §6.11.

**Lot 5 — recette — PRÉPARÉ, appel réel à jouer** : la procédure est écrite
dans `docs/maintenance/recette-transcodeur-sans-thread.md` : les quatre
combinaisons (audio/vidéo, pont/transcodage), renégociation en cours d'appel
(`SetCodec` et bornes négociées à chaud), détachement/rattachement, arrêt de la
source pendant l'appel (3.3), source lente (Linphone limité à 15 im/s vers un
puits négocié à 30 : débit sortant et période des trames clés, 3.6), mute vidéo
de 5 s puis reprise (aucune trame clé ni changement de cadence à la reprise si
la cadence est la même). Elle porte aussi la feuille de relevé et la comparaison
au lot 0 (18 threads attendus avant, 10 après, pour un appel audio + vidéo
transcodé dans les deux sens).

Ce qui n'exigeait PAS d'appel réel est automatisé dans
`mcu/tests/test_transcoder_recette.cpp` (6 tests) : rattachement à une nouvelle
source, arrêt de la source par `EndStream` puis reprise, source détruite sans
`Dettach`, renégociation de codec à chaud et bornes négociées à chaud — chacune
sans thread créé ni joint, et avec le SSRC neuf qui signe la réouverture de
l'encodeur. Tableau de bord du chantier : `cd mcu && make check-transcoder`.

**Deux séances de recette jouées (2026-08-28 et 2026-08-29, Linphone ↔ Linphone,
VP8/Opus ↔ H.264/PCMU, transcodage dans les deux sens), deux défauts trouvés,
deux corrigés :**

1. *Image écrasée* (2026-08-28). Sortie figée au `mode` du contrôleur (CIF,
   4:3) pour une source 16:9 ; `Rescale(..., false)` étire. Pas une régression
   (VideoPipe faisait pareil), mais un second défaut réel dessous : la
   géométrie native était PERDUE à chaque réouverture d'encodeur
   (`ComputeEffective` revenait au `mode`, le drapeau `pushedSizeChanged` déjà
   consommé). Corrigé : transcodeur adaptatif par défaut
   (`VideoTranscoderCreate` → `Init(true, true)`, le contrôleur garde
   `useInputSize=0`), géométrie native posée AVANT la création de l'encodeur,
   trace `taille native de la source`, `GetEffectiveWidth/Height`, 2 tests.

2. *Qualité mauvaise dans un sens* (2026-08-29). Conséquence directe de 1 (les
   sources montent à 720p) et du passage inline : l'encodeur VP8 (libvpx ouvert
   avec les défauts ffmpeg, pas de `deadline=realtime`) est plus lent que la
   source à 720p ; le thread de démux prend du retard, le `RTPBuffer` (borne du
   lot 0) jette les paquets — 10 697 en 64 s —, le décodeur y voit des pertes et
   réclame une trame clé au pair chaque seconde (57 FIR), la mesure de cadence
   (lot 4 bis) est trompée et rouvre l'encodeur 10 fois. **C'est la faille du
   §4.5** : la borne du RTPBuffer transforme le retard en pertes, et les pertes
   en tempête de trames clés — là où VideoPipe perdait des IMAGES sans toucher
   aux paquets. Corrigé par `FrameDecimator` (`src/jsr309/FrameDecimator.h`) :
   un pas k = ceil(coût / (budget × 4/5)) sur le coût mesuré de `EncodePicture`
   et l'écart moyen entre images de la source ; monte tout de suite, redescend
   après 3 s de calme, écrête l'échantillon isolé (trame clé), plafonne à 15 et
   le dit ; la cadence poussée à l'encodeur devient source/k. Trace
   `encodeur trop lent […] -> 1 image sur k encodee, sortie N im/s`, rappel
   toutes les 30 s. Tests `test_frame_decimator.cpp` (temps simulé). Le
   LEVIER 1 est fait aussi : `VP8Encoder::ConfigureContext` (libmedikit) ouvre
   libvpx en temps réel — `deadline=realtime`, `cpu-used=6`, `lag-in-frames=0`,
   `error-resilient=default`, 2 threads. Mesuré par
   `libmedikit/tests/test_vp8_realtime.cpp` (40 images 720p sur 2 cœurs) :
   125 ms par image avec les défauts ffmpeg (pire cas 782 ms), 22 ms après
   (pire cas 41 ms). Au passage, la sonde de `test_video_encoder_reconfig.cpp`
   est passée d'un bruit pur à un bruit d'amplitude ±32 : en temps réel, le
   bruit pur a un plancher de 16 Ko par image 320x240 sous lequel aucune
   consigne ne descend, et le test mesurait ce plancher. La décimation reste : elle couvre ce que le réglage ne couvre pas
   (machine chargée, autre codec, autre résolution).

Troisième appel (2026-08-29 15:39, les trois correctifs) : qualité bonne dans
les deux sens, 0 paquet jeté, 1 FPU pour une perte réelle, 0 décimation. Bob est
resté en 640x480 : le cas VP8 720p n'a pas été rejoué.

Quatrième appel (2026-08-29 15:43-16:03, 20 min) : qualité bonne dans les deux
sens, 0 paquet jeté, VP8 ouvert une seule fois. Un défaut de la décimation trouvé
et corrigé : x264 720p à 31-34 ms pour 32 ms utilisables faisait battre le pas
1↔2 (74 réouvertures, une trame clé toutes les 16 s) — montée et descente
partageaient le même seuil ; la descente demande maintenant 7/10 de la part
utilisable (`FrameDecimator::DownShare*`). Le mute vidéo de Linphone n'est pas
une pause (image fixe de remplacement, flux continu) : scénario 6 à jouer caméra
coupée. Annexe hors chantier : `PipeAudioInput could not transrate` 59 fois en
20 min, hors du transcodeur audio (qui n'en a plus).

Relevé `thread_census.sh "" 30` du 2026-08-29 17:09, en appel transcodé dans les
deux sens : 26-28 threads (11 au repos, donc 15-17 pour l'appel = les 10 attendus
+ threads internes x264/libvpx), CPU 94 % d'un cœur sur deux.

**RESTE** : Bob en 720p, le scénario 6 caméra coupée, et le
relevé de référence du lot 0 point 5 (la fiche dit comment bâtir le binaire
d'avant chantier).

Hors lot, à noter dans la fiche mémoire du chantier : `RTPEndpoint::run`
(pthread brut) et `MultiplexLoop` (`msleep(200)`), `RTPMultiplexerSmoother`
face au `RTPSmoother` de `RTPSession`.

## 7. Tests

- Existants à garder verts : `test_transcoder_bridging.cpp` (16 tests),
  `test_wait_sites.cpp` (sémantique de `VideoPipe`, qui reste pour le mixeur),
  `test_audio_pipes.cpp`, `test_worker.cpp`.
- À écrire (lot 0) : caractérisation de la sortie d'un transcodeur ;
  interblocage `Update()`/`Dettach` ; borne du `RTPBuffer`.
- À écrire (lots 1-4) : « `onRTPPacket` produit la sortie **avant** de rendre
  la main » (c'est la propriété nouvelle : le test appelle `onRTPPacket` et lit
  le puits immédiatement, sans attente ni sommeil) ; « `Dettach` pendant un
  `onRTPPacket` concurrent ne libère pas le codec sous le paquet » (deux
  threads, un puits qui compte, ASan/TSan en local).
- À écrire (lot 4 bis) : « une source à 15 im/s dans un encodeur configuré à
  30 sort, après 2 s, un flux dont le débit mesuré sur 5 s est à ±15 % de la
  consigne » (c'est le test qui échoue si 3.6 manque) ; « une source à 28 im/s
  ne provoque aucune réouverture » (hystérésis) ; « `fps=30, intraPeriod=300`,
  source à 15 im/s → `gop_size=150` à la réouverture, en une seule trame
  clé » ; **pause** : « 30 im/s, 5 s sans image, reprise à
  30 im/s → aucune réouverture, aucune image encodée à moins de 30 im/s » et
  « 30 im/s, pause, reprise à 15 im/s → l'encodeur reste à 30 pendant les 30
  premières images puis rouvre à 15, une seule fois » ; côté libmedikit, la
  réouverture sur `fps`.
- **Piège d'écriture des tests, trouvé au lot 2 : ne JAMAIS copier un
  `RTPPacket`.** Il porte un `header` qui pointe dans son propre `buffer` et
  n'a pas de constructeur de copie : la copie implicite garde le pointeur de
  l'original, et le lire après la mort de l'original est un accès à de la
  mémoire libérée. Un `std::vector<RTPPacket>` est donc un piège — le bit de
  marque s'y perdait, si bien qu'aucune image n'était jamais complète pour le
  décodeur et que `test_transcoder_characterization.cpp` mesurait la sortie
  d'un encodeur qui ré-émettait une image vide. Corrigé : les sources de test
  livrent leurs paquets par rappel, sans copie. (`RTPPacket::Clone()` est le
  seul duplicateur correct : il reconstruit.)
- **FAIT (lot 5)** : suite jouée sous `-fsanitize=thread` (commutateur
  `TSAN=yes` du `mcu/Makefile`, mode d'emploi dans
  `docs/maintenance/recette-transcodeur-sans-thread.md`). Une VRAIE course
  trouvée et corrigée : `RTPMultiplexerSmoother::inited` était un `bool` nu,
  écrit par `Stop()` et lu par `Run()` comme condition de boucle — en `-O3` la
  lecture peut être hissée hors de la boucle et le `StopThread()` de `Stop()`
  n'en revient jamais. Passé en `std::atomic<bool>`, comme `Worker::running`.
  Après correctif, plus rien sur le chemin des paquets d'un transcodeur. Le
  reste du dépôt en rapporte ~630, dont 85 dans `src/rtpsession.cpp` : hors
  périmètre, à traiter ailleurs. `include/RTPSmoother.h` (lisseur de
  `RTPSession`) porte le MÊME `bool inited` non atomique — non touché ici.

## 7 bis. Reste à faire : la bande morte de la montée du pas

Mesuré le 2026-09-01, appel JSR-309 transcodé H.264 ↔ VP8 de 27 min, 720p,
x264 `veryfast` mono-thread. Défaut réel, **non corrigé**.

### Ce que le journal dit

```
-VideoTranscoder: encodeur trop lent [video transcoder outbound] :
 33 ms par image pour un budget de 41 ms (source 24 im/s)
 -> 1 image sur 2 encodee, encodeur recale a 12 im/s
```

33 ms sous un budget de 41 ms, et pourtant le pas monte. C'est correct : le
seuil n'est pas le budget mais la **part utilisable**, qui réserve un cinquième
au décodage et au démux du même thread.

```
usable = 41 × UsableShareNum/UsableShareDen = 41 × 4/5 = 32,8 ms
StepFor(32,8)                              = ceil(33 / 32,8) = 2
```

Le pas bascule donc pour **0,2 ms** de dépassement. Et pour redescendre il faut
`32,8 × DownShareNum/DownShareDen = 23 ms` tenus `RecoveryUs` (3 s) — qu'un
encodeur oscillant entre 25 et 34 ms n'atteint jamais.

### Ce que cela produit

| Mesure sur 27 min | Valeur |
|---|---|
| Traces « encodeur trop lent » | **4 par minute, chaque minute, pendant 23 min** |
| Cadence de sortie | 12 im/s au lieu de 24 |
| `OpenCodec` | 24, dont **14 dans les 70 premières secondes** |
| `Got Intra` | 183 |
| Paquets jetés par le `RTPBuffer` | **0** |

Ce n'est pas un transitoire : c'est le régime permanent. Symptôme observé côté
pair : environ une seconde de latence vidéo au démarrage, qui se résorbe
lentement. La lenteur est la somme de quatre délais délibérés qui s'ajoutent :
`MinSamples` (8 images) avant la première décision, `FpsWindow` (30 images)
avant qu'une cadence puisse être appliquée, 5 s minimum entre deux
applications, `RecoveryUs` (3 s) avant qu'un pas redescende.

### Le défaut

**La montée du pas n'a aucune bande morte**, alors que la descente en a 30 %.
L'asymétrie est voulue — le `RTPBuffer` jette après 500 ms, il n'y a pas le
temps d'attendre — et ce motif reste bon. Mais il n'exige pas une marge
**nulle** : 500 ms à 25 im/s, c'est douze images de mou, de quoi confirmer une
montée sur deux ou trois échantillons.

### Options

| Option | Verdict |
|---|---|
| **Bande morte sur la montée** : monter le pas seulement si `costUs > usable × 11/10`, ou après 2 à 3 échantillons consécutifs au-dessus du seuil | **Retenue.** Local à `FrameDecimator::Observe`, couvert par `mcu/tests/test_frame_decimator.cpp` |
| `UsableShare` de 4/5 à 9/10 | Écartée : déplace le seuil sans le stabiliser, et prend la marge qui protège des pics de décodage |
| `thread_count` > 1 pour libx264 | Écartée sur la seule lecture d'un journal : le mono-thread est un choix assumé pour un mixeur chargé (`h264/h264encoder.cpp`) |

### Ce qui a été vérifié et écarté comme cause

H.264 est **déjà** réglé pour le temps réel dans
`third_party/fontventa/libmedikit/h264/h264encoder.cpp` : `preset veryfast`
au-delà de VGA, `tune zerolatency`, `forced-idr`, `ref=1`, `subme` réduit,
`thread_count = 1`. Il n'y a pas de réglage oublié — 33 ms est ce que coûte un
720p mono-thread à ce preset.

Le chantier « moins de threads RTP » (`docs/conception/RTP-REACTOR/SPEC.md`) est
hors de cause : sur la même séance, zéro alerte du réacteur et zéro paquet jeté
par le `RTPBuffer`. Le même régime est d'ailleurs documenté dans l'en-tête de
`FrameDecimator.h` au 2026-08-29, avant ce chantier.

## 8. Ce que ce plan ne fait pas

- Il ne touche pas aux threads des jambes RTP (`RTPSession::Run`,
  `MultiplexLoop`), ni aux mixeurs, ni au participant MCU (`audiostream`,
  `videostream`). `AudioDecoderWorker` (`mcu/src/audiodecoder.cpp`,
  `mcu/include/audiodecoder.h`) n'avait plus aucun appelant : supprimé au lot 5,
  avec la déclaration avancée morte de `AudioTranscoder.h` et `audiodecoder.o`.
  À ne pas confondre avec `AudioDecoderJoinableWorker` (`src/jsr309/`), qui est
  le décodeur vivant.
- Il ne mesure rien tant que le lot 0.5 n'est pas fait : les gains de CPU sont
  attendus, pas prouvés.
