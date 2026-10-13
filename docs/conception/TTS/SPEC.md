# Synthèse vocale (TTS)

> Statut : **conception, rien n'est codé**. Le lot 0 décide si Piper convient.
> Décision d'architecture : [ADR 003](../../architecture/adr-003-tts-serveur-websocket.md).
> Prérequis : la branche `feat/wss-client` fusionnée dans la branche de version.
>
> TTS veut dire *text-to-speech* : transformer un texte en voix.
> « Contrôleur » désigne l'appelant qui pilote le serveur en XML-RPC (elixip).

## 1. Objectif

Le serveur transforme du texte en voix, pour deux usages.

**Usage 1 — lire le texte T.140.** Un participant sourd écrit en texte temps
réel. Les autres participants entendent son texte, lu par une voix de synthèse.
Exemple : il tape « Bonjour, je suis en retard. » ; les autres entendent la
phrase dès le point final.

**Usage 2 — les annonces.** Le contrôleur demande de prononcer une phrase.
Exemple : « Vous êtes en attente. », ou « Marie a rejoint la conférence. ».

Les deux usages existent dans les deux API : conférence (`/mcu`) et JSR-309
(`/jsr309`).

## 2. Ce qui existe (inventaire vérifié)

### 2.1 Injecter de l'audio

- `PipeAudioOutput::PlayFrame(SamplesPtr)` (`mcu/src/pipeaudiooutput.cpp`)
  accepte du S16 mono à **n'importe quelle fréquence**, et le rééchantillonne
  vers celle du mixeur. Aucune conversion n'est donc à écrire.
- `Samples` (`libmedikit/medkit/audio.h`) se construit par
  `Samples::FromBuffer(const SWORD*, nb, rate)`.
- Conférence : `MultiConf::CreatePlayer` réserve un port du mixeur par
  `audioMixer.CreateMixer(id)`. Le synthétiseur prend un port de la même façon.
- JSR-309 : `AudioEncoderMultiplexerWorker::EncodeSamples(SamplesPtr)`
  (`mcu/src/jsr309/AudioEncoderWorker.cpp`), en mode PUSH (`Init()` sans
  argument), encode et multiplexe vers les jambes jointes.

### 2.2 Lire le texte T.140

- Conférence : les caractères décodés existent dans `TextMixer::Run`
  (`mcu/src/textmixer.cpp`), qui lit chaque participant par
  `output->ReadText(buffer, size)`, toutes les 200 ms au plus.
- JSR-309 : **aucun composant ne décode le texte**. `RTPEndpoint` le fait
  passer tel quel en RTP. `DCEndpoint::onT140Block` ne construit un
  `TextFrame` que pour le réencoder en RED.

### 2.3 Les événements

- JSR-309 : `MediaSession::PostEvent` publie dans la file du contrôleur.
  `PlayerEndOfFileEvent` (`JSR309Event.h`) est le modèle à suivre.
- Conférence : `MCU` publie par `eventMngr->AddEvent(queueId, event)`. **Il
  n'existe aucun événement de fin de lecture** : `MP4Player::onEnd` ne fait que
  reboucler. L'événement de fin de phrase sera le premier de ce genre.

### 2.4 Le WebSocket

- `WebSocketServer` (`mcu/include/websocketserver.h`) sert en mode serveur. Il
  est mono-thread : un callback qui bloque fige toutes les connexions.
- Le mode client (`WebSocketServer::Connect(url, listener)`, reprise
  automatique) vit sur la branche `feat/wss-client`, pas encore fusionnée.
  Voir [WS-CLIENT](../WS-CLIENT/SPEC.md).

### 2.5 Les paquets

Sur AlmaLinux 9.6, seul `espeak-ng` 1.50 est packagé. **Piper et onnxruntime
ne le sont pas.** Il faut les bâtir depuis les sources.

## 3. Architecture

Deux binaires :

