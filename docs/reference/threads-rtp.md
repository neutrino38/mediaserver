# Le modèle de threads du transport RTP

Toute `RTPSession` est battue par un **réacteur partagé** : un thread et un seul
`poll()` pour un **groupe** de sessions. Aucune session ne porte son propre
thread.

Les paquets reçus sont ensuite **tirés** par un thread consommateur, qui appelle
`RTPSession::GetPacket()`. Le réseau et le traitement média sont donc sur des
threads distincts : un décodage long ne retarde jamais la lecture des sockets.

La conception et les mesures qui ont mené là sont dans
`docs/conception/RTP-REACTOR/SPEC.md`. Ce document décrit ce qui tourne.

---

## 1. Qui porte quel thread

| Thread | Combien | Ce qu'il fait |
|---|---|---|
| `RtpSessionSet` (réacteur) | 2 par jambe | `poll()` des sockets du groupe, lecture, RTCP, timers |
| `recAudioThread`, `recVideoThread` (×2), `recTextThread` | 4 par `RTPParticipant` | tirent les paquets et les décodent |
| `MultiplexLoop` | 1 par jambe reçue d'un `Endpoint` JSR-309 | idem, côté JSR-309 |
| `sendXThread`, `RTPSmoother` | 4 + 2 par `RTPParticipant` | chemin d'émission, inchangé par le réacteur |

Un `RTPParticipant` en appel coûte **12 threads**, un `Endpoint` JSR-309 à deux
jambes reçues **4**, un appel B2BUA **8**.

## 2. Les groupes

**Deux réacteurs par jambe** : `{audio, texte}` d'un côté, `{vidéo MAIN, vidéo
SLIDES}` de l'autre. L'audio veut la main toutes les 20 ms ; il ne doit pas
attendre derrière la vidéo.

Les propriétaires sont `RTPParticipant` (membres `mediaGroup` et `videoGroup`)
et `Endpoint` côté JSR-309. Le nom du groupe est ce qui rend les traces
lisibles :

| API | Nom |
|---|---|
| MCU | `part-<id du participant>-media` / `-video` |
| JSR-309 | `jsr309-<adresse de l'Endpoint>-media` / `-video` |

Une session dont personne ne fixe le groupe tombe dans le **groupe par défaut**
du processus, créé à la demande. C'est le repli des tests et de `Broadcaster` :
il n'existe pas de session hors réacteur.

### Deux règles d'écriture, et elles mordent

1. **`SetPollGroup()` s'appelle AVANT `Init()`.** Après, il est refusé : la
   session est déjà inscrite ailleurs.
2. **Un membre `RtpSessionSet` se déclare AVANT les flux et les ports qu'il
   bat.** Les membres se détruisent dans l'ordre inverse de leur déclaration :
   déclaré après, le réacteur mourrait avant ses sessions, et le retrait
   qu'`End()` fait porterait sur un objet détruit.

## 3. Ce que le thread du réacteur porte

À chaque tour :

- **un** datagramme par socket prêt — lecture, déchiffrement SRTP, analyse,
  dépôt dans le jitter buffer ;
- le RTCP entrant et ses callbacks (`onFPURequested`,
  `onTempMaxMediaStreamBitrateRequest`, `onReceiverEstimatedMaxBitrate`,
  `onSenderEstimatedBitrate`) ;
- les timers : watchdog d'inactivité RTP, checks ICE, retransmissions du
  handshake DTLS client, amorçage NAT, rapports transport-cc, tick applicatif
  du data channel.

Ce qu'il ne porte **pas** : décodage, encodage, transcodage, mixage. Ni
l'émission — `SendPacket()` écrit depuis le thread appelant, et les lisseurs
gardent le leur, parce qu'ils sont le pacer du contrôle de débit.

## 4. Écrire un callback appelé par le réacteur

C'est là que se joue tout le risque de ce modèle.

1. **Ne jamais bloquer.** Un callback qui bloque bloque **toutes** les jambes de
   son groupe. Les callbacks du RTCP sont des affectations : garder cette
   propriété.
2. **Ne pas détruire sa propre session depuis le thread du réacteur.**
   `Remove` reconnaît seul l'appel venu du réacteur — il compare son
   `std::thread::id` — et rend la main sans attendre, au lieu de s'attendre
   lui-même. Le retrait est donc sûr depuis un callback ; la destruction de
   l'objet qui porte le callback en cours ne l'est pas.
