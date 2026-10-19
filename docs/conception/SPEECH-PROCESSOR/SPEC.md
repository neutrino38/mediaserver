# SpeechProcessor : la voix et le texte dans un appel JSR-309

> Statut : **conception, rien n'est codé**. Cadrage validé le 2026-10-19.
> Version cible : 1.19.0. C'est le lot 6 de la
> [SPEC speech-server](../SPEECH-SERVER/SPEC.md) (§12, §13.8), qu'elle
> détaille et remplace.
> Prérequis : `speech-server` livré (lots 0 à 5, release 1.18.0), et le
> client WebSocket (`WebSocketServer::Connect`), fusionné en 1.18.0.
>
> Vocabulaire : celui de la SPEC speech-server (ASR, TTS, T.140). En plus :
> **contrôleur** désigne l'appelant qui pilote le serveur en XML-RPC
> (elixip) ; **port** désigne un côté du SpeechProcessor, A ou B (§4).

## 1. Objectif

Un nouveau `Joinable` JSR-309, le **SpeechProcessor**, met le service
`speech-server` dans un appel. Il sert deux usages.

**Le chatbot.** Un seul endpoint est branché. Sa voix est transcrite :
le texte remonte au contrôleur en événements, ou revient à l'endpoint en
T.140, ou les deux. Le contrôleur envoie du texte, et le SpeechProcessor le
dit dans l'audio de l'endpoint.

```
SpeechProcessor (port A) <==> Endpoint
```

**La transcription d'un appel.** Deux endpoints sont branchés, un de chaque
côté. La voix d'un côté est transcrite en texte vers l'autre côté. Le texte
tapé d'un côté est dit, mélangé à l'audio, vers l'autre côté. Le texte et
l'audio passent aussi tels quels d'un côté à l'autre.

```
Endpoint <==> (port A) SpeechProcessor (port B) <==> Endpoint
```

**La vidéo ne passe pas par le SpeechProcessor.** `EndpointAttachToEndpoint`
prend le média en paramètre (`xmlrpcjsr309.cpp`) : le contrôleur branche la
vidéo directement entre les deux endpoints, et seulement l'audio et le texte
sur le SpeechProcessor. Les FPU, REMB et RPSI continuent donc d'aller d'un
endpoint à l'autre sans intermédiaire. Le SpeechProcessor n'a **pas** de
port vidéo.

## 2. Les quatre cas d'appel

A est l'utilisateur sourd, sur un client SIP. B est l'entendant.

| # | Appel | Port A | Port B | Ce qui se passe |
|---|---|---|---|---|
| 1 | texte + voix → voix | audio, texte | audio | la voix de A va à B ; le texte de A est dit, mélangé dans l'audio de B ; la voix de B va à A, et sa transcription arrive en texte à A |
| 2 | texte seul → voix | texte | audio | le texte de A est dit vers B ; la transcription de B arrive en texte à A |
| 3 | texte + voix + vidéo des deux côtés | audio, texte | audio, texte | comme le cas 1, et le texte tapé passe aussi dans les deux sens ; la vidéo va directement de A à B |
| 4 | comme le cas 3, sans vidéo | audio, texte | audio, texte | comme le cas 3 |

Le « sous-titrage » du cas 3 est la transcription en **T.140** vers le
client de A. Aucun sous-titre n'est incrusté dans l'image : il faudrait
alors faire repasser la vidéo par un encodeur, ce qui contredit le relais
direct du §1.

## 3. Ce qui existe (inventaire vérifié le 2026-10-19)

- **Le branchement par média.** `Endpoint::Attach(media, role, joinable)`.
  Un endpoint écoute un `Joinable` (`AddListener`) et reçoit des
  `RTPPacket`. Le modèle à suivre est le port de mixer audio,
  `AudioMixerResource::Port` : un `AudioDecoderJoinableWorker` écoute
  l'endpoint, et un `AudioEncoderMultiplexerWorker` est le `Joinable` que
  l'endpoint écoute. `GetJoinable` rend une vue aliasing sur le port, pour
  que le `weak_ptr` de l'écouteur expire proprement (lien A de la migration
  smart pointers).
- **Pousser de l'audio.** `AudioEncoderMultiplexerWorker::EncodeSamples`,
  en mode push (`Init()` sans entrée).