```
mediaserver                                   mediaserver-tts
  Synthesizer ── TtsClient ── WebSocket ──►    TtsSession ── file ── thread de travail ── Piper
     ▲               │     ◄── PCM binaire ──                                              │
     │               ▼                                                                    voix
  texte T.140    PipeAudioOutput / AudioEncoderMultiplexerWorker
  ou SynthesizerSay
```

- **`Synthesizer`** (mediaserver) : un synthétiseur. Il reçoit des phrases,
  joue la voix reçue et publie la fin de chaque phrase.
- **`TtsClient`** (mediaserver) : une connexion WebSocket vers le serveur TTS,
  **une par synthétiseur**. Pas de multiplexage : un `stop` ou une coupure ne
  touche qu'un synthétiseur.
- **`TtsSession`** (mediaserver-tts) : l'état d'une connexion. Il tient une
  file de phrases et en synthétise une à la fois, dans l'ordre.
- **Les threads de travail** (mediaserver-tts) : une réserve de taille fixe
  (`--workers`). Ils appellent Piper. Le réacteur WebSocket ne fait que
  recevoir, mettre en file et envoyer.

## 4. Protocole WebSocket, version 1

Chemin : `/tts`. Les trames texte portent un objet JSON. Les trames binaires
portent du PCM S16LE mono.

| Sens | Message | Rôle |
|---|---|---|
| client → serveur | `{"type":"hello","version":1,"voice":"fr_FR-siwis-medium"}` | Premier message, obligatoire. Choisit la voix. |
| serveur → client | `{"type":"ready","version":1,"voice":"…","rate":22050}` | Accepte. Donne la fréquence du PCM. |
| client → serveur | `{"type":"say","id":7,"text":"Bonjour."}` | Met une phrase en file. `id` est choisi par le client. |
| serveur → client | trames binaires | La voix de la phrase en cours, dans l'ordre. |
| serveur → client | `{"type":"done","id":7}` | La phrase `7` est entièrement envoyée. |
| client → serveur | `{"type":"stop"}` | Vide la file et abandonne la phrase en cours. |
| serveur → client | `{"type":"stopped","ids":[7,8]}` | Les phrases abandonnées. |
| serveur → client | `{"type":"error","id":7,"reason":"…"}` | La phrase `7` a échoué. `id` absent : erreur de connexion. |
| serveur → client | `{"type":"voices","voices":[…]}` | Réponse à `{"type":"voices"}`, possible avant `hello`. |

Règles :

- Une version inconnue, ou une voix inconnue, donne une `error` sans `id`,
  puis la fermeture. Les deux côtés l'écrivent dans leur log.
- Les trames binaires entre deux `done` appartiennent toutes à une seule
  phrase. Le client n'a donc pas besoin de marquer les trames.
- Le client joue la voix à la vitesse réelle. Le serveur peut envoyer plus vite
  que le temps réel : le client garde la voix en tampon.

## 5. Le serveur TTS (`mediaserver-tts`)

- Bâti par `mcu/Makefile`, cible à part. Il réutilise `WebSocketServer`,
  `Worker` et le log du mediaserver. Il ne lie ni ffmpeg, ni les mixeurs.
- Il charge les voix au démarrage, depuis un dossier (`--voices`). Une voix est
  un couple de fichiers Piper : `.onnx` et `.onnx.json`.
- Options : `--port`, `--voices`, `--workers`, `--wss` (mêmes options de
  certificat que le mediaserver).
- Il découpe un texte long en phrases avant la synthèse, pour que la première
  phrase parte vite.
- Unité systemd `mediaserver-tts.service`, sous-paquet RPM `mediaserver-tts`.

## 6. Le client (`mediaserver`)

- Nouvelle option `--tts-url` (exemple : `ws://127.0.0.1:9095/tts`). Sans
  elle, la synthèse est désactivée.
- `TtsClient` reçoit le PCM sur le thread du réacteur WebSocket. Il construit
  un `Samples` et le pousse, **sans attendre** :
  - conférence : dans le `PipeAudioOutput` du port du synthétiseur ;
  - JSR-309 : dans `AudioEncoderMultiplexerWorker::EncodeSamples`.
