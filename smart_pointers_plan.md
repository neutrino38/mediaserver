# Migration vers les smart pointers — analyse et plan d'implémentation

> Objectif : éliminer les crashs (use-after-free, double delete, dangling listeners, corruption de
> maps) dus à la gestion manuelle de la mémoire et aux refcounts « maison », en introduisant
> progressivement `std::shared_ptr` / `std::weak_ptr` / `std::unique_ptr` (C++17, déjà activé par
> le build `gnu++17`).
>
> Analyse réalisée le 2026-07-06 sur la branche `feat/alma_linux9`. Toutes les références
> `fichier:ligne` correspondent à cet état du code.

---

## 1. État des lieux : les mécanismes de protection actuels

### 1.1 Aucun smart pointer dans le cœur du serveur

Les seules occurrences sont du code Gnash/Flash tiers (`std::auto_ptr` dans
`mcu/include/FlashSoundHandler.h:12`, `mcu/src/FlashPlayer.cpp:111`). Toute la gestion de vie
repose sur `new`/`delete` bruts + compteurs manuels.

### 1.2 La classe `Use` (`mcu/include/use.h:8-106`) — le primitif central

Combine un compteur de références et un verrou écrivain :

- `IncUse()`/`DecUse()` (l.26, l.33) : sémantique « lecteur » — incrémentent `cont` sous mutex puis
  relâchent immédiatement. Deux lecteurs ne s'excluent jamais.
- `WaitUnusedAndLock()` (l.41, variante timeout l.62) : sémantique « écrivain » — attend `cont==0`
  et **retourne en tenant les deux mutex** (non récursifs, l.13-14).
- `Unlock()` (l.95) relâche les deux.

C'est un `shared_ptr` réinventé à la main, en plus fragile : la sûreté dépend de la discipline de
chaque appelant (faire `IncUse` avant d'utiliser, `DecUse` après), et cette discipline **n'est pas
respectée partout** (cf. §2).

### 1.3 Refcount manuel des conférences (`mcu/src/mcu.cpp`)

`ConferenceEntry { int numRef; int enabled; MultiConf* conf; }` (`mcu.h:58-65`) sous `MCU::mutex` :

- `GetConferenceRef` (mcu.cpp:179-220) : vérifie `enabled`, `numRef++`, rend le **pointeur brut**
  hors lock.
- `DeleteConference` (mcu.cpp:295-387) : `enabled=0`, puis **polling `sleep(2)`** tant que
  `numRef>0` ; après 4 essais, **fuite volontaire** de la conférence (l.338-344, commentée) pour
  éviter un crash — symptôme assumé du problème que cette migration doit résoudre.
- La destruction du `MultiConf` est conditionnée au succès de `conf->End()` (l.371) : si `End()`
  échoue, l'objet est fuité.

Ce modèle « refcount + disable + wait » est le plus sain du code ; il est repris correctement par
`Broadcaster` et `JSR309Manager`, mais **pas** par `MediaGateway` (cf. C-8).

### 1.4 Protections correctes à préserver (ne pas casser pendant la migration)

- **File d'événements XML-RPC** (`xmlstreaminghandler.cpp`) : les events **copient** leurs données
  à la construction (buffers `BYTE[]`, `std::wstring`) — aucun pointeur métier ne survit dans la
  file. Sûre, pas de migration nécessaire.
- **Pattern zombie** des serveurs RTMP/WebSocket (`rtmpserver.cpp:371-403`, `230-248`) : la
  connexion se retire elle-même, le `join` + `delete` est fait par un thread tiers. Bon design.
- **`RTMPMediaStream::listeners`** protégé par `Use` avec `WaitUnusedAndLock` sur attach/detach
  (`rtmpstream.cpp:28,44,58`) : cohérent.
- **`MP4Recorder`** et **`FLVEncoder`** : correctement sérialisés (mutex, joins).

---

## 2. Inventaire des fenêtres de crash (classées par gravité)

### CRITIQUE — crashs quasi certains sous charge concurrente

| # | Localisation | Défaut |
|---|--------------|--------|
| **C-1** | `MediaSession.h:259-301` + `JSR309Manager.cpp:181-224` | **`MediaSession` (JSR309) n'a AUCUN mutex interne.** Plusieurs handlers XML-RPC obtiennent simultanément une ref sur la même session (le manager n'incrémente qu'un compteur d'usage) et mutent les maps `endpoints`/`players`/`recorders`/`eventContexts` sans verrou → corruption de `std::map`. |
| **C-2** | `jsr309/xmlrpcjsr309.cpp:2842-2848` | `GetMediaCandidates` : `endpoint` (pointeur brut sorti de la session) utilisé **après** `ReleaseMediaSessionRef` → UAF si `DeleteMediaSession` concurrent. Autres extractions de pointeurs : l.125, 642, 720, 1477, 2538. |
| **C-3** | `JSR309Manager.cpp:351-384` (`PostEvent`) | `entry`, `entry->tag`, `entry->queueId` et `*evtctx` lus **après** `pthread_mutex_unlock`, sans `IncUse` — donc non couverts par le `WaitUnusedAndLock` de `DeleteMediaSession`. Chemin emprunté par les handlers **et** par le thread `RecorderTimer`. |
| **C-4** | `videostream.cpp:393-425, 438-465` + `multiconf.cpp:707-740` | `VideoStream::StopSending/StopReceiving` abandonnent après ~1 s **sans `pthread_join`** si le thread ne repasse pas `TaskIdle`. `DestroyParticipant` enchaîne alors `DeleteMixer` (delete des pipes, `videomixer.cpp:1006-1008`) et `delete part` → le thread survivant utilise `videoInput->GrabFrame` / `videoOutput->NextFrame` sur des objets libérés. |
| **C-5** | `participant.h:117` + grep exhaustif | **Le garde `Participant::use` est inopérant pour les `RTPParticipant`** : aucun code n'appelle jamais `use.IncUse()` pour eux (seul `RTMPParticipant` le fait en interne). Les `WaitUnusedAndLock(2000)` de `multiconf.cpp:724` et `participant.h:103` réussissent immédiatement sans rien attendre. La seule barrière réelle est le join de `End()` — faillible (C-4). |
| **C-6** | `multiconf.cpp:1963-2009, 1914-1961, 2115-2116, 2171-2198` | **Tokens participants** : `ConsumeParticipantOutputToken/InputToken` accèdent à `participants` **sans lock ni IncUse** et rendent un pointeur brut, stocké durablement dans `MultiConf::NetStream::part` puis réutilisé (`doPause`, `doResume`, `onCongestion`) sans revérification → UAF si `DeleteParticipant` entre-temps. |
| **C-7** | `multiconf.cpp:1533-1672, 838-841` | **La map `players` (`MP4Player*`) n'a aucun verrou.** `CreatePlayer`/`StartPlaying`/`StopPlaying`/`DeletePlayer` concurrents : itérateurs invalidés, `delete player` (l.1667) pendant un `Play()` → UAF/double free. |
| **C-8** | `mediagateway.cpp:296-341` | `DeleteMediaBridge` : le `//TODO: Wait for numref == 0` (l.316) n'a jamais été implémenté. `delete session` sans `enabled=0` ni attente du refcount, alors que `GetMediaBridgeRef` distribue des pointeurs → UAF direct. |
| **C-9** | `videomixer.cpp:1274-1306` | `VideoMixer::DeleteMosaic` fait `mosaics.erase` + `delete mosaic` sous simple `IncUse` (lecteur) : ne s'exclut pas du thread `MixVideo` qui itère `mosaics` sous `WaitUnusedAndLock` → corruption de map + UAF sur `Mosaic`. (`DeleteMixer` et `SetCompositionType`, eux, font correctement `WaitUnusedAndLock`.) |
| **C-10** | `rtpsession.cpp:1954-1962` | `RTPSession::GetPacket(DWORD& ssrc)` : `streamUse.DecUse()` **avant** `s->Wait()` — `DeleteStreams` (l.2481-2506) peut supprimer le `RTPStream` entre les deux. C'est le chemin de réception vidéo (`RecVideo`). La surcharge `GetPacket()` sans ssrc (l.1945-1952) est, elle, correcte. |
| **C-11** | `multiconf.cpp:657-704` + `mcu/include/use.h` | **Deadlock** : `MultiConf::End` tient `participantsLock` (WaitUnusedAndLock, l.821) puis appelle `DestroyParticipant` qui fait `participantsLock.IncUse()` (l.742) → `pthread_mutex_lock` sur mutex non récursif déjà détenu. Candidat sérieux pour expliquer les `End()` « qui échouent » et la fuite volontaire de `DeleteConference`. |
| **C-12** | `multiconf.cpp:1080-1157` | **DocSharing** : `AcceptDocSharingRequest`, `RefuseDocSharingRequest`, `StopDocSharing`, `SetDocSharingMosaic` appellent `GetParticipant` **sans IncUse/DecUse** (contrairement aux setters voisins) puis passent `part` à `sharedDocMixer` → UAF si `DeleteParticipant` concurrent. |
| **C-13** | `jsr309/Endpoint.cpp:463-472, 672-675` + `RTPMultiplexer.cpp:20-35` | **Dangling bidirectionnel du graphe d'attach JSR309.** `Endpoint::Port::joined` (→ `Joinable` source) et `RTPMultiplexer::listeners` (→ les `Port`/`RTPEndpoint`) se pointent mutuellement, aucun ne possède l'autre, et **aucun ne prévient l'autre à la destruction**. Si la source meurt d'abord (`PlayerDelete` détruit le `Player` et ses 3 `RTPMultiplexer` membres) alors qu'un endpoint reste attaché, le `~RTPMultiplexer` se contente de `listeners.clear()` : le `Port` garde un `joined` pendouillant. L'`EndpointDelete` suivant fait `~Endpoint → ~Port → Detach() → joined->RemoveListener()` sur l'objet libéré → **« pure virtual method called »**. **Confirmé en production 2026-07-08** (play d'un fichier inexistant : le chemin d'erreur elixip détruit le player avant les endpoints attachés). **Corrigé et validé 2026-07-08** (correctif ciblé 0.10). Sens inverse (endpoint détruit d'abord) sûr car `~Port` fait `RemoveListener` sur une source encore vivante. NB : `WSEndpoint` déclare un `joined` masquant qui double le défaut latent. |