- **Décoder le texte.** `RedundentCodec::Decode` sépare le T.140 RED
  (RFC 4103). `Recorder::onTextPacket` est le modèle : RED ou T.140 nu,
  vers une `TextOutput`.
- **Produire le texte.** `RedundentCodec::Encode` / `EncodeNull` /
  `EncoderIsIdle`. `WSEndpoint` s'en sert déjà pour émettre du T.140 RED.
- **La VAD.** `VAD` (`mcu/include/vad.h`, libfvad), à 8, 16, 32 ou 48 kHz.
- **Les événements.** `JSR309Event`, dont les numéros sont un contrat de fil
  et ne se réutilisent jamais (le dernier est 7).
- **Le client WebSocket.** `WebSocketServer::Connect` et `ConnectLater`.
- **Le speech-server.** Le protocole est décrit aux §7.3 (`tts`) et §7.4
  (`asr`) de sa SPEC. Le faux Mistral des tests (`tests/fake_mistral_*.h`)
  permet de lancer un vrai `speech-server` sans réseau.
- **Ce qui manque.** Le mcu ne lie **ni libcurl ni lecteur JSON**
  (`mcu/Makefile`, ligne 113). Seuls `speech-server` et les tests les lient.

## 4. Le modèle : deux ports

```
          ┌──────────────────────── SpeechProcessor ──────────────────────────┐
 EP-A ─audio─▶ décodeur A ─┬─▶ VAD + ASR A ──▶ événements                      │
          │                │        └────────▶ multiplexeur texte B ──▶ RTP ──┼─▶ EP-B
 EP-A ─texte─▶ décodeur ───┼───────── relais ──▶ multiplexeur texte B         │
          │                │  └─▶ TTS (A→B) ──▶ tampon de lecture B ─┐        │
          │                └──── relais audio ───────────────────────┴▶ mix B ┼─▶ EP-B
          │                                         bruit de confort ─┘       │
          │                    (même chose, symétrique, de B vers A)          │
          └───────────────────────────────────────────────────────────────────┘
```

- **Deux ports exactement**, A (0) et B (1). Le chatbot n'utilise que A.
  Une transcription à plusieurs, c'est une conférence, donc le MCU, hors de
  cette SPEC.
- Chaque port est un `Joinable` pour l'**audio** et pour le **texte**, et
  chaque média y est **facultatif** : c'est ce qui couvre le cas 2.
- **Les relais sont automatiques.** Dès que l'entrée d'un média est branchée
  sur un port et que la sortie du même média est branchée sur l'autre, ce
  média passe. Aucun appel ne les active. C'est ce que dit le cadrage : « le
  texte envoyé par un endpoint est passé tel quel à l'autre endpoint ».
- **La reconnaissance et la synthèse s'activent par XML-RPC** (§8). Le
  routage est une petite matrice :
  - l'ASR d'un port source va vers les événements et/ou vers la sortie texte
    d'un port destination ;
  - la TTS d'un port destination lit le contrôleur (`Say`) et/ou l'entrée
    texte d'un port source.

  En chatbot, source et destination sont le même port.

### 4.1 Pourquoi un mixeur interne, et pas `AudioMixer`

Un port de mixer reçoit tous les autres ports, sauf lui-même. Si la
synthèse du texte de A passait par un mixer, elle reviendrait aussi dans
l'audio de A : dans le cas 1, un sourd qui entend un peu s'entendrait lire
son propre texte. Il faut un routage plus fin que « tous sauf moi », et ce
routage tient dans une somme de deux ou trois entrées cadencée à 20 ms.

### 4.2 L'audio est toujours décodé, puis réencodé

Même quand la TTS se tait, l'audio relayé est décodé, mélangé puis
réencodé. On ne fait jamais de relais RTP brut au repos :

- le flux sortant garde **un SSRC et des timestamps continus**, alors
  qu'une bascule entre relais brut et flux encodé changerait de flux en
  pleine phrase ;
- le bruit de confort (§4.4) et la synthèse se mélangent sans cas
  particulier ;
- **chaque port a son codec**, posé par le contrôleur comme sur un port de
  mixer (`SpeechProcessorPortSetCodec`). Une jambe Opus et une jambe PCMA se
  parlent alors sans `AudioTranscoder`.

