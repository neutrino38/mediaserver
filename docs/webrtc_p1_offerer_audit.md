# P1 — Audit du côté OFFERER WebRTC de l'Endpoint (medooze MCU)

Scénario audité : le contrôleur SIP utilise l'endpoint **en offreur** d'un appel
WebRTC. Le pair distant est **un autre gateway medooze** : **ICE-lite** (il
n'émettra jamais de STUN binding request) et **DTLS passive** (il n'émettra
jamais de ClientHello ; il attend le nôtre).

Séquence pilotée par le contrôleur :

```
EndpointGetLocalCryptoDTLSFingerprint("sha-256")
EndpointSetLocalSTUNCredentials(...)
EndpointStartReceiving(...)                 -> port local
... offre SDP locale émise, réponse distante reçue ...
EndpointSetRemoteSTUNCredentials(...)
EndpointSetRemoteCryptoDTLS(setup="passive", hash, fp)
EndpointStartSending(ip, port, rtpMap)
```

Chaîne d'appel commune : `xmlrpcjsr309.cpp` → `MediaSession.cpp` →
`Endpoint.cpp` → `RTPEndpoint`/`RTPSession` (`rtpsession.cpp`) → `DTLSConnection`
(`dtls.cpp`).

---

## 1. Rôle DTLS sur `setup="passive"` / `"actpass"`

**Le rôle CLIENT existe et est correctement câblé, mais le ClientHello n'est mis
sur le fil que sur réception d'un STUN entrant — jamais dans ce scénario.**

- Le `setup` reçu est le setup **distant**. `RTPSession::SetRemoteCryptoDTLS`
  (`rtpsession.cpp:462-491`) mappe la chaîne vers `dtls.SetRemoteSetup(...)`
  (467-474), puis pose `encript=true; decript=true` (487-488) et appelle
  `dtls.Init()` (491).
- `DTLSConnection::SetRemoteSetup` (`dtls.cpp:300-352`) **inverse** le rôle :
  remote `passive` → local `SETUP_ACTIVE` (**client**) ; remote `active` → local
  `SETUP_PASSIVE` (serveur).
- Le rôle OpenSSL est réellement posé dans `Init()` (`dtls.cpp:248-264`) :
  `SETUP_ACTIVE` → `SSL_set_connect_state()` puis `SSL_do_handshake()` — ceci
  **génère** le ClientHello mais l'écrit seulement dans `write_bio` (aucun envoi
  réseau à ce stade). La dérivation des clés SRTP dépend aussi du rôle
  (`dtls.cpp:486`).
- **Déclencheur d'émission sur le réseau** : le ClientHello n'est vidé de
  `write_bio` (via `dtls.Read`) et `sendto` **que dans la branche STUN entrant**
  de `RTPSession::ReadRTP` — `rtpsession.cpp:1520-1528`, avec le commentaire
  explicite *« Needed for DTLS in client mode »*. Destination = `from_addr`
  (source du paquet STUN reçu). Les paquets de handshake suivants passent par la
  branche DTLS entrant (`rtpsession.cpp:1566-1585`), aussi vers `from_addr`.

**Conséquence pour ce scénario** : le pair est ICE-lite ⇒ aucun STUN entrant ⇒
la branche `1520-1528` ne s'exécute jamais ⇒ le ClientHello **reste bloqué dans
`write_bio`**. Le pair est DTLS-passive ⇒ il n'envoie pas non plus de
ClientHello. **Handshake mort de faim des deux côtés.**

> Piège annexe `actpass` : le défaut `dtls_setup = SETUP_PASSIVE`
> (`dtls.cpp:203`) fait que remote `actpass` ne bascule en actif **que si**
> `dtls_setup == SETUP_ACTPASS` (`dtls.cpp:319-322`) — sinon on reste
> **serveur**. Sans réglage préalable, un pair `actpass` nous laisse passifs.

## 2. STUN sortant après `SetRemoteSTUNCredentials`

**L'endpoint n'émet jamais de binding request proactif ; il ne fait que répondre
et renvoyer un check « triggered » en réaction à un check entrant.**

- `SetRemoteSTUNCredentials` (`rtpsession.cpp:447-457`) ne fait que stocker
  `iceRemoteUsername`/`iceRemotePwd`. Aucun envoi.
- Tout STUN sortant est dans `ReadRTP`, **à l'intérieur** du traitement d'un
  `Request/Binding` entrant (`rtpsession.cpp:1390`) : réponse
  (`CreateResponse`, envoi `:1420`) puis back-request en miroir (`:1472`, envoi
  `:1512`), tous deux vers `from_addr`.
- Rôle ICE purement en miroir (`IceControlling`+`UseCandidate` si l'entrant est
  `IceControlled`, sinon `IceControlled` — `:1478-1484`). **Aucun timer de
  connectivity-check** : la boucle `Run` (`:1869-1956`) est pilotée par `poll()`
  et son seul timeout sert au watchdog d'inactivité RTP, pas à émettre du STUN.

**Conséquence** : face à un pair ICE-lite qui n'initie jamais, l'endpoint reste
muet ⇒ rien ne latche l'adresse par STUN, et le déclencheur du ClientHello (§1)
n'arrive jamais.

## 3. Gating média

**Pas de verrou explicite « STUN complété ». Le seul verrou est SRTP, lui-même
posé après la fin du handshake DTLS.**

