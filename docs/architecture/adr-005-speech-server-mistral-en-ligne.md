# ADR 005 — La voix est calculée en ligne par Mistral, dans un `speech-server` séparé

Statut : accepté, implémenté en 1.18.0 (lots 0 à 5).
Conception associée : [SPEECH-SERVER](../conception/SPEECH-SERVER/SPEC.md).
Remplace en partie : [ADR 003](adr-003-tts-serveur-websocket.md), dont la
séparation en deux binaires reste vraie et dont le moteur (Piper) est
abandonné.

## Contexte

L'ADR 003 prévoyait un serveur TTS séparé, autour de Piper. Le besoin a
grandi avant qu'il soit écrit : un utilisateur sourd ou malentendant, dans
un navigateur, doit lire la voix de son interlocuteur (ASR), parler par le
texte (TTS) et faire corriger son texte avant qu'il soit dit (COMP). Le
mediaserver reste un second client, pour la lecture du T.140 et les
annonces.

Quatre faits pèsent sur la décision :

- **Piper et onnxruntime ne sont pas packagés pour AlmaLinux 9**, et Piper
  ne fait que la synthèse : l'ASR et la correction demandaient chacun un
  autre moteur local, avec ses propres modèles.
- **Mistral rend les trois fonctions par API** : Voxtral Realtime en `wss`
  pour l'ASR, Voxtral TTS et le chat en `https`. La sonde du lot 0
  (SPEC §4.2) l'a vérifié sur le fil : premier son à 0,47 s, transcription
  en flux.
- **Les poids de Voxtral TTS sont en CC BY-NC** : un TTS Voxtral local est
  interdit en usage commercial. Ceux de Voxtral Realtime sont en
  Apache 2.0.
- **Mistral ne fournit aucune voix française** (10 voix anglaises), mais
  clone une voix depuis un échantillon.

## Décision 1 — Un seul moteur : Mistral AI, en ligne

Le serveur ne porte aucun modèle. Il tient les connexions, vérifie les
jetons, adapte les formats (PCM S16LE 16 kHz dans les deux sens), borne la
charge et compte ce qui est consommé. Mistral calcule.

### Écarté : Piper (l'ADR 003)

Bâti depuis les sources avec onnxruntime, pour la synthèse seule. Il
laissait l'ASR et la correction sans moteur.

### Écarté : Voxtral local en vLLM

Un GPU par serveur, et la licence interdit le TTS en usage commercial. Il
reste une issue de secours pour l'ASR seul : `mistral.url` peut un jour
pointer sur un vLLM local (SPEC §9).

### Conséquences

- **La voix et le texte des utilisateurs quittent l'infrastructure** vers
  Mistral, sous-traitant au sens du RGPD (SPEC §9). L'information des
  utilisateurs est portée par l'application.
- **Le service dépend du réseau et de la disponibilité de Mistral.**
  Realtime a fermé 6 sessions sur 8 le 2026-10-14 (erreur 3800,
  `QueueOverflowError`). Le serveur le dit (`/status/general`, fermeture
  1013) ; il ne bascule sur rien.
- Le coût est à l'usage (caractères synthétisés, durée transcrite, jetons
  de correction), ventilable par le champ `account` d'une seule clé.
- L'ASR est **en ajout seul** : Mistral ne corrige jamais un delta déjà
  rendu. Le T.140 avec retours arrière, demandé au départ, n'existe pas
  chez le fournisseur.
- **Les voix françaises sont clonées par IVèS**, avec le consentement écrit
  de la personne (SPEC §13.2.1). Une voix clonée est privée au compte
  Mistral : son identifiant ne sert qu'avec une clé de ce compte.

## Décision 2 — Un binaire `speech-server`, une unité, un paquet

Ce que l'ADR 003 décidait pour Piper vaut pour le nouveau serveur : un
binaire à part, bâti dans ce dépôt avec les classes du mediaserver
(`WebSocketServer`, Abyss, `Worker`), sous sa propre unité systemd
(`speech-server.service`, compte `speech`) et dans un sous-paquet
autonome (`mediaserver-speech`), qui ne dépend pas du paquet principal.