Le prix est un décodage et un encodage par sens, et environ 20 à 40 ms de
latence.

**Fréquence interne : 16 kHz**, celle de `speech-server` et du mixeur de
conférence. Un relais Opus ↔ Opus (cas 3 en WebRTC) perd donc la bande
au-delà de 8 kHz. C'est accepté en version 1, parce que l'usage est la voix.

### 4.3 Le cadencement

Un seul `Worker` par SpeechProcessor bat **20 ms**. À chaque tour, pour
chaque port dont la sortie audio est branchée :

```
mix = relais(entrée de l'autre port) + tampon TTS(port) + bruit(port)
EncodeSamples(mix)
```

- L'addition est **saturée** (S16) : deux voix fortes écrêtent au lieu de
  boucler.
- Le tampon du relais est court : 60 ms au plus. Au-delà, le plus ancien
  est jeté, pour que la latence ne dérive pas quand les horloges des deux
  pairs divergent.
- Ce thread ne bloque jamais sur le réseau. Les WebSocket vers
  `speech-server` vivent dans le réacteur du `WebSocketServer`, et les
  échanges passent par des files (§5.3).

### 4.4 Le bruit de confort

Quand rien ne parle sur un port, le mix émet un **bruit blanc léger**, à
-65 dBov par défaut, réglable par port, et 0 pour un silence exact :

- un silence numérique pur ressemble à une ligne coupée (« allô ? ») ;
- un pair qui ne gère pas la DTX peut déclencher son délai d'inactivité
  RTP ;
- le flux sortant ne s'interrompt jamais, ce qui garde aussi le NAT ouvert.

## 5. Le client speech dans le mediaserver

### 5.1 Configuration

- `--speech-url` : la base commune des deux ports de `speech-server`, par
  exemple `https://speech.example.net`, avec le port de contrôle et le port
  de streaming (SPEC speech-server §12).
- `--speech-account` : facultatif. C'est le champ `account` de
  `POST /control/token`, qui ventile les compteurs de `speech-server`.
- La confiance TLS de `wss://` reprend `--websocket-client-ca`, qui existe
  déjà.

Sans `--speech-url`, `SpeechProcessorCreate` réussit quand même : les
relais fonctionnent seuls. `SpeechProcessorStartRecognition` et
`SpeechProcessorStartSynthesis` échouent alors avec « speech service not
configured », et `/status/general` le signale.

### 5.2 Un flux par fonction et par port

- **Une session ASR par port source** et **une session TTS par port
  destination**. Un SpeechProcessor ouvre donc au plus quatre flux.
- Chaque flux commence par un jeton (`POST /control/token`, libcurl), puis
  ouvre sa WebSocket. Le jeton est demandé **dans un thread de travail**
  (petit pool, sur le modèle de `TaskPool`), jamais dans le réacteur ni dans
  le thread de cadencement.
- Un jeton ne sert qu'une fois : chaque réouverture en demande un nouveau.

### 5.3 Garder un flux ouvert pendant tout l'appel

`speech-server` ferme un flux qui reste `limits.idleSecs` (60 s) sans trame
du client, ou qui a dépassé `limits.maxSessionSecs` (3 600 s). Un appel dure
souvent plus longtemps que les deux. Le client s'en occupe ainsi :

- **TTS** : tant que la synthèse est active, le client envoie
  `{"type":"flush"}` toutes les 30 s s'il n'a rien envoyé d'autre. Un
  `flush` sur un tampon vide ne coûte rien chez Mistral. Le lot 1 le
  vérifie par un test.
- **ASR** : le client est mis en pause par la VAD (§6.1). En pause, il
  renvoie `{"type":"pause"}` toutes les 30 s. Le lot 1 vérifie que
  `speech-server` accepte une pause répétée sans effet. Sinon, il faut l'y
  rendre idempotente : c'est un changement d'une ligne dans `AsrSession`.
- **Rotation** : après 55 min, le flux est remplacé **au prochain silence**
  (ASR en pause, ou TTS entre deux phrases). Le client ouvre un nouveau
  flux, bascule dessus, puis ferme l'ancien par `done`.
- **Panne** : sur une fermeture 1013 ou 1011, ou un échec d'ouverture, le
  client réessaie après 1, 2, 5, 10, puis 30 s en boucle. La panne et le
  retour du service sont publiés en événements (§8.4).

