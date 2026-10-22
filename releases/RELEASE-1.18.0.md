# Medooze mediaserver 1.18.0

Nouveau service speech-server : reconnaissance vocale, synthèse vocale et correction de texte par Mistral AI, livré dans son propre paquet. Build vérifié sous Ubuntu et Mint

La version publiée est la **release 2** (`1.18.0-2`). Elle corrige la pose de la clé Mistral dans un conteneur LXC.

## Mise à jour : ce qui change pour l'exploitant

- **Nouveau paquet `mediaserver-speech`**. Il est autonome : il ne dépend pas du paquet du mediaserver. Il installe `/opt/ives/bin/speech-server`, l'unité `speech-server.service` et `/etc/mediaserver/speech-config.json`. L'unité tourne sous le compte `speech`. Elle n'est **pas activée** à l'installation
- Le service a besoin d'une clé Mistral AI. L'unité la copie elle-même dans `/run/speech-server` avant chaque démarrage. La procédure est dans `docs/SPEECH-SERVER.md` §3
- Une erreur de configuration ou de clé fait sortir le service avec le code 78. systemd ne le relance pas en boucle
- `systemctl reload speech-server` vérifie le fichier (`--check`) et refuse un fichier invalide, sans arrêter le service
- **Rotation des journaux livrée** : `/etc/logrotate.d/mediaserver` et `/etc/logrotate.d/speech-server`
- **Le journal porte une date ISO sur toutes les lignes.** Avant, les lignes d'erreur et de debug étaient horodatées en secondes Unix. Un script qui lit le journal doit suivre ce changement

## speech-server

Un binaire à part, piloté comme un service. Le calcul se fait en ligne chez Mistral AI : aucun moteur à installer.
Décision : `docs/architecture/adr-005-speech-server-mistral-en-ligne.md`.

Trois fonctions, chacune en flux WebSocket :
- `asr` : de la voix vers le texte (Voxtral Realtime). Le texte arrive par morceaux, ajoutés à la fin, jamais corrigés
- `tts` : du texte vers la voix (Voxtral TTS), phrase par phrase. Sortie PCM 16 bits, 16 kHz
- `comp` : correction d'un morceau de texte avant de le dire

Accès :
- **Port de contrôle** (IPv4) : un client autorisé par une liste d'adresses demande un jeton par `POST /control/token`. Le paramètre `account` permet de ventiler les compteurs
- **Port de streaming** (IPv4 et IPv6, `ws://` ou `wss://`) : un jeton ouvre un seul flux
- `/status/general` publie l'état du service, les compteurs et la santé de Mistral

Les voix sont déclarées dans `speech-config.json` sous un alias. Une voix peut être clonée chez Mistral depuis un échantillon, avec le consentement de la personne.

Documentation exploitant : `docs/SPEECH-SERVER.md`. Conception : `docs/conception/SPEECH-SERVER/SPEC.md`.

## Ajouts au code partagé

- `WebSocket::Close(code, raison)` envoie une vraie trame Close. Avant, le client ne recevait aucun code
- Une connexion WebSocket sortante peut porter des en-têtes en plus (par exemple `Authorization`). Ils ne sont jamais écrits dans le journal
- Un second serveur XML-RPC dans le même processus ne fait plus planter le serveur

## Build et paquets

- **ffmpeg 8 ou plus** est exigé : `check_prereq` refuse un libavutil plus ancien. ffmpeg 8 est accepté pour Ubuntu 26.04 et la prochaine Mint
- Sur Mint 22 ou Ubuntu 24.04, un ffmpeg 9 installé dans `/opt/ffmpeg9` passe en priorité sur celui de la distribution
- `install.ksh prereq` trouve le paquet xmlrpc-c C++ sous apt, quel que soit son numéro (`libxmlrpc-c++8-dev` ou `9`)
- `install.ksh` s'arrête si libmedkit ou libvad ne compile pas. Avant, l'erreur se perdait derrière un message sans rapport
- La compilation du mediaserver sous Ubuntu est vérifiée

## Corrections

- Le bandeau de nom d'une mosaïque masquait toute la vidéo de la case sous ImageMagick 6.9 (Debian, Ubuntu, Mint). Il est de nouveau transparent
- Un test de threads échouait par intermittence. Il attend maintenant que le nombre de threads se stabilise avant de mesurer

# Limitations

- Le paquet `.deb` de speech-server est écrit, mais n'a pas été installé sur Debian ni Ubuntu
- Le port de contrôle écoute en IPv4 seulement. La bibliothèque Abyss tronque l'adresse d'un pair IPv6
- Mistral fournit des voix anglaises seulement pour Voxtral TTS 2 (aucune voix française). Les voix françaises sont des voix clonées
- Voxtral Realtime a été instable pendant les essais : des sessions fermées par Mistral (erreur 3800)
- Les clés `asr.vad` et `asr.silenceMs` sont lues mais restent sans effet
- Le mediaserver ne se sert pas encore de speech-server. Le client arrive en 1.19.0
