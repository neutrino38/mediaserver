# Problème

L'audio décompressé circule dans le mediaserver sous forme de `SWORD*` (S16 mono),
la taille et la fréquence voyageant *à côté* des échantillons, à la main. Chaque
frontière re-déclare son propre tampon fixe et son propre contrat implicite de
fréquence — et chacun de ces contrats casse silencieusement dès qu'un codec sort
du monde 8 kHz pour lequel ils ont été écrits.

Ce n'est pas théorique : le test de transcodage opus↔speex16 du **2026-08-14**
a cassé trois de ces contrats en un seul appel, chacun mesuré en capture :

1. `AudioEncoderWorker::Encode` — `recBuffer[512]` face aux trames opus de 960
   échantillons : `RecBuffer` écrit sans borne, **écrasement de pile**, l'objet
   `RTPPacket` voisin corrompu (« packet contains media 851977 »), zéro paquet
   émis vers la jambe opus.
2. `AudioDecoderWorker::Decode` — `raw[512]` : `Decode` borne à `outLen` et
   retient le reste en fifo, 512 échantillons délivrés sur 960 par paquet —
   53 % du débit, latence croissante, l'aval à 25 paquets/s au lieu de 50.
3. `PipeAudioInput::Init` — le décodeur n'apprend sa fréquence qu'au premier
   paquet, souvent APRÈS le `StartRecording` de l'encodeur : resampler ouvert
   sur la fréquence périmée, échantillons 48 kHz lus comme du 16 kHz — audio
   3× trop rapide (83 paquets/s mesurés au lieu de 50).

Les trois ont été corrigés ponctuellement (tampons portés à 8192 + garde-fou,
réouverture du resampler dans `Init`), mais la **classe** de bug demeure : le
dépôt compte encore une dizaine de tampons `SWORD [512]`/`[1024]` du même acabit
(voir §1.3), tous latents pour la même raison, tous armés le jour où un flux
48 kHz les traverse.

Un cas clair : la chaîne RTP → AudioDecoder → PipeAudioInput → AudioEncoder → RTP
du transcodeur JSR-309, où la donnée naît (`avcodec_receive_frame`) et meurt
(`avcodec_send_frame`) déjà en `AVFrame` — la représentation `SWORD*` plate au
milieu est une couche imposée qui perd précisément les métadonnées dont les
trois bugs avaient besoin.

# Décision

Le pendant audio de la migration vidéo vers AVFrame : **les échantillons décompressés
circulent sous forme d'`AVFrame` refcompté** au sein du mediaserver. Un
`AVFrame` audio porte `nb_samples`, `sample_rate`, `ch_layout` et `format`
**avec** les données — les trois bugs du 14/08 deviennent impossibles par
construction :

- plus de tampon appelant à dimensionner (la trame arrive allouée à sa taille) ;
- plus de contrat implicite « outLen suffisant » entre producteur et
  consommateur (on passe la trame entière) ;
- plus d'état de fréquence à synchroniser entre les deux bouts d'un pipe
  (`swr_convert_frame`/`aresample` lisent la fréquence sur la trame entrante et
  se reconfigurent seuls quand elle change).

Mêmes conventions que la reprise manuelle vidéo (décisions du
2026-07-16) : wrapper RAII partagé par `shared_ptr`, migration **remplaçante**
(pas additive).

**Ce qui est remplacé, c'est l'interface virtuelle.** Un implémenteur n'a plus
qu'un seul contrat possible, celui en `SamplesPtr` : aucune classe ne peut
rester sur l'ancien. Mais les anciennes signatures plates survivent le temps de
la migration comme **adaptateurs non virtuels**, écrits une fois dans
`AudioEncoder`/`AudioDecoder` (`audio.cpp`), qui reposent sur les nouvelles.
C'est ce qui permet à chaque phase de finir **build vert** (§6) sans renoncer au
remplacement : les chaînes du mcu non encore migrées continuent d'appeler
`Encode`/`Decode` à plat, et l'adaptateur **borne l'écriture** qu'elles ne
bornaient pas — la classe de bug du 14/08 disparaît donc chez elles avant même
leur migration. Les deux adaptateurs partent avec leur dernier appelant, à la
fin de la phase 5.