- **Envoi RTP** : `SendPacket` (`rtpsession.cpp:1022`) exige une destination
  connue (`sendAddr`, sinon repli sur `recIP`, sinon drop — `:1028-1046`) et,
  si `encript`, exige `sendSRTPSession` (`:1166-1172`, sinon *« encryption is
  not yet setup »* + drop). Comme `SetRemoteCryptoDTLS` a posé `encript=true`,
  **l'émission RTP est de facto bloquée jusqu'à la fin du handshake DTLS**.
- **Réception SRTP** : l'adresse est **latchée sur le premier paquet entrant
  plausible** (STUN `:1446-1461`, ou RTP `:1599-1613`) — aucun STUN préalable
  requis. Le déchiffrement exige `recvSRTPSession` + succès de `srtp_unprotect`
  (`:1630-1665`).
- La policy SRTP (send+recv) n'est installée qu'à `SSL_CB_HANDSHAKE_DONE` :
  `dtls.cpp:397-406` → `SetupSRTP` (vérif fingerprint pair + export keying,
  `:421-515`) → `onDTLSSetup` (`rtpsession.cpp:2940`) qui pose `sendSRTPSession`
  et `recvSRTPSession`.

**Conséquence** : sans handshake DTLS (§1), ni `sendSRTPSession` ni
`recvSRTPSession` ⇒ aucun média ne circule dans les deux sens.

## 4. Propriété RTP `"secure"`

**Non requise et jamais lue.** `RTPSession::SetProperties` (`rtpsession.cpp:346`)
ne traite pas `"secure"` : toute clé inconnue tombe dans `else` →
`Error("Unknown RTP property [secure]")` (`:423-424`). Un `grep` confirme que
`secure` n'existe que dans le code WebSocket, jamais dans le chemin RTP/SRTP.

SRTP est armé **implicitement** par les setters crypto, indépendamment de
`"secure"` : `SetLocalCryptoSDES`→`encript=true` (`:333`),
`SetRemoteCryptoSDES`→`decript=true` (`:567`),
`SetRemoteCryptoDTLS`→`encript=decript=true` (`:487-488`).

**Verdict** : le nouveau contrôleur Elixir a raison de ne pas l'envoyer ; les
contrôleurs legacy qui l'envoient ne sont pas impactés (no-op inoffensif).

## 5. `EndpointAddICECandidate`

Handler → `rtpsession.cpp:735-800`. Ne retient que **composant 1 (RTP), UDP,
type `host`/`srflx`**, priorité strictement supérieure à la meilleure connue
(`:760-778`). **RTCP (comp 2) et `relay` ignorés** (Trickle « Niveau 1 », pas
d'agent ICE complet).

**Un candidat peut servir de destination avant `StartSending`** : `AddICECandidate`
écrit directement `sendAddr`/`sendRtcpAddr` (`:788-794`) puis émet un **paquet
RTP vide** de priming NAT (`:797`), indépendamment de `StartSending`. En
revanche il **n'émet ni STUN ni ClientHello** — il ne fait que fixer l'adresse et
ouvrir le chemin ; l'émission média applicative reste soumise au flag `sending`
(`RTPEndpoint.cpp:187`).

---

## Conclusions pour P2 et P3

### P2 — Rôle DTLS actif (client) sur l'endpoint → **NÉCESSAIRE (avec caveats)**

Le rôle client lui-même **fonctionne déjà** (`SETUP_ACTIVE` complet :
`connect_state`, `do_handshake`, ordre des clés SRTP). Ce qui manque : **le
ClientHello n'est mis sur le fil que déclenché par un STUN entrant**
(`rtpsession.cpp:1520-1528`), condition jamais remplie face à un pair
ICE-lite + DTLS-passive. P2 doit **découpler l'émission du ClientHello** du STUN
entrant : flusher `write_bio` vers `sendAddr` dès qu'une destination est connue
(sur `StartSending`, ou sur `AddICECandidate`, ou via un timer de
retransmission), et retransmettre tant que le handshake n'est pas terminé.
Caveat : sans destination (`sendAddr` encore `INADDR_ANY`), rien à faire — il
faut donc que `StartSending`/candidat ait fixé l'adresse.

### P3 — Binding requests STUN sortants vers un pair ICE-lite → **NÉCESSAIRE**

Aujourd'hui **zéro** STUN proactif (§2) : seulement des checks « triggered ».
Un pair ICE-lite n'initie jamais, donc l'endpoint doit émettre lui-même des
binding requests (rôle controlling, `USE-CANDIDATE`) vers l'adresse de
`StartSending`/du candidat, avec retransmission, jusqu'à obtenir une réponse.
Caveat à traiter dans P3 : le handler STUN entrant ne traite que
`type==Request` (`rtpsession.cpp:1390`) ; **les binding responses entrantes sont
actuellement ignorées** (aucun flush DTLS, aucun latch dédié). P3 doit donc
aussi traiter la **réponse** du pair (latch d'adresse + déclenchement du flush
ClientHello) — ce qui recoupe P2. **P2 et P3 sont interdépendants** et devraient
être livrés ensemble pour débloquer le handshake avec un pair ICE-lite.

### Anomalies annexes repérées (hors périmètre, à noter)

- `rtpsession.cpp:1300` : `sendRtcpAddr.sin_port = from_addr.sin_addr.s_addr;`
  copie l'**IP** dans le champ **port** (bug probable, RTCP latché).
- `rtpsession.cpp:1454-1460` : incohérence d'ordre d'octets (`htons` vs `ntohs`)
  sur le port d'envoi latché par STUN.
- `dtls.cpp:319-322` : défaut `actpass` reste passif (voir §1).
- `rtpsession.cpp:1397` (TODO) : le username des binding requests entrants n'est
  pas vérifié.