**Les relais ne dépendent jamais du service speech.** Un `speech-server`
tombé fait perdre la transcription et la synthèse, jamais l'appel.

## 6. La reconnaissance (ASR)

### 6.1 La VAD décide de ce qui est payé

Mistral facture les secondes d'audio reçues. Dans les cas 3 et 4,
transcrire tout l'appel ferait payer aussi les silences. Le SpeechProcessor
passe donc l'audio décodé de chaque port source dans la `VAD` du mcu
(libfvad, mode `AGGRESSIVE`) :

- **parole détectée** : le client envoie `resume`, puis le **pré-roulement**
  (les 300 dernières ms gardées en anneau, pour ne pas couper la première
  syllabe), puis l'audio en direct ;
- **silence depuis `silenceMs`** (700 ms par défaut) : le client envoie
  `pause`. `speech-server` y répond par un `input_audio.flush`, ce qui fait
  arriver tout de suite la fin de l'énoncé.

La même VAD produit l'événement `SpeechActivity`, qui sert au barge-in du
chatbot (§7.4).

### 6.2 Où va le texte

`SpeechProcessorStartRecognition` indique deux destinations, cumulables :

- **les événements** (`toEvents`) ;
- **une sortie texte** (`textPort`) : le même port en chatbot (l'utilisateur
  lit ce qu'il a dit), l'autre port en transcription.

Les deltas de Mistral sont ajoutés à la fin, jamais repris (SPEC
speech-server §7.4, option A). Ils passent donc en T.140 sans aucun
caractère d'effacement.

### 6.3 Les événements de texte

Un delta Mistral fait souvent quelques lettres. Un événement par delta
noierait la file du long-poll. Il y a donc deux formes :

- `SpeechRecognized` avec `final=false` : les deltas regroupés, au plus un
  événement toutes les **250 ms** par port ;
- `SpeechRecognized` avec `final=true` : l'**énoncé entier**, émis quand la
  VAD a mis le flux en pause et que le texte du flush est arrivé. C'est la
  forme dont un chatbot a besoin pour interroger un modèle de langage.

## 7. La synthèse (TTS)

### 7.1 Deux sources

- **Le contrôleur** : `SpeechProcessorSay(port, text)` rend un `sayId`.
- **L'entrée texte d'un port** (`textSourcePort`), en transcription. Le flux
  T.140 est transmis à `speech-server` tel quel, au fil de la frappe :
  `speech-server` découpe déjà les phrases et applique les U+0008.

Les deux sources peuvent viser le même port en même temps. Par exemple, une
annonce du contrôleur pendant une transcription. Elles partagent alors le
flux TTS de ce port, dans l'ordre d'arrivée.

### 7.2 Associer les phrases à un `Say`

`speech-server` numérote les phrases (`say`/`done` avec `id`), pas les
`Say`, et un `Say` « A. B. » donne deux phrases. Le client envoie chaque
`Say` suivi de `flush`. Il passe aussi le texte dans **la même classe
`PhraseSplitter`** que le serveur (classe pure, liée aussi au mcu) pour
savoir combien de phrases il doit attendre. `SpeechSynthesisStarted` part au
premier `say` du `Say`, `SpeechSynthesisDone` au `done` de sa dernière
phrase.

### 7.3 Le tampon de lecture

Le PCM arrive plus vite que le temps réel. Il va dans un tampon par port,
que le cadencement (§4.3) vide à raison de 20 ms par tour. Rien ne borne le
tampon dans le temps, puisque la phrase entière doit être dite. Seuls
`StopSpeaking` et la fin de la synthèse le vident.

### 7.4 L'arrêt et le barge-in

`SpeechProcessorStopSpeaking(port)` envoie `stop` à `speech-server`, vide le
tampon de lecture, et émet `SpeechSynthesisDone` avec `interrupted=true`
pour chaque `Say` non terminé.

**Le barge-in est l'affaire du contrôleur.** Quand l'utilisateur se met à
parler pendant la synthèse, le SpeechProcessor émet `SpeechActivity`, et
c'est le contrôleur qui décide d'appeler `StopSpeaking`. Le serveur
n'interrompt jamais une synthèse de lui-même : seul le dialogue du chatbot
sait si une interruption est attendue.

### 7.5 Les corrections

En transcription, un retour arrière du sourd qui remonte dans du texte déjà
envoyé à la synthèse est une correction (SPEC speech-server §7.3). Le
SpeechProcessor suit la bulle T.140 du port source. Quand un retour arrière
dépasse ce qui a été envoyé, il envoie `stop`, puis l'annonce
(« Correction : » en français), puis le texte retapé jusqu'à la fin de la
ligne.

L'annonce est une option de `StartSynthesis` (`announceCorrections`, vraie
par défaut). Sans elle, le texte retapé est simplement dit.

