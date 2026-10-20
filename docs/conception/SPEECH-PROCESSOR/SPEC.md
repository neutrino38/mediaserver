# SpeechProcessor : la voix et le texte dans un appel JSR-309

> Statut : **lots 1 à 4 faits** (client speech ; SpeechProcessor sans
> speech ; reconnaissance ; synthèse, 2026-10-19) ; reste le lot 5 (elixip)
> et la recette.
> Cadrage validé le 2026-10-19.
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
- **Ce qui manquait.** Le mcu ne liait **ni libcurl ni lecteur JSON**. Le
  lot 1 les y a ajoutés (`SPEECHCLIENTOBJS` dans `mcu/Makefile`).

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
Tranché le 2026-10-19 : si la recette (§12) juge la qualité insuffisante, la
fréquence interne passe à **48 kHz**. Seuls le mix et le bruit de confort
changent : l'ASR et la TTS restent à 16 kHz, rééchantillonnées à
l'entrée et à la sortie du flux speech. La fréquence est donc une constante
de la classe, jamais une valeur écrite en dur dans les calculs.

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

- `--speech-url` : le **port de contrôle** de `speech-server`, où se
  demandent les jetons, par exemple `http://speech.example.net:9200`.
- `--speech-streaming-url` : le **port de streaming**, où s'ouvrent les
  flux, par exemple `wss://speech.example.net:9201`.
- `--speech-account` : facultatif. C'est le champ `account` de
  `POST /control/token`, qui ventile les compteurs de `speech-server`.
- La confiance TLS de `wss://` reprend `--websocket-client-ca`, qui existe
  déjà.

Deux URL et non une base commune (décidé au lot 1) : les deux ports
diffèrent par le schéma (le contrôle est en HTTP clair, le streaming en
`ws://` ou `wss://` selon `streaming.secure`) et par le numéro, et rien
côté client ne permet de déduire l'un de l'autre sans deviner. Les deux
options vont ensemble : une seule des deux, ou `--speech-account` sans
elles, et le mediaserver refuse de démarrer.

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
  `flush` sur un tampon vide ne coûte rien chez Mistral (vérifié, §10.2).
- **ASR** : le client est mis en pause par la VAD (§6.1). En pause, il
  renvoie `{"type":"pause"}` toutes les 30 s. Une pause répétée est sans
  effet (vérifié, §10.2).
- **Rotation** : après 55 min, le flux est remplacé **au prochain silence**
  (ASR en pause, ou TTS entre deux phrases). Le client ouvre un nouveau
  flux, bascule dessus, puis ferme l'ancien par `done`.
- **Panne** : sur une fermeture que le client n'a pas demandée (1013, 1011,
  ou TCP coupé : la pile WebSocket ne remonte pas le code reçu, la raison
  vient du dernier `error` fatal), ou un échec d'ouverture, le client
  réessaie après 1, 2, 5, 10, puis 30 s en boucle. Chaque essai demande un
  nouveau jeton : c'est l'horloge du `SpeechService` qui les déclenche, pas
  `ConnectLater`, qui rejouerait la même URL et donc un jeton déjà brûlé. La
  panne et le retour du service sont publiés en événements (§8.4).

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
  VAD a mis le flux en pause et que le texte du flush est arrivé (500 ms
  sans delta après la pause, §10.5). C'est la
  forme dont un chatbot a besoin pour interroger un modèle de langage.

## 7. La synthèse (TTS)

### 7.1 Deux sources

- **Le contrôleur** : `SpeechProcessorSay(port, text)` rend un `sayId`.
- **L'entrée texte d'un port** (`textSourcePort`), en transcription. Le client
  découpe la frappe T.140 en phrases, avec la même classe `PhraseSplitter`
  que speech-server : une phrase finit sur une ponctuation finale suivie d'un
  blanc, une fin de ligne, ou 1 200 ms sans frappe. Les U+0008 qui ne
  touchent que du texte pas encore dit l'effacent sur place.

