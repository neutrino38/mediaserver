# BFCP dans le mediaserver

BFCP (Binary Floor Control Protocol) est le protocole de contrôle de parole.
Il désigne qui a le droit d'émettre sur un flux partagé — dans le mediaserver,
le partage de document ou d'écran.

Le mediaserver porte **une pile BFCP, la sienne**, dans `mcu/src/bfcp/` et
`mcu/include/bfcp/`. Aucun sous-module, aucune bibliothèque externe.

## Périmètre

- **MCU seule.** L'API XML-RPC `/jsr309` n'expose pas BFCP.
- **TCP et UDP.** Pas de TLS : une offre `TLS/BFCP` est refusée.
- **Un floor par conférence**, d'identifiant 1, celui du document partagé.

## Les trois couches

### Le modèle

Une classe par primitive (`BFCPMsg*`), une par attribut (`BFCPAttr*`). Chaque
en-tête porte le dessin du TLV ou la grammaire du message tirée de la RFC.

### Le codec

`BFCPMessage::Parse` et `Serialize` lisent et écrivent le format binaire :
en-tête commun de la RFC 8855, attributs en TLV de la RFC 4582.

Trois règles gouvernent l'écriture, et ne se devinent pas :

- **Le champ Length compte l'en-tête de l'attribut et exclut le bourrage** qui
  l'amène à la frontière de 4 octets. Le bourrage est sur le fil, jamais dans
  la longueur.
- **Length tient sur 8 bits.** Un attribut de plus de 255 octets est refusé,
  jamais tronqué : la longueur ne saurait pas le dire.
- **Le bit M appartient à la position, pas à l'attribut.** Le même attribut est
  obligatoire dans un message et facultatif dans un autre, donc c'est le parent
  qui l'écrit qui le décide : `Serialize(out, max, mandatory)`.

Et un piège de lecture : **SUPPORTED-PRIMITIVES porte des octets pleins,
SUPPORTED-ATTRIBUTES décale d'un bit**, parce qu'un type d'attribut tient sur
7 bits. Les deux listes se ressemblent et ne se codent pas pareil.

### Le contrôle de parole

`BFCPFloorControlServer` tient les utilisateurs, le floor et les demandes. Il
est appelé depuis deux fils : le réacteur BFCP pour ce qui vient du réseau, le
fil XML-RPC pour ce que décide le contrôleur. Un verrou unique les sérialise,
et il est **relâché avant chaque rappel du `Listener`** — un chair peut donc
rappeler le serveur depuis une notification sans s'interbloquer.

`SharedDocMixer` est ce chair : il reçoit `onFloorRequest`, `onFloorGranted`,
`onFloorReleased` et `onUserConnected`, et les traduit en événements pour le
contrôleur (`WAITING_ACCEPT`, `ACTIVE`, `NONE`).

## Les transports

Les deux sont des `PollHandler` battus par le réacteur partagé
(`RtpSessionSet`), sans thread propre. Les règles d'écriture d'un rappel sont
celles de `docs/reference/threads-rtp.md` : ne jamais bloquer, ne jamais se
détruire depuis le fil du réacteur.

### TCP

Une écoute par conférence, `BFCPTcpListener`, dual-stack : une socket
`AF_INET6` avec `IPV6_V6ONLY` désarmé, donc un client IPv4 arrive en
`::ffff:a.b.c.d`. Le port annoncé dans le SDP est celui que le noyau a lié.

`BFCPTcpConnection` découpe le flux sur la longueur de l'en-tête. Une lecture
rend **tous** les messages entiers qu'elle contient, et garde un message
partiel pour la suite.

Deux pièges, et ils sont structurels :

- **Un handler qui se termine doit réveiller le réacteur.** La récolte des
  connexions mortes vit dans le travail périodique de l'écoute, donc il lui
  faut un tour ; or les handlers BFCP demandent une échéance infinie.
