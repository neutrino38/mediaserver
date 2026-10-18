# ADR 003 — La synthèse vocale tourne dans un serveur WebSocket séparé

Statut : remplacé en partie par l'[ADR 005](adr-005-speech-server-mistral-en-ligne.md)
(le moteur Piper est abandonné ; la séparation en deux binaires reste).
Conception associée : [TTS](../conception/TTS/SPEC.md).

## Contexte

Le serveur doit transformer du texte en voix, pour deux usages :

- lire à voix haute le texte T.140 d'un participant ;
- prononcer une annonce demandée par le contrôleur.

Le moteur retenu est **Piper**. Piper calcule la voix avec un réseau de
neurones exécuté par **onnxruntime**. Il transforme le texte en phonèmes avec
**espeak-ng**.

Trois faits pèsent sur la décision :

- Ni Piper ni onnxruntime ne sont packagés pour AlmaLinux 9. Il faut les bâtir
  depuis les sources. Seul espeak-ng 1.50 existe (dépôts appstream et CRB).
- espeak-ng est sous licence **GPLv3**, comme le dépôt (`LICENSE.md`). La
  licence ne départage donc pas les options.
- onnxruntime crée ses propres threads. Une erreur dans ce code tue le
  processus qui l'héberge.

## Décision — un binaire serveur TTS, joint par WebSocket

Un second binaire, `mediaserver-tts`, porte Piper. Il est bâti dans ce dépôt,
avec les classes du mediaserver (`WebSocketServer`, `Worker`). Il écoute en
WebSocket. Le texte y entre en trames texte (JSON). La voix en sort en trames
binaires (PCM).

Le mediaserver s'y connecte en client, avec `WebSocketServer::Connect` de la
branche `feat/wss-client`. Il ne lie ni Piper, ni onnxruntime, ni espeak-ng.

Quatre avantages :

- une erreur de Piper tue le serveur TTS, jamais les appels ;
- systemd supervise le serveur TTS comme une seconde unité : le mediaserver
  n'a aucun processus enfant à relancer ;
- le serveur TTS peut tourner sur une autre machine, et servir plusieurs
  mediaservers ;
- la voix arrive en flux : la première phrase se joue pendant que la suivante
  se calcule.

Le coût :

- un protocole à tenir entre deux binaires. Ils peuvent être d'une version
  différente, donc chaque connexion commence par un message `hello` qui porte
  la version du protocole. Une version inconnue ferme la connexion, avec une
  erreur dans le log des deux côtés ;
- un second binaire à livrer, dans un sous-paquet RPM à part ;
- le serveur WebSocket est mono-thread. La synthèse tourne dans des threads de
  travail du serveur TTS, jamais dans un callback du réacteur.

### Écarté : Piper lié dans le mediaserver

C'est la latence la plus faible. Mais une erreur d'onnxruntime coupe tous les
appels du serveur. Et onnxruntime deviendrait une dépendance de tous les
déploiements, même ceux qui n'utilisent pas la synthèse.

### Écarté : des processus `piper` sur pipes

Le mediaserver lance des processus `piper` et les garde en vie. Il écrit le
texte sur leur entrée standard et lit le PCM brut sur leur sortie
(`--output-raw`). Le crash reste isolé, et il n'y a aucun protocole à écrire.

Mais le mediaserver doit alors superviser des processus enfants : les lancer,
détecter leur mort, les relancer, borner leur nombre. Le moteur reste sur la
même machine. Et le PCM brut sur un pipe ne porte aucune marque de fin de
phrase : le mediaserver ne saurait pas quand émettre l'événement de fin.

### Écarté : un service HTTP de TTS tiers

Rien à écrire côté serveur. Mais la voix arrive en un bloc par requête, pas en
flux. C'est aussi un service de plus, d'un autre projet, avec son propre cycle
de version.

## Conséquences

- Sans serveur TTS joignable, le mediaserver démarre et sert les appels. Il
  refuse les commandes de synthèse, et `/status/general` le dit.
- La synthèse dépend de la fusion de `feat/wss-client` dans la branche de
  version.