Les deux sources peuvent viser le même port en même temps. Par exemple, une
annonce du contrôleur pendant une transcription. Elles partagent alors le
flux TTS de ce port, dans l'ordre d'arrivée. Une phrase tapée en cours n'est
jamais chez speech-server : elle reste chez le client jusqu'à sa coupure. Une
annonce ne peut donc pas se coller à elle.

### 7.2 Associer les phrases à un `Say`

`speech-server` numérote les phrases (`say`/`done` avec `id`), pas les
`Say`. Il les numérote dans l'ordre où il les reçoit, à partir de 1 pour
chaque flux. Le client envoie donc **des phrases déjà coupées**, chacune
suivie de `flush` : chaque envoi est une phrase du serveur, et son `id` se
déduit de l'ordre d'envoi. Un `Say` « A. B. » part en deux phrases.

`SpeechSynthesisStarted` part au premier `say` du `Say`,
`SpeechSynthesisDone` au `done` de sa dernière phrase. Une phrase refusée par
la modération (`error` avec son `id`) compte comme dite. Un flux rouvert, ou
un `Say` envoyé sans flux prêt, finit ses `Say` avec `interrupted=true`.

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
SpeechProcessor suit la bulle T.140 du port source. Il coupe lui-même les
phrases sur un silence de frappe (1 200 ms, sous les 1 500 ms du serveur),
pour savoir exactement ce qui a été envoyé (§10.6). Quand un retour arrière
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
accepté, du T.140 nu sinon. `RTPEndpoint` répond d'après sa `rtpMap` de
sortie (vérifié, §10.2).

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
- `docs/JSR-309-API.md`, section par section (§10.8) ;
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

### 10.1 Vue d'ensemble

| Lot | Contenu | Condition de fin |
|---|---|---|
| 1 | Client speech dans le mediaserver, sans API JSR-309 | Tests §11.1 verts |
| 2 | SpeechProcessor sans speech : ports, relais, mix, bruit, `TextMux` ; API §8.1 | Tests §11.2 verts |
| 3 | ASR : VAD, pré-roulement, événements, sortie texte ; API §8.2 | Tests §11.3 verts |
| 4 | TTS : `Say`, source port, tampon de lecture, arrêt, corrections ; API §8.3 | Tests §11.4 verts |
| 5 | Côté elixip : les quatre cas d'appel et le chatbot | Recette §12 |

Chaque lot de 2 à 4 enrichit `docs/JSR-309-API.md` dans son propre commit (§10.8).

L'ancien lot 0 est soldé : chaque vérification qu'il prévoyait a été faite
en lisant le code (§10.2). Chaque lot donne un commit qui se suffit à
lui-même, avec un `make check` vert. Le lot 2 a déjà une valeur sans
service speech : il remplace un transcodeur audio et texte.

Ordre de travail à l'intérieur d'un lot : d'abord les classes pures et
leurs tests, puis le branchement, puis l'API et son contrat (§8.5).

### 10.2 Vérifications faites le 2026-10-19 (ancien lot 0)

| Question | Réponse, d'après le code |
|---|---|
| Une `pause` répétée est-elle sans effet ? | Oui. `AsrSession::OnText` rend la main si le flux est déjà en pause. Et toute trame du client remet à zéro le délai d'inactivité (`SpeechSession::onMessageEnd`). |
| Un `flush` à vide coûte-t-il quelque chose chez Mistral ? | Non. `TtsSession::Enqueue` jette une phrase vide avant la file. |
| L'endpoint dit-il quel codec texte il accepte ? | Oui. `RTPMultiplexer::TryCheckCodec` interroge chaque écouteur, et `RTPEndpoint::TryCheckCodec` vérifie la `rtpMap` de sortie (`CanSendCodec`) avant de basculer. Pas besoin de `SetTextCodec`. |
| À quelle fréquence arrive l'audio décodé ? | À celle du codec : `AudioDecoderJoinableWorker` appelle `StartPlaying(codec->GetRate())` puis `PlayFrame`. Le port rééchantillonne avec `AudioResampler`, comme `EncodeSamples` le fait déjà de son côté. |
| Le client HTTP de speech-server sert-il pour le jeton ? | Presque. `MistralHttp::Post` pose toujours `Authorization: Bearer`. Il faut ne plus le poser quand la clé est vide : une ligne. |
| Le protocole `asr` marque-t-il la fin du texte d'un `flush` ? | Non (SPEC speech-server §7.4). La fin d'énoncé est donc déduite d'un délai (§10.5). |