- **Détacher le transport du serveur avant de le détruire.** L'écoute possède
  la connexion, le serveur en garde un pointeur sur l'utilisateur.
  `BFCPFloorControlServer::TransportClosed` est ce détachement.

### UDP

Une socket par participant, `BFCPUdpEndpoint`, dual-stack elle aussi. Le
premier datagramme reçu accroche l'adresse du pair et **remplace celle du
SDP** : c'est ce qui le rend joignable derrière un NAT. Un datagramme d'une
autre source est jeté.

UDP ne donne aucune fiabilité, donc le point d'accès la porte :

- **Ce que le serveur amorce est réémis** : T1 à 500 ms, doublé à chaque fois,
  abandonné au-delà de 16 s. Un pair qui n'acquitte plus rend sa parole.
- **Ce que le serveur répond est gardé** et rejoué octet pour octet si la
  requête revient. Servir deux fois une demande de parole en créerait deux.
- **Le drapeau R de l'en-tête dit lequel des deux.** Le serveur le pose sur
  chaque réponse, le transport le lit.

Deux pièges d'horloge, tous deux vérifiés par la suite de tests :

- **L'échéance d'abandon a sa propre horloge.** Adossée au tour de réémission,
  elle ne se déclenche jamais : les intervalles doublent, et le tour qui suit
  15,5 s tombe à 47,5 s.
- **Ne jamais soustraire deux horodatages pour tester une échéance.** Un
  message émis depuis le fil XML-RPC porte un temps postérieur à celui que le
  réacteur a pris pour son tour ; la soustraction non signée repasse par le
  haut, et la transaction est abandonnée sur-le-champ.

### La version émise est celle du pair

En réception, les versions 1 et 2 sont acceptées. En émission, c'est **la
version du dernier message reçu** de ce pair ; avant d'avoir rien entendu, la
version 2 de la RFC 8855.

Cela n'est pas une coquetterie : les endpoints en service parlent la version 1
sur UDP, et jettent en silence ce qui arrive en version 2.

Sur UDP, le serveur salue aussi de lui-même après son HelloAck, et
`initDocSharing` le fait dès que le SDP donne l'adresse du pair.

### Un HelloAck sans ses listes est accepté

La grammaire rend SUPPORTED-PRIMITIVES et SUPPORTED-ATTRIBUTES obligatoires
dans un HelloAck, et nous les émettons toujours. Nous ne les **exigeons pas**
en réception : sur UDP, un HelloAck est ce qui clôt notre Hello, et ce rôle ne
tient qu'à l'identifiant de transaction. Le refuser nous ferait réémettre
jusqu'à l'abandon, et lâcher un pair qui a répondu.

## La convention d'identité

Elle n'est écrite nulle part ailleurs, et un contrôleur doit la connaître :

| BFCP | Mediaserver |
|---|---|
| Conference ID | le `confId` XML-RPC |
| User ID | le `partId` XML-RPC du participant |
| Floor ID | toujours 1 |

Le contrôleur les place dans le SDP (`a=confid`, `a=userid`, `a=floorid`).

## Ce que la pile ne fait pas

- **TLS/BFCP**, et le BFCP de JSR-309.
- **La fragmentation** de la RFC 8855 : non émise, et un message qui porte le
  drapeau F est jeté.
- **ChairAction, UserQuery, FloorRequestQuery** : refusés par une erreur
  UnknownPrimitive.
- **Plusieurs floors** par conférence.

## Les tests

| Fichier | Ce qu'il éprouve |
|---|---|
| `mcu/tests/test_bfcp_server.cpp` | le contrôle de parole, sur un transport factice |
| `mcu/tests/test_bfcp_codec.cpp` | le format binaire, contre des octets écrits depuis la RFC |
| `mcu/tests/test_bfcp_codec_hardening.cpp` | suite adverse du parseur, derrière une page de garde |
| `mcu/tests/test_bfcp_tcp.cpp` | le transport TCP, sur une vraie socket en bouclage |
| `mcu/tests/test_bfcp_udp.cpp` | le transport UDP et ses délais, non simulés |