### 7.6 L'écho

Si le téléphone de B renvoie la synthèse par écho acoustique, l'ASR de B la
transcrit vers A, qui relit son propre texte. Les annuleurs d'écho des
terminaux devraient suffire, et la recette (§11) le vérifie. En secours,
l'option `halfDuplex` de `StartRecognition` met l'ASR d'un port en pause
tant que la synthèse parle vers ce même port. Elle est fausse par défaut.

## 8. API XML-RPC (`/jsr309`)

Les ports valent 0 (A) ou 1 (B). Les conventions de nommage sont celles de
l'existant : `XAttachToY` veut dire que X **reçoit** de Y.

### 8.1 Cycle de vie et branchement

| Méthode | Paramètres | Rend |
|---|---|---|
| `SpeechProcessorCreate` | sessionId, tag | spId |
| `SpeechProcessorDelete` | sessionId, spId | — |
| `SpeechProcessorPortSetCodec` | sessionId, spId, port, codec audio | — |
| `SpeechProcessorSetComfortNoise` | sessionId, spId, port, niveau dBov (0 = silence) | — |
| `SpeechProcessorAttachToEndpoint` | sessionId, spId, port, endpointId, media | — |
| `EndpointAttachToSpeechProcessor` | sessionId, endpointId, spId, port, media | — |
| `SpeechProcessorDettach` | sessionId, spId, port, media | — |

`media` vaut Audio ou Text. Video est refusé (§1).

**Le codec de la sortie texte.** L'endpoint attaché dit ce qu'il accepte
(`Joinable::Listener::TryCheckCodec`) : le port produit du T.140 RED s'il est
accepté, du T.140 nu sinon. Le lot 0 vérifie que `RTPEndpoint` répond juste
pour le texte. Sinon, on ajoute un `SpeechProcessorPortSetTextCodec`
explicite.

### 8.2 Reconnaissance

| Méthode | Paramètres | Rend |
|---|---|---|
| `SpeechProcessorStartRecognition` | sessionId, spId, port source, toEvents (bool), textPort (-1 = aucun), halfDuplex (bool) | — |
| `SpeechProcessorStopRecognition` | sessionId, spId, port source | — |

### 8.3 Synthèse

| Méthode | Paramètres | Rend |
|---|---|---|
| `SpeechProcessorStartSynthesis` | sessionId, spId, port destination, textSourcePort (-1 = contrôleur seul), voix (alias, vide = défaut), announceCorrections (bool) | — |
| `SpeechProcessorStopSynthesis` | sessionId, spId, port destination | — |
| `SpeechProcessorSay` | sessionId, spId, port destination, texte | sayId |
| `SpeechProcessorStopSpeaking` | sessionId, spId, port destination | — |

La voix est un alias de la configuration de `speech-server`, jamais un
`voice_id` Mistral. La liste vient de `speech-server` (`GET /control/voices`)
et n'est **pas** recopiée côté contrôleur (CLAUDE.md : ce que le serveur sait
de lui-même, c'est à lui qu'on le demande).

### 8.4 Événements

Nouveaux numéros, à la suite des existants, jamais réutilisés :

| N° | Événement | Contexte | Données |
|---|---|---|---|
| 8 | `SpeechRecognizedEvent` | spId, Text, port | texte, `final` |
| 9 | `SpeechSynthesisStartedEvent` | spId, Audio, port | sayId (0 = texte du port source) |
| 10 | `SpeechSynthesisDoneEvent` | spId, Audio, port | sayId, `interrupted` |
| 11 | `SpeechActivityEvent` | spId, Audio, port | `speaking` (bool) |
| 12 | `SpeechServiceEvent` | spId, port | fonction (`asr`/`tts`), état (`down`/`up`), raison |

