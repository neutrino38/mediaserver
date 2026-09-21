# Recette : la jambe texte WebSocket sortante

## À quoi sert cette fiche

Le chantier WS-CLIENT donne au mediaserver une jambe texte **cliente** : au lieu
d'attendre qu'un navigateur se connecte à son serveur WebSocket, il se connecte
lui-même à une URL `ws://` ou `wss://`. Conception :
`docs/conception/WS-CLIENT/SPEC.md`.

Les tests automatiques prouvent que le code fait ce qu'il dit, contre un serveur
d'écho en processus. Ils ne prouvent pas qu'un appel réel marche.

Cette fiche donne la recette en appel réel : un fichier MP4 à trois médias est
joué vers un écho Asterisk, à travers une passerelle WebRTC. Le mediaserver y
tient le rôle du navigateur.

Jouez-la après tout changement dans `WSEndpoint`, `WebSocketConnection`,
`WebSocketServer` ou le transport WebSocket.

## Ce qui est déjà couvert par les tests

Ne rejouez pas ces points à la main :

```sh
cd mcu && make check
```

Les suites `WsClientSeams`, `WsClientHandshake`, `WsClientConnect`,
`WsClientTls`, `WsClientEndpoint` et `WsClientXmlRpc` couvrent le masquage des
trames, la poignée de main, le 101, la résolution de nom, l'IPv4 et l'IPv6, le
port fermé, l'URL inutilisable, le TLS vérifié et refusé, le pontage dans les
deux sens, la coupure annoncée, la reprise, la cible publiée au lieu de
l'écoute, et l'API de contrôle elle-même — méthode recensée, ordre des
paramètres, fautes.

La recette couvre ce qu'un test en processus ne voit pas : un vrai pair, un vrai
réseau, un vrai codec, et la durée.

## Avant de commencer

### 1. Ce que le serveur expose

La commande XML-RPC est `ConnectMediaConnection(sessionId, endpointId, media,
role, url)` sur `/jsr309` (`docs/JSR-309-API.md` §6.12). Le client Java
`XmlRPCJSR309Client` la porte sous le même nom.

Côté elixip, le verbe MOTELI v2 est `connect_media_connection`
(`apps/elixip2/priv/proto/moteli_jsr309.proto`).

### 2. Le binaire est déployé

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release
ln -s /home/<user>/mediaserver/bin/debug/mcu mediaserver
systemctl restart mediaserver
tail -f /var/log/mcu.log
```

### 3. Le serveur WebSocket doit tourner

Lancez le mediaserver avec `--websocket-port`, **même si aucune jambe entrante
n'est attendue**. Le réacteur qui pilote les connexions sortantes appartient au
serveur WebSocket. Sans lui, `Connect` échoue tout de suite :

```
-WebSocketServer::Connect: server is not running [url:…]
```

### 4. Trois faits à vérifier sur la passerelle, avant de l'accuser

Ces trois points ne se devinent pas, et chacun peut faire échouer la recette
sans que le mediaserver soit en cause.

- **Le protocole de la passerelle.** La jambe envoie et reçoit du **texte UTF-8
  brut** dans des trames WebSocket texte. Elle ne négocie **aucun
  sous-protocole**, n'envoie **ni cookie ni en-tête d'authentification**, et ne
  gère ni proxy HTTP ni compression (SPEC §8). Un jeton dans le chemin ou dans
  la requête de l'URL, lui, passe. Si la passerelle attend autre chose — du JSON
  de signalisation, un `Sec-WebSocket-Protocol` —, c'est un lot à part, pas un
  défaut de recette.
- **L'écho boucle-t-il le texte ?** Prouvez-le une fois avec un vrai navigateur
  ou un terminal SIP T.140 avant de jouer la recette. Un écho Asterisk qui ne
  renvoie que l'audio et la vidéo rendrait la jambe texte muette sans qu'elle
  ait tort.
- **L'URL exacte à passer.** C'est celle qu'un navigateur utiliserait pour la
  même conversation, jeton compris.

### 5. Le fichier source

Il faut un `.mp4` portant **trois pistes** : audio, vidéo, texte.

```sh
ffprobe -v error -show_entries stream=index,codec_type,codec_name \
        -of csv=p=0 source.mp4