### ÉLEVÉ

| # | Localisation | Défaut |
|---|--------------|--------|
| **H-1** | `mcu.cpp:422-437` | `MCU::Connect` : `ReleaseConferenceRef` (l.434) **puis** `return conf;` (l.437) — le pointeur `RTMPNetConnection*` est rendu à la couche RTMP après libération de la ref. Dangling si `DeleteConference` concurrent. |
| **H-2** | `MediaSession.cpp:65-88, 464-480` | Thread `RecorderTimer` détient un `MediaSession*` brut et appelle `onRecorderMaxDuration` sans mutex (C-1) ni ref manager → course avec les handlers en fonctionnement normal. |
| **H-3** | `rtpparticipant.cpp:369` + `videostream.cpp:761, 374-375` | **Partage de `RTPSession` entre flux principal et slides** : `video[SLIDES]->rtpSession` pointe sur la `rtp` interne de `video[MAIN]`. `End()` termine MAIN avant SLIDES (`rtpparticipant.cpp:92-93`) : le `recVideoThread` des slides peut appeler `GetPacket` sur une session en cours de teardown (se combine avec C-10). Risque aussi de double-`End()`. |
| **H-4** | `pipeaudioinput.cpp:198 vs 128-133` | `End()` fait `swr_free(&swr)` **hors** de la section mutex, alors que `PutSamples` (thread mixer) utilise `swr` sous mutex → UAF sur le resampler. Motif à vérifier sur les autres pipes. |
| **H-5** | `audiostream.cpp:35-38`, `videostream.cpp:58-64`, `textstream.cpp:32-34`, `rtpparticipant.cpp:22-27` | Les destructeurs de streams **ne joignent aucun thread et n'appellent pas `End()`** ; `~RTPParticipant` non plus (contrairement à `~RTMPParticipant` qui le fait, l.50-57). Toute destruction hors du chemin nominal = UAF. |
| **H-6** | `rtmpparticipant.cpp:1347-1354` | `onMediaFrame` (thread de la connexion RTMP source) ne prend pas `use` : écrit dans les `WaitQueue` en parallèle d'une destruction/détachement. |
| **H-7** | `videomixer.cpp:1055-1064` | `VideoMixer::GetOutput` : `return (*it).second->output` exécuté **même si `it==end()`**, et après `DecUse()` → deref de `end()` ou UAF. Appelé par `CreateParticipant` (`multiconf.cpp:606`). |
| **H-8** | `multiconf.cpp:672-696` | `DeleteParticipant` : lock relâché (l.685) puis repris (l.690) en réutilisant l'itérateur `it` obtenu avant → `erase(it)` sur itérateur potentiellement invalidé ; deux `DeleteParticipant` concurrents du même id → double destruction. |

### MOYEN / FAIBLE

| # | Localisation | Défaut |
|---|--------------|--------|
| **M-1** | `multiconf.cpp:2408-2513` | Map `publishers` sans verrou ; `delete info.stream/conn` depuis le callback réseau `onDisconnected` ; le `//TODO: should we lock? I expect so` (l.2486) est dans le code. |
| **M-2** | `mcu.cpp:168, 473-489` | Les callbacks conférence (`onParticipantRequestFPU`…) portent un `ConferenceEntry*` brut comme `param` ; UAF possible si un événement est en vol pendant `delete entry` (l.381). |
| **M-3** | `mp4player.cpp:11-18` | `~MP4Player` supprime `audioDecoder`/`videoDecoder` dans le corps, avant la destruction du membre `streamer` (dont le worker les utilise). Atténué par le `Stop()` de `DeletePlayer`. |
| **M-4** | `dtls.cpp:287-298` | `DTLSConnection::Reset()` déréférence `ssl` sans test NULL après un `End()` qui fait `ssl=NULL`. |
| **M-5** | `broadcaster.cpp:427-447` | `DeleteBroadcast` : attente `numRef` sans poser `enabled=0` → famine théorique ; pas d'UAF. |
| **M-6** | `rtpsession.h:288` | `RTPSession::listener` brut, jamais désinscriptible (`SetListener(NULL)` inexistant) ; sûr uniquement par ordre de destruction. |

---

## 3. Stratégie générale

### 3.1 Principes

1. **`shared_ptr` pour la propriété partagée entre threads** : les maps propriétaires
   (`conferences`, `participants`, `players`, `sessions` JSR309, `bridges`) stockent des
   `shared_ptr` ; les fonctions `Get*Ref` retournent des **copies de `shared_ptr`** au lieu de
   pointeurs bruts + refcount manuel. L'objet survit tant qu'un handler/thread le tient — c'est
   exactement ce que `numRef`/`Use` tentaient de faire à la main.
2. **`weak_ptr` pour les observateurs** : listeners (`RTPSession::Listener`,
   `RTMPMediaStream::Listener`), back-pointers (`RecorderTimer→MediaSession`,
   `NetStream::part`, `RTMPParticipant::attached`), sessions partagées
   (`VideoStream::rtpSession` slides). Un `weak_ptr::lock()` au site d'appel remplace le pari
   « l'objet est encore vivant ».
3. **`unique_ptr` pour la propriété exclusive** : membres possédés sans partage
   (`MultiConf::recorder`, `PublisherInfo::stream/conn`, `RTPParticipant::video[i]`, décodeurs de
   `MP4Player`).