# Travail demandé

- ✅ définir le type de transport `SamplesPtr` (wrapper RAII d'`AVFrame*`, modèle
  `PictPtr`) dans libmedikit ;
- migrer les interfaces `AudioDecoder`/`AudioEncoder`/`AudioInput`/`AudioOutput`
  de `medkit/audio.h` (le `mcu/include/audio.h` n'est qu'une redirection — un
  seul header canonique à toucher) — ✅ pour les deux premières,
  `AudioInput`/`AudioOutput` suivent avec les pipes (phase 2) puisque leurs
  seuls implémenteurs vivent dans le mcu ;
- remplacer la fifo `SWORD` + resampler manuel de `PipeAudioInput`/`PipeAudioOutput`
  par une file de trames + `aresample` ;
- migrer chaîne par chaîne : transcodeur JSR-309 d'abord (la chaîne du 14/08),
  puis mixer/conférence, puis player/recorder, puis les chemins RTMP/bridge ;
- consigner ici la conception et tenir ce plan à jour au fil des phases.

# Conception

## 1. Cartographie de l'existant

### 1.1 Le « type » d'échantillon aujourd'hui

`medkit/audio.h` impose le contrat `SWORD*` à quatre interfaces :

- `AudioEncoder::Encode(SWORD *in, int inLen, BYTE *out, int outLen)` — consomme
  exactement `numFrameSamples` échantillons à la fréquence `GetRate()`, que
  l'appelant doit connaître et fournir ;
- `AudioDecoder::Decode(BYTE *in, int inLen, SWORD *out, int outLen)` — restitue
  au plus `min(numFrameSamples, outLen)` échantillons par appel, le surplus
  retenu dans une fifo interne (ffaudiocodec) que l'appelant ignore ;
- `AudioInput::RecBuffer(SWORD*, DWORD size)` / `StartRecording(rate)` — lit
  `size` échantillons **sans connaître la taille du tampon appelant** ;
- `AudioOutput::PlayBuffer(SWORD*, DWORD, DWORD frameTime)` / `StartPlaying(rate)`.

`AudioFrame` (`medkit/media.h`) ne décrit que le **bitstream compressé** — il
n'est pas concerné et reste tel quel (packetization RTP comprise).

### 1.2 La fréquence voyage par poignées de main

Trois mécanismes distincts, non synchronisés, se partagent la vérité :

