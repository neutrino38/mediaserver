# Le problème
le mediaserver crée trop de thread pour ses traitements

## Le cas de rtpsession 
un endroit que j'ai identifié est la thread de réception des paquets de rtpsession : 
elle bloque sur les sockets RTP/RTCP et place les paquets dans une file. Les consommateur
de ces paquets sont souvent obligé de faire des sleep si la session ne retourne pas de paquet.

# Le refactoring

Supprimer complètement la thread de rtpsession en
- transformant la fonction Run() en une fonction qui bloque une fois sur les sockets (avec un poll limité à 1000 ms)
- une fois qu'elle a traité un paquet reçu, elle sort en indiquant si elle a traité un paquet ou non.
- le GetPacket() appelé par le consommateur dépile le paquet le plus récent et s'il n'en an pas appelle Run()
- sur Run a traité un paquet RTP alors GetPaket dépile ce dernier. Sinon il retourne null

Dans le consommateur : on continue à appeler GetPacket() mais on supprime tous les sleep() puisque GetPacket() est bloquant de fait

Ainsi la lecture des paquets RTP s'effectue dans la thread de l'appelant ( audio/video/textstream ou JSR309 RTPEndPoint)

# Les instructions :

- Examine ce refactoring et estime sa faisabilité et sa pertinences
- identifie les bloquages
- crée une conception logicielle précise en te focalisant sur les cas d'usages dans rtpparticipant et jsr309/RTPEndpoint
- enrichir ce document dans la section 'conception logicielle'
- crée un plan d'implémentation et note le dans ce document

# Conception logicielle

## 1. État des lieux

### Threads du chemin de réception (aujourd'hui)

Pour **une** session RTP entrante il existe actuellement **deux** threads :

| Thread | Créé par | Rôle | Blocage |
|--------|----------|------|---------|
| **Thread lecteur RTP** (`RTPSession::run`/`Run`) | `RTPSession::Init() → Start()` (l. 911) | `poll()` sur `simSocket`+`simRtcpSocket`, `ReadRTP()`/`ReadRTCP()`, empile dans `RTPStream`(=`RTPBuffer`) | `poll(-1)` (ou 1000 ms si watchdog armé) |
| **Thread consommateur** (`RecAudio`/`RecVideo`/`RecText`, `RTPEndpoint::Run`) | `StartReceiving()` de chaque stream/endpoint | boucle `GetPacket()` → décode/multiplexe | `RTPBuffer::Wait()` (condvar) **+ `msleep(100..1000)` si NULL** |

Le thread lecteur remplit une file (`RTPBuffer`), le thread consommateur la vide via `Wait()`. Quand la file est vide, `GetPacket()` renvoie NULL et le consommateur fait un `msleep()` (busy-poll dégradé) — c'est exactement le symptôme décrit.

Le **chemin d'émission** (threads `sendAudioThread`, `AudioEncoderWorker`, encodeurs…) est indépendant et **n'est pas touché** par ce refactoring : `SendPacket()` écrit sur le socket depuis le thread appelant.

### Ce que fait *réellement* la thread lecteur (au-delà de « lire du RTP »)

`ReadRTP()`/`ReadRTCP()` ne traitent pas que du média. Sur `simSocket` transitent aussi **STUN (ICE)**, **DTLS (clés SRTP)** et, en mode `rtcp-mux`, le **RTCP**. La boucle assure donc aussi :
- le *handshake* ICE/DTLS (réponses STUN, poursuite DTLS) ;
- le traitement des feedbacks RTCP entrants (RR/SR/NACK/FIR/REMB) → `SetRTT()`, `RequestFPU`, ré-émission RTX (`ReSendPacket`) ;
- le **watchdog d'inactivité** (`onRTPTimeout`, gap 5) ;
- l'alimentation du **jitter buffer** en avance de phase.

Ces responsabilités doivent survivre au refactoring : elles migrent dans la thread appelante, mais **ne doivent pas disparaître**.

