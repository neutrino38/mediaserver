# Serveur de traitement de la voix (`speech-server`)

> Statut : lots 0 à 5 faits (§4.2, §13.3 à §13.7) ; l'installation Debian
> et Ubuntu reste à vérifier.
> Version cible : **1.18.0 pour le serveur seul (lots 0 à 5)** ; le
> mediaserver client (lot 6) suit en 1.19.0.
> Décision d'architecture : **le calcul de la voix est fait en ligne, par
> Mistral AI ; aucun moteur local** (tranché le 2026-10-13). L'ADR 005 qui le
> consigne est à écrire ; elle garde de l'ADR 003 la séparation en deux
> binaires et en remplace le moteur.
> Prérequis vérifié : le client WebSocket (`WebSocketServer::Connect`,
> branche `feat/wss-client`) est fusionné dans `release/1.18.0`.
>
> Vocabulaire. **ASR** (*automatic speech recognition*) : transformer la voix
> en texte. **TTS** (*text-to-speech*) : transformer le texte en voix.
> **COMP** : corriger et compléter un texte en cours de frappe. **T.140** :
> norme du texte temps réel, caractère par caractère, avec un caractère
> d'effacement (U+0008) et un séparateur de ligne (U+2028). **Application** :
> le serveur web qui sert les pages aux utilisateurs et qui demande les jetons.
> **Client** : le navigateur, ou le mediaserver, qui ouvre le flux.

## 1. Objectif

Un binaire à part, `speech-server`, rend trois services en flux :

| Fonction | Entrée | Sortie |
|---|---|---|
| `asr` | voix (PCM) | texte temps réel |
| `tts` | texte temps réel | voix (PCM) |
| `comp` | un morceau de texte | le même, corrigé (orthographe et grammaire) |

Le calcul est fait par **Mistral AI**, en ligne. Le serveur ne porte aucun
modèle. Il tient les connexions des clients, vérifie les jetons, adapte les
formats, borne la charge, et compte ce qui est consommé.

Usage visé : un utilisateur sourd ou malentendant, dans un navigateur, lit la
voix de son interlocuteur (`asr`), parle par le texte (`tts`), et fait
corriger son texte avant qu'il soit dit (`comp`), pour que la synthèse parte
d'une bonne base. Le mediaserver est un second client, pour
les usages déjà décrits dans [TTS](../TTS/SPEC.md) : lecture du T.140 d'un
participant et annonces.

## 2. Périmètre

| Sujet | Dedans | Dehors |
|---|---|---|
| Fournisseur | Mistral AI, API hébergée | modèle local (Piper, Voxtral en vLLM) |
| Clients | navigateur, mediaserver | Asterisk, SIP |
| Transport | WebSocket, `ws://` et `wss://` | WebRTC, RTP |
| Audio | PCM S16LE mono 16 kHz, dans les deux sens | codecs (Opus, G.711) |
| Jeton | créé par l'application, filtré par IP | compte utilisateur, OAuth |
| Réseau | streaming en IPv4 et IPv6 (double pile), contrôle en IPv4 (§6.1) | proxy HTTP sortant, IPv6 sur le port de contrôle |

Le serveur ne résout pas la détection de langue par lui-même : il transmet
ce que Mistral sait faire, et rien de plus (§7.2).

## 3. Ce qui existe (inventaire vérifié le 2026-10-13)

| Élément | Emplacement | Réutilisé |
|---|---|---|
| Réacteur WebSocket mono-thread, serveur **et** client, `ws://` et `wss://` | `mcu/src/websocketserver.cpp`, `WebSocketServer::Connect` | tel quel |
| Refus avant le 101 avec un code HTTP | `WebSocket::Reject(code, reason)` | tel quel |
| Envoi depuis un autre thread | `WebSocketConnection::SendMessage`, thread-safe, réveille le réacteur | tel quel |
| Serveur HTTP Abyss | `mcu/src/xmlrpcserver.cpp` | avec un second constructeur, **IPv4 seul** pour ce serveur (§6.1) |
| Gestionnaire HTTP sur Abyss, deux rendus JSON et texte | `mcu/src/statushandler.cpp` | modèle à suivre |
| Adresses, littéraux, résolution A + AAAA | `mcu/include/ipaddress.h` | tel quel, **un ajout** : test d'appartenance à un préfixe (`a.b.c.d/24`, `2001:db8::/32`), absent |
| Threads de travail | `mcu/include/worker.h` | tel quel |
| Log | `mcu/include/log.h` | tel quel |
| Lecture JSON | **aucune** : `statushandler.cpp` ne fait que produire du JSON | à ajouter (§4.3) |
| Client HTTPS | **aucun** : pas de libcurl, pas de client HTTP maison | à ajouter (§4.3) |
| Détection de voix dans l'audio | `third_party/libvad` (libfvad), déjà bâtie | option du §7.4 |

Le dépôt ne porte ni coffre à secrets, ni fichier de configuration : le
mediaserver lit ses options sur la ligne de commande, depuis
`/etc/sysconfig/mediaserver` (`OPTIONS=`), sourcé par l'unité systemd.

## 4. Ce que Mistral offre (vérifié dans sa documentation le 2026-10-13)

### 4.1 Les trois services

| Fonction | Service Mistral | Protocole | Modèle |
|---|---|---|---|
| `asr` | Voxtral Realtime | WebSocket `wss://api.mistral.ai/v1/audio/transcriptions/realtime?model=…` | `voxtral-mini-transcribe-realtime-2602` |
| `tts` | Voxtral TTS, mode streaming | HTTPS, réponse en flux | `voxtral-mini-tts-2603` |
| `comp` | Chat completions, sans flux | HTTPS, `POST /v1/chat/completions` | `mistral-small-latest` par défaut (option `comp.model`) |

Faits utiles, et ce qu'ils imposent :

- **ASR.** Entrée `pcm_s16le`, 16 kHz mono, en trames binaires : c'est
  exactement notre format, aucune conversion. Le texte sort en **deltas
  ajoutés à la fin, jamais corrigés**. Le paramètre
  `target_streaming_delay_ms` règle le compromis délai contre justesse
  (240 ms à 2 400 ms). 13 langues dont le français, **détectées, jamais
  forcées**.
  Prix affiché : 0,006 $ par minute d'audio.
- **TTS.** Sortie `pcm` en **float32 petit-boutiste, 24 kHz** (mesuré,
  §4.2) : le serveur convertit en S16LE 16 kHz. Premier son mesuré à 0,47 s.
  9 langues dont le français. Pas de détection de langue : la langue vient
  de la voix, et **aucune voix française n'est fournie** (§4.2). Une voix est un `voice_id`, créé par
  l'API Voices depuis un échantillon, ou fourni par Mistral. Texte conseillé :
  moins de 300 mots par requête. Prix affiché : 0,016 $ par millier de
  caractères. **Une modération** peut refuser un texte avec un 403.
- **COMP.** Aucun service dédié : c'est une complétion de chat avec un prompt
  à écrire, et une réponse JSON demandée au modèle. Latence de l'ordre de
  quelques centaines de millisecondes par requête ; le coût est au token.

### 4.2 Ce que la sonde du lot 0 a établi (2026-10-13, clé de test, dev71)

Les sept points « à vérifier » sont remplacés par les faits observés. Les
enregistrements bruts, anonymisés, sont dans `mcu/tests/fixtures/speech/`.

**ASR, Voxtral Realtime** (`wss://api.mistral.ai/v1/audio/transcriptions/realtime?model=voxtral-mini-transcribe-realtime-2602`)