- **Point à vérifier au lot 3** : `EncodeSamples` prend `encodeLock`. Si ce
  verrou peut être tenu longtemps par un autre thread, il bloque le réacteur
  WebSocket. Le cas échéant, une file entre les deux.
- À la coupure, les phrases en cours sont perdues. Chacune reçoit un événement
  d'échec. La reprise est celle du client WebSocket : toutes les 5 s.

## 7. Usage 1 — le texte T.140 lu à voix haute

### 7.1 Le découpeur de phrases

Le texte T.140 arrive caractère par caractère. Le découpeur accumule les
caractères d'un participant et envoie une phrase au synthétiseur :

- sur une ponctuation de fin : `.`, `?`, `!`, `…` ;
- sur un saut de ligne (U+2028 ou `\n`) ;
- après un silence de frappe (1,5 s par défaut, à mesurer en recette).

Il applique le retour arrière (U+0008) **avant** d'envoyer : un caractère
effacé n'est jamais lu. Une phrase déjà envoyée ne peut plus être corrigée.

Le découpeur est une classe pure, sans thread. Elle se teste seule.

### 7.2 Conférence

`TextMixer::Run` lit déjà le texte de chaque participant. Pour un participant
dont la lecture est active, il passe aussi ce texte à son découpeur.

**Question ouverte — l'auteur entend-il sa voix ?**

- Option A : un port de mixeur à part. Tout le monde entend la voix, auteur
  compris. Simple.
- Option B : la voix s'ajoute à l'audio du participant. Le mixeur l'exclut de
  son propre mix, et la VAD le montre « en train de parler ». Mais le port du
  participant est déjà alimenté par son audio décodé. Deux producteurs dans un
  même `PipeAudioOutput` s'intercaleraient au lieu de s'additionner : il faut
  un étage de somme.

Recommandation : **B**. Un auteur sourd n'entend pas sa voix de toute façon,
mais un auteur entendant qui tape en serait gêné. Et le participant qui
« parle » par le texte doit apparaître comme orateur dans la mosaïque.

### 7.3 JSR-309

Aucun composant ne décode le texte. Le `Synthesizer` JSR-309 est donc un
Joinable neuf : il se joint à la sortie texte d'un endpoint, décode le T.140
(RED compris), passe le texte au découpeur et émet la voix vers les jambes
jointes à sa sortie audio.

## 8. Usage 2 — les annonces

Le contrôleur crée un synthétiseur, lui envoie des phrases et reçoit un
événement à la fin de chacune. Plusieurs phrases s'enchaînent dans l'ordre.
`Stop` coupe la phrase en cours et vide la file.

## 9. API de contrôle

### 9.1 Conférence (`/mcu`)

| Méthode | Paramètres | Retour |
|---|---|---|
| `CreateSynthesizer` | confId, nom, voix | synthId |
| `DeleteSynthesizer` | confId, synthId | — |
| `SynthesizerSay` | confId, synthId, texte | sayId |
| `SynthesizerStop` | confId, synthId | — |
| `SetParticipantTextToSpeech` | confId, partId, voix (vide = arrêt) | — |

Événements : `SynthesizerSaid` (synthId, sayId), `SynthesizerFailed`
(synthId, sayId, raison).

### 9.2 JSR-309 (`/jsr309`)

| Méthode | Paramètres | Retour |
|---|---|---|
| `SynthesizerCreate` | sessionId, nom, voix | synthId |
| `SynthesizerDelete` | sessionId, synthId | — |
| `SynthesizerSay` | sessionId, synthId, texte | sayId |
| `SynthesizerStop` | sessionId, synthId | — |
| `SynthesizerAttachToText` | sessionId, synthId, joinableId | — |

Plus les jointures audio existantes. Événements : sur le modèle de
`PlayerEndOfFileEvent`, avec le `sayId`.

### 9.3 Obligations de contrat

- Les protos MOTELI v2 d'elixip (`apps/elixip2/priv/proto/moteli_*.proto`)
  changent **dans le même jeu de changements**.
