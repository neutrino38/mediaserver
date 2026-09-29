# Medooze mediaserver 1.16.0

Passage à ffmpeg 9, accélération matérielle VAAPI qui marche vraiment et se prouve au démarrage, pile BFCP interne en TCP et UDP, jambe texte WebSocket sortante, enregistrement JSR-309 en `.mkv`, panne du texte sur WebSocket corrigée

## Mise à jour : ce qui change pour l'exploitant

- Le serveur demande **ffmpeg 9** (paquet `ffmpeg-devel` du dépôt IVèS). Il ne se construit plus contre une version plus ancienne
- **Le GPU est utilisé par défaut** quand un device VAAPI est présent. L'option `--no-hwaccel` le désactive. Elle se place dans `OPTIONS=` de `/etc/sysconfig/mediaserver`
- À chaque démarrage, une sonde teste le GPU dans un processus séparé. Elle a pris entre 0,23 s et 0,35 s dans nos mesures. Son délai maximal se règle par `--hwprobe-timeout` (10 s par défaut)
- La bibliothèque externe **libbfcp** n'est plus utilisée. Le BFCP est porté par le serveur lui-même
- La détection de voix (VAD) utilise **libfvad**. La dépendance `webrtc-audio-processing` disparaît
- Le projet est publié sous licence **GPL v3** (fichier `LICENSE.md`). La notice BSD de libfvad est dans `LICENSE.libfvad`, installée par les paquets

## Accélération matérielle (VAAPI)

Avant cette version, le chemin GPU ne fonctionnait pas, même quand `/status/general` annonçait `vaapi: true`.