### 10.3 Lot 1 : le client speech

État : **fait** le 2026-10-19. Tests `test_speech_client.cpp` (suites
`SpeechClient` et `SpeechServiceConfig`) et deux tests du bloc `speech` dans
`test_status.cpp`. Écarts au plan ci-dessous, tous tranchés au codage :

- **Deux URL** : `--speech-url` (contrôle) et `--speech-streaming-url`
  (streaming), voir §5.1.
- **La reprise** passe par l'horloge du `SpeechService` (un tour toutes les
  100 ms), pas par `ConnectLater` : chaque essai doit demander un nouveau
  jeton (§5.3).
- **Les rappels** de `SpeechFlow::Listener` arrivent du réacteur, du
  `TaskPool` ou de l'horloge, jamais sous un verrou du flux. La règle ne
  change pas : ils déposent dans une file et rendent la main.
- **`onReady`** est rappelé à chaque nouveau flux, rotation comprise, alors
  que `onState` ne l'est qu'au changement d'état : les `id` de phrase de la
  TTS repartent de 1 avec chaque flux, et le propriétaire doit le savoir.
- **`MistralHttp`** accepte tout code 2xx : `POST /control/token` répond
  **201**, pas 200.
- **`JSR309Manager::SetSpeechService`** remet le service à chaque
  `MediaSession`. Le service n'est donné que s'il est configuré.
- Le mediaserver arrête le service **avant** le réacteur WebSocket, pour
  qu'aucun jeton ni aucune connexion sortante ne vise un serveur arrêté.

**Construction.**
- `mcu/Makefile` : un groupe `SPEECHCLIENTOBJS` (`taskpool.o`,
  `phrasesplitter.o`, `mistralhttp.o`, plus les objets neufs ci-dessous),
  ajouté à `OBJS`, avec `-lcurl` sur la ligne de lien du mcu. Le
  commentaire « the mcu links neither the JSON reader nor libcurl » est à
  corriger. Les objets propres au serveur (`speechsession.o`,
  `asrsession.o`, etc.) restent hors du mcu.
- `install.ksh prereq` et `mcumediaserver.spec` : `libcurl-devel` et
  `json-devel` en dépendances de build du paquet principal, et `libcurl` en
  dépendance d'exécution. Les deux sont déjà là pour `mediaserver-speech`.
  Même chose côté `.deb`.
- `MistralHttp::Post` : ne pose pas l'en-tête `Authorization` quand la clé
  est vide.

**Classes** (`mcu/include/speech/`, `mcu/src/speech/`) :
- `SpeechTokenClient` : `POST /control/token` avec `function` et `account`,
  et rend le jeton par un rappel. L'appel tourne **dans un `TaskPool`**
  (deux threads), jamais dans le réacteur.
- `SpeechFlow` : un flux `asr` ou `tts`. C'est une machine à états
  (`Idle → Token → Connecting → Ready → Down`), avec :
  - l'ouverture par `WebSocketServer::Connect` sur
    `/streaming/<fonction>/<jeton>` ;
  - la garde ouverte du §5.3 : un `flush` ou une `pause` toutes les 30 s
    sans autre trame ;
  - la rotation à 55 min, au premier silence que signale le propriétaire du
    flux (`CanRotate()`) ;
  - la reprise à 1, 2, 5, 10, puis 30 s, par `ConnectLater`.

  Son écouteur (`SpeechFlow::Listener`) reçoit `onReady`, `onText`,
  `onPcm`, `onSay`, `onDone`, `onStopped` et `onState(up|down, raison)`.
  **Tous ces rappels arrivent sur le thread du réacteur WebSocket** : ils
  ne font que déposer dans une file protégée par un verrou court
  (règle du `websocket-refactor-plan`, et `docs/reference/threads-rtp.md`).
