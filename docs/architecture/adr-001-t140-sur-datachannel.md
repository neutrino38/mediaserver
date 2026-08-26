# ADR 001 — T.140 sur data channel : pile SCTP et transport

Statut : proposé.
Conception associée : [T140-DC](../conception/T140-DC/SPEC.md).

## Contexte

Un client WebRTC ne transporte pas de `m=text` sur profil RTP. Pour le texte
temps réel, il n'a que deux voies : une connexion hors bande (le WebSocket, déjà
livré), ou un data channel dans sa `RTCPeerConnection` (RFC 8865).

Le data channel demande deux choses que le serveur n'a pas : une pile SCTP, et
un chemin pour les données applicatives DTLS. Il demande aussi trois choses
qu'il a déjà : ICE, DTLS et un socket UDP avec sa boucle de lecture.

## Décision 1 — la pile SCTP est usrsctp, en mode sans thread

Nous lions `usrsctp` (EPEL 9, `usrsctp-devel`, 260 ko de bibliothèque), par
`pkg-config`, en dynamique. Nous l'initialisons par `usrsctp_init_nothreads` et
nous cadençons ses timers depuis la boucle `poll` de la session.

### Écarté : écrire la pile

Un sous-ensemble de SCTP suffisant pour un data channel, c'est l'association
(INIT, COOKIE), la retransmission, le contrôle de flux, le contrôle de
congestion, la fragmentation, le reset de flux, et les CRC32c. Ce n'est pas une
phase de chantier, c'est un projet — et un projet dont chaque défaut se paie en
séance d'interop contre Chrome.

### Écarté : lksctp (SCTP du noyau)

Déjà installé sur la machine, et hors sujet : le noyau parle SCTP sur IP, pas
SCTP sur DTLS. Un data channel WebRTC exige une pile en espace utilisateur,
parce que les datagrammes doivent traverser DTLS avant l'UDP.

### Écarté : libdatachannel

Elle ferait tout — SCTP, DCEP, mais aussi ICE, DTLS et une couche de
`PeerConnection` complète. Or ICE et DTLS, nous les avons, avec nos profils
d'adressage, notre latch, notre watchdog et nos deux rôles DTLS. L'intégrer
signifierait faire cohabiter deux piles ICE dans le même binaire, ou renoncer à
la nôtre. Elle n'est de plus pas empaquetée pour AlmaLinux 9, ce qui la
ramènerait dans `staticdeps` alors que le dépôt en sort.

### Pourquoi le mode sans thread

C'est la moitié de la décision. La sortie SCTP arrive alors sur **notre** thread
de session, celui qui tient déjà l'objet `SSL`. OpenSSL n'est pas concurrent :
en mode threads, il faudrait un verrou autour du DTLS, sur un chemin où quatre
autres mécanismes signalent déjà la même boucle. Le mode sans thread supprime le
problème au lieu de le protéger.

### Conséquences

- Une dépendance système de plus, dans le dépôt EPEL déjà utilisé par ailleurs.
- La bibliothèque est celle de Chrome, de Firefox et de Janus : la même pile aux
  deux bouts de chaque appel réel.
- `usrsctp_finish()` ne se fait qu'à l'arrêt du binaire, sous compteur de
  références : l'appeler alors qu'une socket vit est un crash connu.
- La boucle de la jambe texte tourne à 100 Hz tant qu'une association vit.

## Décision 2 — le transport du data channel est une `RTPSession`

La jambe data channel est une `RTPSession` de plus, dont on ne se sert pas pour
transporter du RTP. SCTP, DCEP et T.140 s'empilent sur une couture unique : la
frontière DTLS de cette session.

### Écarté : une classe de transport dédiée

Écrire un `DTLSTransport` propre au data channel voudrait dire réécrire la
réponse aux binding requests STUN, le latch d'adresse, les deux rôles DTLS avec
leurs retransmissions, les profils d'adressage, le watchdog d'inactivité et la
boucle `poll` — soit la moitié de `rtpsession.cpp`, dans un fichier neuf, non
éprouvé, et à maintenir en parallèle du premier.

### Écarté : basculer à la couture du mixeur, comme le pont WebSocket

`ParticipantTextWS` contourne l'absence de transport commutable dans
`RTPParticipant` en s'accrochant aux pipes du mixeur. C'était la bonne réponse
pour un WebSocket, qui n'a **pas** de jambe ICE/DTLS/UDP. Le data channel en a
une, et `TextStream` en possède déjà exactement une. Reproduire le détour
reviendrait à créer un second transport pour la même jambe.

### Conséquences

- L'API de contrôle n'a besoin que d'un appel nouveau par API pour dire « cette
  jambe texte est un data channel », plus un getter pour les paramètres SCTP.
  Empreinte DTLS, credentials STUN, candidats ICE, `StartReceiving`,
  `StartSending` : tout marche sans y toucher.
- Le contrat de thread est celui de la session, et le texte sortant doit donc
  passer par une file réveillant cette boucle.
- La couche `T140DataChannel` ne connaît ni ICE, ni DTLS, ni UDP. Le jour où le
  BUNDLE devient nécessaire, on la pose sur la session audio de l'endpoint au
  lieu d'une session dédiée, et rien au-dessus ne change.
- Une jambe qui ne porte pas de RTP n'a rien à couper : les émissions RTCP
  autonomes de la session sont toutes déclenchées par l'envoi ou la réception
  d'un paquet RTP.
