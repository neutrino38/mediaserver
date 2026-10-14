# speech-server : installer et exploiter

`speech-server` rend trois services, calculés en ligne par Mistral AI :

| Fonction | Ce qu'elle fait |
|---|---|
| `asr` | transforme la voix en texte |
| `tts` | transforme le texte en voix |
| `comp` | corrige l'orthographe et la grammaire d'un texte avant qu'il soit dit |

Le serveur ne porte aucun modèle. Il tient les connexions, vérifie les
jetons, adapte les formats et compte ce qui est consommé. La conception
complète est dans [la SPEC](../conception/SPEECH-SERVER/SPEC.md).

## 1. Ce qu'il faut savoir avant d'installer

- **Les données quittent l'infrastructure.** La voix et le texte partent
  chez Mistral AI, sous-traitant au sens du RGPD. Trois faits ne sont pas
  encore établis : si Mistral entraîne ses modèles sur ces données (un
  réglage du compte à vérifier), combien de temps il les garde, et où il les
  héberge. Les vérifier avant toute mise en service, et informer les
  utilisateurs.
- **La voix de synthèse est fournie en ligne.** Les poids du modèle de
  synthèse de Mistral sont sous licence non commerciale : pas de synthèse
  locale possible sans licence négociée.
- **Le service Realtime de Mistral peut être instable.** Le 2026-10-14, 6
  sessions `asr` sur 8 ont été fermées par Mistral. Le client reçoit alors
  le code 1013 (« réessayer plus tard »). `/status/general` le montre.

## 2. Installer

AlmaLinux 9 :

```sh
dnf install ./mediaserver-speech-1.18.0-1.ives.el9.x86_64.rpm
```

Debian ou Ubuntu :

```sh
apt install ./mediaserver-speech_1.18.0_amd64.deb
```

Le paquet installe :

| Fichier | Rôle |
|---|---|
| `/opt/ives/bin/speech-server` | le binaire |
| `/etc/mediaserver/speech-config.json` | la configuration, gardée à la mise à jour |
| unité `speech-server.service` | le service systemd |
| compte `speech` | le compte système sous lequel le service tourne |

Le paquet ne dépend pas du mediaserver : il s'installe seul sur une machine
dédiée. Le service **n'est pas activé** : sans clé Mistral, il ne démarre
pas.

## 3. Poser la clé Mistral

La clé n'est jamais dans la configuration, ni sur la ligne de commande, ni
dans l'environnement. systemd la donne au service au démarrage.

**Variante simple**, la clé en clair, lisible par root seul :

```sh
install -m 0600 -o root -g root /dev/stdin /etc/mediaserver/speech-api.key <<< "$KEY"
```