3. **Ne pas prendre un verrou que peut tenir un thread lent** — consommateur,
   encodeur, mixeur.
4. **Chaque travail périodique garde sa propre horloge.** `OnPeriodic` peut être
   appelé plus souvent que son échéance : un travail qui compterait les appels
   au lieu de lire le temps se déclencherait à tort.
5. `GetNextTimeoutMs` rend **-1** quand rien n'est armé : le réacteur dort, il ne
   tourne pas à vide. Ne borner que si un travail l'exige, et rendre le minimum.

## 5. Le retrait est synchrone, et c'est un contrat

```cpp
Group()->Remove(this);   // ne rend la main qu'une fois le réacteur sorti
close(simSocket); close(simRtcpSocket);
```

Au retour de `Remove`, le réacteur ne poll plus les sockets de cette session et
n'appellera plus aucun de ses callbacks. C'est ce qui rend le `close()` sûr :
sans lui, le descripteur pourrait être réattribué à une autre session pendant
que le `poll()` d'un autre thread le tient encore.

**Ne jamais fermer un socket avant le retrait.**

## 6. Le mode tiré

```cpp
RTPPacket* GetPacket(DWORD ssrc, DWORD timeoutMs);   // ssrc 0 = flux par défaut
```

| Cas | Comportement |
|---|---|
| Le flux existe | attente bornée, fenêtre de réordonnancement préservée |
| Le flux est `disabled` (arrêt en cours) | NULL **immédiat** : sortir de la boucle |
| Le flux n'existe pas encore | attente de sa **naissance**, jamais de sondage |

Les consommateurs passent `RTPSession::ConsumerPollMs`, soit **200 ms** : ils
relisent leur drapeau d'arrêt au pire toutes les 200 ms, et les `Cancel*()`
existants les réveillent tout de suite.

## 7. Les couplages connus, et leurs bornes

Un groupe partage un thread : ce qui retarde une jambe retarde ses voisines de
groupe. Trois sources, toutes bornées.

| Source | Borne | Quand |
|---|---|---|
| Verrou `streamUse` tenu par un consommateur parqué | **200 ms** | à la naissance d'un SSRC sur une session dont un consommateur attend |
| Handshake DTLS | ~50 ms | à l'établissement, avant tout paquet média |
| Un tour de réacteur | quelques dizaines de µs | en permanence ; le tour coûte O(N) sur les sessions du groupe |

Le premier mérite un mot, parce qu'il n'est pas devinable. `GetPacket` prend le
verrou **lecteur** `streamUse` avant l'attente bornée et ne le rend qu'après ; le
réacteur prend ce même verrou en **écriture** quand un SSRC inconnu arrive
(`SetDefaultStream`, `ChangeStream`), et les lecteurs sont prioritaires. Un flux
SLIDES qui naît à côté de MAIN, ou un changement de SSRC sur re-INVITE, peut
donc faire attendre le réacteur. C'est une des raisons du découpage en deux
groupes : cette attente ne touche jamais l'audio.

## 8. La trace qui dit qu'un groupe a attendu

Le réacteur mesure la durée de chaque tour et trace au-delà de **50 ms**, au
plus une fois par seconde et par groupe :

```
-RtpSessionSet [jsr309-0x7f…-video] tour long : 62 ms — les autres jambes du groupe ont attendu
```

Dépouillement :

```sh
grep -a "tour long" /var/log/mcu.log | sed -E 's/.*\[(.*)\] tour long.*/\1/' | sort | uniq -c
```

Une trace à l'**établissement** sur un groupe `-media` vient du handshake DTLS :
lire le thread qui l'a écrite pour le confirmer. Une trace en **régime établi**
est un vrai défaut : du travail long est entré dans un thread de réacteur.

## 9. Où c'est tenu

- `mcu/tests/test_rtpsessionset.cpp` : le réacteur seul — un thread pour N
  handlers, l'aiguillage, le minimum des échéances, le retrait synchrone, le
  retrait réentrant, un socket mort qui ne rend pas son groupe sourd.
- `mcu/tests/test_rtp_reactor.cpp` : la session dans le réacteur — six jambes
  n'ajoutent aucun thread, l'inscription à `Init` et le retrait à `End`, le
  découpage en deux groupes par jambe, l'attente bornée qui ne sonde pas.
- `docs/maintenance/recette-reacteur-rtp.md` : la recette en appel réel, que les
  tests ne remplacent pas.