- `SpeechService` : un seul par processus, possédé par `main()`. Il porte
  l'URL, le compte, le `TaskPool`, une référence au `WebSocketServer`, et les
  compteurs que lit `/status/general` (flux ouverts par fonction, dernier
  échec, réouvertures). Il fabrique les `SpeechFlow`.

**Branchement.**
- `main.cpp` : `--speech-url` et `--speech-account`, avec le texte d'aide.
  `SpeechService` est construit après `wsServer`, puis passé au
  `JSR309Manager` (qui le passe à chaque `MediaSession`).
- `statushandler.cpp` : le bloc `speech`, en JSON et en texte.
  `docs/reference/status-http.md` le décrit.

**Tests** (`test_speech_client.cpp`). Ils lancent le vrai binaire
`speech-server` contre les faux Mistral, sur le modèle de
`test_speech_server.cpp`, et couvrent les cas du §11.1. La durée maximale et
les délais sont des paramètres de `SpeechFlow`, pour que la rotation se
teste en quelques secondes.

**Commit** : `speech : client du speech-server dans le mediaserver`.

### 10.4 Lot 2 : le SpeechProcessor sans speech

État : **fait** le 2026-10-19. Tests `test_textmux.cpp`,
`test_speech_processor_media.cpp`, `test_speech_processor_xmlrpc.cpp`, plus un
test de `RedundentCodec` dans `test_redcodec.cpp`. Écarts au plan ci-dessous,
tranchés au codage :

- **Le texte sort par paquets de 300 ms au plus** (RFC 4103 §5), pas à chaque
  tour : le `TextMux` est vidé à chaque tour de 20 ms, dans un tampon que le
  port émet dès que 300 ms se sont écoulées depuis le paquet précédent. La
  redondance vide (`EncodeNull`) suit le même rythme.
- **Le relais ne démarre qu'avec 40 ms d'avance** dans l'anneau (60 ms au
  plus), et reprend cette avance après un manque : sans elle, la gigue du pair
  creusait un trou de 20 ms à chaque paquet en retard.
- **Le bruit de confort** se règle de -96 à 0 dBov ; hors de là, refus.
- **Sans `SpeechProcessorPortSetCodec`, un port n'émet pas d'audio**, comme un
  port de mixer.