```

La sortie doit montrer une piste audio, une piste vidéo et une piste
`mov_text` (le texte temporisé 3GPP, seul format de sous-titre que le lecteur
sait ouvrir).

Deux contraintes, qui viennent du lecteur (`Player::NegotiateCodecs`) :

- **La vidéo n'est jamais transcodée.** Le fichier doit porter un codec que la
  passerelle accepte : H.264 en pratique, VP8 si elle le négocie. Un autre codec
  donne un appel sans vidéo, sans erreur bruyante.
- **L'audio l'est au besoin.** PCMU, PCMA, G.722, AMR, Opus et GSM partent tels
  quels s'ils sont négociés ; sinon le serveur décode et ré-encode vers Opus,
  PCMU ou PCMA.

Pour fabriquer le fichier, enregistrez un appel réel à trois médias avec le
Recorder JSR-309 (`docs/JSR-309-API.md` §6.4). Rappel de son contrat : **tous
les `RecorderAttachTo…` précèdent `RecorderRecord`**.

Le texte doit être **reconnaissable** : des phrases numérotées et horodatées.
C'est ce qui rend l'aller-retour jugeable ligne par ligne.

## Le montage

```
source.mp4 ──> Player ──┐
                        ├─ audio + vidéo  ──RTP/SRTP──> passerelle ──SIP──> Asterisk
                        │                                   │                 (écho)
             Endpoint ──┤                                   │                   │
                        └─ texte T.140 ──WebSocket sortant──┘                   │
                        ▲                                                       │
                        └──────── retour des trois médias <──────────────────────
                        │
                   Recorder ──> retour.mkv
```

Un seul `Endpoint` porte les trois médias. Sa jambe audio et vidéo est une jambe
WebRTC ordinaire — ICE et DTLS-SRTP, montée par elixip comme pour un navigateur
(`docs/JSR-309-API.md` §6.6). Sa jambe texte est la nouveauté du chantier.

Ordre des appels, côté elixip :

1. `EventQueueCreate`, puis `MediaSessionCreate`.
2. `EndpointCreate` avec les trois médias.
3. Sécurité et transport de l'audio et de la vidéo : `EndpointSetRemoteCryptoDTLS`,
   `EndpointStartReceiving`, `EndpointStartSending`.
4. **`ConnectMediaConnection(sessionId, endpointId, Text, VIDEO_MAIN, url)`** —
   l'URL de la passerelle. C'est le point de la recette.
5. `PlayerCreate`, `PlayerOpen(source.mp4)`.
6. `EndpointAttachToPlayer` pour les trois médias.
7. `RecorderCreate`, les trois `RecorderAttachToEndpoint`, puis
   `RecorderRecord(retour.mkv)`.
8. `PlayerPlay`.

**Enregistrez en `.mkv`.** Ce conteneur prend PCMU, Opus, H.264, VP8 et le texte
tels quels. En `.mp4`, un retour PCMU serait transcodé en AAC et un retour VP8
perdu : la comparaison avec la source ne dirait plus rien du réseau.

## Les scénarios

### 0. Passe locale, sans infrastructure

À jouer avant de mobiliser la plateforme. Un seul mediaserver tient les deux
bouts.

1. elixip monte deux jambes JSR-309. La jambe A garde son texte WebSocket en
   mode serveur : son URL vient de `ConfigureMediaConnection` et de
   `GetMediaCandidates`.
2. La jambe B reçoit `ConnectMediaConnection` avec l'URL de A.
3. Tapez du texte des deux côtés.

Attendu : le texte passe dans les deux sens, caractère par caractère, sans
doublon ni perte, et la jambe B reçoit `EndpointConnectedEvent` (type 7).

Cette passe isole le code du reste. Si elle échoue, inutile d'aller plus loin.

### 1. Appel complet en `ws://`

Jouez le montage ci-dessus jusqu'au bout du fichier.

Attendu, dans l'ordre :

- `WSEndpoint: outgoing text leg armed on ws://…` ;
- `-Outgoing connection [fd:…,to:…,path:…,plain]` ;
- `-Outgoing connection adopted [fd:…,id:…,host:…,path:…]` ;
- `EndpointConnectedEvent` (type 7) sur la file d'événements, pour le média
  texte ;
- `PlayerStartedEvent` puis, en fin de fichier, `PlayerEndOfFileEvent`.

Attendu dans `retour.mkv`, après `RecorderStop` :

```sh
ffprobe -v error -show_entries stream=index,codec_type,codec_name \
        -of csv=p=0 retour.mkv
```

- trois pistes, dont la piste texte `S_TEXT/UTF8` ;
- le texte enregistré reprend celui de la source, dans l'ordre, sans doublon ;
- l'audio et la vidéo reviennent (écoute et visionnage, pas de mesure fine ici).