### 8.5 Ce qui doit suivre dans le même jeu de changements

Ce sont les règles du CLAUDE.md, appliquées **à chaque lot qui ajoute une
méthode** :

- les schémas protobuf MOTELI v2 d'elixip (`apps/elixip2/priv/proto/moteli_*.proto`) ;
- les clients Java (`XmlRpcMcuClient`, `jsr309impl`) ;
- `docs/JSR-309-API.md` ;
- `/status/general` : un bloc `speech` avec l'URL configurée ou non, les
  flux ouverts par fonction, le dernier échec et les réouvertures cumulées
  (`docs/reference/status-http.md`).

## 9. Le texte : un multiplexeur par sortie

Dans les cas 3 et 4, la sortie texte de A reçoit deux sources : le texte
tapé par B (relais) et la transcription de la voix de B (ASR). Le T.140 n'a
pas de multiplexage. Une lettre tapée au milieu d'un delta ASR mélangerait
les deux phrases.

`TextMux` est une **classe pure**, testée seule, sans réseau ni horloge
réelle :

- une seule source **tient** la sortie à un instant donné ;
- la source qui tient rend la main en fin de ligne (U+2028 ou `\n`), ou
  après **1 s** sans caractère ;
- l'autre source attend dans son tampon, sans limite de contenu, puisque
  rien ne se perd ;
- à chaque changement de source, `TextMux` insère un saut de ligne (U+2028)
  si la ligne en cours n'est pas vide ;
- un U+0008 venu du relais n'efface que du texte de sa propre source dans la
  ligne en cours. S'il remonterait dans le texte de l'autre source, il est
  jeté.

Un préfixe qui désigne la source (par exemple « 🎤 ») pourra venir plus
tard, en option. Il n'est pas prévu en version 1 (§13).

## 10. Plan d'implémentation

| Lot | Contenu | Condition de fin |
|---|---|---|
| 0 | Vérifications : `TryCheckCodec` sur une jambe texte, fréquence de sortie de `AudioDecoderJoinableWorker`, `flush` sur un tampon vide, `pause` répétée | Réponses écrites ici, au §3 |
| 1 | Client speech : libcurl et nlohmann-json liés au mcu, `SpeechTokenClient`, `SpeechFlow` (asr/tts, garde ouverte, rotation, reprise), `--speech-url`, `--speech-account`, bloc `speech` de `/status/general` | Tests §11.1 verts |
| 2 | SpeechProcessor sans speech : ports, branchement, mix et cadencement, bruit de confort, relais texte, `TextMux` ; API §8.1 et contrat §8.5 | Tests §11.2 verts |
| 3 | ASR : VAD, pré-roulement, événements regroupés, sortie texte, `halfDuplex` ; API §8.2, événements 8, 11 et 12 | Tests §11.3 verts |
| 4 | TTS : `Say`, source port, tampon de lecture, arrêt, corrections ; API §8.3, événements 9 et 10 | Tests §11.4 verts |
| 5 | Côté elixip : les quatre cas d'appel et le chatbot | Recette §12 |

Chaque lot est livrable seul. Le lot 2 a déjà une valeur sans service
speech : il joue le rôle d'un transcodeur audio et texte.

## 11. Tests

Tous dans `mcu/tests/`, joués par `make check`, **sans réseau**. Les tests
de bout en bout lancent le vrai binaire `speech-server` contre le faux
Mistral de ses propres tests (`fake_mistral_realtime.h`,
`fake_mistral_http.h`).

### 11.1 Client speech

- Un jeton est demandé avant chaque ouverture. Une réouverture en demande un
  nouveau.
- Un `speech-server` arrêté fait passer en `down`, puis les essais suivent
  1, 2, 5… s. Au redémarrage, l'état repasse à `up`, et un seul événement
  est émis par changement d'état.
- Une TTS muette pendant 90 s reste ouverte, grâce aux `flush`. Une ASR en
  pause pendant 90 s aussi, grâce aux `pause` répétées.
- Rotation : avec une durée maximale réduite pour le test, le flux est
  remplacé au premier silence, et aucun texte n'est perdu ni doublé.
- La demande de jeton ne bloque ni le réacteur ni le cadencement :
  `speech-server` répond en 3 s, et pendant ce temps le cadencement
  continue de battre.