- Les clients Java (`XmlRpcMcuClient`, `jsr309impl`) reçoivent les mêmes
  méthodes.
- `docs/JSR-309-API.md` décrit les nouvelles méthodes et les événements.
- `/status/general` publie un bloc `tts` : serveur joignable ou non, voix
  disponibles. La liste des voix est **demandée au serveur TTS**, jamais
  écrite dans le mediaserver ni chez le contrôleur.

## 10. Lots

| Lot | Contenu | Condition de fin |
|---|---|---|
| 0 | Bâtir Piper et onnxruntime sur AlmaLinux 9. Mesurer : délai avant le premier son, facteur temps réel, CPU par phrase, mémoire par voix chargée. Écouter les voix françaises. Vérifier la licence de chaque voix. Vérifier l'état du dépôt amont de Piper et de son API C/C++. | Go ou no-go écrit dans ce document. |
| 1 | Fusionner `feat/wss-client`. | `make check` vert. |
| 2 | `mediaserver-tts` : protocole v1, file, threads de travail, Piper. | Tests du §11.1 verts. |
| 3 | `TtsClient` et `Synthesizer` côté mediaserver, sans API. | Tests du §11.2 verts. |
| 4 | Annonces : API conférence et JSR-309, événements, `/status/general`, protos MOTELI, clients Java. | Tests XML-RPC verts. |
| 5 | Découpeur de phrases et lecture T.140, conférence puis JSR-309. | Tests du §11.3 verts. |
| 6 | Packaging : sous-paquet RPM, unité systemd, voix livrées. | RPM installé sur une machine neuve. |
| 7 | Recette en appel réel (§12). | Recette passée. |

## 11. Tests

### 11.1 Serveur TTS

Contre un faux moteur qui rend un sinus de durée connue, sans Piper :

- `hello` absent, version inconnue, voix inconnue : erreur puis fermeture ;
- deux `say` : deux blocs de PCM, dans l'ordre, chacun suivi de son `done` ;
- `stop` au milieu d'une phrase : aucune trame binaire après `stopped` ;
- un moteur lent ne bloque pas une autre connexion.

Un test avec la vraie voix, hors `make check` : la phrase donne un PCM non
silencieux, de durée plausible.

### 11.2 Client

Contre un faux serveur WebSocket :

- le PCM reçu arrive dans la sortie audio, au bon nombre d'échantillons ;
- la coupure du serveur publie un échec pour chaque phrase en cours ;
- le serveur absent au démarrage n'empêche pas le mediaserver de démarrer.

### 11.3 Découpeur

- ponctuation, saut de ligne, silence : une phrase chacun ;
- `abc` + U+0008 + `d.` donne `abd.` ;
- un texte sans ponctuation part après le silence, jamais avant.

## 12. Recette

- Conférence de trois participants. L'un tape du T.140 en RTP, un autre en
  data channel depuis un navigateur. Les autres entendent les phrases.
- Appel JSR-309 avec une annonce, puis un texte T.140 lu.
- Arrêt du serveur TTS pendant une phrase : l'appel continue, l'événement
  d'échec arrive, la synthèse reprend après le redémarrage.

## 13. Risques

- **Latence** : Piper synthétise une phrase entière avant d'en rendre le son.
  Une phrase longue retarde son début. Le découpage en phrases du §5 la
  borne ; le lot 0 la mesure.
- **Charge** : une synthèse occupe un cœur le temps du calcul. Le nombre de
  threads de travail borne la charge. Au-delà, les phrases attendent dans la
  file.
- **Construction** : onnxruntime est long et lourd à bâtir. S'il ne se bâtit
  pas proprement sur AlmaLinux 9, Piper tombe au lot 0.

## 14. Questions ouvertes

1. L'auteur entend-il sa voix (§7.2) ? Recommandation : non.
2. Les langues : le français seul, ou une voix par participant ?
3. La 1.17.0 : le TTS n'y est promis qu'après le go du lot 0.