- L'encodage H.264 matériel produit enfin des images. Les surfaces VAAPI étaient allouées en YUV420P, alors que le driver iHD n'encode que du NV12. L'encodeur ne rendait rien, puis arrêtait le processus après environ seize images
- La composition des mosaïques sur GPU fonctionne. Le graphe ne se construisait jamais : `hwupload` recevait son device trop tard. Toutes les mosaïques étaient composées sur CPU
- Le graphe de mosaïque n'est plus reconstruit à chaque image. Il dépendait d'une référence qui change à chaque image décodée : 550 ms par composition, puis manque de mémoire à 16 mixeurs
- Le décodeur H.264 accepte sur GPU les flux Baseline des endpoints SIP. Nos propres flux Baseline gardent `constraint_set1` et se décodent aussi sur GPU
- Le prologue et le logo ne produisaient rien sur GPU. C'est corrigé
- **Sonde de démarrage** : `mediaserver --hwprobe` teste chaque capacité GPU sur une vraie image (encodage, décodage, mise à l'échelle, mosaïque). Le serveur la lance à chaque démarrage dans un processus fils. Un driver qui plante la sonde ne fait donc plus tomber le serveur. Une capacité en échec est refusée, et le serveur la traite sur CPU. Un échec du device lui-même désactive tout le GPU. Décision : `docs/architecture/adr-002-sonde-gpu-hors-processus.md`
- `/status/general` publie les compteurs `VideoAccel` dans le bloc `hardware` : encodeurs et décodeurs ouverts, combien sur GPU, replis depuis le démarrage. Un décodeur compte comme matériel seulement après sa première surface GPU. Un refus de la sonde n'est pas un repli. Référence : `docs/reference/status-http.md` §3.7 et §3.8
- Construit contre un libavfilter plus ancien que la version 11 (ffmpeg 8), le serveur désactive le GPU et l'écrit dans le log

## BFCP : pile interne, TCP et UDP

Le BFCP (Binary Floor Control Protocol) contrôle qui partage un document ou un écran. La bibliothèque libbfcp est remplacée par la pile interne du serveur (`mcu/src/bfcp/`). Décision : `docs/architecture/adr-002-bfcp-pile-interne.md`.

- Codec binaire RFC 4582, en-tête RFC 8855 (versions 1 et 2)
- Transport **TCP** : une écoute par conférence, en IPv4 et IPv6. libbfcp n'écoutait qu'en IPv4. Plusieurs messages reçus en une seule lecture sont tous traités
- Transport **UDP** : une socket par participant, en IPv4 et IPv6. Le premier datagramme reçu fixe l'adresse du pair, ce qui traverse un NAT. Retransmission selon RFC 8855 (500 ms, puis doublement, abandon après 16 s). Le serveur répond dans la version du pair : les endpoints en service parlent la version 1
- Le port annoncé est celui que la socket a réellement pris. Avant, une socket de test choisissait un port libre, puis le relâchait, et un autre processus pouvait le prendre
- Les deux transports tournent sur le réacteur RTP partagé. Aucun thread supplémentaire
- Corrigé : un participant retiré gardait la parole que le serveur lui avait reprise. La notification `Revoked` n'avait plus de destinataire
- Corrigé : `SetSharedMosaic` ne retenait la mosaïque que si quelqu'un partageait déjà
- Périmètre : API MCU seulement, sans TLS. L'API XML-RPC ne change pas. Identité à écrire dans le SDP (`confid`, `userid`) : `docs/MCU-API.md`. Pièges du protocole : `docs/reference/bfcp.md`

## JSR-309

- **Jambe texte WebSocket sortante** : le serveur peut ouvrir lui-même une connexion `ws://` ou `wss://`, comme le ferait un navigateur. Nouvelle méthode XML-RPC `ConnectMediaConnection(sessionId, endpointId, media, role, url)`, texte T.140 seulement. Le succès dit que la jambe est armée, pas qu'elle est ouverte. L'ouverture et chaque perte arrivent par la file d'événements (`EndpointConnectedEvent`, `EndpointDisconnectedEvent`). Usage prévu : recette automatisée d'appels « total conversation » par elixip. Conception : `docs/conception/WS-CLIENT/SPEC.md`
- Le client Java XML-RPC (`XmlRpcMcuClient`) est aligné sur l'API
- Le Recorder JSR-309 écrit en **`.mkv`** et en **`.mp4`** (libavformat). Le conteneur suit l'extension du nom de fichier. Le PCMU est écrit tel quel en `.mkv`, et transcodé en AAC en `.mp4`. Les `RecorderAttachTo…` doivent précéder `RecorderRecord`. Contrat : `docs/JSR-309-API.md` §6.4
- Un profil d'adressage demandé sur un endpoint texte WebSocket est accepté

## Correction de la panne du texte sur WebSocket

Un appel avec Recorder perdait tout son média environ 15 s après le décroché : plus d'audio, plus de texte, vidéo figée. Un seul défaut, quatre maillons.

- Le keepalive BOM est renvoyé avec un numéro de séquence croissant
- Le décodage RED ne part plus en boucle. Un en-tête dont la longueur ment produisait 4,28 milliards de caractères de remplacement
- `VideoDecoderJoinableWorker::DecodePacket` portait un défaut du même type. Il est corrigé
- Les traces n'écrivent plus le contenu des sous-titres texte à l'enregistrement
- L'arrêt d'un pont texte WebSocket (`ParticipantTextWS`) ne se perd plus. Le thread écrasait l'ordre d'arrêt, tournait sans fin sur un cœur, et l'appelant attendait pour toujours. Cas observé en production

## RTP, DTLS et STUN

- Changement de SSRC : le réacteur RTP ne reste plus bloqué. Un écran noir de 34 s disparaît
- Un `ClientHello` DTLS reçu **avant** `SetRemoteCryptoDTLS` est conservé, puis rejoué et répondu dès l'initialisation. Le pair n'attend plus sa retransmission, une seconde plus tard
- Latching NAT : une nouvelle cible posée par le plan de contrôle efface la source observée. Un pair qui se déplace est suivi, même si l'émission n'a pas cessé
- Un serveur STUN donné en littéral IPv6 est refusé avec un message explicite. Le NAT servi par ce produit est IPv4 seulement
- Amortisseur de débit : sous un plafond externe, seule la valeur **annoncée** compte. Une hausse de la mesure locale qui laisse ce plafond inchangé ne réémet plus de TMMBR
- AMF : le zéro fait un aller-retour exact, signe compris, au lieu de rendre un dénormal

## Détection de voix (VAD)

- La VAD repose sur libfvad (sous-module `third_party/libvad`). Ses quatre niveaux d'agressivité sont de nouveau disponibles sur toutes les distributions
- Corrigé : la VAD ne tournait jamais sur une jambe à 48 kHz
- Référence : `docs/reference/vad.md`

## Build et paquets

- Portage **ffmpeg 9**, et compilation avec GCC 15
- `install.ksh` reconnaît `apt` et produit un paquet `.deb` (`./install.ksh deb`). Le RPM reste réservé à AlmaLinux 9. La même unité systemd sert les deux familles
- Sur Debian et Ubuntu, le nom d'hôte pointe sur 127.0.1.1. Le serveur refusait alors de démarrer, faute d'adresse à annoncer. Il prend maintenant la première adresse annonçable d'une interface active
- Le binaire est relié de nouveau quand `libmedkit.a` change. Avant, il pouvait garder l'ancienne bibliothèque sans le dire
- Piège de mise à jour : après un changement du paquet `ffmpeg-devel`, lancer `./install.ksh clean` avant de reconstruire. Sinon les objets mélangent deux ABI

## Sous-module libmedikit

- Portage ffmpeg 9 (dont `AV_PROFILE_AAC_LOW`)
- `FfMediaFileWriter` : écriture `.mp4` et `.mkv` par libavformat, avec annonce de piste (`ExpectTrack`)
- Corrections VAAPI : surfaces NV12, `hw_frames_ctx` posé avant l'initialisation du buffersrc, encodeur matériel qui retient sa première image, aucun drainage d'un encodeur jamais alimenté. Les cinq règles d'allocation sont dans `third_party/fontventa/CLAUDE.md`
- Registre des refus GPU (`VideoAccel::RefuseHw`), alimenté par la sonde de démarrage
- Verrou autour de l'ouverture et de la fermeture des encodeurs SVT-AV1. Une version de SVT-AV1 antérieure à 4.1 plante quand deux encodeurs AV1 s'ouvrent en même temps
- Le décodeur vidéo rend toutes les images en attente
- Transcodeur remanié, `framescaler` supprimé. Parseur RED durci

## Tests et documentation

- Nouvelles suites : BFCP (codec contre des octets écrits depuis la RFC, serveur de contrôle de parole, transports TCP et UDP sur de vraies sockets), sonde GPU, VAD, fichiers produits par le Recorder JSR-309, profil d'adressage d'un endpoint, décodeur de redondance texte, famine de l'écrivain dans `Use`
- Un chien de garde arrête un test figé et le nomme. Budget par test : `GTEST_MCU_WATCHDOG_S`, 120 s par défaut
- La suite mcu affiche maintenant les messages de libmedikit. Avant, ses erreurs étaient perdues
- Les tests GPU exigent une composition VAAPI quand un device existe. Avant, ils acceptaient le repli CPU, ce qui cachait les défauts
- Revue de rationalisation des tests : `docs/conception/TESTS-RATIONALISATION/SPEC.md`
- État mesuré avant publication : À COMPLÉTER

# Limitations

- La sonde GPU décide de ce qui tourne sur GPU, mais `/status/general` ne publie pas encore ses verdicts. Ils sont seulement dans le log (lot 4 de `docs/conception/HWACCEL-SONDE/SPEC.md`)
- La sonde prouve qu'une capacité marche à petite taille. Elle ne prouve pas qu'elle tient la charge. Le repli logiciel en cours d'appel reste actif
- La composition de mosaïques sur GPU réel n'a pas encore été validée en appel de production
- La pile BFCP interne est validée par les tests seulement. La recette avec de vrais endpoints reste à faire
- La correction de la panne du texte sur WebSocket est **validée par les tests seulement**. La recette en appel de bout en bout reste à faire : il faut dépasser deux keepalives BOM, soit plus de 30 s après le décroché
- Le mediaserver ne lit **aucun SR** de la jambe Asterisk 1.4. Ce pair émet ses compounds RTCP avec un octet de queue en trop, et le parseur jette alors le datagramme entier. Donc ni RTT, ni pertes rapportées de ce côté. Non corrigé
- Le contrôle de débit (rate control) n'est toujours pas satisfaisant. Le chantier est reporté à une prochaine version
- Un flux vidéo VP8 ne s'enregistre toujours pas en MP4 : le conteneur ne porte pas ce codec, et le transcodage VP8 vers H.264 n'est pas fait. Enregistrer en `.mkv` est la réponse
- `GetSupportedCodecs` (XML-RPC `/mcu`) reste une liste écrite à la main, sans OPUS. Interroger `/status/general` à la place