- **Le PT des blocs RED** émis est `TextCodec::T140` (106), comme `DCEndpoint` :
  un `Joinable::Listener` ne dit pas le PT T.140 négocié par sa jambe. Un pair
  qui aurait négocié un autre PT pour le T.140 lit mal les blocs redondants
  (le bloc primaire n'est pas touché). À reprendre si la recette le montre.
- **Quatre défauts de `RedundentCodec` corrigés** au passage, parce que le
  port s'y appuie : `EncoderIsIdle()` lisait un compteur non initialisé ; le
  paquet rendu `NULL` n'était jamais libéré ; le dernier paquet de
  redondance, qui porte encore la dernière frappe, était jeté — elle n'était
  protégée qu'une fois au lieu de deux ; et la classe n'avait pas de
  destructeur, donc ses générations gardées fuyaient (vu par ASan). Elle n'est
  plus copiable. `textstream`, `WSEndpoint` et `Recorder` en profitent.
- **`jsr309impl` n'est pas touché** : aucun objet JSR-309 standard ne
  correspond au SpeechProcessor.

**Classes pures, d'abord.**
- `TextMux` (`mcu/include/textmux.h`, `mcu/src/textmux.cpp`) : les règles
  du §9. On lui passe l'heure à chaque appel, sans horloge propre, et il
  rend les caractères à émettre.
- `ComfortNoise` : un générateur de bruit blanc à niveau en dBov, sur un
  tirage pseudo-aléatoire à graine fixe pour que les tests soient
  reproductibles. Il est assez petit pour vivre dans `SpeechProcessor.cpp`.

**`SpeechProcessor`** (`mcu/src/jsr309/SpeechProcessor.{h,cpp}`, ajouté à
`JSR309OBJ`) :
- C'est un `Worker` qui bat 20 ms (§4.3). La fréquence interne est une
  constante de la classe (`MixRate = 16000`).
- `Port` (deux instances) contient :
  - **entrée audio** : `AudioDecoderJoinableWorker`, initialisé avec un
    `PortAudioSink : AudioOutput` qui rééchantillonne à `MixRate` et dépose
    dans l'anneau de relais (60 ms au plus) ;
  - **sortie audio** : `AudioEncoderMultiplexerWorker` en mode push
    (`Init()` sans entrée), qui reçoit le mix par `EncodeSamples` ;
  - **entrée texte** : `PortTextSink : Joinable::Listener`, qui décode le
    RED par `RedundentCodec::Decode`, ou le T.140 nu, sur le modèle de
    `Recorder::onTextPacket` ;
  - **sortie texte** : un `RTPMultiplexer` qui produit des paquets T.140
    ou T.140 RED selon `TryCheckCodec(T140RED)`. Le RED est encodé par
    `RedundentCodec::Encode`, avec `EncodeNull` tant que
    `EncoderIsIdle()` est faux, comme dans `WSEndpoint`. Le `TextMux` de
    la sortie se vide à chaque tour de 20 ms ;
  - le niveau de bruit.
- `GetJoinable(port, media)` rend un `shared_ptr` aliasing sur le port
  (lien A, comme `AudioMixerResource`). `Attach(port, media, joinable)` et
  `Dettach(port, media)`.
- `End()` est idempotent : il arrête le `Worker`, puis détache les
  décodeurs, puis arrête les encodeurs.

**`MediaSession`** : une map `SpeechProcessors` de `shared_ptr`, avec
`SpeechProcessorCreate`, `Delete`, `Get`, `PortSetCodec`,
`SetComfortNoise`, `AttachToEndpoint`, `Dettach`, et
`EndpointAttachToSpeechProcessor` (par `Endpoint::Attach(media,
VIDEO_MAIN, sp->GetJoinable(port, media))`). `Delete` suit le motif du
CLAUDE.md : extraire sous le verrou, effacer de la map, puis `End()` hors
du verrou. Video et un port hors de {0, 1} sont refusés.

**API** (`xmlrpcjsr309.cpp`) : les sept méthodes du §8.1, et leurs entrées
dans `jsr309CmdList`.

**Contrat, dans le même commit** :
- elixip `apps/elixip2/priv/proto/moteli_jsr309.proto` ;
- `XmlRpcMcuClient/src/org/murillo/MediaServer/XmlRPCJSR309Client.java`.
  `jsr309impl` seulement si l'objet y trouve un pendant JSR-309 (aucun
  `MediaMixer` standard ne lui correspond : à juger au moment du lot) ;
- `docs/JSR-309-API.md` : modèle objet, §6.13 (méthodes du §8.1), et le
  §9.8 des call flows pour les relais (§10.8).

**Tests** :
- `test_textmux.cpp` : les règles du §9, sans thread ;
- `test_speech_processor_media.cpp` : un faux `Joinable` qui injecte des
  `RTPPacket` (PCMA, T.140, T.140 RED avec pertes) et un faux écouteur qui
  les recueille et les décode. Sans réseau, sans `speech-server`. Les cas
  sont ceux du §11.2, plus un passage sous `runtests-asan` et
  `runtests-tsan` pour la suppression pendant un relais.

**Commits** : (a) `TextMux` ; (b) SpeechProcessor, API et contrat.

### 10.5 Lot 3 : la reconnaissance