- `TrySetRate`/`GetRate` entre worker et codec (le décodeur ne connaît sa
  fréquence réelle qu'après ouverture, donc au premier paquet) ;
- `PipeAudioInput::Init(rate)` (côté écrivain) vs `StartRecording(rate)` (côté
  lecteur), le resampler n'étant (r)ouvert que par le second — le bug n°3 ;
- des resamplers privés supplémentaires dans `ffaudiocodec` (conversion S16
  mono à fréquence égale) et `ffmp4reader` (dpcm).

### 1.3 Inventaire des tampons fixes (tous porteurs de la même classe de bug)

| Site | Tampon | État |
|---|---|---|
| `jsr309/AudioEncoderWorker.cpp` | `recBuffer[8192]` + garde | **supprimés** (phase 2) |
| `jsr309/AudioDecoderWorker.cpp` | `raw[8192]` | **supprimé** (phase 2) |
| `pipeaudioinput.cpp` | `resampled[4096]` + `fifo<SWORD,4096>` | **supprimés** (phase 2) |
| `pipeaudiooutput.cpp` | `resampled[4096]` | **supprimé** (phase 2) |
| `audiostream.cpp:321,437` | `playBuffer[1024]`, `recBuffer[512]` | latent (chaîne conférence SIP) |
| `audioencoder.cpp:154`, `audiodecoder.cpp:83` | `[512]` | latent (streams du mixer) |
| `mediabridgesession.cpp` ×3 | `[512]` | latent (bridge RTP↔RTMP) |
| `rtmpparticipant.cpp` ×2, `rtmpmp4stream.cpp:229` | `[512]` | latent |
| `mp4player.cpp:89` | `buffer[1024]` | latent |
| `jsr309/Recorder.cpp:188` | `pcm[4096]` | suffisant ≤ 85 ms @48k |
| `ffaudiocodec.cpp` (décodeur) | `conv[8192]` + `fifo<SWORD,8192>` | **supprimés** (phase 1) |
| `ffmp4reader.cpp:648` | `dpcm[8192]` + `pcmFifo` | latent (migré en phase 4) |

La migration supprime ces tampons plutôt que de les redimensionner un à un.

### 1.4 Les extrémités sont déjà « AVFrame-natives »

Comme pour la vidéo : `FfAudioDecoder` reçoit chaque trame dans un `AVFrame`
(`avcodec_receive_frame`) puis l'**aplatit** dans sa fifo `SWORD` ;
`FfAudioEncoder` reconstruit un `AVFrame` autour du `SWORD*` d'entrée avant
`avcodec_send_frame`. Les codecs à la main (G.711, GSM, G.722) sont trivialement
adaptables (une trame = un memcpy dans un `AVFrame` alloué). La couche plate ne
sert donc qu'à perdre `nb_samples` et `sample_rate` entre deux endroits qui les
possèdent déjà.

## 2. Principes directeurs

1. **`AVFrame` refcompté = unité de transport.** Une trame décodée est publiée
   une fois, immuable, partagée par référence (`av_frame_ref`), libérée quand
   son dernier lecteur la relâche. Les files (pipe, mixer) stockent des
   `SamplesPtr`, jamais des échantillons copiés.
2. **Auto-descriptif, pas format-agnostique.** Contrairement à la vidéo (GPU,
   formats multiples), l'audio interne reste S16 mono — mais la trame DIT sa
   fréquence et son nombre d'échantillons, et tout consommateur convertit
   explicitement via `aresample` s'il attend autre chose. Aucune conversion
   implicite, aucune fréquence supposée.
3. **Migration remplaçante** (même décision que côté vidéo, 2026-07-16) : les signatures
   `SWORD*` disparaissent de `medkit/audio.h`, build rouge tant que tous les
   implémenteurs et appelants n'ont pas basculé, chaîne par chaîne.
4. **La logique métier reste.** VAD, niveaux du mixer, élection du locuteur,
   packetization RTP : des décisions, pas des échantillons. Seul le transport
   change.
5. **Le réassemblage en trames fixes appartient à l'encodeur.** C'est le seul
   à connaître `numFrameSamples` : il embarque un `av_audio_fifo` et accepte
   des trames de n'importe quelle taille. Plus personne d'autre ne découpe.

## 3. Modèle d'ownership : `Samples` / `SamplesPtr`

Dans `medkit/audio.h`, le jumeau de `Pict` (`medkit/video.h`) :

- `Samples` : wrapper RAII minimal d'un `AVFrame*` — destructeur
  `av_frame_free`, copie interdite, partage par `SamplesPtr =
  std::shared_ptr<Samples>` uniquement ;
- accesseurs : `GetAVFrame()`, `GetRate()` (= `sample_rate`), `GetNbSamples()`,
  `GetPTS()`, `GetData()` (plan 0 en `SWORD*` — valide car S16 mono garanti par
  construction, assert sinon) ;
- fabriques : `Samples::Alloc(nb, rate)` (tampon neuf refcompté) et
  `Samples::FromAVFrame(AVFrame*)` (adoption, pour la sortie décodeur).

Pas de pendant GPU : l'audio n'a ni surfaces matérielles ni politique
d'upload/download.

## 4. Interfaces migrées (`medkit/audio.h`)

```
AudioDecoder :  int        Decode(BYTE *in, int inLen);   // consomme le paquet
                SamplesPtr GetFrame();                    // 0..n trames par paquet,
                                                          // boucler jusqu'à nullptr
AudioEncoder :  AudioFrame* EncodeFrame(SamplesPtr);      // av_audio_fifo interne :
                                                          // accepte toute taille,
                                                          // émet à numFrameSamples,
                                                          // nullptr si pas assez
AudioInput   :  SamplesPtr RecFrame(DWORD timeoutMs);     // remplace RecBuffer
AudioOutput  :  int        PlayFrame(SamplesPtr);         // remplace PlayBuffer
```

`EncodeFrame` est **symétrique de `GetFrame`** : une trame d'entrée peut en
remplir plusieurs, l'appelant boucle en repassant `nullptr` pour purger.
Concaténer les trames excédentaires dans une seule `AudioFrame` serait un bug —
une trame codée d'un codec « frame-based » (Opus, AMR…) doit tenir dans UN
paquet RTP. La trame rendue appartient à l'encodeur et vaut jusqu'à l'appel
suivant, comme `VideoEncoder::EncodeFrame`.

- `Decode`/`GetFrame` : calqué sur `VideoDecoder::GetFrame()→PictPtr` — chaque
  trame dans un `AVFrame` NEUF, jamais de re-référence du frame interne
  (le piège `avcodec_receive_frame` déjà évité côté vidéo).
- `StartRecording(rate)`/`StartPlaying(rate)` deviennent des **préférences de
  sortie** : le consommateur déclare la fréquence qu'il veut recevoir, la
  conversion vit dans le fournisseur (pipe) via `aresample`, reconfiguré par
  trame entrante. `TrySetRate` disparaît des décodeurs (la trame fait foi) et
  reste aux encodeurs (choix de la fréquence d'encodage).
- `frameTime`/pts : porté par la trame (`AVFrame.pts`, base `1/sample_rate`),
  plus d'argument séparé. L'aval RTP (timestamps, SSRC au restart — cf.
  correctifs du 14/08 dans `RTPEndpoint`) reste l'autorité d'horodatage filaire.

## 5. Le pipe : une file de trames + `aresample`

`PipeAudioInput`/`PipeAudioOutput` perdent leurs trois rôles enchevêtrés
(fifo d'échantillons, resampler manuel, synchronisation de fréquences) :

- une **file bornée de `SamplesPtr`** (profondeur en millisecondes, politique
  actuelle conservée : on vide en cas de débordement plutôt que de bloquer le
  producteur) ;
- côté lecteur, un `aresample` paresseux vers la fréquence demandée par
  `StartRecording`, nourri trame par trame — un changement de fréquence
  d'écriture (le décodeur qui découvre 48 kHz au premier paquet) est absorbé
  sans réouverture ni état partagé : c'est le bug n°3 supprimé par conception ;
- `Init(rate)` disparaît (la trame porte sa fréquence).

## 6. Chaînes migrées, dans l'ordre

1. **Codecs libmedikit** — ✅ **FAIT**. `ffaudiocodec` a cessé d'aplatir : la
   fifo `fifo<SWORD,8192>` du décodeur est remplacée par une file de
   `SamplesPtr`, chaque trame décodée étant publiée telle quelle par
   `av_frame_move_ref` (zéro recopie). `FfAudioEncoder` porte l'`av_audio_fifo`
   et rééchantillonne d'après la fréquence **portée par la trame**, qu'il
   reconfigure à chaud — après avoir vidé l'ancien resampler, sinon le
   changement de fréquence ferait un trou. Les deux files sont **bornées à une
   seconde**, avec journalisation du rejet : plus aucun tampon sans limite.

   G.711 est **passé par ffmpeg** (`AV_CODEC_ID_PCM_ALAW`/`PCM_MULAW`) : les
   quatre classes écrites à la main et les tables `g711.c` ont disparu, PCMA/PCMU
   dérivent de `FfAudio{En,De}coder` comme tous les autres. Les encodeurs PCM
   n'annonçant aucun `frame_size`, `Open()` impose la tranche de 20 ms — c'est
   ce qui garde `numFrameSamples = 160`, valeur sur laquelle le mcu dimensionne
   ses lectures. Aucun autre codec n'avait de code propre à adapter : tous
   dérivaient déjà de la base ffmpeg.

   Suite de tests créée : `tests/test_audio_codecs.cpp` (15 tests), avec un test
   par bug du 14/08 — tampon de sortie trop court, trame tronquée au décodage,
   changement de fréquence en cours de flux.
2. **Transcodeur JSR-309** (`AudioDecoderWorker`, `AudioEncoderWorker`,
   `PipeAudioInput`) — ✅ **FAIT**. La chaîne du 14/08 ne porte plus un seul
   tampon fixe : les `raw[8192]`/`recBuffer[8192]` et le garde-fou
   `numFrameSamples` sont partis avec `RecBuffer`.

   `AudioInput`/`AudioOutput` passent à `RecFrame(timeoutMs)`/`PlayFrame`,
   `PipeAudioInput` à une file de `SamplesPtr` bornée en **durée** (500 ms) avec
   `aresample` reconfiguré par la fréquence de la trame entrante. `Init(rate)`
   ne pilote plus rien : il ne reste que pour dire leur fréquence aux
   producteurs encore en `SWORD*` (mixeur, bridge). `PipeAudioOutput` reçoit le
   même traitement côté `PlayFrame`, sa `fifo` plate restant pour le mixeur
   (phase 3).

   Deux corrections de fond au passage : le décodeur publie **toutes** les
   trames d'un paquet (l'appel unique en perdait), et l'horloge RTP de
   l'encodeur n'avance plus que sur ce qui est **réellement émis**.

   Le critère de sortie est joué hors ligne par
   `AudioTranscodeChain.OpusQuaranteHuitVersSpeexSeize`
   (`mcu/tests/test_audio_pipes.cpp`) : 50 trames opus 48 kHz entrées, 50 trames
   speex 16 kHz sorties. La recette en appel réel reste à faire.
3. **Conférence** (`audiostream`, `audioencoder`/`audiodecoder`, `audiomixer`,
   `PipeAudioOutput`) — le mixer consomme des `SamplesPtr` par participant,
   `aresample` par entrée vers la fréquence de mixage ; VAD inchangé.
4. **Player/Recorder/MP4** (`mp4player`, `jsr309/Recorder`, `ffmp4reader` —
   ce dernier a déjà son resampler interne à absorber).
5. **RTMP et bridge** (`rtmpparticipant`, `rtmpmp4stream`,
   `mediabridgesession`) — à évaluer : si ces chemins sont morts en production,
   les geler plutôt que les migrer (décision à prendre avant la phase).

Chaque phase se termine build vert. Le critère de sortie de la phase 2 est le
test du 14/08 rejoué : opus↔speex16, **50 paquets/s dans chaque sens**,
re-INVITE compris, sans un seul tampon fixe sur le chemin.

## 7. Risques et pièges

- **Trames de taille variable vs encodeurs à trame fixe** : réglé par
  l'`av_audio_fifo` DANS l'encodeur (§2.5) — nulle part ailleurs.
- **Discipline de refcount à travers les threads** : mêmes règles que `PictPtr`
  (partage par `shared_ptr` uniquement, pas d'`AVFrame*` nu qui s'échappe).
- **Latence** : la file du pipe doit rester bornée en durée, pas en trames
  (une trame peut faire 10 ou 120 ms).
- **Layout** : mono partout aujourd'hui ; `Samples` le vérifie à la
  construction, la stéréo (opus 2 canaux) est down-mixée à l'entrée comme
  aujourd'hui (resampler du décodeur).
- **Hors périmètre** : l'API XML-RPC ne change pas (donc pas d'impact
  `moteli_*.proto`), la négociation SDP ne change pas, `AudioFrame` compressé
  et la packetization RTP ne changent pas.
