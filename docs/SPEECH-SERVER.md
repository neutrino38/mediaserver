# speech-server : guide de l'exploitant

`speech-server` rend trois services vocaux, calculés en ligne par Mistral AI :

| Fonction | Ce qu'elle fait |
|---|---|
| `asr` | transforme la voix en texte, au fil de la parole |
| `tts` | transforme le texte en voix, au fil de la frappe |
| `comp` | corrige l'orthographe et la grammaire d'un texte avant qu'il soit dit |

Le serveur ne porte aucun modèle. Il tient les connexions, vérifie les
jetons, convertit les formats audio et compte la consommation. Il est livré
dans un paquet à part, `mediaserver-speech`, qui ne dépend pas du
mediaserver : il s'installe sur la même machine ou sur une machine dédiée.

Ce document s'adresse à l'exploitant et au développeur d'application
cliente. La conception et ses choix sont dans
[la SPEC](conception/SPEECH-SERVER/SPEC.md) ; la construction depuis les
sources, dans le [README](../README.md).

Sommaire :

1. [Avant d'installer](#1-avant-dinstaller)
2. [Installer](#2-installer)
3. [Poser la clé Mistral](#3-poser-la-clé-mistral)
4. [Configurer](#4-configurer)
5. [Démarrer, recharger, arrêter](#5-démarrer-recharger-arrêter)
6. [Réseau et pare-feu](#6-réseau-et-pare-feu)
7. [API de contrôle](#7-api-de-contrôle-http)
8. [API de streaming](#8-api-de-streaming-websocket)
9. [Surveiller](#9-surveiller)
10. [Dépannage](#10-dépannage)
11. [Désinstaller](#11-désinstaller)

---

## 1. Avant d'installer

- **Un compte Mistral AI et une clé d'API.** Une seule clé par serveur ;
  toute la consommation passe par elle.
- **Un accès sortant HTTPS** (port 443) vers `api.mistral.ai`, en IPv4 ou
  IPv6.
- **Un certificat TLS reconnu par les navigateurs** pour le nom DNS sous
  lequel les pages joindront le serveur (Let's Encrypt, ou l'autorité
  interne déployée sur les postes). Un certificat auto-signé est refusé par
  les navigateurs, sans recours possible.
- **Les données quittent l'infrastructure.** La voix et le texte partent
  chez Mistral AI, sous-traitant au sens du RGPD. Avant toute mise en
  service, vérifier dans le compte et le contrat Mistral : si les données
  servent à entraîner les modèles (réglage du compte), leur durée de
  rétention et leur lieu d'hébergement. Informer les utilisateurs.
- **La synthèse vocale reste en ligne.** Les poids du modèle de synthèse
  de Mistral sont sous licence non commerciale : aucune synthèse locale
  n'est possible sans licence négociée.
- **Les voix.** Seules des voix fournies par Mistral, ou des voix clonées
  avec le consentement écrit de la personne, peuvent être déclarées.
- **Le service temps réel de Mistral peut être instable.** Le 2026-10-14,
  6 sessions `asr` sur 8 ont été fermées par Mistral. Le client reçoit
  alors le code de fermeture 1013 (« réessayer plus tard »), et
  `/status/general` montre l'échec.

Systèmes pris en charge : AlmaLinux 9 (paquet RPM), Debian et Ubuntu
(paquet .deb, systemd 247 ou plus).

## 2. Installer

AlmaLinux 9 :

```sh
dnf install ./mediaserver-speech-1.18.0-2.ives.el9.x86_64.rpm
```

Debian ou Ubuntu :

```sh
apt install ./mediaserver-speech_1.18.0_amd64.deb
```

Le paquet installe :

| Élément | Rôle |
|---|---|
| `/opt/ives/bin/speech-server` | le binaire |
| `/etc/mediaserver/speech-config.json` | la configuration, conservée lors des mises à jour |
| unité `speech-server.service` | le service systemd |
| compte système `speech` | le compte sous lequel le service tourne |
| `/var/log/speech-server.log` | le journal (créé au premier démarrage) |
| `/etc/logrotate.d/speech-server` | la rotation du journal, conservée lors des mises à jour |

Le paquet ne livre **aucun fichier de clé**, et le service **n'est pas
activé** : sans clé Mistral, il refuse de démarrer. À l'installation, le
paquet signale ce qui reste à faire :

- `No Mistral key yet` : la clé n'est pas posée, il affiche la commande (§3) ;
- `WARNING: … must belong to root, mode 0600` : la clé est posée, mais
  d'autres que root peuvent la lire ;
- `Installation on LXC detected` : simple information, rien à faire de plus
  (§3.3).

Fabriquer le paquet depuis les sources (machine de build) :

```sh
./install.ksh rpm nosign     # produit aussi mediaserver-speech-*.rpm
```

## 3. Poser la clé Mistral

La clé n'apparaît jamais dans la configuration, sur la ligne de commande ni
dans l'environnement. Avant chaque démarrage, l'unité la copie, en root,
dans `/run/speech-server/api.key` : un répertoire en mémoire réservé au
compte `speech`, qui disparaît à l'arrêt. Le service la garde en mémoire
sans l'écrire nulle part.

Choisir **une** des deux variantes ci-dessous. Dans les exemples, `$KEY`
contient la clé ; la saisir plutôt par `read -rs KEY` que dans l'historique
du shell.

### 3.1 Variante simple : la clé en clair, lisible par root seul

```sh
install -m 0600 -o root -g root /dev/stdin /etc/mediaserver/speech-api.key <<< "$KEY"
```

C'est la variante attendue par l'unité livrée : rien d'autre à faire, sur
une machine physique, une VM ou un conteneur LXC.

### 3.2 Variante chiffrée : la clé liée à la machine

> **Pas en conteneur LXC** : `systemd-creds` ne peut rien y chiffrer. Voir §3.3.

La clé est chiffrée par `systemd-creds`, avec le TPM s'il existe, sinon avec
la clé d'hôte de systemd. Une copie du fichier hors de la machine
(sauvegarde, image) est illisible. Demande systemd 250 ou plus (AlmaLinux 9,
Debian 12).

```sh
systemd-creds encrypt --name=mistral-api-key - /etc/mediaserver/speech-api.cred <<< "$KEY"
mkdir -p /etc/systemd/system/speech-server.service.d
cat > /etc/systemd/system/speech-server.service.d/key.conf <<'EOF'
[Service]
LoadCredentialEncrypted=mistral-api-key:/etc/mediaserver/speech-api.cred
EOF
systemctl daemon-reload
```

Le service préfère alors la clé déchiffrée par systemd
(`$CREDENTIALS_DIRECTORY`) à la copie de §3.1. Ne pas laisser de clé en
clair dans `/etc/mediaserver/speech-api.key` : elle serait encore copiée,
et la protection serait perdue.

Deux règles :

- ne jamais sauvegarder la clé d'hôte (`/var/lib/systemd/credential.secret`)
  avec le fichier chiffré ;
- après une réinstallation de la machine, rechiffrer la clé.

### 3.3 Conteneur LXC (Proxmox)

La variante 3.1 fonctionne telle quelle. L'unité n'utilise pas
`LoadCredential=` justement parce que, dans un conteneur LXC, l'hôte le
vide pour tous les services
(`/run/systemd/system/service.d/zzz-lxc-service.conf`). Le paquet signale
le conteneur à l'installation (« Installation on LXC detected ») sans rien
écrire.

La variante 3.2 n'est pas disponible : pour une clé chiffrée sur disque,
utiliser une VM avec TPM virtuel plutôt qu'un conteneur.

Un drop-in `lxc-key.conf` posé à la main avec une version précédente de
cette documentation n'est plus utile. Il fonctionne toujours, mais peut
être retiré :

```sh
rm /etc/systemd/system/speech-server.service.d/lxc-key.conf
systemctl daemon-reload
```

### 3.4 Changer de clé

1. Créer une seconde clé dans la console Mistral.
2. Remplacer le fichier (ou le rechiffrer), comme à la pose.
3. `systemctl restart speech-server` — un `reload` ne suffit pas : la clé
   n'est lue qu'au démarrage. Les flux en cours sont coupés quelques
   secondes.
4. Révoquer l'ancienne clé dans la console Mistral.

## 4. Configurer

### 4.1 Le fichier

La configuration est le fichier JSON `/etc/mediaserver/speech-config.json`.
Une clé absente prend sa valeur par défaut. Une clé **inconnue est une
erreur** : une faute de frappe empêche le démarrage au lieu de passer en
silence.

Le fichier livré :

```json
{
  "control": {
    "listen": "0.0.0.0",
    "port": 9200,
    "allow": ["127.0.0.1"]
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
    "workers": 10,
    "probeSecs": 30
  },
  "limits": {
    "maxSessions": { "asr": 20, "tts": 20, "comp": 20 },
    "idleSecs": 60,
    "maxSessionSecs": 3600
  },
  "voices": [
    { "alias": "fr-homme", "lang": "fr", "voiceId": "en_paul_neutral" }
  ],
  "tts":  { "model": "voxtral-mini-tts-2603", "phraseSilenceMs": 1500 },
  "asr":  { "model": "voxtral-mini-transcribe-realtime-2602", "vad": false, "silenceMs": 1500, "pauseCloseSecs": 30 },
  "comp": { "model": "mistral-small-latest" }
}
```

### 4.2 Référence des clés

**`control`** — le port de contrôle (HTTP, IPv4 seulement), où les serveurs
d'application demandent des jetons.

| Clé | Défaut | Valeurs | Rôle |
|---|---|---|---|
| `control.listen` | `0.0.0.0` | adresse IPv4 | adresse d'écoute. Une adresse IPv6 est refusée |
| `control.port` | `9200` | 1 à 65535 | port d'écoute ; doit différer de `streaming.port` |
| `control.allow` | — (**obligatoire**) | liste non vide d'adresses IPv4 ou de préfixes CIDR IPv4 (`10.0.0.0/8`) | qui peut joindre le port de contrôle, `/status/general` compris. Y mettre le serveur d'application et le mediaserver. Une liste vide ou une entrée IPv6 empêche le démarrage |

**`streaming`** — le port WebSocket, joint par les navigateurs et le
mediaserver.

| Clé | Défaut | Valeurs | Rôle |
|---|---|---|---|
| `streaming.listen` | `::` | adresse IPv4 ou IPv6 | adresse d'écoute ; `::` sert IPv4 et IPv6 |
| `streaming.port` | `9201` | 1 à 65535 | port d'écoute |
| `streaming.secure` | `false` | `true`/`false` | `true` : `wss://` (TLS). **Obligatoire en production** : une page `https://` ne peut ouvrir qu'un `wss://` |
| `streaming.cert` | `/etc/mediaserver/mcu.crt` | chemin | certificat TLS (PEM). Le défaut, auto-signé, ne sert qu'aux essais : les navigateurs le refusent |
| `streaming.key` | `/etc/mediaserver/mcu.key` | chemin | clé privée du certificat (PEM), lisible par le compte `speech` |

**`mistral`** — l'accès au fournisseur.

| Clé | Défaut | Valeurs | Rôle |
|---|---|---|---|
| `mistral.url` | `https://api.mistral.ai` | URL `https://` ou `http://` | base des appels ; à changer seulement pour passer par un relais |
| `mistral.probeSecs` | `30` | 0 à 3600 | période de la sonde de Mistral, en secondes : un `GET /v1/models` avec la clé. `0` coupe la sonde, et `backend.mistral.state` reste `unknown`. Voir [§9.2](#92-statusgeneral) |
| `mistral.workers` | `10` | 1 à 64 | nombre d'appels Mistral simultanés (ouvertures `asr`, phrases `tts`, demandes `comp`). Au-delà, les demandes attendent leur tour : augmenter si plus de `workers` flux `tts` parlent en même temps |

**`limits`** — les bornes, qui bornent aussi la dépense.

| Clé | Défaut | Valeurs | Rôle |
|---|---|---|---|
| `limits.maxSessions.asr` / `.tts` / `.comp` | `20` | 0 à 100000 | flux ouverts au plus, par fonction ; au-delà, l'ouverture reçoit 429. `0` désactive la fonction |
| `limits.idleSecs` | `60` | 1 à 86400 | un flux sans message du client pendant ce délai est fermé |
| `limits.maxSessionSecs` | `3600` | 1 à 86400 | durée maximale d'un flux |

**`voices`** — les voix de synthèse proposées aux clients `tts`. Liste
d'objets :

| Champ | Valeurs | Rôle |
|---|---|---|
| `alias` | `[A-Za-z0-9._-]`, unique | le nom que le client demande (`voice=`) |
| `lang` | code de langue court (`fr`, `en`) | la langue de la voix ; la première voix d'une langue est celle par défaut |
| `voiceId` | identifiant Mistral | la voix chez Mistral. N'est jamais demandé par un client |

Sans aucune voix, les flux `tts` reçoivent 503. L'exemple livré utilise
`en_paul_neutral`, une voix anglaise de Mistral qui prononce le français, en
attendant la voix française d'IVèS.

**`tts`, `asr`, `comp`** — réglages par fonction.

| Clé | Défaut | Valeurs | Rôle |
|---|---|---|---|
| `tts.model` | `voxtral-mini-tts-2603` | modèle Mistral | modèle de synthèse |
| `tts.phraseSilenceMs` | `1500` | 0 à 60000 | silence de frappe après lequel le texte accumulé part en synthèse |
| `asr.model` | `voxtral-mini-transcribe-realtime-2602` | modèle Mistral | modèle de reconnaissance temps réel |
| `asr.pauseCloseSecs` | `30` | 1 à 55 | durée de pause après laquelle la session Mistral est fermée, puis rouverte à la reprise |
| `asr.vad`, `asr.silenceMs` | `false`, `1500` | — | réservés : acceptés, sans effet dans cette version |
| `comp.model` | `mistral-small-latest` | modèle Mistral | modèle de correction |

Un flux garde le modèle en vigueur à son ouverture.

### 4.3 Vérifier la configuration

```sh
/opt/ives/bin/speech-server --config /etc/mediaserver/speech-config.json \
    --api-key-file /etc/mediaserver/speech-api.key --check
```

La commande lit le fichier et la clé, dit ce qui ne va pas, et sort sans
ouvrir de port. Elle lit la clé : la lancer en root.

### 4.4 Ligne de commande

| Option | Défaut | Rôle |
|---|---|---|
| `--config <chemin>` | `/etc/mediaserver/speech-config.json` | le fichier de configuration |
| `--api-key-file <chemin>` | `$CREDENTIALS_DIRECTORY/mistral-api-key`, sinon `/run/speech-server/api.key` | la clé Mistral |
| `--check` | — | valide la configuration et la clé, puis sort |

## 5. Démarrer, recharger, arrêter

```sh
systemctl enable --now speech-server     # démarrer, et à chaque démarrage de la machine
systemctl reload speech-server           # vérifier puis relire la configuration (SIGHUP)
systemctl restart speech-server          # redémarrer (nouvelle clé, ports, TLS)
systemctl stop speech-server             # arrêter
```

**Au démarrage**, le service refuse de démarrer, avec la cause dans le
journal, si : le fichier est absent ou n'est pas du JSON valide ; une clé
est inconnue ou hors bornes ; `control.allow` est vide ou contient une
entrée IPv6 ; `control.listen` est une adresse IPv6 ; les deux ports sont
égaux ; la clé Mistral est absente ou lisible par d'autres que son
propriétaire. Il sort alors avec le code 78, et systemd **ne le relance
pas** (`RestartPreventExitStatus=78`) : `systemctl status` affiche
`status=78`, et il faut corriger puis `systemctl start`.

**Au rechargement**, s'appliquent sans couper les flux : `control.allow`,
`limits`, `voices`, `tts`, `asr`, `comp`. Les flux déjà ouverts gardent
leurs réglages d'ouverture. Demandent un redémarrage : `control.listen`,
`control.port`, `streaming.*`, `mistral.*` — le journal signale un tel
changement en attente. Un fichier invalide au rechargement est **ignoré en
entier** et la configuration en cours reste : `systemctl reload` vérifie
le fichier avant d'envoyer le signal (`--check`), **échoue** et affiche la
cause.

**À l'arrêt** (`SIGTERM`), les flux sont fermés avec le code 1001, puis les
sessions Mistral, en moins de 15 s. systemd redémarre le service s'il
s'arrête sur une autre erreur (port déjà pris, plantage).

## 6. Réseau et pare-feu

| Flux | Port | Protocole | Qui |
|---|---|---|---|
| entrant | 9200 (`control.port`) | HTTP, IPv4 | les adresses de `control.allow` seulement |
| entrant | 9201 (`streaming.port`) | WebSocket (`ws`/`wss`), IPv4 et IPv6 | les navigateurs et le mediaserver |
| sortant | 443 | HTTPS et WSS | vers `api.mistral.ai` |

Le port de contrôle est en HTTP clair : il ne doit être joint que par des
serveurs, sur un réseau d'administration. S'il doit traverser un réseau non
sûr, placer un relais TLS (nginx, HAProxy) devant lui ; dans ce cas,
`control.allow` doit contenir l'adresse du relais.

Les deux ports sont au-dessus de 1024 : le service n'a pas besoin de root.

### Relier le mediaserver

Le mediaserver est un client comme les autres : il demande un jeton sur le
port de contrôle, puis ouvre ses flux sur le port de streaming. Trois
options de sa ligne de commande (`/etc/sysconfig/mediaserver`) :

```sh
--speech-url http://speech.example.net:9200 \
--speech-streaming-url wss://speech.example.net:9201 \
--speech-account clinique-nord
```

- son adresse doit figurer dans `control.allow` ;
- `--speech-streaming-url` en `wss://` vérifie le certificat contre le
  magasin du système, plus `--websocket-client-ca` si l'autorité est interne ;
- `--speech-account` est facultatif : il ventile les compteurs `usage` par
  compte.

Le bloc `speech` de son `/status/general` dit si la parole est disponible
(`docs/reference/status-http.md` §3.9). Le mediaserver interroge le
`/status/general` de speech-server toutes les 5 s : son adresse doit donc
figurer dans `control.allow`.

## 7. API de contrôle (HTTP)

Toutes les routes sont sur le port de contrôle. Une adresse hors de
`control.allow` reçoit **403** sur toutes les routes, sans que le corps soit
lu.

### 7.1 `POST /control/token` — obtenir un jeton

Un jeton autorise **une** ouverture de flux, pour **une** fonction. Le
serveur de l'application demande un jeton par flux, puis le transmet au
navigateur (ou l'utilise lui-même). Il n'existe aucune ouverture de flux
sans jeton, même depuis une adresse autorisée.

Requête :

```sh
curl -s -X POST http://speech.example.net:9200/control/token \
     -H 'Content-Type: application/json' \
     -d '{"function": "tts", "ttl": 300, "account": "clinique-nord"}'
```

| Champ | Obligatoire | Règle |
|---|---|---|
| `function` | oui | `asr`, `tts` ou `comp` |
| `ttl` | non | durée de validité en secondes, 1 à 3600 ; défaut 3600 |
| `account` | non | étiquette libre, 1 à 64 caractères `[A-Za-z0-9._-]` (un client, un service). Elle ventile la consommation dans `/status/general` et le journal. Absente : `"-"` |

Réponse **201** :

```json
{
  "token": "…43 caractères base64url…",
  "function": "tts",
  "account": "clinique-nord",
  "expiresAt": "2026-10-14T15:04:05Z"
}
```

| Code | Cause |
|---|---|
| 201 | jeton créé |
| 400 | corps ou champ invalide (`ttl` hors bornes, `account` mal formé, fonction inconnue) |
| 403 | adresse non autorisée |
| 415 | corps qui n'est pas du JSON |

Le jeton est consommé à l'ouverture du flux : une seconde ouverture avec le
même jeton reçoit 403. Les jetons vivent en mémoire : un redémarrage du
service les invalide tous.

### 7.2 `GET /control/voices` — lister les voix

```sh
curl -s http://speech.example.net:9200/control/voices
```

```json
{ "voices": [ { "alias": "fr-homme", "lang": "fr", "voiceId": "en_paul_neutral" } ] }
```

C'est la liste à proposer aux utilisateurs : l'application la demande au
serveur plutôt que d'en tenir une copie.

### 7.3 `GET /status/general` — l'état du serveur

Voir [§9](#9-surveiller).

## 8. API de streaming (WebSocket)

### 8.1 Ouvrir un flux

```
wss://speech.example.net:9201/streaming/<fonction>/<jeton>?<paramètres>
```

| Paramètre | Fonction | Défaut | Rôle |
|---|---|---|---|
| `lang` | toutes | `fr` | langue (code court). Pour `tts`, choisit la voix par défaut ; pour `comp`, la langue de correction ; pour `asr`, ignoré (la langue est détectée seule) |
| `voice` | `tts` | première voix de `lang` | alias de voix (voir `GET /control/voices`) |
| `delay` | `asr` | `1000` | délai de transcription en ms, 200 à 2400 : plus court, le texte arrive plus vite ; plus long, il est plus juste |

Un paramètre d'une autre fonction, inconnu ou répété, est refusé (400).

Réponse HTTP avant l'ouverture :

| Code | Cause |
|---|---|
| 101 | flux ouvert |
| 400 | paramètre inconnu ou répété, langue ou voix non servie, `delay` hors bornes |
| 403 | jeton inconnu, expiré, déjà consommé, ou d'une autre fonction |
| 404 | chemin mal formé, ou fonction inconnue |
| 429 | `limits.maxSessions` atteint pour cette fonction |
| 503 | fonction non servie, ou `tts` sans voix configurée |
| 500 | erreur interne |

Le jeton n'est consommé que si tout le reste est accepté : un client
refusé en 400, 429 ou 503 garde son jeton et peut réessayer.

Un Mistral injoignable ne donne pas d'erreur HTTP : le flux s'ouvre, puis
reçoit un message `error` fatal suivi de la fermeture 1013.

### 8.2 Règles communes

- **Trames texte** : un objet JSON par trame, avec un champ `type`.
- **Trames binaires** : audio PCM signé 16 bits petit-boutiste (S16LE),
  mono, 16 kHz, dans les deux sens.
- **Premier message** du serveur : `ready`. Le client attend `ready` avant
  d'envoyer de l'audio (`asr`).
- **Erreurs** : `{"type":"error","reason":"…","fatal":true|false}`.
  `fatal: true` : le serveur ferme juste après. `fatal: false` : le flux
  continue. Un message mal formé (« invalid JSON », « missing type »,
  « unexpected message », « unexpected binary frame ») donne une erreur non
  fatale.
- **Durées** : un flux sans message du client pendant `limits.idleSecs` est
  fermé ; un flux dure au plus `limits.maxSessionSecs`.
- **Codes de fermeture** :

| Code | Sens | Que faire |
|---|---|---|
| 1000 | fin normale (à l'initiative du client, ou après `done` en `asr`) | — |
| 1001 | arrêt du serveur | rouvrir plus tard, avec un nouveau jeton |
| 1011 | erreur interne du serveur | consulter le journal |
| 1013 | Mistral indisponible ou fermé en cours de flux | rouvrir avec un **nouveau** jeton ; en `asr`, le texte de l'intervalle est perdu |

Le serveur ne reprend jamais seul une session Mistral tombée : il ferme,
et le client sait qu'il manque un morceau.

Le jeton passant dans l'URL, le journal n'écrit jamais le chemin en
entier : seulement la fonction et les six premiers caractères du jeton.

### 8.3 `tts` — du texte vers la voix

Message `ready` :

```json
{ "type": "ready", "function": "tts", "rate": 16000, "voice": "fr-homme", "lang": "fr" }
```

**Le client envoie** son texte au fil de la frappe :

| Message | Effet |
|---|---|
| `{"type":"text","text":"…"}` | ajoute du texte. `U+0008` efface le caractère précédent pas encore parti en synthèse ; `\n` ou `U+2028` termine la phrase |
| `{"type":"flush"}` | envoie tout de suite en synthèse le texte accumulé |
| `{"type":"stop"}` | vide la file et abandonne la phrase en cours |

Une phrase part en synthèse sur une ponctuation finale (`.`, `?`, `!`, `…`)
**suivie d'un blanc**, une fin de ligne, `flush`, ou après
`tts.phraseSilenceMs` sans frappe. « 3.5 » ou « ives.fr » ne coupent pas.
Une phrase sans fin de plus de 1 500 octets est coupée au dernier blanc.

Un effacement qui remonte dans du texte déjà parti en synthèse est ignoré :
le serveur ne revient jamais sur une phrase envoyée. Corriger du texte déjà
dit est l'affaire du client (par exemple : `stop`, puis « Correction : … »).

**Le serveur répond**, phrase par phrase, dans l'ordre :

```
{"type":"say","id":7,"text":"Bonjour, je suis en retard."}
… trames binaires : l'audio de la phrase 7 …
{"type":"done","id":7}
```

Les trames binaires entre `say` et `done` appartiennent à cette seule
phrase. L'audio arrive plus vite que le temps réel : le client le tamponne
et le joue à 16 kHz.

Après `stop`, le serveur répond `{"type":"stopped","ids":[…]}` (les
phrases abandonnées), et plus aucune trame audio n'arrive pour elles.

Erreurs propres :

| Message | Sens |
|---|---|
| `{"type":"error","id":7,"reason":"rejected","fatal":false}` | Mistral a refusé la phrase 7 (modération) ; la suivante part |
| `{"type":"error","reason":"backend unavailable","fatal":true}` + 1013 | Mistral ne répond pas |
| `{"type":"error","reason":"text must be a string","fatal":false}` | message `text` mal formé |

### 8.4 `asr` — de la voix vers le texte

Message `ready`, envoyé quand Mistral a accepté la session :

```json
{ "type": "ready", "function": "asr", "rate": 16000, "langIgnored": true }
```

**Le client envoie** des trames binaires de 20 à 500 ms d'audio, au rythme
réel, et ces messages :

| Message | Effet |
|---|---|
| `{"type":"pause"}` | le serveur cesse de transcrire et jette l'audio reçu ; le texte en attente arrive tout de suite. Au-delà de `asr.pauseCloseSecs`, la session Mistral est fermée, et rouverte à la reprise |
| `{"type":"resume"}` | reprend la transcription |
| `{"type":"done"}` | fin de parole : le serveur rend le texte restant, puis ferme avec 1000 (au plus tard 5 s après) |

L'audio envoyé avant `ready`, ou à la reprise après une pause longue, est
tamponné jusqu'à 5 s ; au-delà, il est jeté et le client reçoit
`{"type":"error","reason":"audio dropped while the backend opens","fatal":false}`.

**Le serveur rend** le texte par morceaux, ajoutés à la fin :

```json
{ "type": "text", "text": " en retard" }
```

Le texte n'est jamais corrigé après coup (pas de `U+0008`) : le paramètre
`delay` règle le compromis entre vitesse et justesse.

Erreurs fatales propres, suivies de 1013 :

| `reason` | Sens |
|---|---|
| `backend unavailable` | Mistral injoignable à l'ouverture |
| `backend timeout` | Mistral n'a pas confirmé la session en 10 s |
| `backend closed` | Mistral a fermé ou est tombé en cours de flux |

### 8.5 `comp` — corriger le texte avant qu'il soit dit

`comp` est sans état : chaque demande est indépendante.

**Le client envoie** un morceau de texte (en général les derniers mots
terminés) et, en lecture seule, la fin du texte qui précède :

```json
{ "type": "check", "id": 7, "context": "Bonjour je voudrais", "text": " une piza svp" }
```

| Champ | Règle |
|---|---|
| `id` | obligatoire, nombre ou chaîne choisi par le client ; repris dans la réponse |
| `text` | obligatoire, au plus 500 caractères ; seul ce texte est corrigé |
| `context` | facultatif, au plus 500 caractères ; sert à la grammaire, jamais modifié |

**Le serveur répond** :

```json
{
  "type": "checked", "id": 7, "text": " une pizza svp",
  "changes": [ { "start": 5, "end": 9, "from": "piza", "to": "pizza" } ]
}
```

- `text` : le morceau corrigé ;
- `changes` : chaque correction, avec sa position `[start, end)` dans le
  morceau **envoyé**, en unités UTF-16 (celles des chaînes JavaScript).
  Sans correction, `changes` est vide et `text` identique à la demande.

Le serveur ne garde que des corrections de mots (orthographe, accord,
apostrophe) ; toute reformulation proposée par le modèle est écartée.

Une demande est traitée à la fois ; une seconde attend ; une troisième
remplace celle qui attend, qui reçoit
`{"type":"error","id":…,"reason":"superseded","fatal":false}`.

Erreurs non fatales, avec l'`id` de la demande : `backend unavailable`
(Mistral en échec ou réponse inexploitable), `superseded`, `text and
context must be strings`, texte trop long. Sur une erreur, le client peut
utiliser son texte tel quel.

Usage recommandé côté client : envoyer `check` après 3 mots terminés, une
ponctuation ou 0,8 s sans frappe (jamais le mot en cours) ; sans
changement, envoyer le texte tout de suite ; avec des changements, les
proposer à l'utilisateur et n'envoyer qu'après sa validation.

### 8.6 Exemple de bout en bout

```sh
# 1. Le serveur d'application obtient un jeton
TOKEN=$(curl -s -X POST http://speech.example.net:9200/control/token \
        -H 'Content-Type: application/json' \
        -d '{"function":"comp","ttl":60}' | jq -r .token)

# 2. Le client ouvre le flux et envoie une demande (websocat)
echo '{"type":"check","id":1,"context":"","text":"je voudrait une piza"}' |
    websocat "wss://speech.example.net:9201/streaming/comp/$TOKEN?lang=fr"
```

## 9. Surveiller

### 9.1 Le journal

`/var/log/speech-server.log` (aussi `journalctl -u speech-server` pour les
démarrages et arrêts). Une ligne à l'ouverture et à la fermeture de chaque
flux (fonction, `account`, début du jeton, adresse du client, durée, unités
consommées) et une ligne par échec Mistral (code HTTP ou code de
fermeture). Le journal ne contient **jamais** le texte ni la voix des
utilisateurs, ni un jeton entier.

Le paquet livre sa rotation, `/etc/logrotate.d/speech-server` : chaque
semaine, huit semaines gardées, compressées. `copytruncate` parce que
systemd garde le fichier ouvert. Le fichier est conservé lors des mises à
jour : il peut être adapté.

### 9.2 `/status/general`

Sur le port de contrôle, depuis une adresse de `control.allow` :

```sh
curl -s http://127.0.0.1:9200/status/general              # JSON
curl -s 'http://127.0.0.1:9200/status/general?format=text'  # lecture humaine
```

| Bloc | Contenu |
|---|---|
| `server` | `product`, `version`, `pid`, `startedAt`, `uptimeSecs` |
| `backend` | `apiKey` (`present`/`absent`, jamais la clé) ; `mistral` : la dernière réponse de la sonde (`state` `up`, `down` ou `unknown`, `reason`, `checkedAt`) ; puis par service Mistral (`asr`, `tts`, `comp`) : `lastSuccess`, `lastFailure`, `lastError`, `discarded` (réponses de correction écartées car inexploitables) |
| `sessions` | par fonction : `open` (ouverts), `max` (borne configurée), `refused` (refus 429 cumulés) |
| `tokens` | `live` (valides, pas encore utilisés), `issued`, `consumed`, `expiredUnused` |
| `usage` | depuis le démarrage : `asrAudioSecs` (secondes d'audio transcrites), `ttsChars` (caractères synthétisés), `compTokens` (tokens de correction), puis le même détail par compte dans `usage.byAccount` |

Les trois unités de `usage` sont celles de la facturation Mistral : le coût
s'en déduit directement. Les compteurs repartent de zéro à chaque
redémarrage ; pour un suivi dans la durée, relever `/status/general`
périodiquement.

À surveiller en priorité :

- `backend.mistral.state` à `down` : Mistral ne répond pas, ou refuse la
  clé. `reason` dit lequel, par exemple `Mistral refuses the API key (HTTP 401)`
  ou `Mistral unreachable: Couldn't connect to server`. La sonde tourne même
  sans aucun appel : elle voit la panne commencer et finir. Le journal écrit
  une ligne `-BackendProbe:` à chaque changement d'état ;
- `backend.<service>.lastFailure` plus récent que `lastSuccess` : Mistral
  est en échec pour ce service ;
- `sessions.<fonction>.refused` qui augmente : `limits.maxSessions` est
  trop bas ;
- `tokens.expiredUnused` qui augmente vite : l'application demande des
  jetons qu'elle n'utilise pas, ou avec un `ttl` trop court.

## 10. Dépannage

| Symptôme | Cause probable | Action |
|---|---|---|
| Le service ne démarre pas, « no Mistral key » | `/etc/mediaserver/speech-api.key` absente | §3.1, puis `systemctl start` |
| Le service ne démarre pas, `status=78`, pas de relance | erreur de configuration ou de clé : systemd ne relance pas | lire le journal, corriger, `systemctl start` |
| `systemctl reload` échoue | fichier invalide : rien n'a été rechargé | lire la cause affichée, corriger, recommencer |
| Le service ne démarre pas, « unknown key '…' » | faute de frappe dans le fichier | corriger ; `--check` (§4.3) |
| Le service ne démarre pas, « 'control.allow' must be a non-empty list » | liste vide ou absente | y mettre au moins une adresse |
| Le service ne démarre pas, « holds an IPv6 entry » | entrée IPv6 dans `control.allow` | le port de contrôle est IPv4 seulement : retirer l'entrée |
| `reload` sans effet | clé à redémarrage (ports, TLS, `mistral.*`) | lire le journal ; `restart` |
| 403 sur le port de contrôle | adresse hors `control.allow` (derrière un relais : l'adresse du relais) | ajouter l'adresse, puis `reload` |
| 403 à l'ouverture d'un flux | jeton expiré, déjà utilisé, ou d'une autre fonction | un jeton par flux, de la bonne fonction |
| 429 à l'ouverture d'un flux | `limits.maxSessions` atteint | augmenter la borne, puis `reload` |
| 503 sur `tts` | aucune voix dans `voices` | déclarer une voix, puis `reload` |
| Le navigateur n'ouvre pas le WebSocket | page `https://` et `streaming.secure` à `false`, ou certificat non reconnu | `wss://` avec un certificat d'une autorité connue (§4.2) |
| Flux fermés en 1013 | Mistral indisponible | `backend` dans `/status/general` ; état du service sur le site de Mistral |
| La synthèse traîne quand beaucoup parlent | `mistral.workers` saturé | augmenter `mistral.workers`, puis `restart` |
| Fermeture au bout d'une minute sans parler | `limits.idleSecs` | le client envoie de l'audio (même du silence) ou un message ; ou augmenter la borne |

## 11. Désinstaller

```sh
systemctl disable --now speech-server
dnf remove mediaserver-speech        # ou : apt remove mediaserver-speech
```

La clé (`/etc/mediaserver/speech-api.key` ou `.cred`), les drop-ins de
`/etc/systemd/system/speech-server.service.d/` et le journal restent :
les supprimer à la main, et révoquer la clé dans la console Mistral si le
serveur n'est pas réinstallé.