4. **`shared_ptr` ne remplace pas les mutex** : il garantit que l'objet *existe*, pas que ses
   maps internes sont cohérentes. Les mutex manquants (MediaSession, `players`, `publishers`)
   doivent être ajoutés dans la même passe.
5. **Le flag `enabled` reste nécessaire** sous la forme d'un état « closed/ended » : un
   `shared_ptr` garde l'objet vivant mais il ne faut plus distribuer de nouvelles refs sur un
   objet en cours de fermeture, et `End()` doit rester idempotent.
6. **Destruction ≠ arrêt des threads** : les destructeurs doivent appeler `End()` (idempotent,
   joins inclus) — un `shared_ptr` qui détruit un objet aux threads vivants crashe pareil.
   Corriger le no-join de `VideoStream` (C-4) est un préalable, pas une conséquence.

### 3.2 Contraintes

- Build `gnu++17` : `shared_ptr`/`weak_ptr`/`enable_shared_from_this` disponibles sans réserve.
- Migration **incrémentale par sous-système**, build vert à chaque étape (pas de big-bang sur
  `use.h` : la classe `Use` reste en place là où elle fonctionne, on la retire au fil de l'eau).
- Pas de suite de tests automatisée : chaque phase doit être validée par un scénario manuel de
  charge (création/destruction en boucle, cf. §5).
- Attention à l'ABI interne mcu↔libmedikit : les pipes sont passés à des classes de libmedikit ?
  Non — les pipes restent dans `mcu/`, mais vérifier tout header partagé avant de changer une
  signature.

---

## 4. Plan d'implémentation par phases (ordre de criticité)

### Phase 0 — Corrections ponctuelles indépendantes des smart pointers
*Petites, à faible risque, elles suppriment des crashs immédiats et assainissent le terrain.
Chacune peut être un commit isolé.*

| Étape | Correction | Fichiers | Etat |
|-------|-----------|----------|------|
| 0.1 | **C-10** : dans `GetPacket(DWORD& ssrc)`, garder `streamUse` jusqu'après `s->Wait()` (aligner sur la surcharge sans ssrc). | `rtpsession.cpp:1954-1962` | **Fait, vérifié** — pas d'interblocage possible : `DeleteStreams` fait `Cancel()` sous `IncUse` avant son `WaitUnusedAndLock`, ce qui débloque le `Wait()`. |
| 0.2 | **C-9** : `DeleteMosaic` passe de `IncUse` à `WaitUnusedAndLock` (comme `DeleteMixer`). | `videomixer.cpp:1274-1311` | **Fait, vérifié** — complété : le `mosaics.find()` est maintenant fait **sous** le verrou (comme `SetCompositionType`), sinon itérateur invalidé / double delete entre deux appels concurrents. |
| 0.3 | **C-11** : casser la réentrance `End()→DestroyParticipant→IncUse` (extraire les participants de la map sous lock, les détruire hors lock). | `multiconf.cpp:815-845, 707-760` | **Fait** — `End()` fait un `swap` de la map sous verrou puis détruit hors verrou. Bonus : `DeleteParticipant` re-cherche l'entrée par id après la fenêtre déverrouillée au lieu de réutiliser l'itérateur, ce qui couvre aussi **H-8** (double destruction concurrente = no-op). |
| 0.4 | **C-12** : encadrer les 4 fonctions DocSharing par `participantsLock.IncUse()/DecUse()` comme leurs voisines. | `multiconf.cpp:1088-1230` | **Fait** — Accept/Refuse/StopDocSharing et SetDocSharingMosaic (les `IncUse` imbriqués des boucles internes restent valides, c'est un compteur). |
| 0.5 | **H-4** : déplacer `swr_free` dans la section mutex de `PipeAudioInput::End` ; auditer les autres pipes. | `pipeaudioinput.cpp`, `pipeaudiooutput.cpp` | **Fait** — le défaut était plus large : `PutSamples` (input) et `PlayBuffer` (output) utilisaient `swr` **avant** de prendre le mutex, en course avec le `swr_free` de `StartRecording`/`StartPlaying`/`End`. Verrou remonté en tête des deux fonctions + `swr_free` de `End()` sous mutex. Pipes vidéo/texte audités : corrects (les `free` sont sous leurs mutex). |
| 0.6 | **H-7** : corriger `GetOutput`/`GetInput` du VideoMixer (retourner la variable calculée sous garde, tester `end()`). | `videomixer.cpp:1019-1065` | **Fait, vérifié** — `GetOutput` retourne `output` ; `GetInput` était déjà correct. |
| 0.7 | **M-4** : garde NULL dans `DTLSConnection::Reset`. | `dtls.cpp:287-298` | **Fait, vérifié**. |
| 0.8 | **C-4 (mitigation)** : dans `VideoStream::StopSending/StopReceiving`, remplacer l'abandon silencieux par un join inconditionnel après le signalement (quitte à allonger le timeout), et tracer l'anomalie. Sans cela, aucune gestion de durée de vie n'est fiable. | `videostream.cpp:393-480` | **Fait** — après les 10 tentatives (cancel + signal), trace `Error` puis `pthread_join` inconditionnel : un blocage diagnosticable vaut mieux qu'un use-after-free des pipes. Les threads posent bien `TaskIdle` en sortant (l.714, l.972). |
| 0.9 | *(ajouté)* `RecorderTimer` réécrit en `std::thread` : correction de la version initiale — `wait_for` **avec prédicat** (`cancelled \|\| stopClaimed`), flag `cancelled` posé sous mutex dans le destructeur (sinon réveil perdu → `join()` bloqué toute la durée max), et re-vérification du claim au timeout (sinon double `Close()` + double événement si `RecorderStop` passe en même temps). | `jsr309/MediaSession.{h,cpp}` (RecorderTimer) | **Fait** — sémantique one-shot de `ClaimStop` préservée. |
| 0.10 | **C-13 (correctif ciblé)** : notification retour à la destruction de la source. Ajout d'un `virtual void onJoinableEnded(Joinable*)` no-op sur `Joinable::Listener` ; `~RTPMultiplexer` notifie chaque listener (`(*it)->onJoinableEnded(this)`) **avant** `listeners.clear()` ; `RTPEndpoint` et `WSEndpoint` (les deux `Port` qui sont `Listener`) remettent `Endpoint::Port::joined = NULL` si le pointeur correspond. Le `~Port → Detach()` ultérieur voit alors `joined==NULL` → plus de deref sur l'objet libéré. Sémantique = expiration d'un `weak_ptr` faite à la main ; ne touche pas la propriété du graphe (réservé Phase 4). | `jsr309/Joinable.h`, `RTPMultiplexer.cpp`, `RTPEndpoint.{h,cpp}`, `WSEndpoint.{h,cpp}` | **Fait, vérifié** — build vert (`./install.ksh localcompile`, 2026-07-08) + scénario reproduit (play fichier inexistant + endpoints attachés puis teardown) : plus de « pure virtual method called ». |

**Livrable** : build vert ✅ (2026-07-06, `make -f Makefile.rpm mcu`) + test manuel conférence
création/destruction en boucle (à dérouler). Ces correctifs suppriment déjà les crashs les plus
reproductibles.

### Phase 1 — JSR309 : `MediaSession` et `JSR309Manager` (zone la plus dangereuse) — **FAITE (2026-07-06, build vert)**
*Priorité maximale : c'est la couche pilotée par elixip (cf. extension API en cours), elle n'a
aucun mutex, et 4 fenêtres CRITIQUES s'y concentrent (C-1, C-2, C-3, H-2).*

1. **Mutex interne à `MediaSession`** protégeant `endpoints`, `players`, `recorders`,
   `eventContexts`, `maxEventContextId` (`std::mutex` + `std::lock_guard`, style moderne déjà
   visé par le chantier std::thread). — **Fait** : toutes les méthodes publiques verrouillent ;
   règle clé : les destructions qui joignent des threads (`RecorderTimer`, `Player`, `Endpoint`,
   mixers, transcodeurs) se font **hors verrou** (extraction sous lock, destruction après),
   sinon deadlock avec `onRecorderMaxDuration`/`onEndOfFile` bloqués sur ce même mutex.
2. **Maps en `shared_ptr`** — **Fait** : `endpoints`, `players`, `recorders`, `audioTranscoders`,
   `eventContexts` en `shared_ptr` (ils s'échappent via getters/threads) ; `audioMixers`,
   `videoMixers`, `videoTranscoders` restent des pointeurs bruts strictement internes.
   `GetEndpoint/GetPlayer/GetEventContext/GetAudioTranscoder` rendent des copies de
   `shared_ptr` — C-2 corrigé. `recorderTimers` en `unique_ptr`.
3. **`JSR309Manager::sessions`** — **Fait** : `map<int, MediaSessionEntry{shared_ptr<MediaSession>}>`,
   `GetMediaSessionRef(id, shared_ptr&)`, `ReleaseMediaSessionRef` supprimé (les ~75 handlers de
   `xmlrpcjsr309.cpp` adaptés mécaniquement, RAII). `DeleteMediaSession` = extraction de la map
   sous lock (plus aucune nouvelle ref possible) + `End()` hors lock ; le dernier `shared_ptr`
   détruit. Plus de `Use`/`WaitUnusedAndLock`. `End()` de la session est idempotent (le
   destructeur le rappelle), donc les refs encore en vol restent sûres.
4. **`PostEvent`** (C-3) — **Fait**, scindé en deux chemins pour éviter l'inversion de verrous :
   `JSR309Manager::PostEvent(sessionId, ctxId, ev)` (appelants externes : `Joinable`/threads RTP ;
   résout le contexte via le mutex de session) et `JSR309Manager::DeliverEvent(sessionId, ev)`
   (événement déjà rempli ; ne touche que l'état manager, appelable sous le mutex de session —
   utilisé par `MediaSession::PostEvent` interne). `tag`/`queueId` copiés sous lock ; plus aucun
   accès à l'entrée après unlock ; l'événement est libéré sur tous les chemins d'erreur.
5. **`RecorderTimer`** (H-2) — **Fait** : `weak_ptr<MediaSession>` + `enable_shared_from_this`
   (sessions créées par `make_shared`) ; `lock()` au déclenchement.

Bonus : `PlayerDelete`/`RecorderDelete` libèrent maintenant leur contexte d'événement
(`playerEventCtx`/`recorderEventCtx` + `eventContexts`) ; `MediaSession::End()` nettoie aussi
les mixers/transcodeurs (fuites) ; ordre de verrous global : mutex session → mutex manager,
jamais l'inverse.

**Livrable** : build vert ✅ (2026-07-06, `make -f Makefile.rpm mcu`). Reste à dérouler : scénario
JSR309 de charge (création session/endpoints/players/recorders en parallèle de destructions,
scénarios elixip).

### Phase 2 — Conférences : `MCU` et cycle de vie de `MultiConf` — **FAITE (2026-07-09, build vert)**

*Même patron que la Phase 1 (`JSR309Manager`) : `Conferences` = `map<int,ConferenceEntry>` avec
`ConferenceEntry{int queueId; shared_ptr<MultiConf> conf;}` (valeur, pas pointeur).*

1. **Fait** : `GetConferenceRef(int id, shared_ptr<MultiConf>&)` (même style que
   `GetMediaSessionRef`) ; `ReleaseConferenceRef` **supprimé**. `CreateConference` utilise
   `make_shared<MultiConf>`. `DeleteConference` extrait le `shared_ptr` sous lock puis appelle
   `conf->End()` hors lock — **suppression du polling `sleep(2)`/4 essais et de la fuite
   volontaire** (l.297-329,348 de l'ancien code). `MCU::End()` (destructeur) simplifié pareil.
2. **M-2 fait** : suppression complète du `void* param` de `MultiConf::Listener`
   (`SetListener`, `onParticipantRequestFPU/DocSharing`) — un seul émetteur/récepteur (`MCU`).
   Les callbacks résolvent `queueId`/`id` par lookup `tags`→`conferences` sous le mutex de
   `MCU`, comme `JSR309Manager::PostEvent` résout son contexte sous son propre mutex ; si
   l'entrée n'est plus dans la map (conférence en cours de destruction), l'événement est
   simplement abandonné.
3. **H-1 fait** : `RTMPApplication::Connect` (et `RTMPConnection::Listener::OnConnect`,
   `RTMPConnection::app`) renvoie désormais `shared_ptr<RTMPNetConnection>` au lieu d'un
   pointeur brut. `MCU::Connect` renvoie le vrai `shared_ptr<MultiConf>` sorti de la map — le
   `shared_ptr` que détient `RTMPConnection::app` maintient la conférence vivante pendant toute
   la durée de la session RTMP (`CreateStream`/`DeleteStream`/`Disconnect`), corrigeant le UAF
   potentiel. `MediaGateway::Connect` et `Broadcaster::Connect` (mêmes défauts cousins, C-8/M-5,
   non corrigés ici) enveloppent leur pointeur existant dans un `shared_ptr` à **deleter no-op**
   — adapte juste la signature, gestion de vie inchangée, réservée à la Phase 5.
4. Mécanique : `xmlrpcmcu.cpp` (57 sites) et `mcustatushandler.cpp` (2 sites) adaptés comme
   `xmlrpcjsr309.cpp` en Phase 1 (suppression de `ReleaseConferenceRef`, RAII). `rabbitmqmcu.cpp`
   (moteli, 7 Get/24 Release, pattern non uniforme) adapté en best effort mais **non compilé** —
   `MOTELI` n'est activé par aucun chemin de `install.ksh` actuel.

**Livrable** : build vert ✅ (2026-07-09, `./install.ksh localcompile`). Reste à dérouler : boucle
create/delete conference sous trafic XML-RPC concurrent, et scénario RTMP
(connect → `DeleteConference` concurrent → vérifier l'absence de crash grâce au `shared_ptr`
tenu par `RTMPConnection::app`).

### Phase 3 — Participants, players, tokens (cœur de `MultiConf`) — **FAITE (2026-07-09, build vert)**

*Revue de code effectuée le 2026-07-09 sur le commit `34536c2` (« plan smart pointers - début de
la phase 3 - manuel ») + les modifications non commitées à ce moment sur `multiconf.cpp`.*

1. **Fait** : `participants` est passé à `map<int, ParticipantPtr>` (`ParticipantPtr =
   shared_ptr<Participant>`, typedef dans `participant.h`) ; `GetParticipant(id)` et
   `GetParticipant(id, type)` rendent des `shared_ptr` (copies) au lieu de pointeurs bruts.
   Nouvel helper `GetRTPParticipant(id)` (`dynamic_pointer_cast<RTPParticipant>`) qui remplace
   proprement les `(RTPParticipant*)GetParticipant(...)` C-style éparpillés dans `multiconf.cpp`
   (SetLocalCryptoSDES, StartSending/StopSending, StartReceiving/StopReceiving, recording
   participant, etc.) — bonne clarification de type, cohérent avec le principe §3.1.1.
   `SharedDocMixer::part` (bonus, hors périmètre initial de la phase) est aussi passé en
   `weak_ptr<Participant>`, avec `.lock()` correct à chaque site d'usage et l'équilibre
   `IncUse`/`DecUse` de `StopSharing()` préservé.
   **Cast non sécurisé de `SetDocSharingMosaic` corrigé (2026-07-09)** : les deux boucles
   (`multiconf.cpp`, ~l.1193/1227) faisaient `static_pointer_cast<RTPParticipant>(it->second)`
   sur **tous** les participants de la map sans vérifier `GetType()`, y compris les
   `RTMPParticipant` (le `// TODO` marquait déjà le défaut). Remplacé par
   `std::dynamic_pointer_cast<RTPParticipant>(it->second)` avec `continue` si nul — même
   pattern que `GetRTPParticipant()` (qui fait déjà ce dynamic_pointer_cast en interne après
   vérification de `GetType()`). Bonus : la variable `part` du haut de fonction est repassée en
   `RTPParticipantPtr` (elle avait été retypée en `ParticipantPtr` dans une tentative de
   correctif intermédiaire qui ne compilait pas — `Participant` n'a pas de
   `StartSending/StopSending(MediaFrame::Type, MediaFrame::MediaRole)`, ce sont des méthodes
   propres à `RTPParticipant`). Build vérifié vert.
2. **C-6 fait (2026-07-09)** : `NetStream::part` (`multiconf.h`) est passé de `RTMPParticipant*`
   à `std::weak_ptr<RTMPParticipant>`, avec `.lock()` à chaque site d'usage (`doPlay` — pour
   l'`Attach()` initial —, `doPause`, `doResume`, `doCommand`/`onCongestion`, `Close()`).
   `ConsumeParticipantOutputToken` rend désormais un `std::weak_ptr<RTMPParticipant>` (au lieu
   d'un `RTMPParticipant*` extrait via `part.get()`), et la recherche dans `participants` y est
   protégée par `participantsLock.IncUse()/DecUse()` (elle ne l'était pas du tout avant — bug de
   plus, corrigé au passage). Le type est vérifié par `dynamic_pointer_cast<RTMPParticipant>`
   (remplace le test `GetType()!=Participant::RTMP` + cast C-style). `ConsumeParticipantInputToken`
   reçoit le même traitement pour la recherche protégée + `dynamic_pointer_cast`, mais **garde
   volontairement** son retour en pointeur brut (`RTMPMediaStream::Listener*`) : il est consommé
   de façon synchrone par `NetStream::doPublish` (`AddMediaListener`), et la durée de vie une fois
   enregistré comme listener relève du mécanisme générique `RTMPMediaStream::listeners`
   (déjà correct, cf. §1.4) — le passage de ce mécanisme générique en `weak_ptr` est le chantier
   H-6/M-6, explicitement réservé à la Phase 4, pas repris ici.
3. **C-7 fait (2026-07-09)** : `players` → `map<int, unique_ptr<MP4Player>>` (ownership exclusif,
   jamais partagé hors de `MultiConf` → `unique_ptr` plutôt que `shared_ptr`, plus simple), avec
   nouveau verrou dédié `playersLock` (`Use`, même style que `participantsLock`) :
   `WaitUnusedAndLock`/`Unlock` autour des insertions/suppressions (`CreatePlayer`,
   `DeletePlayer`), `IncUse`/`DecUse` autour des lectures (`StartPlaying`, `StopPlaying`).
   `DeletePlayer` extrait le `unique_ptr` sous verrou puis fait `Stop()`/mixers/`End()` **hors**
   verrou (même principe que `DestroyParticipant`, Phase 1) — plus de `delete` manuel, RAII.
   Le nettoyage de `MultiConf::End()` (boucle `while(players.size()>0)`) lisait la map sans aucune
   protection ; remplacé par un instantané protégé (`playersLock.IncUse()/DecUse()` pour lire un
   id valide à chaque itération, `DeletePlayer` refait sa propre recherche/lock).
4. **H-8** : déjà traité en Phase 0.3 (`DeleteParticipant` re-cherche par id, itérateur non
   réutilisé) ; le passage en `shared_ptr` de cette étape ne change pas le raisonnement, juste le
   type stocké — RAS.
5. **C-5 — pas encore retiré (correctement, c'est volontaire)** : `Participant::use` reste en
   place — `use.IncUse/DecUse` est toujours utilisé massivement dans `rtmpparticipant.cpp`
   (~8 sites, autour du traitement des frames), et rien d'autre ne garantit aujourd'hui que ces
   accès ont cessé avant la destruction des mixers (H-6, non traité).
   **Bug corrigé le 2026-07-09** : le commit `34536c2` avait supprimé l'appel
   `Participant::DestroyParticipant(part)` (qui faisait `part->use.Unlock()` puis `delete part`)
   mais avait oublié le `Unlock()` — `MultiConf::DestroyParticipant` acquérait
   `part->use.WaitUnusedAndLock(2000)` sans jamais le relâcher avant que le dernier `shared_ptr`
   ne détruise l'objet (`~Use` sur un mutex encore verrouillé → UB POSIX). La modification
   intermédiaire (non commitée à l'époque) qui retirait la garde entièrement au lieu de corriger
   l'oubli a été abandonnée : elle supprimait la seule barrière de synchronisation contre un
   `RTMPParticipant::onMediaFrame` concurrent. Correctif retenu : garder la garde
   `WaitUnusedAndLock(2000)` et ajouter le `part->use.Unlock()` manquant juste après, avant la
   suppression des mixers (`multiconf.cpp`, `DestroyParticipant`). Build vérifié vert.
   Le retrait définitif de `Participant::use` (objectif final de C-5) reste réservé à après le
   traitement de H-6 (protection structurelle de `onMediaFrame`, phase 4).
6. **M-1 fait (2026-07-09)** : `PublisherInfo::conn`/`stream` (`RTMPClientConnection*` /
   `RTMPClientConnection::NetStream*`) passent en `unique_ptr` — ownership exclusif, jamais
   partagé hors de `MultiConf`. Nouveau verrou dédié `publishersLock` (`Use`) sur toutes les
   fonctions touchant `publishers` (`StartPublishing`, `StopPublishing`, `StopBroadcaster`,
   et les callbacks `RTMPClientConnection::Listener` `onConnected`/`onNetStreamCreated`/
   `onDisconnected`, jusque-là **totalement non protégés** — le `//TODO: should we lock? I
   expect so` de `onDisconnected` est résolu) : `WaitUnusedAndLock`/`Unlock` pour les
   insertions/suppression (`StartPublishing`, `StopBroadcaster`, `onDisconnected` qui fait
   l'`erase`), `IncUse`/`DecUse` pour les lectures/mutations en place (`StopPublishing`,
   `onConnected`, `onNetStreamCreated`). Les `PublisherInfo info = it->second;` (copies, devenues
   impossibles avec des membres `unique_ptr`) sont remplacées par des références
   (`PublisherInfo&`, déjà le style utilisé par `onConnected`/`onNetStreamCreated`). Plus de
   `delete` manuel : `stream`/`conn` sont détruits par leur `unique_ptr` à l'`erase()`. Le
   comportement fonctionnel de `StopPublishing` (qui ne fait pas l'`erase`, laissé à
   `onDisconnected`) et le test `if (inited) return;` d'`onDisconnected` (qui semble inversé par
   rapport à l'intention du commentaire) sont **inchangés** — bug logique préexistant signalé
   mais volontairement non corrigé ici (hors périmètre smart pointers).
7. **`MultiConf::recorder` fait (2026-07-09)** : `RecorderControl*` → `std::unique_ptr<RecorderControl>`
   (ownership exclusif, jamais partagé). `StartRecordingBroadcaster`/`StopRecordingBroadcaster`
   utilisent `std::make_unique<FLVRecorder>()`/`std::make_unique<MP4Recorder>()` et
   `static_cast<FLVRecorder*>(recorder.get())`/`static_cast<MP4Recorder*>(recorder.get())` pour
   les `switch(recorder->GetType())` (cast toujours sûr : le type réel correspond à celui créé).
   **Bug latent corrigé au passage** : si `recorder->Record()` échouait, le code faisait
   `return 0;` directement au lieu de sauter au label `start_recording_failed`, fuitant le
   recorder et laissant `recorder` non-nul (bloquant tout `StartRecordingBroadcaster` futur) —
   remplacé par un `goto start_recording_failed;` cohérent avec le cas d'échec de `Create()`
   juste au-dessus. `recorder.reset()` remplace les `delete recorder; recorder = NULL;` manuels ;
   le destructeur de `MultiConf` n'a plus besoin de gérer `recorder` explicitement (RAII).

**État du build** : vert (`./install.ksh localcompile`, 2026-07-09) après chaque sous-étape
(cast `SetDocSharingMosaic`, `recorder`, `players`, `publishers`, `C-6`).

**Livrable** : build vert. Reste à dérouler manuellement (aucune suite de tests automatisée) :
scénario join/leave massif de participants RTP et RTMP pendant lecture/enregistrement MP4,
create/delete player et publisher RTMP en boucle, publish/unpublish RTMP concurrent avec
déconnexions réseau (pour exercer `onDisconnected`/M-1), et play RTMP `participant/<token>`
concurrent avec `DeleteParticipant` (pour exercer le `weak_ptr` de C-6).

### Phase 4 — Streams, pipes et listeners (couche transport) — **POINTS 1-6 FAITS (2026-07-10, build vert) ; POINT 7 (C-13) RÉSERVÉ**

*Points 1 à 6 implémentés le 2026-07-10, build vert (`./install.ksh localcompile`) après chaque
étape. Ordre suivi : H-5 → M-3 → Point 1 (pipes) → H-3 → H-6 → M-6. Le point 7 (C-13 structurel)
est explicitement reporté à une session dédiée : l'exploration a montré qu'il est bien plus large
que ce que ce texte supposait (voir la note « écarts » en fin de point 7).*

1. **Pipes — FAIT.** `AudioSource`/`VideoSource`/`TextSource`(+`TextPrivate`) des 3 mixers stockent
   `shared_ptr<Pipe*put>` (`make_shared` dans `CreateMixer`, plus de `delete` du pipe dans
   `DeleteMixer`/`End`/`EndMixer`). Nouveaux accesseurs `GetSharedInput`/`GetSharedOutput` (copies
   de `shared_ptr` sous `IncUse`) ; `GetInput`/`GetOutput` bruts conservés (`.get()`) pour les
   consommateurs internes de `MultiConf`. Streams participants (`AudioStream`/`VideoStream`/
   `TextStream` **et** membres directs de `RTMPParticipant`) détiennent une copie `shared_ptr` du
   pipe → C-4 structurellement impossible. **Pièges traités** : (a) `RTMPParticipant` a le même
   défaut que `RTPParticipant` (traité aussi) ; (b) `SharedDocMixer` passe un pointeur NON possédé
   à `SetVideoOutput` → **double surcharge** Video (brute non-possédante à deleter no-op /
   `shared_ptr` possédante), jamais unifiée ; (c) `TextStream::Init` partagé avec
   `MediaBridgeSession` → surcharge `shared_ptr` ajoutée à côté de la brute. `SetAudioInput/Output`,
   `SetTextInput/Output` passés directement en `shared_ptr` (seuls les mixers les alimentent).
   Consommateurs internes (`audioEncoder`/`flvEncoder`/`recEncoder`/`appMixer`/`sharedDocMixer`/
   `player`) **volontairement non convertis** (leur sûreté repose sur l'ordre `Stop*/End*` avant
   `Delete*`, inchangé) — risque résiduel documenté. `PipeVideoOutput` reste couplé au mutex/cond du
   `VideoMixer` (documenté, hors périmètre).
2. **H-3 — FAIT.** (a) `RTPParticipant::End()` boucle désormais **SLIDES avant MAIN** (index
   décroissant) : le vrai bug était l'ordre (fermeture des sockets de MAIN pendant que le
   `recVideoThread` de SLIDES lit, fd reuse race), pas seulement la durée de vie. (b) Défense en
   profondeur : `VideoStream::rtpSession` → `std::weak_ptr<RTPSession>`, `.lock()` à chaque site
   (End, StopReceiving, RecVideo re-lock **par itération** avec repli non-possédant sur sa propre
   session, GetStatistics). `RTPParticipant : enable_shared_from_this` ; liaison par défaut de
   chaque flux sur sa propre session dans `Init()` (pas le constructeur), SLIDES→MAIN via
   `onNewStream`. `RTPSession::End()` déjà idempotent.
3. **H-5 — FAIT.** `~RTPParticipant`, `~AudioStream`, `~VideoStream`, `~TextStream` appellent
   `End()` (idempotent) en tête (modèle `~RTMPParticipant`).
4. **M-6 — FAIT.** `RTPSession` gagne `SetWeakListener(weak_ptr<Listener>)` + `LockListener()`
   (coexistence : `weakListener.lock()` si armé, sinon wrapper non-possédant du `listener` brut
   historique — préserve `MediaBridgeSession`, non converti). Les ~10 sites d'appel passent par
   `if (auto l = LockListener()) l->onX(...)`. `VideoStream`/`AudioStream` gagnent `SetWeakListener`
   (délègue à `rtp`). Liaison dans `RTPParticipant::Init()` avec **`static_pointer_cast` obligatoire**
   (diamant non virtuel `RTPSession::Listener` via `VideoStream::Listener` **et**
   `AudioStream::Listener`). `text` : listener volontairement NULL, inchangé.
5. **H-6 — FAIT.** `onMediaFrame` encadré par `use.IncUse()/DecUse()` (pour que le
   `WaitUnusedAndLock(2000)` de `DestroyParticipant` voie l'appel du thread réseau source).
   `attached` protégé par un `pthread_mutex_t attachedMutex` dédié (extraction sous verrou,
   `RemoveMediaListener` appelé hors verrou) — **pas** de `weak_ptr` ici (les sources RTMP ne sont
   pas `shared_ptr`-gérées ; chantier C-8/Phase 5).
6. **M-3 — FAIT.** `MP4Player::audioDecoder`/`videoDecoder` → `unique_ptr` ; `~MP4Player()` appelle
   `Stop()` (arrête/joint le worker `streamer`) avant que les `unique_ptr` ne se détruisent.
7. **C-13 (solution structurelle) — LIEN A FAIT (2026-07-10, build vert), lien B toujours réservé.** le graphe d'attach JSR309 (`Endpoint::Port::joined` ↔
   `RTPMultiplexer::listeners`) passe en observateurs `weak_ptr`, remplaçant le correctif manuel
   `onJoinableEnded` de la phase 0.10. Les deux liens sont non-possédants → `weak_ptr` des deux
   côtés, avec `lock()` au site d'appel (protège aussi contre une destruction concurrente, ce que
   la notification manuelle ne fait pas). **Pré-requis** : les cibles de `GetJoinable` doivent être
   sous gestion `shared_ptr`. Les `RTPMultiplexer` du `Player` sont des **membres par valeur** → il
   faut un **aliasing `shared_ptr`** (`shared_ptr<Joinable>(player_sp, &audio)`) exposé par
   `GetJoinable`, `Player : enable_shared_from_this` (déjà `make_shared`). À faire **pour toutes les
   sources** de `GetJoinable` sinon la protection est partielle : `Player`, `AudioMixer`/`VideoMixer`
   (ports), `VideoTranscoder`, et endpoint→endpoint. Le sens `listeners` devient `weak_ptr` seulement
   une fois les `Port` gérés par `shared_ptr` (aujourd'hui `new/delete` bruts dans `Endpoint`) — donc
   dépend de la conversion des `Endpoint`/`Port`.

   **Écarts découverts (exploration 2026-07-10), à intégrer avant d'implémenter C-13** :
   `Endpoint::Port` est **déjà** `shared_ptr` (commit `39b759a`, le texte « new/delete bruts » est
   obsolète). Mais `AudioMixerResource`/`VideoMixerResource`/`VideoTranscoder` sont **encore des
   pointeurs bruts** dans `MediaSession` (seuls `Player`, `Endpoint`, `Recorder`, `AudioTranscoder`
   sont déjà `shared_ptr`) ; leurs `Port` internes (`AudioMixerResource::Port`, distincts de
   `Endpoint::Port`) sont aussi `new/delete` bruts. `RTPMultiplexer::listeners` est un
   `std::set<Listener*>` (pas une liste → comparateur `owner_before` ou changement de conteneur pour
   passer en `weak_ptr`). `Player` a **3** multiplexeurs (audio/vidéo/**texte**). Surtout : le
   correctif Phase 0.10 `onJoinableEnded` **ne couvre que `RTPEndpoint`/`WSEndpoint`** — `Recorder`,
   `AudioDecoderJoinableWorker`, `VideoDecoderJoinableWorker` ont le même `joined` brut non protégé
   (mêmes crashs « pure virtual method called » possibles par ces chemins). `AudioTranscoder` doit
   être ajouté à la liste des sources.

   **INCREMENT SÛR FAIT (2026-07-10, build vert) — solution structurelle toujours réservée.**
   Décision (maintainer) : implémenter la « correction minimale » plutôt que la conversion
   structurelle `weak_ptr` (refactor de propriété ~15 fichiers temps réel, non validable sans
   tests de charge). Fait :
   - `onJoinableEnded` étendu à `AudioDecoderJoinableWorker`, `VideoDecoderJoinableWorker`
     (nulle `joined`) et `Recorder` (retire de la `JoinedMap` toutes les entrées pointant sur la
     source détruite). Même sémantique lock-free que `RTPEndpoint::onJoinableEnded`. Ferme le
     **sens avant** (source détruite d'abord → `joined`/entrée pendouillante → `Dettach` crashe)
     pour ces classes — donc aussi pour `AudioTranscoder`/`VideoTranscoder`, qui délèguent leur
     rôle listener à leur membre `decoder` (et leur rôle source à leur membre `encoder`, un
     `RTPMultiplexer(Smoother)` dont le `~` notifie déjà). Toutes les sources de `GetJoinable`
     sont bien des `RTPMultiplexer`-dérivés (Endpoint::Port, multiplexeurs du Player, encoders
     des mixer resources/transcoders), donc `~RTPMultiplexer` couvre la notification.
   - **Sens inverse pour `Recorder`** : `~Recorder` ne faisait **aucun** `Dettach` et
     `RecorderDelete` ne détache pas avant de libérer → une source encore attachée gardait un
     `Recorder*` pendouillant. Corrigé : `~Recorder` fait `RemoveListener(this)` sur chaque source
     encore dans la map (les sources déjà mortes ont été retirées par `onJoinableEnded`, donc les
     restantes sont vivantes). Les workers, eux, se nettoient déjà via `~ → End() → Dettach() →
     RemoveListener` (sens inverse normal OK).

   **LIEN A IMPLÉMENTÉ (2026-07-10, build vert `./install.ksh localcompile`) — solution structurelle
   du sens avant.** Le lien retour `joined` (listener → source) passe en `weak_ptr<Joinable>` ; tous
   les `GetJoinable` rendent un `shared_ptr<Joinable>`. Ceci **remplace** le correctif manuel
   `onJoinableEnded` de la phase 0.10 (supprimé : boucle de notification dans `~RTPMultiplexer`,
   surcharges `onJoinableEnded` de RTPEndpoint/WSEndpoint/Recorder/workers, et la déclaration de base
   `Joinable::Listener::onJoinableEnded`). Détail :
   - **Sources → `shared_ptr<Joinable>`** : `Endpoint::GetJoinable` rend `static_pointer_cast` du
     `shared_ptr<Port>` (Port déjà `shared_ptr`) ; `Player : enable_shared_from_this`, GetJoinable
     rend un **aliasing** `shared_ptr<Joinable>(shared_from_this(), &audio/&video/&text)` ;
     `AudioMixerResource`/`VideoMixerResource` : `Ports` map → `map<int,shared_ptr<Port>>`, GetJoinable
     rend un **aliasing** `shared_ptr<Joinable>(port_sp, &port->encoder)` ; `DeletePort`/`End`
     tiennent un `shared_ptr` local (fin des `delete`).
   - **Listeners : `joined` → `weak_ptr<Joinable>`** dans `Endpoint::Port` (+ RTPEndpoint qui utilise
     le membre hérité), `Recorder::JoinedMap` (`map<Type,weak_ptr>`), `Audio/VideoDecoderJoinableWorker`.
     `lock()` à chaque déréf (Detach, FPU `Update`, `SetREMB`, `~Recorder`). Signatures `Attach(...)`
     passées de `Joinable*` à `const shared_ptr<Joinable>&` (Endpoint, Port, RTPEndpoint, Recorder,
     mixers, transcoders, workers) + les 2 sites XML-RPC (`ep->Attach(...,tr)`, `tr->Attach(ep->GetJoinable(...))`).
   - **Mixers/transcoder → `shared_ptr` dans `MediaSession`** (décision « tant qu'on y est ») :
     `AudioMixers`/`VideoMixers`/`VideoTranscoders` maps en `shared_ptr`, `make_shared` à la création,
     `delete` supprimés (extraction sous lock + `End()` hors lock, destruction par le `shared_ptr`
     local — patron Phase 5).
   - **Effet** : quand une source meurt (dernier `shared_ptr` relâché), le `weak_ptr` des listeners
     expire ; leur `Detach` ultérieur `lock()` dans le vide → plus de « pure virtual method called »,
     **et** protection contre une destruction concurrente côté source→listener (lock() atomique).

   **Résidu restant (nécessite le LIEN B — session dédiée)** : le sens **inverse**
   `RTPMultiplexer::listeners` reste un `std::set<Joinable::Listener*>` (pointeurs bruts). Un worker
   qui reçoit `onEndStream` remet son `joined` sans `RemoveListener` ; s'il est ensuite détruit alors
   que la source vit, la source garde un `Listener*` pendouillant (crash au `Multiplex` suivant). Ce
   sens est aujourd'hui couvert par le `Dettach`-à-la-destruction des listeners (`~Recorder`
   `RemoveListener`, workers `~→End→Dettach`, `~Port→Detach`) mais **pas** contre une destruction
   concurrente. Le LIEN B (listeners en conteneur `weak_ptr` + `owner_before`, `lock()` au
   `Multiplex`, aliasing des `decoder` workers membres par valeur) est le remède complet — très
   invasif car les listeners sont des membres par valeur, réservé à une session dédiée.

**Correctif collatéral (2026-07-10)** : le rebuild complet (nécessaire car `mcu/Makefile.rpm` ne
suit pas les dépendances headers → objets périmés) a exposé une régression du paquet système
**ffmpeg** (`libavutil/base64.h` a perdu ses gardes `extern "C"` entre le 2026-07-09 et le
2026-07-10) : `websocketconnection.cpp` (include manquant) et `rtpsession.cpp` (include nu →
symboles C++ mangled à l'édition de liens) corrigés en enveloppant l'include dans `extern "C"`.

**Livrable** : build vert ✅ (2026-07-10, `./install.ksh localcompile`). Reste à dérouler
manuellement (pas de suite de tests) : participant RTP + BFCP + partage document, `DeleteParticipant`
pendant flux SLIDES actif (H-3, idéalement sous `-fsanitize=address`) ; participant RTMP,
`DeleteParticipant` pendant `SendVideo`/`RecVideo` (Point 1) ; source RTMP attachée envoyant en
continu + `DeleteParticipant` (H-6) ; lecture MP4 en boucle + `DeletePlayer` pendant lecture (M-3) ;
vérifier que FPU/bitrate arrivent toujours en marche normale (M-6, pas de régression silencieuse).

### Phase 5 — Sous-systèmes restants et nettoyage — **POINTS 1-3 FAITS + DOC (2026-07-10, build vert)**

1. **C-8 — FAIT.** `MediaGateway::bridges` → `map<DWORD, MediaBridgeEntry{DWORD id; wstring name;
   shared_ptr<MediaBridgeSession> session;}>` (compteurs `numRef`/`enabled` supprimés).
   `GetMediaBridgeRef(id, shared_ptr&)` rend une copie du `shared_ptr` ; `ReleaseMediaBridgeRef`
   **supprimé** (les 19 handlers de `xmlrpcmediagateway.cpp` adaptés mécaniquement, RAII).
   `DeleteMediaBridge` extrait le `shared_ptr` sous lock, `erase`, puis `End()` hors lock — le
   `//TODO: Wait for numref == 0` jamais implémenté est résolu par construction. `CreateMediaBridge`
   passe à `make_shared`. `MediaGateway::Connect` renvoie le vrai `shared_ptr<MediaBridgeSession>`
   (qui **est** un `RTMPNetConnection`), tenu par `RTMPConnection::app` → maintient la session vivante
   pendant toute la connexion RTMP (fin de la mitigation deleter no-op de la Phase 2).
2. **M-5 — FAIT.** `Broadcaster::broadcasts` → `shared_ptr<BroadcastSession>` (mêmes suppressions
   `numRef`/`enabled`) ; `GetBroadcastRef(id, shared_ptr&)` rend une copie ; `ReleaseBroadcastRef`
   **supprimé** (handler `xmlrpcbroadcaster.cpp` + `GetBroadcastPublishedStreams` adaptés).
   `DeleteBroadcast` : **suppression du polling `numRef`/`sleep(20)`**, extraction sous lock + `End()`
   hors lock. `Broadcaster::Connect` : le `conn` renvoyé par `ConnectPublisher`/`ConnectWatcher`
   appartient à la `BroadcastSession` (pas la session elle-même) → **`shared_ptr` aliasing**
   `shared_ptr<RTMPNetConnection>(sess, conn)` : expose `conn` mais partage la propriété de `sess`,
   la gardant vivante pendant la connexion (meilleur que le deleter no-op, qui laissait la session
   mourir sous le conn). Garde `if(!conn) return nullptr;` ajoutée. `pthread_mutex` conservé (pas
   converti en `std::mutex`, hors périmètre).
3. **Mosaïques — FAIT.** `VideoMixer::mosaics` → `map<int, shared_ptr<Mosaic>>` (plus de `delete`
   manuel : `End`/`SetCompositionType`/`DeleteMosaic` s'appuient sur le RAII ; `DeleteMosaic` garde
   un `shared_ptr` local pour détruire le `Mosaic` **hors** verrou comme avant). `defaultMosaic` et
   `VideoSource::mosaic` restent des **observateurs bruts** (leur validité est déjà garantie par la
   discipline `lstVideosUse` en écriture qui les remet à jour/NULL avant toute suppression — documenté
   dans le header). `Overlay` → `unique_ptr` : `Mosaic::overlay` (`unique_ptr<Overlay>`) et
   `Mosaic::overlays` (`map<int, unique_ptr<Overlay>>`) ; `MoveOverlays` utilise `std::move` ;
   **bug préexistant corrigé** dans `RemoveParticipant` (`if (ito->second = NULL) delete ito->second;`
   — affectation au lieu de comparaison → fuite ; remplacé par le simple `erase` qui détruit le
   `unique_ptr`).
4. **Nettoyage — DOC FAITE, retrait `use.h` REPORTÉ (volontaire).** Modèle cible documenté dans
   `CLAUDE.md` (section « Memory-ownership model »). Audit : `grep -n "delete " mcu/src | wc -l` = 159
   (beaucoup légitimes : `delete[]`, buffers locaux, sous-systèmes non migrés). `use.h`
   (`Use`/`IncUse`/`WaitUnusedAndLock`) est **encore porteur dans ~20 fichiers** (chemins chauds
   participant/mixer/rtp : `participantsLock`, `playersLock`, `publishersLock`, `lstVideosUse`,
   `streamUse`, `RTMPMediaStream::listeners`, `Participant::use`…) → son retrait reste « au fil de
   l'eau », pas une passe unique. Aucune instance de `Use` n'est devenue morte du fait de la Phase 5
   (Gateway/Broadcaster utilisaient déjà `std::mutex`/`pthread`, pas `Use`).
   **Décision maintainer (2026-07-10) : GARDER `use.h` tel quel.** Il ne reste **aucun `Use` mort** ;
   chaque usage restant fait office de verrou lecteur/écrivain (sémantique `WaitUnusedAndLock` =
   attente de `cont==0`, non récursif — subtilement différente de `std::shared_mutex`). Le remplacer
   par `std::shared_mutex` dans les chemins temps réel introduirait un risque de deadlock/course non
   détectable sans tests de charge. Retrait réservé aux prochains refactors ciblés.

**État du build** : vert ✅ (`./install.ksh localcompile`, 2026-07-10, rebuild complet `rm *.o`).

**Livrable** : build vert. Reste à dérouler manuellement (pas de suite de tests) : create/delete
MediaBridge et Broadcast en boucle sous trafic XML-RPC concurrent ; connexion RTMP `bridge`/
`broadcaster/watcher` + `DeleteMediaBridge`/`DeleteBroadcast` concurrent (vérifier que le `shared_ptr`
tenu par `RTMPConnection::app` évite le crash) ; `CreateMosaic`/`DeleteMosaic`/`SetCompositionType`
en boucle pendant un flux vidéo actif (exercer le RAII des mosaïques et des overlays sous
`lstVideosUse`).

---

## 5. Méthode de validation (pas de suite de tests automatisée)

1. **Build vert** à chaque étape : `make -f mcu/Makefile.rpm mcu` (et `./install.ksh localcompile`
   en fin de phase).
2. **Scénarios de charge manuels** par phase (client XML-RPC en boucle) :
   - create/delete conference × N en parallèle de SetVideoCodec/AddMosaicParticipant ;
   - join/leave participants RTP+RTMP en boucle pendant un StartPlaying/StopPlaying ;
   - JSR309 : create/delete session + endpoints concurrents (rejouer les scénarios elixip) ;
   - suivre `/var/log/mcu.log` et l'empreinte mémoire (les fuites volontaires actuelles doivent
     disparaître).
3. **Outillage** : builds ponctuels avec `-fsanitize=thread` puis `-fsanitize=address` (cible
   locale, hors RPM) pour valider les phases 0/3/4 — le code actuel devrait déjà déclencher TSan
   sur C-1/C-4/C-7, ce qui donne une mesure avant/après.
4. **Rollback** : une branche par phase, merge après validation du scénario de charge.

---

## 6. Estimation et séquencement

| Phase | Contenu | Taille | Risque de régression |
|-------|---------|--------|---------------------|
| 0 | 8 correctifs ponctuels | S (1-2 j) | Faible |
| 1 | JSR309 MediaSession/Manager | M (3-5 j) | Moyen (75 handlers à adapter mécaniquement) |
| 2 | Conférences MCU | M (2-4 j) | Moyen (~40 sites xmlrpcmcu) |
| 3 | Participants/players/tokens | L (4-6 j) | Élevé (cœur du serveur) |
| 4 | Streams/pipes/listeners | L (4-6 j) | Élevé (threads temps réel) |
| 5 | Gateway/broadcaster/nettoyage | M (2-3 j) | Faible |

L'ordre 0 → 1 → 2 → 3 → 4 → 5 est fondé sur : (a) la densité de fenêtres CRITIQUES, (b) le fait
que la phase 0 fiabilise les primitives dont les phases suivantes dépendent, (c) l'activité en
cours sur la couche JSR309 (extension API elixip) qui rend la phase 1 doublement rentable, et
(d) la dépendance de la phase 4 aux `shared_ptr<Participant>` introduits en phase 3.