État : **fait** le 2026-10-19. Tests `test_speech_processor_asr.cpp` (vrai
speech-server, faux Voxtral, voix `tts16k.wav` injectée en PCMA à vitesse
réelle) et un test de refus dans `test_speech_processor_xmlrpc.cpp`. Écarts au
plan ci-dessous :

- **Le tour lit l'entrée de chaque port une seule fois**, et la même trame
  sert au relais vers l'autre port et à la reconnaissance. L'ASR hérite donc
  de l'avance de 40 ms du relais.
- **Les événements 8, 11 et 12** sont déclarés dans `SpeechProcessor.h`, pas
  dans `MediaSession.h` : c'est le SpeechProcessor qui les crée. Leur tuple
  commun est `(type, sessionTag, spId, media, port, …)`. Les numéros 9 et 10
  sont réservés dans `JSR309Event::Events` dès ce lot.
- **Les partiels ne sont pas complétés avant le final** : les deltas arrivés
  dans les 250 ms qui précèdent la fin d'énoncé ne sont que dans le final.
- **`SpeechServiceEvent` part aussi au premier `up`**, dès que le flux est
  prêt : le contrôleur sait alors que la reconnaissance tourne.
- **Refus ajouté** : `toEvents` faux avec `textPort` = -1. Le texte reconnu
  n'irait nulle part, alors que l'audio serait payé.
- **`halfDuplex` est accepté et gardé**, sans effet avant le lot 4, puisqu'il
  porte sur la synthèse. Le test du §11.3 qui le concerne passe au lot 4.
- **`SpeechProcessorStopRecognition` abandonne le texte d'un énoncé pas encore
  clos.** `SpeechFlow` n'expose pas de `done`, qui demanderait d'attendre le
  dernier texte.
- **Les call flows du §9.8 qui démarrent la reconnaissance** sont joués par
  `test_speech_processor_asr.cpp` au niveau de l'objet, pas par
  `test_speech_processor_xmlrpc.cpp` : ce dernier n'a pas de speech-server.

- **Par port source** : une `VAD` (`AGGRESSIVE`, 16 kHz, trames de 20 ms),
  un anneau de pré-roulement de 300 ms et un `SpeechFlow` `asr`. Ils sont
  alimentés par le tour de 20 ms, depuis l'anneau d'entrée du port, et
  jamais depuis le thread du décodeur.
- **Transitions de la VAD** : parole → `resume`, pré-roulement, direct ;
  silence de `silenceMs` → `pause`. `SpeechActivity` est émis à chaque
  transition.
- **Fin d'énoncé.** Le protocole ne marque pas la fin du texte d'un
  `flush` (§10.2). L'énoncé est donc clos 500 ms après la `pause` sans
  nouveau delta, ou plus tôt par un `resume`. Si la recette montre que ce
  délai coupe des fins de phrase, un message `flushed` côté speech-server
  (`AsrSession`, quelques lignes) remplacera la déduction. Ce changement
  serait le seul du lot hors du mediaserver.
- **Routage du texte** : vers la file d'événements (regroupés toutes les
  250 ms, puis `final`) et/ou vers le `TextMux` du port `textPort`.
- **`halfDuplex`** : tant que le tampon TTS du port n'est pas vide, la VAD
  de ce port est forcée au silence.
- **Événements** 8, 11 et 12 : des classes dans `MediaSession.h` sur le
  modèle de `PlayerEvent`, et leurs numéros dans `JSR309Event::Events`.
- **API** §8.2 et contrat §8.5. `docs/JSR-309-API.md` : événements 8, 11
  et 12, méthodes du §8.2, encadré « Ce qui se paie », notes
  d'intégration, et call flows complétés (§10.8).
- **Tests** (`test_speech_processor_asr.cpp`) : le vrai `speech-server`, le
  faux Mistral Realtime qui rejoue `fixtures/speech/asr_*.jsonl`, et une
  voix injectée (`fixtures/speech/tts16k.wav`, encodée en PCMA par le
  test). Les cas sont ceux du §11.3.