Ce qui fait échouer : une piste texte absente, du texte tronqué, des lignes dans
le désordre, ou un `U+FFFD` alors qu'aucune coupure n'a eu lieu.

### 2. La coupure et la reprise

Pendant la lecture, coupez le serveur WebSocket de la passerelle. Laissez
30 secondes. Remettez-le.

Attendu :

- `WSEndpoint: connection associated with endpoint is closing.` ;
- `EndpointDisconnectedEvent` (type 6) ;
- un `U+FFFD` **dans la piste texte enregistrée** : c'est la perte annoncée au
  pair qui survit (T.140 §5.3) ;
- une nouvelle tentative **toutes les 5 secondes**, indéfiniment, chacune
  silencieuse côté événements ;
- au retour : `WSEndpoint: outgoing leg reconnected to … (N text frame(s) lost
  while down).`, puis `EndpointConnectedEvent` (type 7) ;
- le texte joué **pendant** la coupure ne réapparaît pas. Il est perdu, et c'est
  voulu.

### 3. La passerelle n'est pas encore là

Armez la jambe (`ConnectMediaConnection`) **avant** de démarrer le serveur
WebSocket de la passerelle. Lancez la lecture tout de suite.

Attendu : la jambe s'ouvre seule dès que la passerelle écoute, et la trace
`WSEndpoint: replayed N pending text frame(s) on connect (M dropped as stale).`
dit ce qui a été rejoué. La **première phrase** du fichier doit arriver — c'est
précisément ce que la file d'attente existe pour sauver.

Ce qui fait échouer : `N = 0` alors que du texte a été joué avant l'ouverture.

### 4. `wss://`

Trois cas, selon ce que présente la passerelle.

- **Certificat signé par une autorité connue du système.** Rien à régler : la
  vérification du pair est **active par défaut** et le magasin d'autorités est
  celui du système. Attendu : `-Outgoing connection [fd:…,tls]`, puis le
  scénario 1 à l'identique.
- **Certificat signé par une autorité de plateforme.** Lancer le serveur avec
  `--websocket-client-ca <fichier.pem>`. Trace attendue au démarrage :
  `-WebSocket client: extra CA ["…"]`.
- **Sans vérification**, pour isoler un problème de certificat :
  `--websocket-client-insecure`, qui trace
  `-WebSocket client: peer certificate verification DISABLED`. À ne pas laisser
  en exploitation.

Vérifiez aussi le refus attendu : pointez l'URL sur un hôte dont le certificat
ne correspond pas au nom. La jambe doit échouer et **retenter**, pas s'ouvrir.

### 5. Fin propre

Coupez la passerelle, puis, jambe coupée, appelez `MediaSessionDelete`.

Attendu : plus aucune tentative dans les traces après la destruction, et pas de
thread résiduel.

```sh
mcu/tests/tools/thread_census.sh
```

Le total doit revenir à ce qu'il était avant l'appel.

## Feuille de relevé

Notez, pour chaque séance :

- la version du binaire, la branche et le SHA du commit ;
- la passerelle utilisée et sa version ;
- l'URL passée à `ConnectMediaConnection`, jeton masqué ;
- le fichier source : durée, codecs des trois pistes ;
- les scénarios joués, et lesquels ont échoué ;
- les extraits de `/var/log/mcu.log` pour chaque échec ;
- `retour.mkv` et sa comparaison au fichier source.

## Ce que la recette ne prouve pas

- La tenue en charge : un seul appel ne dit rien de cent.
- Le participant de conférence (`ParticipantTextWS`) reste serveur seulement, et
  n'est pas couvert ici.
- L'audio et la vidéo restent en RTP. Ce chantier ne les touche pas : un défaut
  d'image ou de son observé ici appartient à une autre fiche.

## Si un scénario échoue

Cherchez d'abord de quel côté vient le refus.

| Trace | Cause | Où regarder |
|---|---|---|
| `server is not running` | pas de `--websocket-port` | ligne de commande |
| `cannot parse url` / `unsupported scheme` | URL fausse | ce que donne la passerelle |
| `cannot resolve "…"` | DNS, ou nom qui ne mène qu'à une adresse non routable | résolution sur la machine |
| `cannot connect to "…"` | la passerelle n'écoute pas, ou un filtre réseau | port, pare-feu |
| `no TLS client transport` | contexte TLS client inutilisable | autorité demandée, OpenSSL |
| poignée de main sans 101 | la passerelle refuse l'`Upgrade` | en-têtes attendus par elle |
| jambe ouverte, mais rien ne circule | protocole applicatif différent | format des trames de la passerelle |
