# Synthèse vocale en conférence (`/mcu`)

> Statut : **conception, rien n'est codé**.
> Décision d'architecture : [ADR 003](../../architecture/adr-003-tts-serveur-websocket.md)
> (deux binaires), moteur remplacé par l'[ADR 005](../../architecture/adr-005-speech-server-mistral-en-ligne.md)
> (Mistral en ligne).
>
> Ce document ne garde que la **conférence**. Le reste est décrit ailleurs,
> une seule fois :
> - le serveur, son protocole et le découpage en phrases :
>   [SPEC speech-server](../SPEECH-SERVER/SPEC.md) (§7.3) ;
> - le client speech du mediaserver (jetons, flux, reprise) et tout le
>   JSR-309 : [SPEC SpeechProcessor](../SPEECH-PROCESSOR/SPEC.md).
>
> « Contrôleur » désigne l'appelant qui pilote le serveur en XML-RPC (elixip).

## 1. Objectif

Deux usages, dans une conférence MCU :

**Usage 1 : lire le texte T.140.** Un participant sourd écrit en texte temps
réel. Les autres participants entendent son texte, lu par une voix de
synthèse.

**Usage 2 : les annonces.** Le contrôleur demande de prononcer une phrase.
Par exemple : « Vous êtes en attente. » ou « Marie a rejoint la
conférence. ».

## 2. Ce qui existe (inventaire vérifié)

- **Injecter de l'audio.** `PipeAudioOutput::PlayFrame(SamplesPtr)`
  (`mcu/src/pipeaudiooutput.cpp`) accepte du S16 mono à n'importe quelle
  fréquence, et le rééchantillonne vers celle du mixeur.
  `MultiConf::CreatePlayer` réserve un port du mixeur par
  `audioMixer.CreateMixer(id)`. Le synthétiseur prend un port de la même
  façon.
- **Lire le texte T.140.** Les caractères décodés existent dans
  `TextMixer::Run` (`mcu/src/textmixer.cpp`), qui lit chaque participant par
  `output->ReadText(buffer, size)`, toutes les 200 ms au plus.
- **Les événements.** `MCU` publie par `eventMngr->AddEvent(queueId, event)`.
  **Il n'existe aucun événement de fin de lecture** : `MP4Player::onEnd` ne
  fait que reboucler. L'événement de fin de phrase sera le premier de ce
  genre.

## 3. Architecture

```
TextMixer ──texte T.140──▶ Synthesizer ── flux tts (client speech) ──▶ speech-server
                              │  ◀──────────────── PCM ───────────────┘
                              ▼
                        PipeAudioOutput (port du mixeur)
```

Un `Synthesizer` de conférence tient un flux `tts`, ouvert par le client
speech commun (SPEC SpeechProcessor §5). Le texte T.140 lui est passé
**tel quel**. Le découpage en phrases et le retour arrière sont traités par
`speech-server`. La correction de texte déjà dit suit la même règle que
dans le SpeechProcessor (§7.5).

## 4. Usage 1 : le texte T.140 lu à voix haute

`TextMixer::Run` lit déjà le texte de chaque participant. Pour un
participant dont la lecture est active, il passe aussi ce texte à son
synthétiseur.

**Question ouverte : l'auteur entend-il sa voix ?**

- **Option A** : un port de mixeur à part. Tout le monde entend la voix,
  auteur compris. C'est la solution simple.
- **Option B** : la voix s'ajoute à l'audio du participant. Le mixeur
  l'exclut de son propre mix, et la VAD le montre « en train de parler ».
  Mais le port du participant est déjà alimenté par son audio décodé. Deux
  producteurs dans un même `PipeAudioOutput` s'intercaleraient au lieu de
  s'additionner : il faut ajouter un étage de somme.

Recommandation : **B**. Un auteur entendant qui tape serait gêné d'entendre
sa voix, et le participant qui « parle » par le texte doit apparaître comme
orateur dans la mosaïque.

## 5. Usage 2 : les annonces

Le contrôleur crée un synthétiseur, lui envoie des phrases, et reçoit un
événement à la fin de chacune. Plusieurs phrases s'enchaînent dans l'ordre.
`Stop` coupe la phrase en cours et vide la file.

## 6. API de contrôle (`/mcu`)

| Méthode | Paramètres | Retour |
|---|---|---|
| `CreateSynthesizer` | confId, nom, voix | synthId |
| `DeleteSynthesizer` | confId, synthId | — |
| `SynthesizerSay` | confId, synthId, texte | sayId |
| `SynthesizerStop` | confId, synthId | — |
| `SetParticipantTextToSpeech` | confId, partId, voix (vide = arrêt) | — |

Événements : `SynthesizerSaid` (synthId, sayId) et `SynthesizerFailed`
(synthId, sayId, raison).

Contrat : protos MOTELI v2 d'elixip dans le même jeu de changements,
clients Java, `docs/MCU-API.md`. La voix est un alias de `speech-server`,
jamais recopié chez le contrôleur.

## 7. Lots

Ces lots viennent **après** le lot 1 de la SPEC SpeechProcessor (le client
speech).

| Lot | Contenu | Condition de fin |
|---|---|---|
| 1 | `Synthesizer` de conférence et annonces : API, événements | Tests §8 verts |
| 2 | Lecture du T.140 d'un participant (option retenue au §4) | Tests §8 verts |
| 3 | Recette en appel réel (§9) | Recette passée |

## 8. Tests

Contre le vrai `speech-server` et le faux Mistral, sans réseau :

- `SynthesizerSay` : le PCM arrive au port du mixeur, puis
  `SynthesizerSaid` est émis ;
- `SynthesizerStop` au milieu d'une phrase : plus d'audio, et un événement
  d'échec pour chaque phrase abandonnée ;
- le texte T.140 d'un participant est entendu par les autres, et entendu
  ou non par lui selon l'option du §4 ;
- `speech-server` absent au démarrage n'empêche pas le mediaserver de
  démarrer.

## 9. Recette

- Une conférence de trois participants. L'un tape du T.140 en RTP, un autre
  en data channel depuis un navigateur. Les autres entendent les phrases.
- Arrêt de `speech-server` pendant une phrase : la conférence continue,
  l'événement d'échec arrive, et la synthèse reprend après le redémarrage.

## 10. Questions ouvertes

1. L'auteur entend-il sa voix (§4) ? Recommandation : non (option B).
2. Les langues : le français seul, ou une voix par participant ?