- **Commit** : `jsr309 : reconnaissance vocale du SpeechProcessor`.

### 10.6 Lot 4 : la synthèse

État : **fait** le 2026-10-19. Tests `test_correction_tracker.cpp` (pur),
`test_speech_processor_tts.cpp` (vrai speech-server, faux Voxtral TTS) et un
test de refus dans `test_speech_processor_xmlrpc.cpp`. Écarts au plan
ci-dessous :

- **Le client envoie des phrases déjà coupées**, pas le T.140 au fil de la
  frappe (§7.1, §7.2). Une copie locale du découpeur aurait pu diverger de
  celle du serveur ; ici chaque envoi est une phrase du serveur, et la
  correspondance `id` ↔ `sayId` est exacte. La latence ne change pas : le
  serveur aurait coupé au même moment. La règle « `flush` avant un `Say` »
  devient sans objet.
- **Une correction se ferme aussi sur le silence de frappe** (1 200 ms), pas
  seulement en fin de ligne : un client SIP n'envoie pas toujours de fin de
  ligne. La phrase dite est la ligne entière corrigée, précédée de
  « Correction : » si l'option est vraie.
- **`SpeechFlow` remonte le refus d'une phrase** (`onRejected`), qui sinon ne
  recevait jamais de `done` : le `Say` ne finissait pas.
- **`StopSynthesis` ne publie pas de Done** pour les `Say` en cours.
- **`halfDuplex` n'est pas testé de bout en bout** : il faudrait les deux
  faux Mistral (Realtime en WebSocket, TTS en HTTP) derrière une seule
  `mistral.url`, ce que speech-server ne permet pas. La recette (§12) le
  vérifie en appel réel.

- **Par port destination** : un `SpeechFlow` `tts` et un tampon de lecture
  (S16, `MixRate`) que vide le tour de 20 ms. Le PCM de `speech-server`
  (16 kHz) est rééchantillonné si `MixRate` change (§4.2).
- **`Say`** : le texte puis `flush`. Le nombre de phrases attendues est
  calculé avec `PhraseSplitter` (§7.2). **Règle ajoutée** : si une phrase
  tapée sur le port source est en cours sur ce flux, le client envoie
  d'abord un `flush`. Sinon, le découpeur du serveur collerait l'annonce à
  la fin de la phrase tapée. Le §7.1 de cette SPEC est complété dans le
  même commit.
- **Source port** : le T.140 décodé part tel quel vers le flux. Le client
  garde une **copie locale de `PhraseSplitter`** pour savoir ce qui a déjà
  été envoyé à la synthèse. Pour que cette copie ne diverge pas du serveur,
  c'est **le client qui coupe sur le silence de frappe** : il envoie
  `flush` après 1 200 ms, sous le `tts.phraseSilenceMs` du serveur
  (1 500 ms). Le serveur ne coupe alors plus jamais seul sur un silence.
- **Corrections** (§7.5) : un `CorrectionTracker` pur suit la ligne en
  cours, avec la part déjà envoyée et la part non envoyée. Un U+0008 qui
  entame la part envoyée déclenche `stop`, puis l'annonce, puis le texte
  retapé jusqu'à la fin de la ligne.
- **`StopSpeaking`** : `stop`, vidage du tampon, puis `Done` avec
  `interrupted=true`.
- **Événements** 9 et 10, **API** §8.3 et contrat §8.5.
  `docs/JSR-309-API.md` : événements 9 et 10, méthodes du §8.3, call flows
  du chatbot et des quatre cas complets (§10.8).
- **Tests** : `test_correction_tracker.cpp` (pur) et
  `test_speech_processor_tts.cpp` (vrai `speech-server`, faux Mistral HTTP
  qui rejoue `fixtures/speech/tts_stream_*`). Les cas sont ceux du §11.4.
- **Commit** : `jsr309 : synthèse vocale du SpeechProcessor`.

### 10.7 Lot 5 : elixip et la recette

