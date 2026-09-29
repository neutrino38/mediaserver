# ADR 004 — Le BUNDLE passe par une session porteuse

Statut : proposé.
Conception associée : [UNIFIED-PLAN](../conception/UNIFIED-PLAN/SPEC.md).

## Contexte

Un participant doit pouvoir apporter plusieurs flux vidéo dans une même
communication, chacun dans un slot de mosaïque. Le client visé est un
navigateur, qui suit la RFC 9429.

Trois faits pèsent sur la décision :

- Avec la politique `bundlePolicy` par défaut (`balanced`), un navigateur
  offre la deuxième section `m=video` en `bundle-only`, port 0 (RFC 9429
  §4.1.1 et §5.2.1). Si la réponse ne prend pas le groupe BUNDLE, cette
  section est rejetée (RFC 8843 §7.2). **Sans BUNDLE, la deuxième vidéo
  n'est pas négociable** avec un client tiers.
- Le serveur n'a jamais fait de BUNDLE. `RTPSession` fond en une classe le
  transport (sockets, ICE, DTLS, SRTP, latch, `PollHandler`) et la session
  (rtpMap, SSRC, RTCP, estimateurs). Le fichier fait 4 700 lignes de chemin
  chaud, et porte les chantiers ICE sortant, DTLS client, SCTP, watchdog et
  réacteur.
- Le serveur sait déjà lire plusieurs SSRC sur une session (`AddStream`,
  `GetPacket(ssrc)`) : le partage de document BFCP fait lire à la jambe
  SLIDES un SSRC de la session MAIN. Le contexte SRTP n'est lié à aucun SSRC
  (`ssrc_any_inbound`, `ssrc_any_outbound`).

## Décision 1 — Le serveur accepte le BUNDLE

Le serveur sait porter toutes les jambes RTP d'un participant sur un seul
transport, et le contrôleur répond `a=group:BUNDLE`.

### Écarté : refuser le BUNDLE et régler les clients en `max-compat`

`max-compat` donne un transport à chaque section, et le modèle actuel
suffirait. Mais ce réglage vit dans le code JavaScript du client, pas dans le
SDP. Il ne vaut que pour un client écrit par IVèS. Un client tiers reste en
`balanced`, et sa deuxième vidéo est perdue. Cette voie sert tout de même de
**recette intermédiaire** du mixeur, avant que le transport partagé n'existe.

### Conséquences

- `a=rtcp-mux` devient obligatoire sur les sections du groupe (RFC 8843
  §9.3). Le serveur le fait déjà.
- Le serveur doit démultiplexer par MID, SSRC puis payload type (RFC 8843
  §9.2). L'extension d'en-tête `sdes:mid` est à lire et à écrire.
- Un participant en BUNDLE ouvre deux ports au lieu de huit.

## Décision 2 — Une session porteuse, des sessions jointes

Une `RTPSession` du participant garde le transport : la session audio,
toujours. Les autres sessions s'y joignent. Une session jointe n'ouvre pas de
socket et n'est pas un `PollHandler`. Elle émet par la porteuse, avec le
contexte SRTP de la porteuse, et reçoit les paquets que la porteuse lui route
après déchiffrement. Tout le reste d'une session jointe est inchangé.

### Écarté : scinder `RTPSession` en transport et session

C'est la forme la plus propre. Elle touche toutes les sorties réseau, tout le
chemin de réception et tous les chantiers posés sur ce fichier. Le risque de
régression sur les appels d'aujourd'hui est le plus élevé des trois, pour un
gain qui ne se voit pas de l'extérieur. La session porteuse ouvre la même
couture, mais une fonction à la fois, et laisse le code des participants
sans BUNDLE strictement identique.

### Écarté : plusieurs SSRC sur la session vidéo MAIN

C'est le précédent BFCP : un `VideoStream` par SSRC, tous lus sur la même
session. Il ne demande presque rien. Mais il partage la rtpMap et le RTCP
entre les sections, il ne met pas l'audio ni le texte sur le même transport,
et il casse le « un port = une session » de `RTPEndpoint` en JSR-309. Il ne
fait donc pas de BUNDLE au sens de la RFC.

### Conséquences

- Toute la réception RTP d'un participant en BUNDLE passe par un seul thread.
  Un callback qui bloque bloque l'audio comme la vidéo. Les règles de
  `docs/reference/threads-rtp.md` s'appliquent sans exception.
- Les réglages de transport reçus sur une session jointe (ICE, DTLS,
  candidats, adresse d'envoi) sont acceptés et ignorés : la RFC 8843 les rend
  identiques sur toutes les sections du groupe.
- REMB et transport-wide-cc sont portés par la porteuse, car ils parlent du
  transport.
- Le data channel T.140 se pose sur la DTLS de la porteuse, comme la SPEC
  T140-DC §13.1 l'avait prévu.