### Le jitter buffer est le point dur

`RTPBuffer::Wait()` (rtpbuffer.h) **n'est pas** une simple file FIFO : c'est un tampon de gigue avec réordonnancement temporel. La tête n'est rendue que si `next==-1 || seq==next || time+maxWaitTime<now || hurryUp`. `maxWaitTime` vaut 0 pour l'audio/texte mais **60 à 300 ms pour la vidéo** (fixé dans `SetRTT`, l. 2290/2295, selon RTT et activation NACK). Autrement dit, pour la vidéo WebRTC, `Wait()` **retient volontairement** un paquet jusqu'à 300 ms pour laisser arriver les paquets manquants (réordonnancement + récupération NACK/RTX).

Dans le modèle actuel c'est le thread lecteur, qui tourne en continu, qui remplit le tampon *pendant* que le consommateur attend l'échéance. Fusionner les deux threads casse cette avance de phase si on n'y prend pas garde (cf. blocage **B1**).

## 2. Faisabilité et pertinence

**Verdict : faisable et pertinent**, avec une réserve forte sur le jitter buffer et sur l'amorçage ICE/DTLS.

- **Gain réel** : on supprime **une thread par session RTP entrante** (le lecteur), soit typiquement audio+vidéo(+slides)+texte par participant → 2 à 4 threads économisés par participant, et l'élimination de tous les `msleep()` de sondage. La lecture se fait dans la thread consommatrice qui existe déjà.
- **Pertinent** car le `msleep()` ajoute jusqu'à 1 s de latence de bout en bout à vide et le thread lecteur passe l'essentiel de son temps bloqué dans `poll()`.
- **Réserve** : le modèle « lire seulement quand le consommateur demande » est correct pour l'audio/texte (FIFO, `maxWaitTime=0`) mais doit préserver la fenêtre de réordonnancement vidéo et l'amorçage ICE/DTLS. Ces deux points structurent la conception ci-dessous.

## 3. Blocages identifiés

