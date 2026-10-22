# Medooze mediaserver 1.19.0

Le SpeechProcessor JSR-309 : un appelant entendant entend ce que tape un appelant sourd, et le texte reconnu de la voix part en T.140 ou en événements. Le serveur dit quand la parole ne peut pas servir. AMR et AMR-WB au bon format sur le fil

## Mise à jour : ce qui change pour l'exploitant

- **Trois nouvelles options** relient le mediaserver à speech-server. Elles se placent dans `OPTIONS=` de `/etc/sysconfig/mediaserver` :
  - `--speech-url` : port de contrôle de speech-server, où le mediaserver demande ses jetons
  - `--speech-streaming-url` : port de streaming (par exemple `wss://speech.example.net:9201`). Obligatoire avec `--speech-url`
  - `--speech-account` : compte nommé dans les demandes de jeton, pour ventiler les compteurs
- Sans ces options, rien ne change : le SpeechProcessor relaie l'audio et le texte, sans reconnaissance ni synthèse
- Le mediaserver est maintenant lié à **libcurl**
- `/status/general` du mediaserver publie un bloc `speech` : `available`, `unavailableReason`, `checkedAt`. Le mediaserver interroge l'état de speech-server toutes les 5 s. Un contrôleur doit lire ce bloc avant d'offrir la parole à un appel
- speech-server sonde Mistral toutes les `mistral.probeSecs` (30 s par défaut, 0 l'arrête). Le résultat est dans `backend.mistral` de son `/status/general`
- Le journal de speech-server est écrit ligne par ligne. Avant, une panne de Mistral n'apparaissait qu'au remplissage du tampon

## SpeechProcessor (JSR-309)

Un nouvel objet JSR-309 relie **deux ports**, A et B. Chaque port se branche sur l'audio et le texte d'un Endpoint. Contrat complet : `docs/JSR-309-API.md` §6.13.

- **Relais** : l'audio est décodé, mélangé à un bruit de confort, puis réencodé pour chaque port. Le texte part en T.140 RED si l'Endpoint l'accepte, en T.140 simple sinon. Ce relais marche sans speech-server : il remplace un transcodeur audio et texte entre deux jambes
- **Reconnaissance** (`SpeechProcessorStartRecognition`) : le texte reconnu part en événements, sur la sortie T.140 d'un port, ou les deux. Une détection de voix décide de ce qui part chez Mistral, qui facture chaque seconde reçue. La parole est déclarée après 250 ms de voix. Un souffle ou un clic ne coupe donc plus la phrase en cours
- **Synthèse** (`SpeechProcessorStartSynthesis`, `SpeechProcessorSay`, `SpeechProcessorStopSpeaking`) : le texte tapé sur un port est dit sur l'autre. Une phrase part sur `.` `?` `!` `:` `…`, en fin de ligne, ou sur un blanc après plus de cinq mots en attente. Un mot n'est jamais coupé
- **Corrections** : le premier retour arrière dans un texte déjà dit coupe la voix et fait dire « Correction ». La phrase touchée est redite quand la ligne est stable depuis 2 s
- Un `Say` envoyé avant que le flux de synthèse soit prêt attend ce flux. Un flux **perdu** clôt aussitôt les `Say` en cours, comme interrompus : rien n'est dit en retard
- Nouveaux événements 8 à 12 : texte reconnu, début et fin de synthèse, activité de parole, service disponible ou non
- Le client Java XML-RPC est aligné sur l'API

## Texte temps réel

- Une sortie T.140 peut être partagée entre deux sources (texte relayé et texte reconnu). Une source garde la sortie jusqu'à la fin de ligne ou 1 s de silence. Un retour arrière n'efface jamais le texte de l'autre source
- Encodeur RED : quatre défauts corrigés. La dernière frappe est de nouveau protégée deux fois (RFC 4103), et deux fuites mémoire disparaissent
- Un port texte basculé sur un data channel est initialisé. Avant, le texte temps réel sur data channel ne marchait jamais par l'API JSR-309

## AMR et AMR-WB

- Les trames partent et arrivent au format RFC 4867 : octet-aligned si le fmtp l'a négocié, bandwidth-efficient sinon. Avant, elles partaient brutes, et un correspondant en AMR-WB n'entendait rien d'utilisable
- Un correspondant en DTX garde ses silences : les décodeurs passent par libopencore quand ffmpeg l'embarque
- `docs/CODECS.md` est réécrit d'après le code. Il décrit le piège : sans libopencore dans ffmpeg, le serveur se replie sans erreur sur les décodeurs natifs, qui rejettent les trames de silence

## speech-server

- Plus de `flush` envoyé à Mistral sans audio depuis le précédent. Mistral le refusait et fermait la session : un appelant muet au départ perdait toute la reconnaissance

## Diagnostic

- Le SpeechProcessor écrit dans le journal chaque `Say`, sa fin, chaque arrêt, et chaque début et fin de parole entendue
- `make TRACE_EXCHANGE=yes` fait écrire le texte des conversations (texte reçu, phrases coupées, phrases dites). C'est le contenu des appels : l'option est désactivée par défaut et absente du binaire livré

# Limitations

- AMR et AMR-WB n'ont pas été essayés en trafic réel. Un `mode-set` dans la réponse à notre offre n'est pas lu, et l'encodeur MCU ignore `mode-set`
- La recette du SpeechProcessor (§12 de `docs/conception/SPEECH-PROCESSOR/SPEC.md`) n'est pas terminée
- `GetSupportedCodecs` (XML-RPC `/mcu`) reste une liste écrite à la main. Interroger `/status/general` à la place