**Variante chiffrée**, liée à la machine (TPM s'il existe, sinon la clé
d'hôte de systemd). Une copie du fichier hors de la machine est illisible :

```sh
systemd-creds encrypt --name=mistral-api-key - /etc/mediaserver/speech-api.cred <<< "$KEY"
mkdir -p /etc/systemd/system/speech-server.service.d
cat > /etc/systemd/system/speech-server.service.d/key.conf <<'EOF'
[Service]
LoadCredential=
LoadCredentialEncrypted=mistral-api-key:/etc/mediaserver/speech-api.cred
EOF
systemctl daemon-reload
```

Avec la variante chiffrée, ne jamais sauvegarder la clé d'hôte
(`/var/lib/systemd/credential.secret`) avec le fichier chiffré. Après une
réinstallation de la machine, rechiffrer la clé.

**Dans un conteneur LXC** (Proxmox), systemd ne transmet aucune clé :
l'hôte ajoute à tous les services un réglage `LoadCredential=` vide. Le
service refuse alors de démarrer, avec « no Mistral key » dans son journal.
Dans ce cas, la clé est lue directement dans un fichier qui appartient au
compte `speech` :

```sh
install -m 0600 -o speech -g speech /dev/stdin /etc/mediaserver/speech-api.key <<< "$KEY"
mkdir -p /etc/systemd/system/speech-server.service.d
cat > /etc/systemd/system/speech-server.service.d/lxc-key.conf <<'EOF'
[Service]
ExecStart=
ExecStart=/opt/ives/bin/speech-server --config /etc/mediaserver/speech-config.json --api-key-file /etc/mediaserver/speech-api.key
EOF
systemctl daemon-reload
```

Cette variante est moins protégée : le compte du service peut relire la clé
sur le disque. Ne l'utiliser que là où la variante simple ne marche pas.

**Changer de clé sans coupure longue** : créer une seconde clé dans la
console Mistral, remplacer le fichier, redémarrer le service, puis révoquer
l'ancienne clé. Un rechargement ne suffit pas : systemd ne lit la clé qu'au
démarrage.

## 4. Configurer

Le fichier `/etc/mediaserver/speech-config.json` est en JSON. Une clé
inconnue est une erreur : une faute de frappe empêche le démarrage, au lieu
de passer en silence.

À régler avant le premier démarrage :

| Clé | Quoi mettre |
|---|---|
| `control.allow` | les adresses IPv4, ou les préfixes (`10.0.0.0/8`), qui ont le droit de demander des jetons : le serveur de l'application, le mediaserver. Liste vide : le serveur refuse de démarrer |
| `control.listen` | l'adresse IPv4 du port de contrôle. Ce port n'écoute qu'en IPv4 |
| `streaming.secure`, `cert`, `key` | `true` en production, avec un certificat d'une autorité que les navigateurs reconnaissent. Un certificat auto-signé est refusé par les navigateurs, sans recours |
| `voices` | les voix de synthèse : un alias, une langue, l'identifiant Mistral de la voix. L'exemple livré utilise `en_paul_neutral`, une voix anglaise qui prononce le français, en attendant la voix française d'IVèS |

Les autres clés ont des valeurs par défaut raisonnables. Toutes sont
décrites dans la SPEC, §8.2.

Vérifier le fichier sans démarrer le service :

```sh
/opt/ives/bin/speech-server --config /etc/mediaserver/speech-config.json \
    --api-key-file /etc/mediaserver/speech-api.key --check
```

Cette commande lit la clé : la lancer en root.

## 5. Démarrer, recharger, arrêter

```sh
systemctl enable --now speech-server     # démarrer, et au démarrage de la machine
systemctl reload speech-server           # relire la configuration
systemctl stop speech-server             # arrêter : les flux reçoivent le code 1001
```

Le rechargement applique sans couper les flux : `control.allow`, `limits`,
`voices`, et les blocs `tts`, `asr`, `comp`. Les ports, les adresses
d'écoute, TLS et `mistral.*` ne changent qu'au redémarrage ; le journal le
signale. Un fichier invalide au rechargement est ignoré en entier, et la
configuration en cours reste.

## 6. Surveiller

- Le journal : `/var/log/speech-server.log`. Il ne contient jamais le texte
  ni la voix des utilisateurs, ni un jeton entier.
- L'état : `curl http://127.0.0.1:9200/status/general`, ou
  `?format=text` pour une lecture humaine. On y lit la présence de la clé,
  le dernier succès et le dernier échec de chaque service Mistral, les flux
  ouverts, les jetons, et la consommation par compte : secondes d'audio,
  caractères dits, tokens. Ce sont les unités de facturation de Mistral.

## 7. Côté application : les jetons

Un client ne parle jamais au port de contrôle. C'est le serveur de
l'application qui demande un jeton, puis le donne au navigateur.

```sh
curl -s -X POST http://speech.example.net:9200/control/token \
     -H 'Content-Type: application/json' \
     -d '{"function": "tts", "ttl": 300, "account": "clinique-nord"}'
```

Réponse :

```json
{ "token": "…43 caractères…", "function": "tts", "account": "clinique-nord", "expiresAt": "2026-10-14T15:04:05Z" }
```

Le navigateur ouvre ensuite le flux :

```
wss://speech.example.net:9201/streaming/tts/<jeton>?lang=fr
```

Un jeton ne sert qu'une fois, et pour une seule fonction. `account` est une
étiquette libre qui ventile la consommation dans `/status/general`. Le
protocole de chaque fonction est décrit dans la SPEC, §7.

## 8. Ouvrir le pare-feu

| Port | Qui le joint |
|---|---|
| 9200 (contrôle, HTTP, IPv4) | les serveurs de `control.allow` seulement |
| 9201 (streaming, WebSocket) | les navigateurs et le mediaserver |
| sortant 443 vers `api.mistral.ai` | le serveur lui-même |