### 11.2 Média

- Relais audio A → B : une sinusoïde à 1 kHz entre en PCMA et sort en Opus
  avec la même fréquence, dans les ±1 dB. Le SSRC et les timestamps sortants
  sont continus.
- Rien en entrée : le port émet du bruit au niveau demandé, ou un silence
  exact à 0.
- Les horloges dérivent de 1 % : la latence du relais reste sous 80 ms.
- Relais texte : du RED en entrée sort en RED, ou en T.140 nu vers un
  endpoint qui ne prend pas le RED. Un paquet RED perdu est récupéré.
- `TextMux` : l'entrelacement à la ligne, la reprise après 1 s, le saut de
  ligne au changement, et le U+0008 qui ne traverse pas une frontière de
  source.
- Une vidéo branchée sur un port est refusée par l'API.
- `SpeechProcessorDelete` pendant un relais : pas de fuite (ASan) ni
  d'accès à un objet libéré, et l'endpoint attaché voit son `weak_ptr`
  expirer.

### 11.3 ASR

- Une voix enregistrée (fixture du lot 0) entre par le port A : son texte
  sort en T.140 sur le port choisi, et en événements.
- Un silence de 10 s n'envoie aucun audio au faux Mistral : la VAD a mis le
  flux en pause.
- Le pré-roulement : la première syllabe arrive au faux Mistral.
- Pas plus d'un événement `final=false` toutes les 250 ms. Un seul
  `final=true` par énoncé, qui contient tout le texte de l'énoncé.
- `halfDuplex` : pendant une synthèse vers A, rien de A n'arrive au faux
  Mistral.

### 11.4 TTS

- `Say("A. B.")` donne un `Started`, puis un `Done` après la seconde phrase,
  et le PCM sort sur le port à vitesse réelle.
- `StopSpeaking` au milieu : plus aucun échantillon de synthèse dans les
  20 ms qui suivent, et `Done` arrive avec `interrupted=true`.
- Source port : le texte tapé sur A est dit sur B, et seulement sur B.
- Correction : un retour arrière dans du texte déjà dit envoie `stop`, puis
  « Correction : » suivi du texte retapé.
- La synthèse se mélange au relais : voix de A et synthèse sortent ensemble
  sur B, sans débordement de l'addition.

## 12. Recette

Chaque cas se joue en appel réel, avec un client SIP (Linphone) côté sourd
et un téléphone côté entendant.

- Les quatre cas du §2, chacun pendant plus de 2 minutes avec des silences
  de plus d'une minute, puis un appel de plus d'une heure pour la rotation.
- Chatbot : le contrôleur répond à l'énoncé `final`, et l'utilisateur coupe
  la parole à la synthèse (barge-in).
- La latence de bout en bout, mesurée : de la fin d'une phrase dite à son
  texte affiché, et du point final tapé au premier son entendu.
- L'écho : un téléphone en mains libres côté B. La synthèse revient-elle en
  texte chez A ? La réponse décide si `halfDuplex` doit être vrai par
  défaut.
- Coupure de `speech-server` pendant un appel : l'appel continue, et les
  événements `down` puis `up` arrivent au contrôleur.
- Le coût d'un appel d'une heure, lu dans `usage` de `speech-server`, avec
  et sans la VAD.

## 13. Questions ouvertes

Les réponses par défaut entre parenthèses s'appliquent tant que la question
n'est pas tranchée.

1. **Le préfixe de source dans le texte multiplexé** (§9) : faut-il marquer
   en T.140 ce qui vient de la voix ? Non.
2. **La correction `comp` pour un client SIP** : un client navigateur
   corrige sa frappe avant l'envoi (SPEC speech-server §7.5), un client SIP
   ne le fait pas. Le SpeechProcessor devrait-il corriger le texte du sourd
   avant de le dire ? Non. C'est le rôle du client d'appeler le speech server
3. **La fréquence interne** (§4.2) : passer à 48 kHz si la recette juge que
   la perte de bande du cas 3 en WebRTC s'entend. (16 kHz.). Ok
4. **La conférence MCU** : la lecture du T.140 d'un participant et les
   annonces sont décrites dans la [SPEC TTS](../TTS/SPEC.md), sur le même
   client speech (§5). (Hors de cette SPEC.). Ok mais pas dans cette phase.