Les raisons changent : il n'y a plus de bibliothèque à isoler, mais le
serveur sert des navigateurs, doit tenir une charge propre et peut tourner
sur une autre machine que le mediaserver.

### Conséquences

- Le mediaserver sans `speech-server` sert les appels et refuse la synthèse.
- Les appels `https` vers Mistral tournent dans des threads de travail
  (`mistral.workers`) ; une session ASR est un relais entre deux connexions
  du même réacteur, sans thread.

## Décision 3 — Deux ports : des jetons filtrés par IP, un streaming public

Le port de contrôle (Abyss) crée des jetons (`POST /control/token`) et ne
répond qu'aux adresses de `control.allow`. Le port de streaming
(WebSocket) est ouvert à tous, et c'est le jeton — à usage unique, d'une
fonction donnée, à durée courte — qui filtre. `wss://` est obligatoire en
production : le jeton passe dans l'URL, et une page `https://` ne sait
ouvrir rien d'autre.

**Le mediaserver est un client comme les autres** : il demande un jeton,
ouvre une WebSocket par session, et rien ne le distingue côté serveur.

### Écarté : un accès privilégié pour le mediaserver

Un second chemin d'authentification, donc un second jeu de tests et de
failles possibles, pour économiser une requête HTTP par session.

### Écarté pour la 1.18.0 : un petit serveur HTTP maison pour le contrôle

Abyss (xmlrpc-c 1.51) tronque l'adresse du pair dans un `struct sockaddr`
de 16 octets : en double pile, un client `127.0.0.1` arrive avec une
adresse vide, et le filtre serait faux. Un serveur qui fait son propre
`accept()` aurait couvert IPv6. Il a été écarté au profit d'Abyss, déjà là
et testé.

### Conséquences

- **Le port de contrôle n'écoute qu'en IPv4** ; une entrée IPv6 dans
  `control.allow` est refusée au démarrage. Le port de streaming reste
  double pile.
- Le mediaserver gagnera un client HTTP (libcurl) au lot 6.

## Décision 4 — La clé Mistral : un fichier root, copié par l'unité

La clé vit dans `/etc/mediaserver/speech-api.key`, `0600 root`. Avant
chaque démarrage, l'unité la copie en root (`ExecStartPre=+`) dans
`/run/speech-server/api.key` (`RuntimeDirectory`, tmpfs, `0400 speech`) ;
la copie disparaît à l'arrêt. Le serveur la lit une fois et ne l'écrit
nulle part. La variante chiffrée (`LoadCredentialEncrypted` par drop-in)
reste possible là où systemd sait chiffrer, et le serveur préfère alors
`$CREDENTIALS_DIRECTORY`.

### Écarté : `LoadCredential=`

C'était le premier choix (livré en release 1). Dans un conteneur LXC
Proxmox, l'hôte le vide pour tous les services, et le remettre après son
drop-in laisse un `$CREDENTIALS_DIRECTORY` vide (mesuré sur dev71 le
2026-10-14). La copie par l'unité donne la même protection partout, sans
variante à poser à la main.

### Écarté : la clé dans la configuration, l'environnement ou `argv`

Elle finirait dans les tickets, `/proc/<pid>/environ` ou `ps`.

### Écarté : un gestionnaire de secrets (Vault, OpenBao)

Un service de plus, et un second secret pour parler au coffre.

### Conséquences

- Pas de clé chiffrée sur disque en conteneur LXC : `systemd-creds` ne peut
  y créer sa clé d'hôte. Il faut une VM avec TPM virtuel, ou un coffre.
- Une erreur de configuration ou de clé sort en 78 (`EX_CONFIG`), que
  systemd ne relance pas (`RestartPreventExitStatus=78`).

## Décision 5 — Le `speech-server` reste bête

`comp` est sans état : le client découpe son texte, demande une
correction, la valide, puis l'envoie en T.140. `tts` ne revient jamais sur
une phrase déjà envoyée, même si un retour arrière la touche. C'est le
client — le mediaserver au lot 6 — qui détecte la correction et la
prononce (« Correction : … »).

### Conséquences

- Le serveur n'a aucun état conversationnel à garder, ni à perdre au
  redémarrage.
- La logique de correction vit chez chaque client, et le navigateur devra
  faire la sienne.
