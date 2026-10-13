# Medooze mediaserver 1.16.0

Passage à ffmpeg 9, accélération matérielle VAAPI qui marche vraiment et se prouve au démarrage, pile BFCP interne en TCP et UDP, jambe texte WebSocket sortante, enregistrement JSR-309 en `.mkv`, panne du texte sur WebSocket corrigée

## Mise à jour : ce qui change pour l'exploitant

- Le serveur demande **ffmpeg 9** (paquet `ffmpeg-devel` du dépôt IVèS). Il ne se construit plus contre une version plus ancienne
- **Le GPU est utilisé par défaut** quand un device VAAPI est présent. L'option `--no-hwaccel` le désactive. Elle se place dans `OPTIONS=` de `/etc/sysconfig/mediaserver`
- À chaque démarrage, une sonde teste le GPU dans un processus séparé. Elle a pris entre 0,23 s et 0,35 s dans nos mesures. Son délai maximal se règle par `--hwprobe-timeout` (10 s par défaut)
- La détection de voix (VAD) utilise **libfvad**. La dépendance `webrtc-audio-processing` disparaît
- Le projet est publié sous licence **GPL v3** (fichier `LICENSE.md`). La notice BSD de libfvad est dans `LICENSE.libfvad`, installée par les paquets

## Accélération matérielle (VAAPI)

Avant cette version, le chemin GPU ne fonctionnait pas, même quand `/status/general` annonçait `vaapi: true`.

- L'encodage H.264 matériel produit enfin des images. 
- La composition des mosaïques sur GPU fonctionne. 
- Le décodeur H.264 accepte sur GPU les flux Baseline des endpoints SIP. Nos propres flux Baseline gardent `constraint_set1` et se décodent aussi sur GPU
- Le prologue et le logo ne produisaient rien sur GPU. C'est corrigé
- **Sonde de démarrage** : `mediaserver --hwprobe` teste chaque capacité GPU sur une vraie image (encodage, décodage, mise à l'échelle, mosaïque). Le serveur la lance à chaque démarrage dans un processus fils. Un driver qui plante la sonde ne fait donc plus tomber le serveur. Une capacité en échec est refusée, et le serveur la traite sur CPU. Un échec du device lui-même désactive tout le GPU. Décision : `docs/architecture/adr-002-sonde-gpu-hors-processus.md`
- `/status/general` publie les compteurs `VideoAccel` dans le bloc `hardware` : encodeurs et décodeurs ouverts, combien sur GPU, 

## BFCP : pile interne, TCP et UDP

Le BFCP (Binary Floor Control Protocol) contrôle qui partage un document ou un écran. La bibliothèque libbfcp est remplacée par la pile interne du serveur (`mcu/src/bfcp/`).
Décision : `docs/architecture/adr-002-bfcp-pile-interne.md`.

- Codec binaire RFC 4582, en-tête RFC 8855 (versions 1 et 2)
- Transport **TCP** : une écoute par conférence, en IPv4 et IPv6. 
- Transport **UDP** : une socket par participant, en IPv4 et IPv6. 
- Le port annoncé est celui que la socket a réellement pris. Avant, une socket de test choisissait un port libre, puis le relâchait, et un autre processus pouvait le prendre
- Périmètre : API MCU seulement, sans TLS. L'API XML-RPC ne change pas. 

## JSR-309

- **Client texte WebSocket sortante** : le serveur peut ouvrir lui-même une connexion `ws://` ou `wss://`, comme le ferait un navigateur.
- Nouvelle méthode XML-RPC `ConnectMediaConnection(sessionId, endpointId, media, role, url)`, texte T.140 seulement. 
- Le client Java XML-RPC (`XmlRpcMcuClient`) est aligné sur l'API
- Le Recorder JSR-309 écrit en **`.mkv`** et en **`.mp4`** (libavformat). Le conteneur suit l'extension du nom de fichier. 

## Correction de blocages sur WebSocket

Un appel avec Recorder perdait tout son média environ 15 s après le décroché : plus d'audio, plus de texte, vidéo figée. Un seul défaut, quatre maillons.

- Le keepalive BOM est renvoyé avec un numéro de séquence croissant
- Correction d'une boucle quasi infinie dans le décodage RED. Idem dans`VideoDecoderJoinableWorker::DecodePacket`.
- Les traces n'écrivent plus le contenu des sous-titres texte à l'enregistrement
- L'arrêt d'un pont texte WebSocket (`ParticipantTextWS`) ne se perd plus et la thread correspondante s'arrête correctement.

## RTP, DTLS et STUN

diverses corrections.

## Détection de voix (VAD)

- La VAD repose sur libfvad (sous-module `third_party/libvad`). 

## Build et paquets

- Portage **ffmpeg 9**, et compilation avec GCC 15
- `install.ksh` reconnaît `apt` et produit un paquet `.deb` (`./install.ksh deb`). Le RPM reste réservé à AlmaLinux 9. La même unité systemd sert les deux familles

## Sous-module libmedikit

- Portage ffmpeg 9 (dont `AV_PROFILE_AAC_LOW`)
- `FfMediaFileWriter` : écriture `.mp4` et `.mkv` par libavformat, avec annonce de piste (`ExpectTrack`)
- Corrections VAAPI : surfaces NV12, `hw_frames_ctx` posé avant l'initialisation du buffersrc, encodeur matériel qui retient sa première image, aucun drainage d'un encodeur jamais alimenté. Les cinq règles d'allocation sont dans `third_party/fontventa/CLAUDE.md`
- Registre des refus GPU (`VideoAccel::RefuseHw`), alimenté par la sonde de démarrage
- Verrou autour de l'ouverture et de la fermeture des encodeurs SVT-AV1. Une version de SVT-AV1 antérieure à 4.1 plante quand deux encodeurs AV1 s'ouvrent en même temps
- Le décodeur vidéo rend toutes les images en attente
- Transcodeur remanié, `framescaler` supprimé. Parseur RED durci

## Tests et documentation

- Nouvelles suites : BFCP 
- Un chien de garde arrête un test figé et le nomme. Budget par test : `GTEST_MCU_WATCHDOG_S`, 120 s par défaut
- La suite mcu affiche maintenant les messages de libmedikit. Avant, ses erreurs étaient perdues
- Les tests GPU exigent une composition VAAPI quand un device existe. Avant, ils acceptaient le repli CPU, ce qui cachait les défauts
- Revue de rationalisation des tests : `docs/conception/TESTS-RATIONALISATION/SPEC.md`

# Limitations

- La sonde GPU décide de ce qui tourne sur GPU, mais `/status/general` ne publie pas encore ses verdicts. Ils sont seulement dans le log (lot 4 de `docs/conception/HWACCEL-SONDE/SPEC.md`)
- La sonde prouve qu'une capacité marche à petite taille. Elle ne prouve pas qu'elle tient la charge. Le repli logiciel en cours d'appel reste actif
- La composition de mosaïques sur GPU réel n'a pas encore été validée en appel de production
- La pile BFCP interne est validée par les tests seulement. La recette avec de vrais endpoints reste à faire
- Le contrôle de débit (rate control) n'est toujours pas satisfaisant. Le chantier est reporté à une prochaine version
- `GetSupportedCodecs` (XML-RPC `/mcu`) reste une liste écrite à la main, sans OPUS. Interroger `/status/general` à la place