| Point | Fait |
|---|---|
| Authentification | en-tête `Authorization: Bearer <clé>` sur l'Upgrade |
| Audio | **trames binaires** WebSocket, PCM S16LE 16 kHz mono, telles quelles (480 ms par trame dans la sonde) ; `session.created` annonce `audio_format: {pcm_s16le, 16000}` |
| Messages client | `session.update` (`{"type":"session.update","session":{…}}`), `input_audio.append` (base64, non utilisé), `input_audio.flush`, `input_audio.end` ; tout autre `type` donne `error` puis fermeture 1011 |
| Messages serveur | `session.created`, `session.updated`, `transcription.text.delta` (`text`, **ajout seul**), `transcription.done` (`text` complet, `usage`), `error` (`error.message`, `error.code`) |
| Langue | **pas de forçage** : `language` dans `session.update` donne « does not support language forcing », erreur 3051 puis fermeture. Le paramètre `lang` du §7.1 est donc ignoré pour `asr`, et `ready` le dit |
| Délai | `target_streaming_delay_ms` accepté par `session.update`. Mesuré, premier mot après le début de l'audio : 240 ms → 1,5 s ; défaut (null) → 1,6 s ; 2 400 ms → 3,1 s. Texte identique et exact dans les trois cas sur la phrase de test |
| Fin | `input_audio.flush` rend un `transcription.done` avec tout le texte restant ; `input_audio.end` rend un `transcription.done` vide ; puis Mistral ferme la connexion |
| Silence | sans audio pendant ~59 s : `error` 3804 « Timeout waiting for response », puis fermeture **1013** à ~62 s. Une pause longue doit donc fermer et rouvrir (`asr.pauseCloseSecs` = 30 s est en dessous) |
| Facturation | `usage.prompt_audio_seconds` par session (4 pour 4,9 s d'audio : à la seconde entière) ; 0,006 $ la minute (fiche modèle) |
| Texte final | après `input_audio.flush`, les derniers deltas arrivent, puis `transcription.done` porte **tout** le texte du segment, deltas compris. Le serveur ne relaie que la partie que les deltas n'ont pas portée |
| Stabilité | le 2026-10-14 vers 9 h 30 (UTC), 6 sessions sur 8, envoyées au rythme réel, ont été fermées par Mistral : `error` 3800 (`vllm QueueOverflowError`), puis fermeture 1013 « Upstream session closed before audio was transcribed ». Le même jour vers 9 h 20, une session complète a réussi. La cause est chez Mistral : le rythme d'envoi n'y change rien d'après la sonde. À suivre avant la mise en service |

**TTS, Voxtral TTS** (`POST https://api.mistral.ai/v1/audio/speech`, modèle `voxtral-mini-tts-2603`)

| Point | Fait |
|---|---|
| Requête | JSON `{model, input, voice, response_format, stream}` ; le paramètre est **`voice`** (slug ou id), pas `voice_id` ; sans `voice` ni `ref_audio` : 400 |
| Sans flux | réponse JSON `{"audio_data": "<base64>"}` ; en `wav` : 24 kHz, 16 bits, mono. 10,3 s pour 4,9 s d'audio : inutilisable en direct |
| En flux | `stream: true` → **SSE** (`text/event-stream`, chunked) : `event: speech.audio.delta` / `data: {"type":"speech.audio.delta","audio_data":"<base64>"}`. 8 événements pour 5 s d'audio |
| PCM | **float32 petit-boutiste, 24 kHz, mono** (vérifié : 124 800 échantillons pour 5,2 s). Le serveur convertit en S16LE 16 kHz |
| Latence | premier morceau à **0,47 s**, phrase entière de 5 s reçue en 1,02 s |
| Voix | `GET /v1/audio/voices` : **10 voix fournies, toutes anglaises** (`en_us`, `en_gb` ; `en_paul_*`, `gb_oliver_neutral`, `gb_jane_sarcasm`), type `preset`, portée `global`. **Aucune voix française fournie.** Une voix anglaise prononce du français : la phrase de test, dite par `en_paul_neutral`, est transcrite mot pour mot par l'ASR ; l'accent reste à juger à l'oreille |
| Voix propres | `POST /v1/audio/voices` avec `name`, `sample_audio` (base64), `sample_filename`, `languages`, `gender` : clonage depuis un échantillon. La politique d'usage Mistral interdit de cloner une voix sans le consentement de la personne |

**COMP, chat completions** (`POST /v1/chat/completions`, `response_format: json_object`)

| Point | Fait |
|---|---|
| Latence | `ministral-8b` : 0,4 à 0,9 s par requête ; `ministral-3b` : 0,3 s ; premier token en flux : 0,31 s |
| Coût | ~250 tokens de prompt + 15 à 50 de réponse par requête |
| Positions | un modèle **ne sait pas compter les caractères** : les indices `start`/`end` demandés au premier essai étaient tous faux. Le prompt demande donc `{"from": "<mot tapé>", "to": "<mot corrigé>"}`, et c'est le serveur qui retrouve la position dans son tampon. Le §7.5 est corrigé en ce sens |
| Qualité | corrections justes avec `ministral-8b` et `ministral-14b` (`sui → suis`, `retar → retard`, `Jai → J'ai`, `la → là`) ; la complétion du mot suivant est **vide ou faible** avec tous les modèles essayés. Mesuré le 2026-10-14 avec un prompt dédié à la seule complétion : 4 bonnes sur 7 avec `ministral-8b`, inutilisable avec `ministral-3b`. La complétion est reportée (§16) |
| Modèle de correction | mesuré le 2026-10-14 sur six cas, avec le prompt du lot 4 : `mistral-small` corrige tout sauf un trait d'union (0,5 à 1,8 s) ; `ministral-8b` rate la grammaire (« je voudrait », « dix minute ») ; `ministral-14b` rend deux sorties fausses et une réponse en 19,6 s. Sur les dix cas de référence (§14.4) : `mistral-small` 10/10, `ministral-8b` 5/10. Défaut : `mistral-small-latest`. Environ 350 tokens par vérification ; prix à confirmer dans la console |

**Données** (lu le 2026-10-13 dans le Data Processing Addendum et les Commercial Terms)

- Entraînement : Mistral n'utilise pas les données client pour entraîner
  « sauf » opt-in ou absence d'opt-out ; le document ne dit pas quel défaut
  s'applique à l'API. **À vérifier dans le compte** (réglage d'opt-out) avant
  la mise en service.
- Rétention : aucune durée chiffrée pour les entrées et sorties de l'API ;
  une option « zero data retention » existe (elle suspend la surveillance
  d'abus), sans description. **À demander à Mistral**.
- Hébergement : non précisé dans ces documents ; sous-traitants listés sur
  `trust.mistral.ai/subprocessors`. **À lire**.
- Prix TTS : 0,016 $ les 1 000 caractères (annonce) ; prix des `ministral` :
  non trouvés sur les fiches, à lire dans la console.

### 4.3 Dépendances nouvelles

| Besoin | Option | Paquet el9 | Recommandation |
|---|---|---|---|
| Client HTTPS en flux | libcurl | `libcurl-devel` (appstream, 7.76) | **oui** : standard, déjà installé sur la machine de build |
| Client HTTPS en flux | client maison sur le transport TLS du WebSocket | — | non : un parseur HTTP de réponse en morceaux à écrire et à durcir |
| Lecture JSON | nlohmann json | `json-devel` (EPEL, 3.11) | **oui** : en-tête seul, C++, EPEL est déjà requis (usrsctp) |
| Lecture JSON | json-c | `json-c-devel` (CRB, 0.14) | possible, API C plus lourde à tenir |
| Rééchantillonnage TTS | libswresample | déjà lié par libmedikit | oui, par libavutil + libswresample seuls, sans libmedikit |

L'ASR ne demande **aucune** dépendance : c'est le client WebSocket du dépôt.

## 5. Architecture

```
navigateur / application                 speech-server                         Mistral AI
                                   ┌──────────────────────────────┐
app ── POST /control/token ──────► │ Abyss (port de contrôle)     │
       (IP filtrée)                │   TokenHandler, StatusHandler │
                                   │                              │
client ── GET /streaming/asr/… ──► │ Réacteur WebSocket ──────────┼── wss ──► Realtime
         PCM ►   ◄ texte           │   (port de streaming)        │
                                   │   AsrSession : relais pur    │
client ── GET /streaming/tts/… ──► │   TtsSession ─► file ─► Workers ── https ─► TTS
         texte ►  ◄ PCM            │   CompSession ─► file ─► Workers ── https ─► Chat
                                   └──────────────────────────────┘
```

Règles :

- **Deux ports.** Le port de contrôle (Abyss) ne reçoit que les IP autorisées.
  Le port de streaming (WebSocket) reçoit tout le monde, et c'est le jeton
  qui filtre.
- **Un seul réacteur WebSocket**, mono-thread, qui tient à la fois les
  connexions des clients et les connexions sortantes vers Mistral pour l'ASR.
  Une session ASR est un **relais** : ce qui arrive d'un côté repart de
  l'autre, sans thread.
- **Les appels HTTPS tournent dans des threads de travail** (`Worker`,
  réserve de taille fixe `mistral.workers`). Un callback du réacteur ne bloque
  jamais : il met en file, et le thread de travail répond par
  `SendMessage`, thread-safe.
- **Une session = une connexion WebSocket = une fonction.** Aucun
  multiplexage. La fermeture d'un côté ferme l'autre.
- Le serveur **ne garde rien** : ni audio, ni texte, ni jeton au-delà de sa
  durée. Un redémarrage perd les jetons en cours, et c'est accepté :
  l'application en redemande un.

## 6. Interface de contrôle (HTTP, port de contrôle)

### 6.1 Filtrage par IP

Toute requête d'une adresse hors liste reçoit **403**, sans lire le corps.
Le filtre vaut pour tous les chemins du port, `/status/general` compris.
La liste porte des adresses ou des préfixes CIDR (`control.allow`, §8.2).
Liste vide : le serveur refuse de démarrer, parce qu'un port de contrôle
ouvert à tous crée des jetons pour tous.

**Le port de contrôle n'écoute qu'en IPv4.** Abyss (xmlrpc-c 1.51) range
l'adresse du pair dans un `struct sockaddr` de 16 octets. Une adresse IPv6
y est tronquée : sur une socket double pile, un client `127.0.0.1` arrive
avec une adresse vide (mesuré le 2026-10-14). Et `ChannelUnixCreateFd`
refuse une socket IPv6. Le filtre ne peut donc être juste qu'en IPv4.
Conséquences :

- `control.listen` est une adresse IPv4 ; une entrée IPv6 dans
  `control.allow` est une erreur de configuration, car elle ne
  correspondrait jamais ;
- le port de streaming, lui, reste double pile (§8.3).

Le support d'IPv6 sur le port de contrôle est un besoin noté pour une
version ultérieure (§16).

### 6.2 `POST /control/token`

Corps JSON :

```json
{ "function": "tts", "ttl": 3600, "account": "clinique-nord" }
```

| Champ | Règle |
|---|---|
| `function` | `asr`, `tts` ou `comp`. Un jeton ne vaut que pour une fonction. |
| `ttl` | secondes, 1 à 3 600. Absent : 3 600. Au-delà : 400. |
| `account` | optionnel. Étiquette libre, 1 à 64 caractères `[A-Za-z0-9._-]`, choisie par l'application : un client final, un service. Le serveur ne l'interprète pas ; il la reporte dans les compteurs et le journal (§6.4, §10). Absente : `"-"`. |

Réponse **201** :

```json
{ "token": "Qm9uam91ciBsZSBtb25kZS…", "function": "tts", "account": "clinique-nord", "expiresAt": "2026-10-13T15:04:05Z" }
```

Le jeton est **opaque** : 32 octets tirés par `RAND_bytes` (OpenSSL, déjà
lié), rendus en base64url (43 caractères). Le serveur le garde en mémoire
avec sa fonction et sa date de fin.

Un jeton est **à usage unique** : il est consommé à l'ouverture du flux, et
une seconde ouverture reçoit 403. L'application demande un jeton par flux.
C'est ce qui empêche un jeton copié d'une page de servir ailleurs.

Codes : 201 créé ; 400 corps ou champ invalide ; 403 IP non autorisée ;
415 corps non JSON.

### 6.3 `GET /control/voices`

Rend les voix que le serveur accepte pour `tts` (§7.3) : alias, langue,
`voice_id` Mistral. La liste vient de la configuration du serveur, **pas**
d'une copie côté application. Même règle que les codecs du mediaserver : ce
que le serveur sait de lui-même, c'est à lui qu'on le demande.

### 6.4 `GET /status/general`

Même forme que celui du mediaserver (`docs/reference/status-http.md`) : JSON
par défaut, texte avec `?format=text`. Contenu :

- `server` : produit, version, `pid`, démarrage, uptime ;
- `backend` : clé présente ou non, dernier succès et dernier échec par
  service Mistral ;
- `sessions` : ouvertes par fonction, maximum configuré, refus cumulés ;
- `tokens` : vivants, émis, consommés, expirés sans usage ;
- `usage` depuis le démarrage : secondes d'audio envoyées (`asr`),
  caractères envoyés (`tts`), tokens consommés (`comp`). Ce sont les trois
  unités de facturation de Mistral : l'exploitant en déduit le coût sans
  seconde comptabilité. Le total, puis le détail **par `account`**
  (`usage.byAccount`), pour qu'une refacturation reste possible sans
  changer le protocole. Il n'y a **qu'une clé Mistral par serveur** : la
  ventilation par compte n'existe que dans ces compteurs.

## 7. Interface de streaming (WebSocket, port de streaming)

### 7.1 Ouverture

```
GET /streaming/<function>/<token>?lang=fr&voice=claire&delay=1000
```

| Code avant le 101 | Cause |
|---|---|
| 101 | flux ouvert |
| 404 | chemin qui n'a pas la forme `/streaming/<fonction>/<jeton>`, ou fonction inconnue |
| 400 | paramètre inconnu ou répété, langue ou voix non servie, `delay` hors bornes |
| 403 | jeton inconnu, expiré, déjà consommé, ou d'une autre fonction |
| 429 | `limits.maxSessions` atteint pour cette fonction |
| 503 | fonction pas encore servie, ou `tts` sans voix configurée |
| 500 | erreur interne |

Les contrôles se font dans cet ordre : 404, 400, 503, 429, puis 403. Le
jeton est consommé en dernier. Ainsi, un client refusé pour une autre
raison garde son jeton et peut réessayer. Exemple : un 429 ne brûle pas le
jeton.

Mistral injoignable ne donne **pas** de 503. Le savoir avant le 101
demanderait d'attendre Mistral dans le réacteur, qui sert toutes les
connexions. Le flux s'ouvre donc, et le serveur ne l'annonce prêt
(`ready`) qu'une fois Mistral prêt. S'il ne l'est pas, le client reçoit
`error` fatal, puis la fermeture 1013. Exemple pour `asr` : le 101 part
tout de suite ; `ready` part après le `session.updated` de Mistral.

Paramètres : `lang` vaut pour les trois fonctions (`fr` par défaut),
`voice` pour `tts` seulement, `delay` pour `asr` seulement. Un paramètre
d'une autre fonction est un paramètre inconnu : 400.

Le jeton est dans le chemin parce qu'un navigateur ne peut pas poser
d'en-tête sur un WebSocket. Conséquence : **le serveur ne journalise jamais
un chemin de streaming en entier**. Le log porte la fonction et les six
premiers caractères du jeton.

**Un seul chemin d'entrée, pour tout le monde.** Il n'existe aucune
ouverture sans jeton, pas même depuis une IP de la liste du §6.1. Le
mediaserver fait comme l'application : il demande un jeton, puis ouvre le
flux (§12). Une seule règle à tester, une seule à auditer.

**`wss://` et les navigateurs.** Le port de streaming sert `ws://` ou
`wss://` selon `streaming.secure` (§8.2), avec le même transport TLS que le
mediaserver (`websockettransport`). Un navigateur impose deux choses que le
serveur ne peut pas contourner :

- une page servie en `https://` ne peut ouvrir qu'un `wss://` ; en
  production, c'est donc **toujours** `wss://` ;
- le certificat doit être **reconnu par le navigateur** : un certificat
  auto-signé comme `mcu.crt` est refusé sans recours, l'API WebSocket du
  navigateur n'offrant aucune exception. `streaming.cert` et `streaming.key`
  pointent donc sur un certificat d'une autorité connue (Let's Encrypt, ou
  l'autorité interne déployée sur les postes), pour le nom DNS que la page
  utilise.

Le serveur ne vérifie pas l'en-tête `Origin` : le jeton suffit, et une
application qui a reçu un jeton a déjà passé le filtre du port de contrôle.

### 7.2 Règles communes

- Trames **texte** : un objet JSON, un seul par trame, avec un champ `type`.
- Trames **binaires** : PCM S16LE mono 16 kHz, dans les deux sens.
- Premier message, du serveur : `{"type":"ready","function":"asr","rate":16000}`.
- `{"type":"error","reason":"…","fatal":true}` : `fatal` vaut `true` quand le
  serveur ferme juste après.
- Durées : un flux sans trame du client pendant `limits.idleSecs` (60 s) est
  fermé ; un flux dure au plus `limits.maxSessionSecs` (3 600 s). Ces deux
  bornes bornent aussi la dépense.
- Fermeture : code 1000 à l'initiative du client ; 1011 sur erreur du
  serveur ; 1013 (*try again later*) quand Mistral tombe en cours de flux.
  Le client qui veut reprendre rouvre un flux, avec un **nouveau** jeton.
- Le serveur n'applique pas de reprise automatique vers Mistral en cours de
  session : un relais ASR qui reprendrait perdrait de l'audio sans le dire.
  Il ferme, et le client sait qu'il manque un morceau.

Paramètre `lang` : code BCP 47 court (`fr`, `en`). Pour `tts`, il choisit la
voix par défaut. Pour `comp`, il entre dans le prompt. Pour `asr`, il est
**ignoré** : Voxtral Realtime refuse tout forçage de langue (§4.2) et
détecte seul ; `ready` porte `"langIgnored":true`.

### 7.3 `tts` : du texte vers la voix

Le client envoie son texte **au fil de la frappe**, avec la sémantique
T.140 :

```json
{ "type": "text", "text": "Bonjour, je sus\bis en retard." }
```

- U+0008 efface le caractère précédent **non encore envoyé à la synthèse** ;
  U+2028 ou `\n` termine la phrase. Un retour arrière qui remonte plus loin
  est ignoré : voir « Les corrections » plus bas.
- Le serveur accumule, puis envoie une phrase à Mistral sur une ponctuation
  finale (`.`, `?`, `!`, `…`) **suivie d'un blanc**, un séparateur de ligne,
  ou un silence de frappe (`tts.phraseSilenceMs`, 1 500 ms par défaut). Une
  ponctuation seule ne coupe pas : « 3.5 » et « ives.fr » restent entiers.
  Une phrase sans fin qui dépasse 1 500 octets est coupée à son dernier
  blanc, parce que Mistral conseille moins de 300 mots par requête. Le
  découpeur est la **même classe pure** que celle du [TTS](../TTS/SPEC.md)
  §7.1, testée seule.
- `{"type":"flush"}` force l'envoi de ce qui est accumulé.
- `{"type":"stop"}` vide la file et abandonne la phrase en cours ; le serveur
  répond `{"type":"stopped","ids":[…]}`.

Le serveur répond, dans l'ordre des phrases :

```json
{ "type": "say", "id": 7, "text": "Bonjour, je suis en retard." }
… trames binaires (PCM de la phrase 7) …
{ "type": "done", "id": 7 }
```

Les trames binaires entre un `say` et son `done` appartiennent à une seule
phrase. Le serveur envoie plus vite que le temps réel ; le client tamponne.
Une seule phrase est en cours de synthèse à la fois, pour garder l'ordre.
Après `stopped`, plus aucune trame binaire n'arrive, même si Mistral
continue d'envoyer la phrase abandonnée.

**Les corrections.** Le serveur reste bête : il dit ce qu'on lui envoie, et
ne revient jamais sur une phrase envoyée. Corriger du texte déjà dit est
l'affaire du client. Le mediaserver, qui reçoit la bulle T.140 de
l'utilisateur, voit le retour arrière qui remonte dans du texte déjà dit ;
il envoie alors lui-même l'annonce et le texte retapé, par exemple
« Correction : deux pizzas pour ce soir. ». Il peut d'abord envoyer
`stop`, pour que les phrases encore en file ne soient pas dites (lot 6,
§13.8).

`ready` porte la voix retenue et la langue :
`{"type":"ready","function":"tts","rate":16000,"voice":"fr-homme","lang":"fr"}`.

Voix : `voice` est un **alias** défini dans la configuration du serveur
(`voices`, §8.2), jamais un `voice_id` Mistral en clair chez
le client. Sans `voice`, la première voix de la langue. Une voix inconnue :
400.

Conversion : float32 24 kHz → S16LE 16 kHz (libswresample). Un 403 de
modération Mistral sur une phrase donne
`{"type":"error","id":7,"reason":"rejected","fatal":false}` : la phrase
suivante part. Tout autre échec de Mistral (pas de réponse, code autre que
200 et 403) donne `error` fatal « backend unavailable », puis 1013. Le corps
d'une réponse d'erreur n'est ni lu ni journalisé : il pourrait citer le
texte de l'utilisateur.

Compteur : les caractères envoyés à Mistral (`usage.ttsChars`), comptés en
caractères et non en octets, au moment de la requête.

### 7.4 `asr` : de la voix vers le texte

Le client attend `ready` puis envoie des trames binaires de 20 à 500 ms, au
rythme réel. Le serveur les relaie telles quelles vers Mistral (même
format). `ready` part quand Mistral a accepté le `delay` : avant, rien ne
dit que Mistral répond. Sans `session.updated` 10 s après la demande,
le client reçoit `error` fatal « backend timeout », puis 1013.

Le serveur ne tamponne l'audio que pendant une ouverture de session
Mistral : l'audio reçu avant `ready`, ou à la reprise après une pause
longue. Le tampon tient 5 s. Il part en trames de 100 ms dès que Mistral
est prêt. Au-delà de 5 s, l'audio est jeté et le client reçoit une
`error` non fatale.

Le serveur rend le texte en T.140 :

```json
{ "type": "text", "text": " en retard" }
```

**Les corrections.** Mistral Realtime rend des deltas **ajoutés à la fin,
jamais repris**. Le texte avec retours arrière demandé au départ n'existe
donc pas chez le fournisseur. Deux options :

- **A, ajout seul** : un relais pur, un `delay` (le
  `target_streaming_delay_ms` de Mistral, borné 200 à 2 400 ms, 1 000 par
  défaut) que le client choisit entre vitesse et justesse. Pas de U+0008.
- **B, deux sessions Mistral par flux** : une rapide (240 ms) affichée tout
  de suite, une lente (2 400 ms) qui fait foi ; quand la lente arrive, le
  serveur efface la partie rapide (U+0008 × n) et réécrit. Le texte se
  corrige sous les yeux, mais le coût de l'ASR **double**, et l'alignement
  des deux textes est un algorithme à écrire et à tester.

Tranché le 2026-10-13 : **A**. `delay` est exposé pour que l'usage tranche.
B reste possible plus tard sans changer le protocole du client : U+0008 est
déjà dans la sémantique.

**La pause.** `{"type":"pause"}` : le serveur cesse de relayer, et jette
l'audio reçu. `{"type":"resume"}` reprend. Si la pause dure plus de
`asr.pauseCloseSecs` (30 s), le serveur ferme la session Mistral et la
rouvre à la reprise : Mistral ferme de lui-même une session muette après
60 s environ (§4.2), donc 30 s reste en dessous. À la mise en pause, le
serveur envoie `input_audio.flush` pour recevoir tout de suite le texte en
attente. En option, `asr.vad` arme libfvad sur
le serveur : après `asr.silenceMs` de silence, il se met en pause seul, et
reprend sur la voix. Le client est averti par `{"type":"paused"}` et
`{"type":"resumed"}`. Hors version 1 si le lot 0 ne mesure pas un besoin.

`{"type":"done"}` : le serveur envoie `input_audio.flush` puis
`input_audio.end`, relaie le texte restant, et ferme avec 1000 quand
Mistral a fermé, ou au plus tard 5 s après.

Fermetures : Mistral qui ferme ou tombe en cours de flux donne `error`
fatal « backend closed », puis 1013. Une erreur Mistral pendant une pause
(le délai de silence, 3804) est attendue : elle ne compte pas comme un
échec dans `/status/general`.

### 7.5 `comp` : corriger le texte avant qu'il soit dit

`comp` sert la synthèse : le texte part au mediaserver en T.140, et c'est ce
texte qui est dit. Il doit donc être juste **avant** de partir. Le client
découpe et décide ; le serveur corrige, sans rien garder entre deux
demandes.

```
navigateur ── WS comp ──────────► speech-server   (corrige)
    │  ◄── texte corrigé + changements ──┘
    │
    └── appel (T.140, texte validé) ──► mediaserver ── WS tts ──► speech-server ──► voix
```

**La demande.** Le client envoie un morceau de texte : en général les
derniers mots terminés, pas encore partis. Il joint en lecture seule la fin
du texte qui le précède, parce que la grammaire en dépend (« je voudrait »
ne se corrige qu'en voyant « je ») :

```json
{ "type": "check", "id": 7, "context": "Bonjour je voudrais", "text": " une piza svp" }
```

`id` est choisi par le client, et repris dans la réponse. `text` et
`context` font au plus 500 caractères chacun. Seuls les mots de `text` sont
corrigés.

**La réponse.**

```json
{ "type": "checked", "id": 7, "text": " une pizza svp",
  "changes": [ { "start": 5, "end": 9, "from": "piza", "to": "pizza" } ] }
```

`text` est le morceau corrigé. `changes` dit où, dans le morceau **envoyé** :
`[start, end)` en unités UTF-16, celles des chaînes JavaScript. Sans
changement, `changes` est vide et `text` est identique à la demande.

**Le serveur ne réécrit pas, il corrige des mots.** Le modèle rend des
paires « mot tapé → mot corrigé », jamais un texte réécrit, jamais des
positions : un modèle ne sait pas compter les caractères (§4.2), et un
modèle à qui l'on demande un texte corrigé finit par reformuler. Le serveur
relit la réponse avant d'en tirer quoi que ce soit :

- un JSON mal formé est jeté, compté dans `/status/general`
  (`backend.comp.discarded`), et la demande reçoit une `error` non fatale ;
- chaque `from` est cherché dans `text`, en mots entiers, de gauche à
  droite après la correction précédente ; introuvable, la paire est jetée ;
- une paire qui change plus de 2 lettres, ou plus d'un tiers du mot, est
  jetée : c'est une réécriture, pas une correction. Exemple : « Jai » →
  « J'ai » passe (aucune lettre changée) ; « Pouvez » → « Pouvez-vous » est
  jeté (quatre lettres ajoutées).

**Une demande en cours à la fois.** Une demande qui arrive pendant une autre
attend ; une troisième remplace celle qui attend, et la remplacée reçoit
`{"type":"error","id":…,"reason":"superseded","fatal":false}`. Un échec de
Mistral sur une demande donne une `error` non fatale avec son `id` : chaque
demande est indépendante, et le client peut envoyer son texte tel quel.

`lang` (§7.1) entre dans le prompt. Le modèle et le prompt sont de la
configuration du serveur ; le prompt est versionné dans le dépôt avec ses
cas de référence (§14.4).

**Ce que fait le client** (recommandé, hors serveur) :

1. envoyer `check` dès 3 mots terminés, une ponctuation, ou 0,8 s sans
   frappe, au premier des trois ; jamais le mot en cours ;
2. sans changement, envoyer le morceau en T.140 tout de suite ;
3. avec des changements, les proposer, et n'envoyer qu'à la validation
   (Entrée, ou un clic) : une correction imposée en silence trahirait
   l'utilisateur ;
4. pour corriger du texte déjà parti, l'effacer en T.140 et renvoyer la
   version corrigée : le mediaserver annonce la correction (§7.3) ;
5. Entrée ferme la bulle par U+2028.

La complétion du mot suivant est reportée (§16).

## 8. Le secret et la configuration

### 8.1 La clé Mistral

Options :

- **A, systemd `LoadCredential`** (systemd 247 : AlmaLinux 9, Debian 11 et
  plus). L'unité déclare
  `LoadCredential=mistral-api-key:/etc/mediaserver/speech-api.key`. Le
  fichier est en `0600 root`. systemd le copie au démarrage dans un ramfs
  privé au service, et le serveur lit `$CREDENTIALS_DIRECTORY/mistral-api-key`.
  La clé n'est ni dans `argv` (visible dans `ps`), ni dans l'environnement
  (visible dans `/proc/<pid>/environ`, hérité par tout processus fils), ni
  dans le fichier de configuration (qui finit dans les tickets et les
  dépôts). Le service n'a pas besoin d'être root pour la lire.
- **B, la variante chiffrée `LoadCredentialEncrypted`** (systemd 250 :
  AlmaLinux 9, Debian 12 et plus). Le fichier sur disque est chiffré par
  `systemd-creds encrypt`, avec le TPM s'il existe, sinon avec la clé d'hôte
  `/var/lib/systemd/credential.secret`. Un fichier copié hors de la machine
  (sauvegarde, image, dépôt de configuration) est illisible. Root sur la
  machine lit toujours la clé. Deux contraintes : ne jamais sauvegarder la
  clé d'hôte avec le fichier chiffré ; rechiffrer après une réinstallation.
- **C, un gestionnaire de secrets** (Vault, OpenBao, KMS) : rotation, audit,
  révocation centrale. Mais un service de plus, un client HTTPS de plus, et
  un second secret pour parler au coffre, à protéger à son tour.
- **D, un champ dans la configuration** : la clé se retrouve dans tout `cat`
  du fichier et tout ticket qui le cite.

Tranché : **A comme socle, B en variante d'exploitation**. Le code du serveur
est le même dans les deux cas ; c'est l'unité qui choisit, par un drop-in.

Constaté sur dev71, conteneur LXC Proxmox : **ni A ni B ne marchent.**

- A : l'hôte ajoute à tous les services un drop-in `zzz-lxc-service.conf`
  qui vide `LoadCredential=`. Remise après lui, la directive pose bien
  `$CREDENTIALS_DIRECTORY`, mais le répertoire reste vide : le montage
  `ramfs` est refusé (`EACCES`), et le `tmpfs` de repli, monté dans un
  espace de montage à part puis déplacé, n'atteint pas le service.
- B : `systemd-creds` ne crée pas sa clé d'hôte (« Failed to setup
  credentials host key »), donc rien ne se chiffre.

La variante LXC, vérifiée le 2026-10-18 : un `ExecStartPre=+` copie, en
root, le fichier de A dans un `RuntimeDirectory` (`tmpfs`, `0700`, au compte
`speech`), et le serveur la lit par `--api-key-file`. La protection est
celle de A : l'original reste root, la copie disparaît à l'arrêt. Une clé
chiffrée sur disque demande une VM avec TPM virtuel, ou l'option C.
Procédure : `docs/utilisation/speech-server.md` §3.

Pose de la clé, une commande dans chaque cas :

```sh
# A : en clair, root seul
install -m 0600 -o root -g root /dev/stdin /etc/mediaserver/speech-api.key <<< "$KEY"

# B : chiffré, lié à la machine (TPM s'il existe, sinon clé d'hôte)
systemd-creds encrypt --name=mistral-api-key - /etc/mediaserver/speech-api.cred <<< "$KEY"
# drop-in /etc/systemd/system/speech-server.service.d/key.conf :
#   [Service]
#   LoadCredential=
#   LoadCredentialEncrypted=mistral-api-key:/etc/mediaserver/speech-api.cred
```

Règles :

- Le serveur lit la clé **une fois** au démarrage, la garde en mémoire, ne
  l'écrit nulle part. `/status/general` et `--check` disent « présente »,
  rien de plus. Sans clé, il refuse de démarrer, comme le mediaserver sans
  profil d'adressage.
- Pour le développement hors systemd : `--api-key-file <chemin>`, même
  format, même règle `0600`.
- **Rotation sans trou** : créer une seconde clé dans la console Mistral,
  remplacer le fichier, redémarrer le service, révoquer l'ancienne. Un
  `SIGHUP` ne suffit pas : systemd ne copie les credentials qu'au démarrage.
  Le redémarrage coupe les flux quelques secondes ; l'opération est rare.
- Le paquet **ne livre aucun fichier de clé**, même vide.

### 8.2 La configuration : un fichier JSON

Tranché le 2026-10-13 : la configuration vit dans **un fichier JSON**,
`/etc/mediaserver/speech-config.json`, livré en `%config(noreplace)`. La
bibliothèque JSON du §4.3 le lit. Trois raisons : les listes (IP, voix) se
lisent une par ligne ; le fichier se **recharge à chaud** ; et le mediaserver
suivra le même motif.

La ligne de commande ne porte que ce qui sert au développement :

| Option | Défaut | Rôle |
|---|---|---|
| `--config <chemin>` | `/etc/mediaserver/speech-config.json` | le fichier |
| `--api-key-file <chemin>` | `$CREDENTIALS_DIRECTORY/mistral-api-key` | la clé, hors systemd (§8.1) |
| `--check` | — | lit le fichier, dit ce qui cloche, sort sans écouter |

Le fichier, avec toutes ses clés et leurs défauts. Une clé absente prend
son défaut ; une clé inconnue est une **erreur** (une faute de frappe ne
doit pas passer en silence) :

```json
{
  "control": {
    "listen": "0.0.0.0",
    "port": 9200,
    "allow": ["10.0.0.0/8", "192.0.2.7"]
  },
  "streaming": {
    "listen": "::",
    "port": 9201,
    "secure": false,
    "cert": "/etc/mediaserver/mcu.crt",
    "key": "/etc/mediaserver/mcu.key"
  },
  "mistral": {
    "url": "https://api.mistral.ai",
    "workers": 10
  },
  "limits": {
    "maxSessions": { "asr": 20, "tts": 20, "comp": 20 },
    "idleSecs": 60,
    "maxSessionSecs": 3600
  },
  "voices": [
    { "alias": "claire", "lang": "fr", "voiceId": "<voice_id Mistral>" }
  ],
  "tts":  { "model": "voxtral-mini-tts-2603", "phraseSilenceMs": 1500 },
  "asr":  { "model": "voxtral-mini-transcribe-realtime-2602", "vad": false, "silenceMs": 1500, "pauseCloseSecs": 30 },
  "comp": { "model": "mistral-small-latest" }
}
```

| Clé | Rôle | Renvoi |
|---|---|---|
| `control.listen` | adresse IPv4 d'écoute du port de contrôle | §6.1 |
| `control.allow` | obligatoire, non vide : IPv4 ou préfixes CIDR IPv4 autorisés sur le contrôle ; le mediaserver y figure | §6.1, §12 |
| `streaming.secure`, `cert`, `key` | `wss://` ; les défauts (`mcu.crt`, `mcu.key`) ne servent qu'au développement : un navigateur veut un certificat d'une autorité connue | §7.1, §9 |
| `streaming.listen` | adresse d'écoute du port de streaming, littéral v4 ou v6 ; `::` sert les deux familles | §8.3 |
| `mistral.url` | base des appels, pour un relais ou le faux Mistral des tests | §14 |
| `mistral.workers` | threads qui attendent Mistral : ouvertures `asr`, requêtes `tts` et `comp`. Une phrase `tts` en cours occupe un thread : au-delà de `workers` flux qui parlent en même temps, les phrases attendent leur tour | §13.4 |
| `voices` | obligatoire pour `tts` : sans voix, les flux `tts` reçoivent 503 | §7.3 |
| `asr.model`, `tts.model`, `comp.model` | modèle Mistral de la fonction ; une session garde celui de son ouverture | §4.1 |

**Démarrage.** Fichier absent, JSON invalide, clé inconnue, `control.allow`
vide ou porteur d'une entrée IPv6, `control.listen` en IPv6, deux ports
égaux : le serveur écrit la cause dans le log et refuse de démarrer.

**Rechargement.** `SIGHUP` relit le fichier. Ce qui se recharge sans couper
un flux : `control.allow`, `limits`, `voices`, `tts`, `asr`, `comp`. Ce qui
demande un redémarrage : `control.listen`, `control.port`, `streaming.*`,
`mistral.*`. Le log signale un tel changement, qui attend le redémarrage. Un fichier invalide au rechargement est **ignoré en
entier**, avec la cause dans le log, et la configuration en cours reste. Le
serveur ne relit jamais la clé Mistral sur `SIGHUP`.

### 8.3 IPv6

Le port de streaming écoute par défaut sur `::`, sans `IPV6_V6ONLY` : il
sert v4 et v6. `streaming.listen` accepte un littéral v4 ou v6. Le port de
contrôle est IPv4 seul (§6.1). Les connexions sortantes vers Mistral
passent par `IPAddress::Resolve` (A + AAAA) et libcurl, qui essaie les
deux familles. Les tests du §14 se connectent au port de streaming en v4
et en v6.

## 9. Sécurité

- Le port de contrôle n'est joignable que des IP listées. Il ne porte aucun
  secret dans ses réponses.
- Le port de streaming est public. Il n'accepte que des jetons vivants,
  à usage unique, d'une fonction donnée. Il borne les sessions, leur durée,
  et leur silence.
- Le jeton n'est **jamais** écrit en entier dans un journal.
- `wss://` est **obligatoire** en production : le jeton passe dans l'URL, le
  texte et la voix sont des données personnelles, et un navigateur en
  `https://` ne sait rien ouvrir d'autre (§7.1).
- Le port de contrôle reste en HTTP : il n'est joint que par des serveurs
  (l'application, le mediaserver) sur le réseau d'administration, jamais
  par un navigateur. S'il doit traverser un réseau non sûr, un relais TLS
  devant lui (nginx, HAProxy) le couvre sans rien changer au serveur.
- Les données traitées (voix, texte) **quittent l'infrastructure** vers
  Mistral, sous-traitant au sens du RGPD. Tranché le 2026-10-13 : l'API
  reste le moteur ; le contrat de traitement et le chiffre de rétention
  sont obtenus au lot 0 ; l'information des utilisateurs est portée par
  l'application ; les trois faits (entraînement, rétention, hébergement)
  sont écrits dans la doc exploitant au lot 5.
- Issue de secours, à connaître avant toute promesse « vos données restent
  chez nous » : les poids de Voxtral Realtime sont en Apache 2.0, donc un
  vLLM local derrière `mistral.url` peut un jour ramener l'ASR sur place ;
  les poids de Voxtral TTS sont en CC BY-NC d'après l'annonce de Mistral,
  donc **interdits en usage commercial** : le TTS reste en ligne, sauf
  licence négociée.
- **Une seule clé Mistral par serveur.** Tous les comptes passent par elle ;
  le champ `account` (§6.2) ne sert qu'à ventiler les compteurs.
- Le clonage de voix Mistral exige le consentement de la personne clonée
  (conditions d'usage Mistral). Seules des voix fournies par Mistral, ou des
  voix IVèS consenties, entrent dans `voices`.

## 10. Observabilité

- `/status/general` (§6.4) sur le port de contrôle.
- Journal : une ligne à l'ouverture et à la fermeture de chaque flux
  (fonction, `account`, jeton tronqué, adresse du client, durée, unités
  consommées),
  une ligne par échec Mistral avec le code HTTP ou le code de fermeture.
  Jamais le contenu (texte ou audio).
- Arrêt : `SIGTERM` ferme les flux avec 1001 (*going away*), puis les
  sessions Mistral, puis sort. systemd le pilote comme le mediaserver.

## 11. Construction et livraison

### 11.1 Construction

- Cible `speech-server` dans `mcu/Makefile`, à côté de `mcu`. Elle lie les
  objets réutilisés (`websocketserver`, `websocketconnection`,
  `websockettransport`, `httpparser`, `http`, `xmlrpcserver`, `ipaddress`,
  `addressprofiles`), Abyss, OpenSSL et libavutil (le base64 du WebSocket).
  Les lots 2 à 4 ajoutent libcurl et libswresample. Elle **ne lie pas**
  libmedikit, ffmpeg entier, ni les mixeurs.
- `install.ksh prereq` ajoute `libcurl-devel` et `json-devel` (Debian :
  `libcurl4-openssl-dev`, `nlohmann-json3-dev`).
- La version est celle du mediaserver (`mcu/include/version.h`) : un seul
  numéro, une seule chaîne de livraison.
- Le journal sort sur stdout et stderr, comme le mediaserver : c'est l'unité
  qui l'écrit dans un fichier. Le serveur n'ouvre aucun fichier de log
  lui-même, donc il n'a pas à écrire dans `/var/log`.

### 11.2 Un service systemd à part : `speech-server.service`

Le binaire a sa propre unité, indépendante de `mediaserver.service` : il
démarre, s'arrête et se relance seul, et peut tourner sur une autre machine.
Aucun des deux ne dépend de l'autre au démarrage : le mediaserver sans
`speech-server` sert les appels et refuse la synthèse ([ADR 003](../../architecture/adr-003-tts-serveur-websocket.md)).

```ini
[Unit]
Description=IVeS speech-server (ASR, TTS, correction de texte via Mistral AI)
Documentation=http://www.ives.fr
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/ives/bin/speech-server --config /etc/mediaserver/speech-config.json
ExecReload=/bin/kill -HUP $MAINPID

# La clé Mistral : copiée par systemd dans $CREDENTIALS_DIRECTORY, lisible
# par ce service seul. Variante chiffrée par drop-in (SPEC §8.1).
LoadCredential=mistral-api-key:/etc/mediaserver/speech-api.key

# Rien ici ne demande root : les ports sont au-dessus de 1024.
User=speech
Group=speech
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
# La clé est en mémoire : pas de core dump par défaut.
LimitCORE=0

# Arrêt propre : SIGTERM ferme les flux (1001) puis les sessions Mistral.
KillSignal=SIGTERM
TimeoutStopSec=15
Restart=on-failure
RestartSec=2

StandardOutput=append:/var/log/speech-server.log
StandardError=append:/var/log/speech-server.log

[Install]
WantedBy=multi-user.target
```

Ce qui ne se devine pas :

- `User=speech` est un compte système créé par le paquet (`useradd -r`,
  sans shell ni dossier personnel), en `%pre` du RPM et en `preinst` du
  `.deb`.
- `ProtectSystem=strict` rend tout le système en lecture seule pour le
  service. Le fichier de log est ouvert **par systemd**, avant le changement
  d'utilisateur, donc `append:` fonctionne sans exception : vérifié sur
  AlmaLinux 9 le 2026-10-14. Le serveur n'écrit rien d'autre sur disque.
- `ExecReload` porte le `SIGHUP` du §8.2 : `systemctl reload speech-server`
  recharge la configuration sans couper les flux.
- `LimitCORE=0` : pour un diagnostic, un drop-in le relève le temps de
  reproduire, puis on le retire.
- `RestrictAddressFamilies` autorise `AF_UNIX` parce que libcurl et la
  résolution de noms en ont besoin.

### 11.3 Paquets

| | RPM (`mcumediaserver.spec`) | `.deb` (`install.ksh deb`) |
|---|---|---|
| Sous-paquet | `mediaserver-speech` | `mediaserver-speech` |
| Binaire | `/opt/ives/bin/speech-server` | idem |
| Unité | `%{_unitdir}/speech-server.service` | `/lib/systemd/system/speech-server.service` |
| Configuration | `/etc/mediaserver/speech-config.json`, `%config(noreplace)` | idem, `conffiles` |
| Clé | non livrée ; `/etc/mediaserver/speech-api.key` posée par l'exploitant | idem |
| Compte | `speech`, créé en `%pre` | créé en `preinst` |
| Scriptlets | `%systemd_post` / `%systemd_preun` / `%systemd_postun_with_restart speech-server.service` | `deb-systemd-helper`, ou `systemctl` dans `postinst` comme fait le paquet actuel |

Le sous-paquet ne dépend pas du paquet du mediaserver : il possède lui
aussi le répertoire `/etc/mediaserver/`, ce que RPM et dpkg admettent. On
l'installe donc seul sur une machine dédiée. Le paquet du mediaserver, lui,
ne possède plus tout `/opt/ives/bin` mais son seul binaire. Le service n'est **pas activé** à l'installation : sans clé il ne
démarre pas, et l'activer avant la pose de la clé produirait un échec au
premier démarrage.

## 12. Le mediaserver comme client

Tranché le 2026-10-13 : le mediaserver utilise **exactement le même
mécanisme que n'importe quel client**. Rien ne le distingue côté
`speech-server`.

Pour chaque session qui a besoin de la voix (un synthétiseur d'annonces, la
lecture du T.140 d'un participant, usages de la [SPEC TTS](../TTS/SPEC.md)) :

1. le mediaserver demande un jeton `tts` par `POST /control/token` (§6.2).
   Son adresse figure donc dans `control.allow` ;
2. il ouvre **une connexion WebSocket par session** sur
   `/streaming/tts/<jeton>?lang=…&voice=…`, avec `WebSocketServer::Connect`
   (`ws://` ou `wss://`, selon l'URL configurée) ;
3. il parle le protocole du §7.3 tel quel ;
4. à la fin de la session, il ferme le flux. Une session qui tombe rouvre
   un flux avec un **nouveau** jeton, puisque chacun est à usage unique.

Ce que cela demande au mediaserver :

- un **client HTTP** pour l'étape 1, absent aujourd'hui (§3) : libcurl, la
  même dépendance que `speech-server`, dans un thread de travail et jamais
  dans un callback du réacteur ;
- deux options de configuration : l'URL du `speech-server` (`--speech-url`,
  base commune des deux ports, par exemple `https://speech.example.net`) et,
  pour `wss://`, la confiance TLS déjà prévue par `--websocket-client-ca`.
  Sans `--speech-url`, la synthèse est désactivée et `/status/general` le
  dit.

La [SPEC TTS](../TTS/SPEC.md) §4 décrit un protocole proche mais pas
identique (`hello`, `voices`) : elle sera **réécrite** pour désigner ce
document comme seul protocole, et son §5 (serveur Piper) disparaît.
L'[ADR 003](../../architecture/adr-003-tts-serveur-websocket.md) reste vrai
sur la séparation en deux binaires ; son moteur change (ADR 005 à écrire,
voir l'en-tête).

## 13. Plan d'implémentation

### 13.1 Vue d'ensemble

| Lot | Contenu | Condition de fin | Version |
|---|---|---|---|
| 0 | Sonde brute du fil Mistral, mesures, faits sur les données | Go ou no-go écrit au §4.2, avec les mesures | — |
| 1 | Squelette : deux ports, filtrage IP, jetons, configuration, clé, `/status/general`, arrêt | Tests §14.1 verts | 1.18.0 |
| 2 | `asr` : relais vers Mistral Realtime | Tests §14.2 verts | 1.18.0 |
| 3 | `tts` : découpeur, file, workers libcurl, conversion PCM | Tests §14.3 verts | 1.18.0 |
| 4 | `comp` : tampon, débounce, prompt, relecture JSON | Tests §14.4 verts | 1.18.0 |
| 5 | Livraison : paquets, unité, compte, doc exploitant | Installé sur AlmaLinux 9 et Debian 12 | 1.18.0 |
| 6 | Le mediaserver client (§12) | Recette §15 | 1.19.0 |
| 7 | Option `asr.vad` : seulement si la recette la réclame | — | — |

Chaque lot est **un commit livrable** (il bâtit, `make check` est vert),
précédé de commits intermédiaires courts. Les lots 2, 3 et 4 sont
indépendants entre eux : ils peuvent se faire dans n'importe quel ordre, ou
en parallèle sur trois branches issues du lot 1.

Arborescence commune, fixée au lot 1 :

```
mcu/include/speech/   en-têtes du serveur
mcu/src/speech/       sources ; speechmain.cpp porte le main()
mcu/tests/            test_speech_*.cpp, joués par make check
mcu/tests/fixtures/speech/   rejeux Mistral enregistrés au lot 0
```

Dans `mcu/Makefile` : une liste `SPEECHOBJS` (jamais dans `OBJS`, pour que
`mcu` ne lie ni libcurl ni le JSON), une cible `speech-server` qui lie
`SPEECHOBJS` plus les objets réutilisés du §3, et `SPEECHOBJS` ajouté au
lien du binaire de test avec `-lcurl`.

### 13.2 Lot 0 : la sonde

État : **sonde faite** (§4.2), enregistrements dans
`mcu/tests/fixtures/speech/`. Restent le §13.2.1 et les trois faits sur les données à obtenir
de Mistral (compte et contrat).

#### 13.2.1 Les voix françaises, clonées par IVèS

Tranché : Mistral ne fournit aucune voix française, donc IVèS crée la
sienne par clonage. La version 1.18.0 a **une seule voix française** :
celle du responsable du chantier, qui a choisi de cloner sa propre voix.

1. **Consentement écrit** de la personne, conservé par IVèS, qui dit :
   l'usage (voix de synthèse du service), la durée, et le droit de retrait.
   La politique d'usage Mistral interdit le clonage sans ce consentement.
2. **Échantillon** : 10 à 20 s de parole française lue calmement, WAV mono,
   sans bruit de fond, sans musique.
3. **Création** : `POST /v1/audio/voices` avec `name`, `sample_audio`
   (base64), `sample_filename`, `languages: ["fr"]`, `gender`. La voix reste
   privée au compte. Noter son `id` et son `slug`.
4. **Écoute** de la phrase de test (§4.2) par au moins deux personnes :
   accent, rythme, intelligibilité. Une voix qui ne convient pas est
   supprimée (`DELETE`) et refaite avec un autre échantillon.
5. **Configuration** : la voix entre dans `voices` (§8.2) avec un alias
   neutre (`fr-homme`), jamais le nom de la personne.
6. **Retrait** : si la personne retire son accord, la voix est supprimée
   chez Mistral et retirée de `voices` ; les clients qui la demandent
   reçoivent 400 et prennent la voix par défaut de la langue.

En attendant ces voix, le développement utilise `en_paul_neutral`, qui
prononce un français intelligible (§4.2).

But : remplacer les sept « à vérifier » du §4.2 par des faits, et produire
les fixtures de rejeu des lots 2 à 4.

1. Obtenir une clé de test Mistral. La poser selon §8.1, option A, sur la
   machine de développement.
2. Un script jetable, hors dépôt, par service :
   - ASR : ouvre le WebSocket Realtime avec `Authorization: Bearer`, envoie
     10 s de PCM 16 kHz lu d'un fichier WAV, enregistre **toutes** les trames
     reçues brutes (texte et binaire) avec leur horodatage. Trois passes :
     `delay` 240, 1 000, 2 400 ms. Une passe avec un paramètre de langue,
     une sans. Une passe où l'on cesse d'envoyer pendant 60 s, pour voir
     quand Mistral ferme.
   - TTS : une requête de synthèse en flux, format `pcm`, une phrase
     française de 15 mots. Enregistrer la réponse brute avec l'horodatage
     de chaque morceau. Déduire : transport, fréquence (longueur du PCM
     contre durée audible), premier son.
   - COMP : trois requêtes chat avec le prompt du §7.5 sur trois phrases
     fautives. Mesurer la latence et le nombre de tokens facturés.
3. Lire les conditions de l'API : entraînement, rétention, hébergement.
   Lire la licence des voix fournies.
4. Écrire les résultats dans le §4.2 (chaque point remplacé par le fait
   observé), le coût mesuré au §4.1, les trois faits sur les données au §9.
5. Copier les enregistrements bruts, anonymisés, dans
   `mcu/tests/fixtures/speech/` : ils font foi pour le faux Mistral.

No-go si : le fil ASR n'est pas exploitable depuis un client C++ (format
non documenté et non déductible), ou si le premier son TTS dépasse 2 s en
médiane, ou si les conditions sur les données sont incompatibles avec un
usage santé ou accessibilité.

### 13.3 Lot 1 : le squelette

État : **fait**. Tout ce qui ne dépend pas de Mistral.

| Classe | Fichier | Rôle | Test |
|---|---|---|---|
| `SpeechConfig`, `SpeechConfigHolder` | `speech/speechconfig.{h,cpp}` | lit le JSON du §8.2 ; clé inconnue = erreur ; objet immuable ; le porteur le remplace d'un bloc au `SIGHUP` | `test_speech_config.cpp` |
| `IPAddress::InPrefix` | `ipaddress.{h,cpp}` | appartenance à `a.b.c.d/n` ou `x::/n`, v4 mappée dé-mappée | `test_ipaddress.cpp` |
| `AllowList` | `speech/allowlist.{h,cpp}` | liste de préfixes, `Allows(IPAddress)` | `test_speech_allowlist.cpp` |
| `TokenStore` | `speech/tokenstore.{h,cpp}` | création (`RAND_bytes`), consommation sous verrou, expiration, compteurs | `test_speech_tokens.cpp` |
| `UsageCounters` | `speech/usage.{h,cpp}` | compteurs globaux et par `account` ; un compte apparaît dès son premier jeton | `test_speech_tokens.cpp` |
| `XmlRpcServer(IPEndpoint)` | `xmlrpcserver.{h,cpp}` | second constructeur : adresse d'écoute donnée, sans profil d'adressage, **sans `/stop`**, sans fichier `http.log` | `test_speech_control.cpp` |
| `ControlHandler` | `speech/controlhandler.{h,cpp}` | monté sur `/` : filtre IP sur tous les chemins par `SessionGetChannelInfo` (exact en IPv4 seulement, §6.1), puis `POST /control/token`, `GET /control/voices`, `GET /status/general` | `test_speech_control.cpp` |
| `SpeechStatus` | `speech/speechstatus.{h,cpp}` | une collecte, deux rendus, comme `StatusHandler` | `test_speech_control.cpp` |
| `StreamingHandler` | `speech/streaminghandler.{h,cpp}` | `WebSocketServer::Handler` : contrôles du §7.1 dans l'ordre, puis `Accept` vers la session que rend la fabrique de la fonction ; possède les sessions ; un thread vérifie leurs bornes | `test_speech_streaming.cpp` |
| `SpeechSession` | `speech/speechsession.{h,cpp}` | base des trois sessions : `ready`, `error`, bornes `idleSecs`/`maxSessionSecs`, message borné à 64 Kio (au-delà : 1009), fermeture avec code | `test_speech_streaming.cpp` |
| `speechmain.cpp` | `speech/speechmain.cpp` | options, clé, `--check`, signaux par `sigwait` dans un thread dédié | `test_speech_server.cpp` lance le vrai binaire |

Ajouts au code partagé, nécessaires au lot :

- `WebSocket::Close(code, reason)` : la pile n'émettait aucune trame Close.
  Elle coupait le TCP, et le client ne recevait aucun code. La trame part
  après la sortie en attente, et plus rien ne la suit.
- `WebSocket::GetRemoteEndpoint()` : l'adresse du client, pour le journal
  (§10).
- `WebSocketServer::Init(IPEndpoint)` : l'adresse d'écoute
  `streaming.listen`.
- `XmlRpcServer` initialise les globales d'Abyss une seule fois par
  processus : un second `MIMETypeInit()` fait un `abort()`, donc un second
  serveur tuait le processus.

Points d'attention :

- Le réacteur WebSocket est mono-thread. `StreamingHandler` ne fait **rien**
  de bloquant : consommer un jeton est un accès à une map sous verrou court.
- Aucune fonction n'a encore de fabrique dans `speechmain.cpp` : le binaire
  du lot 1 répond 503 à toute ouverture de flux. Chaque lot 2 à 4 branche
  la sienne par `StreamingHandler::SetFactory`.
- Les signaux sont bloqués avant la création du premier thread, puis
  attendus par `sigwait` dans un thread dédié. Le rechargement et l'arrêt
  sont donc du code ordinaire, jamais un gestionnaire de signal. Les
  sessions ouvertes gardent leur configuration.
- `/status/general` ne porte encore que la présence de la clé dans
  `backend`. Les lots 2 à 4 y ajoutent le dernier succès et le dernier échec
  de leur service Mistral.

Commit livrable : « speech-server : squelette, jetons et configuration ».

### 13.4 Lot 2 : `asr`

État : **fait**. Essayé contre le vrai Mistral le 2026-10-14 : texte exact,
fermeture 1000 après `done`, compteurs justes (voir aussi « Stabilité »
au §4.2).

| Classe | Fichier | Rôle | Test |
|---|---|---|---|
| `TaskPool` | `speech/taskpool.{h,cpp}` | `mistral.workers` threads, file de tâches. L'ouverture vers Mistral y tourne, parce que `WebSocketServer::Connect` résout le nom d'hôte. Le lot 3 y fera tourner libcurl | `test_speech_asr.cpp` |
| `BackendHealth` | `speech/backendhealth.{h,cpp}` | dernier succès, dernier échec et son motif, par service ; publiés dans `backend` de `/status/general` | `test_speech_asr.cpp` |
| `SpeechBackend` | `speech/speechbackend.h` | ce qu'une session reçoit pour joindre Mistral et compter : réacteur, réserve de threads, clé, compteurs, santé | — |
| `MistralRealtimeClient` | `speech/mistralrealtime.{h,cpp}` | `WebSocket::Listener` de la connexion sortante, en-tête `Authorization`, `session.update`, événements décodés. Chaque événement nomme son client : une session rouverte ignore les événements tardifs de la précédente | `test_speech_asr.cpp` |
| `AsrSession` | `speech/asrsession.{h,cpp}` | relais, `pause`/`resume`, fermeture de Mistral après `pauseCloseSecs` et réouverture, `done`, délais d'ouverture et de fin | `test_speech_asr.cpp` |
| `FakeMistralRealtime` | `tests/fake_mistral_realtime.h` | faux Voxtral Realtime : rejoue les deltas de `asr_binary_d240_lNone_s0.jsonl`, un par trame audio reçue | — |

Ajout au code partagé : `WebSocketServer::Connect(url, listener, headers)`
pose des en-têtes sur la requête d'ouverture. Ils ne sont jamais
journalisés. La vérification TLS sortante est celle du mediaserver : le
magasin du système, le nom d'hôte contrôlé.

Point d'attention : `WebSocketServer::Init()` rend la main avant que son
thread appelle `listen()`. Un test qui se connecte tout de suite au faux
Mistral échoue parfois ; il attend donc que le port réponde.

Commit livrable : « speech-server : reconnaissance vocale par Voxtral Realtime ».

### 13.5 Lot 3 : `tts`

État : **fait**. Essayé contre le vrai Mistral le 2026-10-14 : deux
phrases, retour arrière appliqué, premier son à 0,72 s. L'audio produit,
repassé dans la transcription par lot de Mistral, rend la phrase exacte.

| Classe | Fichier | Rôle | Test |
|---|---|---|---|
| `PhraseSplitter` | `speech/phrasesplitter.{h,cpp}` | classe pure, T.140 → phrases ; **la même** que celle prévue par la SPEC TTS §7.1 | `test_speech_splitter.cpp` |
| `PcmConverter` | `speech/pcmconverter.{h,cpp}` | float32 → S16LE 16 kHz, libswresample ; une entrée coupée au milieu d'un échantillon est acceptée | `test_speech_splitter.cpp` : sinus de 440 Hz |
| `SseParser`, `MistralSpeech` | `speech/mistralspeech.{h,cpp}` | requête libcurl bloquante, dans le `TaskPool` ; lecture SSE au fil de l'eau ; base64 → float32 | `test_speech_tts.cpp` : l'enregistrement du lot 0, coupé en morceaux de 7 777 octets |
| `TtsSession` | `speech/ttssession.{h,cpp}` | `text`/`flush`/`stop`, file de phrases, `say` puis PCM puis `done`, 403 non fatal | `test_speech_tts.cpp` |
| `FakeMistralHttp` | `tests/fake_mistral_http.h` | faux Voxtral TTS sur Abyss ; le texte choisit la réponse (403, 500, lente, enregistrement) | — |

Point d'attention : l'audio d'une phrase part **sous le verrou de la
session**, après un contrôle du jeton d'annulation. C'est ce qui garantit
qu'aucune trame ne suit `stopped`. Le callback libcurl ne fait rien
d'autre ; un `stop` lui fait rendre 0, et libcurl abandonne la requête.

Commit livrable : « speech-server : synthèse vocale par Voxtral TTS ».

### 13.6 Lot 4 : `comp`

État : **fait**. Essayé contre le vrai Mistral le 2026-10-14 : 10 cas de
référence sur 10 avec `mistral-small-latest`.

| Étape | Contenu | Test |
|---|---|---|
| 1 | `MistralHttp` (`speech/mistralhttp.{h,cpp}`) : le POST libcurl commun à la synthèse et au chat | les tests `tts` restent verts |
| 2 | Le prompt, `mcu/src/speech/comp_prompt.txt`, embarqué à la compilation ; dix cas de référence dans `tests/fixtures/speech/comp/cases.json` | `test_speech_comp_reference.cpp`, hors `make check` : `make -C mcu check-speech-comp` |
| 3 | `CompCheck` : relecture de la réponse du modèle, recherche des mots, positions UTF-16 ; une paire n'est gardée que si elle change au plus 2 lettres, ou un tiers du mot (« Pouvez » → « Pouvez-vous » est une réécriture, rejetée) ; classe pure | `test_speech_comp.cpp` |
| 4 | `CompSession` : `check`/`checked`, une demande en cours, une en attente | `test_speech_comp.cpp` |
| 5 | Compteurs : tokens facturés, par `account` ; réponses jetées | `/status/general` |

Commit livrable : « speech-server : correction de texte avant la synthèse ».

### 13.7 Lot 5 : livraison

État : **fait sur AlmaLinux 9** ; Debian et Ubuntu restent à vérifier.

1. `mcumediaserver.spec` : sous-paquet `mediaserver-speech`, autonome
   (§11.3), `%pre` qui crée `speech`, scriptlets systemd, **sans** activation.
2. `install.ksh deb` : un second paquet `mediaserver-speech`, même contenu,
   `preinst`, `conffiles`. Écrit, pas encore construit ni installé.
3. `speech-server.service` (§11.2) et `speech-config.json` d'exemple à la
   racine. `install.ksh localcompile` bâtit aussi `speech-server`.
4. Vérifié le 2026-10-14 sur dev71 (AlmaLinux 9, conteneur LXC), avec le
   seul sous-paquet installé : compte `speech` créé, service non activé,
   refus de démarrer sans clé, démarrage sous `speech`, `tts` et `comp`
   réels, `reload` qui ajoute une voix sans redémarrer, arrêt propre,
   journal écrit malgré `ProtectSystem=strict`, désinstallation propre. La
   clé y passe par la variante LXC (§8.1).
5. `docs/utilisation/speech-server.md`.
6. `README.md` : la cible `speech-server`, ses dépendances, les deux paquets.
7. Version 1.18.0 : `install.ksh`, `version.h`, le changelog du spec.

Commit livrable : « speech-server : paquets, unité systemd et documentation ».

### 13.8 Lot 6 : le mediaserver client (1.19.0)

Hors 1.18.0. Résumé pour mémoire, détaillé lors de la réécriture de la SPEC
TTS : libcurl dans le mediaserver, `SpeechTokenClient` (jeton par
`POST /control/token`, dans un thread de travail), `TtsClient` sur
`WebSocketServer::Connect`, `Synthesizer` conférence et JSR-309, événements,
`/status/general` bloc `tts`, protos MOTELI, clients Java,
`docs/JSR-309-API.md`.

Le mediaserver suit la bulle T.140 de chaque participant. Un retour arrière
qui remonte dans du texte déjà envoyé à la synthèse est une correction : il
envoie `stop`, puis l'annonce de la langue (« Correction : » en français)
suivie du texte retapé. L'annonce est une option du mediaserver.

## 14. Tests

Tous dans `mcu/tests/`, joués par `make check`, **sans réseau** : un faux
Mistral local, bâti avec les mêmes classes (un `WebSocketServer` pour
Realtime, un petit serveur HTTP pour TTS et chat), joue les réponses lues
au lot 0. Un changement de l'API Mistral se voit alors en lisant un seul
fichier de rejeu.

### 14.1 Squelette

- IP hors liste : 403 sans lecture du corps ; IP en liste : 201 ; une v4
  mappée en v6 passe la liste v4 ; un préfixe CIDR v6 fonctionne.
- Jeton : expiré, déjà consommé, d'une autre fonction, inconnu : 403 ;
  `ttl` à 3 601 : 400 ; `account` hors alphabet ou trop long : 400 ;
  `account` présent : repris dans la réponse 201 et dans `usage.byAccount`.
- `limits.maxSessions` atteint : 429 ; `limits.idleSecs` : fermeture 1000 après le
  délai.
- Connexion en IPv4 et en IPv6 sur le port de streaming ; le port de
  contrôle refuse une configuration IPv6 (§6.1).
- La clé absente empêche le démarrage ; la liste d'IP vide, une clé JSON
  inconnue, un fichier absent aussi.
- `SIGHUP` avec une voix ajoutée : la voix sert au flux suivant, un flux
  ouvert continue ; `SIGHUP` avec un fichier invalide : rien ne change.

### 14.2 `asr`

- L'audio binaire arrive au faux Mistral à l'identique, dans l'ordre.
- Les deltas reviennent au client en `text`, dans l'ordre.
- `pause` : plus rien n'atteint le faux Mistral ; `resume` reprend.
- Le faux Mistral ferme : le client reçoit 1013.
- Un relais lent ne retarde pas une autre session (test de non-blocage du
  réacteur).

### 14.3 `tts`

- `abc` + U+0008 + `d.` donne une phrase `abd.` au faux Mistral.
- Deux phrases : deux blocs PCM dans l'ordre, chacun entre son `say` et
  son `done` ; le PCM est en S16LE 16 kHz quel que soit le float32 d'entrée.
- `stop` au milieu : aucune trame binaire après `stopped`.
- Un 403 Mistral sur une phrase ne ferme pas la session.
- Une voix inconnue : 400 avant le 101.

### 14.4 `comp`

- Une faute dans `text` revient corrigée, avec sa position exacte, en
  UTF-16, accents compris.
- Un mot de `context` n'est jamais corrigé.
- Une réponse JSON mal formée, une paire introuvable ou une réécriture sont
  jetées ; la première est comptée.
- Une troisième demande remplace celle qui attend, qui reçoit `superseded`.
- Le prompt, rejoué sur dix phrases de référence en français, rend les
  corrections attendues (test hors `make check`, contre le vrai Mistral,
  lancé à la main avant chaque changement de prompt ou de modèle :
  `make -C mcu check-speech-comp`).

## 15. Recette

- Dans un navigateur, une page de test ouvre les trois flux avec des jetons
  demandés à une application de test : la voix du micro s'écrit, le texte
  tapé se dit, les fautes se corrigent.
- Le même scénario en `wss://` avec un certificat d'autorité connue, depuis
  une page `https://`, et en IPv6.
- Un appel de conférence avec un participant qui tape du T.140, lu par le
  mediaserver via `speech-server`.
- Le parcours du §7.5 de bout en bout : frappe, correction proposée et
  validée, T.140, voix ; puis une correction de texte déjà dit, annoncée
  par le mediaserver.
- **L'écoute des blocs** : le même texte dit en blocs de trois mots, puis en
  phrase entière. Chaque bloc est une requête Mistral, donc une
  énonciation à part : la voix peut sonner hachée. L'écoute fixe la règle
  des trois mots et `tts.phraseSilenceMs` (sans doute vers 500 ms, le
  texte arrivant par blocs et non plus lettre à lettre).
- Coupure de Mistral (route bloquée) pendant les trois flux : les clients
  reçoivent 1013, `/status/general` le montre, les flux rouvrent après le
  retour.
- Le coût d'une heure de recette, lu dans la console Mistral, rapproché des
  compteurs `usage` de `/status/general`.

## 16. Questions ouvertes

Tranché le 2026-10-13 : un seul moteur, Mistral en ligne (le lot 0 Piper de
la SPEC TTS n'a plus lieu) ; corrections ASR en ajout seul (§7.4) ;
configuration en fichier JSON `/etc/mediaserver/speech-config.json` (§8.2) ;
le mediaserver est un client comme les autres, jeton compris (§12) ;
`wss://` obligatoire en production, certificat reconnu par le navigateur
(§7.1) ; les données restent chez Mistral, contrat et rétention au lot 0
(§9) ; une seule clé Mistral par serveur, `account` optionnel pour ventiler
les compteurs (§6.2) ; lots 0 à 5 en 1.18.0, lot 6 en 1.19.0.

Tranché le 2026-10-13, voix française : Mistral ne fournit que des voix
anglaises (§4.2) ; IVèS **clone ses propres voix françaises** (§13.2.1).

Tranché le 2026-10-14, port de contrôle : **IPv4 seul**, avec Abyss
(§6.1). Le besoin d'IPv6 sur ce port est noté pour une version
ultérieure. Deux voies pour le couvrir : un petit serveur HTTP qui fait
lui-même son `accept()` et filtre avant de lire, ou la bibliothèque
libmicrohttpd (dépôt CRB), dont le rappel d'acceptation reçoit l'adresse
complète.

Tranché le 2026-10-14, `comp` : la correction sert la synthèse (§7.5). Le
client découpe, demande, valide, puis envoie en T.140 ; le serveur corrige
sans état. Le client décide aussi de ce qu'il renvoie : un retour arrière
peut remonter dans du texte déjà dit. `speech-server` reste bête et ne
revient pas sur une phrase envoyée ; c'est le mediaserver qui annonce la
correction (§7.3, lot 6). La complétion du mot suivant est reportée après la
1.18.0 ; si elle revient, ce sera par une requête séparée, avec
`ministral-8b`.

Aucune question n'est ouverte pour la 1.18.0.