Ce lot se fait dans le dépôt elixip, pas dans celui-ci :
- le plan de jambes des quatre cas du §2. La vidéo est branchée par
  `EndpointAttachToEndpoint`, l'audio et le texte sur les ports du
  SpeechProcessor, et le codec de chaque port suit la négociation SDP de
  sa jambe ;
- le chatbot : `final=true` vers le dialogue, `Say` pour les réponses, et
  le barge-in par `SpeechActivity` puis `StopSpeaking` ;
- les alias de voix sont lus par `GET /control/voices` sur speech-server,
  jamais recopiés.

Puis la recette du §12.

### 10.8 La documentation de l'API XML-RPC JSR-309

`docs/JSR-309-API.md` est le contrat qu'un contrôleur lit. Il s'enrichit
**dans le commit de chaque lot qui ajoute une méthode ou un événement**,
pas dans un lot de documentation à part, comme les protos MOTELI (§8.5).

| Section de `JSR-309-API.md` | Ce qui s'ajoute | Lot |
|---|---|---|
| §3 Modèle objet | `SpeechProcessor (spId)` dans l'arbre, avec ses deux ports. Convention : port 0 = A, 1 = B ; Audio et Text seulement | 2 |
| §3 Cycle de vie type | une ligne : créer le SpeechProcessor avant les attaches, le supprimer avant les endpoints | 2 |
| §5 Types d'événements | les types 8 à 12 : tuple XML-RPC exact (`int type, string sessionTag, int joinableId, int media, int port, …`), et pour chacun quand il part et ce que le contrôleur en fait | 3 (8, 11, 12), 4 (9, 10) |
| §6.13 SpeechProcessor (nouvelle) | le tableau des méthodes au format de l'existant (`i sessionId, i spId, i port, …` → `returnVal`), les erreurs propres (port invalide, media Video, « speech service not configured », synthèse non active pour `Say`), le relais automatique, le bruit de confort, le codec texte choisi par `TryCheckCodec` | 2 (§8.1), 3 (§8.2), 4 (§8.3) |
| §6.13, encadré « Ce qui se paie » | l'ASR n'envoie que la parole (VAD), une session `asr` par port source et une `tts` par port destination, `--speech-account` pour ventiler | 3 |
| §8 Notes d'intégration | sans `--speech-url`, seuls les relais fonctionnent ; les alias de voix se demandent à `GET /control/voices` du speech-server ; une panne speech ne coupe jamais l'appel (`SpeechServiceEvent`) ; le barge-in est l'affaire du contrôleur | 3, 4 |
| §9.8 Call flows (nouvelle) | la suite exacte des appels RPC pour chacun des quatre cas du §2 et pour le chatbot, sur le modèle du §9.3 : création, `PortSetCodec` d'après le SDP de chaque jambe, attaches audio et texte sur les ports, vidéo par `EndpointAttachToEndpoint`, `Start*`, puis la terminaison | 2 (relais), complétée aux lots 3 et 4 |

Deux règles pour la rédaction :
- chaque exemple de call flow est **joué par un test** du lot (les tests
  XML-RPC appellent les méthodes dans l'ordre que donne la doc). Ainsi la
  doc ne peut pas dériver du code sans qu'un test casse ;
- `docs/reference/status-http.md` décrit le bloc `speech` de
  `/status/general` au lot 1.

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
- La qualité du relais Opus ↔ Opus à 16 kHz (cas 3 en WebRTC) : si elle
  est jugée insuffisante, passer à 48 kHz (§4.2).
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
   la perte de bande du cas 3 en WebRTC s'entend. Tranché : 16 kHz, et
   48 kHz si la recette juge la qualité insuffisante (§4.2).
4. **La conférence MCU** : la lecture du T.140 d'un participant et les
   annonces sont décrites dans la [SPEC TTS](../TTS/SPEC.md), sur le même
   client speech (§5). (Hors de cette SPEC.). Ok mais pas dans cette phase.