| # | Blocage | Gravité | Résolution retenue |
|---|---------|---------|--------------------|
| **B1** | **Jitter buffer** : `Wait()` retient la tête jusqu'à `maxWaitTime` (≤300 ms vidéo). En mono-thread, si on ne lit que file vide, plus d'avance de phase → réordonnancement/NACK dégradés. | Haute | Ne plus utiliser la condvar de `Wait()`. Extraire un **déqueue non bloquant** `GetDue(msUntilDue)` (respecte la fenêtre) ; l'attente se fait dans le `poll()` de `Run()`, **borné par le temps restant avant échéance de la tête**, pendant lequel les paquets manquants continuent d'arriver et remplissent les trous. |
| **B2** | Réveil du `poll()` sans thread cible : `CancelGetPacket`, `Stop`, `ArmRTPTimeout` font `pthread_kill(thread,SIGIO)`. Plus de thread propriétaire. | Haute | **self-pipe** (ou `eventfd`) ajouté comme 3ᵉ `pollfd`. Cancel/End/Arm écrivent 1 octet ; `Run()` draine le pipe au réveil. Supprime toute dépendance à `SIGIO`. |
| **B3** | **RTCP servi uniquement quand le consommateur appelle `GetPacket`**. Session *sendonly* ou récepteur au repos → RTCP entrant (RR, REMB, NACK) non traité. | Moyenne | Acceptable côté réception (pas de consommateur = pas de média à servir). Pour le RTCP du chemin *sendonly*, cf. §4.4 : conserver un service RTCP minimal (option : `poll` RTCP dans la thread d'émission, ou timer léger). À décider par média. |
| **B4** | **Accès concurrent au `poll()`** : un `poll()` d'un même fd depuis deux threads est illicite dans ce design. Multi-SSRC : `GetPacket(ssrc)` sur une même session. | Moyenne | Un seul consommateur par session (déjà le cas : 1 thread `RecX` par stream). Ajouter un **`recvMutex`** dans `Run()` pour sérialiser ; le premier thread qui lit remplit **tous** les buffers de flux de la session (les autres SSRC en profitent). Documenter l'invariant « 1 pompe de réception par session ». |
| **B5** | **Watchdog** `onRTPTimeout` évalué dans la boucle thread. | Basse | Déplacé dans `Run()` (évalué à chaque cycle `poll`). Reste correct tant que le consommateur appelle `GetPacket` régulièrement (c'est le cas dès `StartReceiving`). |
| **B6** | **Amorçage ICE/DTLS** : aujourd'hui `Init()→Start()` lit *immédiatement*, avant tout `StartReceiving`. En mono-thread, rien ne lit tant que la pompe consommatrice n'est pas lancée → le *handshake* DTLS/STUN peut ne pas démarrer. | Haute (WebRTC) | Garantir que la **pompe de réception démarre à `Init`/setup**, pas au premier média. Deux options : (a) démarrer la boucle `RecX`/`Run` dès `Init` (elle drainera STUN/DTLS puis média) ; (b) conserver une pompe interne *uniquement* pendant le handshake. Option (a) retenue (cf. §4.5), avec validation sur un appel WebRTC réel. |
| **B7** | `running`, `End()`, `Stop()` reposent sur `pthread_join`. Plus de thread. | Basse | `Stop()` ne *join* plus ; il signale le self-pipe et met `running=false`. `End()` reste idempotent, ferme les sockets. Le cycle de vie de l'objet est déjà protégé par les `shared_ptr` (plan smart pointers). |
| **B8** | **NACK/RTX** : la demande de retransmission et la ré-émission dépendent du traitement RTCP, désormais opportuniste. | Moyenne | Le RTCP entrant est traité dans `Run()` à chaque cycle (comme le RTP) ; la fenêtre `maxWaitTime` laisse le temps au RTX d'arriver puisque le `poll` attend l'échéance. Inchangé fonctionnellement tant que la pompe tourne. |
| **B9** | `ArmRTPTimeout` réveille via `pthread_kill`. | Basse | Remplacé par écriture self-pipe (B2). |
| **B10** | Tous les consommateurs contiennent des `msleep()` après `GetPacket` NULL. | Basse | Supprimés : `GetPacket` est bloquant de fait (poll ≤1000 ms). |

## 4. Conception cible

### 4.1 `RTPSession` — nouvelle API interne

Suppression : `pthread_t thread`, `static void* run(void*)`, `Start()` en tant que créateur de thread, `pthread_join/kill`.

```cpp
private:
    // Un cycle de poll borné : draine les datagrammes prêts sur RTP+RTCP+wakefd,
    // aiguille STUN/DTLS/RTP/RTCP, alimente les RTPStream, évalue le watchdog.
    // Retour : nb de paquets média RTP empilés (>0), 0 si timeout/autre trafic, -1 si erreur dure.
    int Run(int timeoutMs);
    std::mutex recvMutex;      // sérialise Run() (B4)
    int  wakePipe[2] = {-1,-1};// self-pipe (B2) ; wakePipe[0] lu par poll, wakePipe[1] écrit par Wake()
    void Wake();               // écrit 1 octet non bloquant sur wakePipe[1]
    std::atomic<bool> cancelPacket{false};
```

`Init()` : ouvre les sockets, crée le self-pipe, met `running=true`, **ne crée plus de thread** (plus d'appel à `Start()`).

`End()` : `running=false`, `Wake()`, ferme sockets et self-pipe. Idempotent, sans `join`.

### 4.2 `Run(int timeoutMs)` — refonte

```
lock(recvMutex)
ufds[0]=simSocket(POLLIN|ERR|HUP); ufds[1]=simRtcpSocket(...); ufds[2]=wakePipe[0](POLLIN)
nready = poll(ufds, 3, timeoutMs)      // timeoutMs fourni par GetPacket (≤1000)
si nready<0 : EINTR/EAGAIN→0 ; sinon Error+return -1
si ufds[2].POLLIN : drainer wakePipe[0] (lecture jusqu'à EWOULDBLOCK)   // réveil Cancel/End/Arm
si ufds[0].POLLIN : boucle recvfrom(MSG_DONTWAIT) jusqu'à EWOULDBLOCK → ReadRTP() par datagramme
                    (met à jour lastRecv, rtpTimedOut=false ; compte les paquets média)
si ufds[1].POLLIN : idem → ReadRTCP()
évaluer watchdog onRTPTimeout (B5)
si POLLHUP|ERR|NVAL sur RTP ou RTCP : return -1
unlock(recvMutex)
return nbPaquetsMediaEmpiles
```

Point clé (**B1**) : on **draine** tout ce qui est prêt à chaque réveil (boucle `recvfrom` jusqu'à `EWOULDBLOCK`), pas un seul datagramme. Ainsi une rafale reçue pendant l'attente d'échéance remplit le tampon et permet le réordonnancement.

### 4.3 `GetPacket` — bloquant, dans la thread appelante

```cpp
RTPPacket* RTPSession::GetPacket(DWORD& ssrc) {
    while (running && !cancelPacket) {
        RTPStream* s = (ssrc!=0) ? getStream(ssrc) : defaultStream;
        DWORD msUntilDue = 0;
        RTPPacket* rtp = (s && !s->disabled) ? s->GetDue(msUntilDue) : nullptr;
        if (rtp) return rtp;                       // tête échue → on la rend
        // rien d'échu : bloquer dans poll, borné par l'échéance de la tête (sinon 1000 ms)
        int to = msUntilDue ? std::min<DWORD>(msUntilDue, 1000) : 1000;
        if (Run(to) < 0) return nullptr;           // erreur socket : le consommateur verra NULL
    }
    cancelPacket = false;
    return nullptr;                                 // annulé / arrêté
}
```

`GetPacket()` sans SSRC = idem sur `defaultStream`. **Plus de `msleep()` interne.** Le blocage est porté par `poll()`.

`GetDue(DWORD& msUntilDue)` = extraction **non bloquante** de `RTPBuffer::Wait()` :
- annulé → `nullptr`, `msUntilDue=0` ;
- vide → `nullptr`, `msUntilDue=0` (→ le `poll` attend 1000 ms) ;
- tête échue (`next==-1 || seq==next || time+maxWaitTime<now || hurryUp`) → dépile et retourne ;
- tête retenue → `nullptr`, `msUntilDue = (time+maxWaitTime) - now` (le `poll` attendra juste ce qu'il faut, en continuant à recevoir).

`Wait()` historique est réécrit par-dessus `GetDue` (ou conservé pour d'éventuels autres appelants, mais plus utilisé sur ce chemin).

### 4.4 Annulation / arrêt

`CancelGetPacket[(ssrc)]` : `cancelPacket=true`, `s->Cancel()` (marque le buffer), puis **`Wake()`** (débloque le `poll`). Latence d'annulation ≈ immédiate (au lieu de ≤1000 ms).

Séquence d'arrêt d'un stream (ex. `AudioStream::StopReceiving`) inchangée côté appelant : `receivingAudio=TaskStopping; rtp.CancelGetPacket(); pthread_join(recAudioThread)`. La boucle `RecAudio` sort car `GetPacket` retourne `nullptr` (cancel) **et** `receivingAudio!=TaskRunning`.

### 4.5 Diagramme de séquence (réception vidéo WebRTC)

```
Thread RecVideo (consommateur, seul thread du chemin RX)
   │  session->GetPacket(recSSRC)
   │     ├─ GetDue → NULL, msUntilDue=0 (buffer vide)
   │     └─ Run(1000): poll → STUN req → réponse ; poll → DTLS → clés SRTP
   │  GetPacket(recSSRC)  (média commence)
   │     ├─ GetDue → NULL, msUntilDue=80ms (tête retenue, réordo)
   │     ├─ Run(80): poll draine 3 datagrammes RTP (dont le manquant) + 1 RTCP RR
   │     └─ GetDue → paquet échu, dans l'ordre → return
   │  décode / multiplexe … (plus de msleep)
```

## 5. Cas d'usage détaillés

### 5.1 RTPParticipant — `audiostream` / `videostream` / `textstream`

- `AudioStream::RecAudio` (l. 332-343) : supprimer `msleep(200)` ; sur NULL, `continue` (la boucle re-teste `receivingAudio==TaskRunning`). `maxWaitTime=0` → FIFO pur, `GetDue` rend immédiatement dès qu'un paquet est là.
- `VideoStream::RecVideo` (l. 730-742) : supprimer `msleep(1000)` ; `GetPacket(recSSRC)` porte l'attente. Fenêtre de réordonnancement préservée par `msUntilDue`. `CancelGetPacket(recSSRC)` via `rtpSession.lock()` (videostream.h l.75).
- `TextStream::RecText` (l. 337) : idem audio.
- `RTPParticipant::Init` (l. 110) appelle `X.Init()` : la boucle `RecX` (démarrée par `StartReceiving`) devient l'unique pompe. **Vérifier (B6)** que pour WebRTC `StartReceiving` est appelé assez tôt pour amorcer ICE/DTLS ; sinon amorcer la pompe dès `Init`.

### 5.2 JSR309 — `RTPEndpoint`

`RTPEndpoint` hérite de `RTPSession` et possède déjà sa thread (`StartReceiving`→`run`→`Run`, l. 258-289). Le refactoring **fusionne naturellement** : `RTPEndpoint::Run` appelle `RTPSession::GetPacket()` qui, désormais, poll lui-même.
- Supprimer `msleep(200)` (l. 268).
- `RTPEndpoint::End`/`StopReceiving` : `receiving=false; RTPSession::CancelGetPacket()` (→ `Wake()`), puis `pthread_join(thread)` (thread **de l'endpoint**, pas celui de RTPSession qui n'existe plus). Retirer `pthread_kill(...,SIGIO)`.
- La thread propre de `RTPSession` disparaissant, `RTPEndpoint` n'a plus qu'une seule thread au lieu de deux.

### 5.3 `MediaBridgeSession`

`rtpVideo`/`rtpAudio` (l. 572/703) : mêmes boucles `GetPacket` + `CancelGetPacket`. `RTPSession` y est utilisé avec le listener brut du constructeur (pas de `weakListener`) — inchangé. Supprimer les `msleep` éventuels.

### 5.4 Sessions *sendonly* / RTCP (B3)

Pour une session qui n'a pas de consommateur actif (sendonly), plus personne n'appelle `Run()` → le RTCP entrant (RR, REMB) n'est pas lu. Décision par média :
- **Audio/texte** : impact faible, on tolère.
- **Vidéo sendonly** (rare ici, MCU mixe) : si nécessaire, servir le RTCP depuis la thread d'émission (un `Run(0)` non bloquant périodique) ou un timer léger partagé. À trancher lot 3.

# Plan d'implémetation

Découpage en lots livrables indépendamment, **build vert à chaque lot** (`./install.ksh localcompile`). Piège connu : le Makefile ne suit pas les headers → `rm mcu/media/build/debug/*.o` avant rebuild après modif de `.h`.

### Lot 0 — Filet de sécurité & instrumentation (préparation)
- Ajouter des logs de comptage de threads / de cycles `poll` sous `-DLOG_` pour comparer avant/après.
- Recenser les appelants de `GetPacket`/`CancelGetPacket`/`Start`/`Stop` (fait : audiostream, videostream, textstream, mediabridgesession, RTPEndpoint).
- **Critère** : aucun changement de comportement ; base de mesure établie.

### Lot 1 — `RTPBuffer` : extraction du déqueue non bloquant (B1)
- Ajouter `RTPPacket* GetDue(DWORD& msUntilDue)` dans `rtpbuffer.h`, extrait de la logique de `Wait()` (mêmes conditions d'échéance), **sans** condvar.
- Réécrire `Wait()` par-dessus `GetDue` (compat) ; ne rien changer aux appelants.
- **Critère** : build vert ; comportement threadé inchangé (le lecteur tourne encore).

### Lot 2 — `RTPSession` : self-pipe + `Run(timeoutMs)` + `GetPacket` bloquant (B1,B2,B4,B5,B7)
- Créer le self-pipe dans `Init` (`pipe2(O_NONBLOCK|O_CLOEXEC)`), `Wake()`, fermeture dans `End`.
- Transformer `Run()` → `int Run(int timeoutMs)` : `poll` sur 3 fds, **drain complet** (`recvfrom` en boucle), aiguillage, watchdog, RTCP. Retour `n>0 / 0 / -1`.
- Réécrire `GetPacket()`/`GetPacket(ssrc)` selon §4.3 (boucle `GetDue`→`Run`, **plus de `msleep`**).
- `CancelGetPacket[(ssrc)]` : `cancelPacket=true` + `Cancel()` + `Wake()`.
- Supprimer `thread`, `run()`, corps de `Start()`/`Stop()` liés au thread ; `Init` ne crée plus de thread ; `End`/`Stop` sans `join`.
- `ArmRTPTimeout` : remplacer `pthread_kill` par `Wake()`.
- Ajouter `recvMutex`.
- **Critère** : build vert ; un appel audio SIP simple lit sans `msleep`, thread lecteur disparu (vérif via logs Lot 0).

### Lot 3 — Consommateurs : suppression des `msleep` (B10) + RTCP sendonly (B3)
- `audiostream.cpp`, `videostream.cpp`, `textstream.cpp`, `mediabridgesession.cpp`, `jsr309/RTPEndpoint.cpp` : retirer les `msleep` post-`GetPacket`, retirer les `pthread_kill(...,SIGIO)` devenus inutiles.
- Trancher et implémenter le service RTCP sendonly (§5.4) si nécessaire.
- **Critère** : build vert ; appel audio+vidéo bidirectionnel OK (SIP puis WebRTC).

### Lot 4 — Amorçage ICE/DTLS (B6) — WebRTC
- Garantir le démarrage de la pompe de réception dès `Init`/setup pour les sessions DTLS (option (a) §4.5), ou pompe de handshake dédiée.
- **Critère** : *handshake* DTLS/ICE réussi sur un appel WebRTC réel ; média chiffré reçu.

### Lot 5 — Validation & nettoyage
- Tests manuels : appel SIP audio, appel SIP vidéo (réordonnancement/FPU), appel WebRTC (SRTP/NACK), conférence multi-participants, `StopReceiving`/`End` sans fuite ni blocage, watchdog `onRTPTimeout`.
- Mesure du nombre de threads avant/après (attendu : −1 thread par session RX).
- Retirer l'instrumentation Lot 0 ; mettre à jour `CLAUDE.md` (§ threads) et la mémoire.
- **Critère** : parité fonctionnelle, gain de threads mesuré, pas de régression latence/gigue.

## 6. Risques résiduels & points à valider
- **Réordonnancement vidéo** : valider empiriquement que `msUntilDue`+drain préserve la qualité par rapport au lecteur continu (mesurer paquets hors séquence non récupérés).
- **ICE/DTLS (B6)** : le point le plus incertain ; à prototyper tôt (Lot 4 peut être avancé si WebRTC prioritaire).
- **RTCP sendonly (B3)** : impact à mesurer sur les rapports (RR/REMB) et le contrôle de débit.
- **Invariant « 1 pompe par session »** : à documenter ; tout appelant multi-thread de `GetPacket` sur une même session est désormais interdit (sérialisé par `recvMutex`, mais un seul consommateur logique est attendu).

