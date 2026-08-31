# Réduire le nombre de threads autour de RTPSession

Conception n°2. Elle remplace `SPEC-n1-ecartee.md`, dans ce même répertoire, dont
l'approche — faire porter le `poll()` par `GetPacket()` — est jugée trop
risquée (§3.4 explique pourquoi, en une phrase par risque).

Objet : un **réacteur partagé** (`RtpSessionSet`) qui bat les sockets d'un
groupe de sessions RTP avec un seul `poll()` et un seul thread, puis la
suppression progressive des threads consommateurs.

---

## 1. Le problème, chiffré

### 1.1 Threads d'une jambe RTP, aujourd'hui

Chaque `RTPSession` dérive de `Worker` (`mcu/include/rtpsession.h:35`). Son
`Init()` ouvre les deux sockets puis appelle `Start()`
(`mcu/src/rtpsession.cpp:1923`), qui crée **un thread** portant la boucle
`poll()` (`mcu/src/rtpsession.cpp:3005`).

**Participant de conférence (`RTPParticipant`)**

| Thread | Nombre | Créé par |
|---|---|---|
| Boucle `poll()` de `RTPSession` | 4 | `Init()` de audio, video MAIN, video SLIDES, text |
| `recAudioThread` / `sendAudioThread` | 2 | `audiostream.cpp:230` / `:194` |
| `recVideoThread` / `sendVideoThread` | 4 | `videostream.cpp:302` / `:259`, ×2 flux |
| `RTPSmoother` (lisseur d'émission) | 2 | un par `VideoStream` (`videostream.h:108`) |
| `recTextThread` / `sendTextThread` | 2 | `textstream.cpp:339` / `:259` |
| **Total** | **14** | |

Les 4 sessions sont créées et `Init()`ées inconditionnellement
(`rtpparticipant.cpp:18-19` et `:111`), que la jambe serve ou non.

**Jambe JSR-309 (`Endpoint`)**

`Endpoint` construit 4 ports (`Endpoint.cpp:28-39`) et les `Init()` tous
(`Endpoint.cpp:49`) : 4 boucles `poll()` et 8 ports UDP liés, même si le
contrôleur ne négocie que l'audio. `StartReceiving()` ajoute un thread de
démultiplexage par jambe reçue (`RTPEndpoint.cpp:129`).

| Thread | Nombre |
|---|---|
| Boucle `poll()` de `RTPSession` | 4 |
| `MultiplexLoop` | 1 par jambe reçue, ≤ 4 |
| **Total par Endpoint** | **5 à 8** |

Un appel B2BUA a deux `Endpoint` : **10 à 16 threads** pour le seul transport
RTP.

### 1.2 Ce que ces threads coûtent vraiment

Soyons exacts, sans gonfler le chiffre.

- Un thread bloqué dans `poll()` **ne consomme pas de CPU**. Il coûte une
  tâche noyau et une pile (8 Mo d'espace d'adressage virtuel par défaut,
  quelques Ko résidents).
- Le coût CPU réel est le **réveil par paquet** : chaque datagramme réveille
  le thread de SA session, donc un changement de contexte. À 50 paquets/s par
  jambe et 4 jambes, cela fait 200 réveils/s par participant, là où un
  réacteur en ferait au plus autant mais sur un seul thread — et souvent
  moins, car un tour de `poll()` draine plusieurs sockets.
- Le coût le plus lourd n'est pas la machine, c'est **le cycle de vie** :
  chaque thread apporte son `join`, son drapeau d'arrêt, son ordre de
  démontage. Les commits `videostream: joindre le thread avant de le
  réaffecter`, `StopReceiving réveille le thread avant de détruire ce qu'il
  lit` et `mediabridgesession: fermer le dernier cas du motif de join
  conditionnel` sont tous des bugs de ce type.

### 1.3 La vraie attente active

`msleep` prend des **MICROsecondes**
(`third_party/fontventa/libmedikit/medkit/tools.h:118`). C'est le piège que
la conception n°1 a raté : elle lisait « 200 ms » et « 1 s » là où le code
dit 200 µs et 1 ms.

Conséquence : quand `GetPacket()` rend NULL, le consommateur ne dort pas, il
**tourne**.

| Site | Sommeil interne à `GetPacket` | Sommeil du consommateur | Cadence de boucle |
|---|---|---|---|
| `audiostream.cpp:340` | 100 µs (`rtpsession.cpp:3210`) | 200 µs | ≈ 3 300 tours/s |
| `videostream.cpp:796` | 100 µs (`rtpsession.cpp:3220`) | 1 000 µs | ≈ 900 tours/s |
| `textstream.cpp:459` | 100 µs | 200 µs | ≈ 3 300 tours/s |
| `RTPEndpoint.cpp:392` | 100 µs | 200 µs | ≈ 3 300 tours/s |

Quand cela arrive-t-il ? `RTPBuffer::Wait()` (`rtpbuffer.h`) bloque sur une
condition tant qu'il n'est pas annulé — **`GetPacket()` est donc déjà
bloquant**. Il rend NULL dans un seul cas courant : le flux n'existe pas
encore. Or `defaultStream` n'est créé qu'à l'arrivée du **premier paquet**
(`rtpsession.cpp:2917-2920`). Donc entre `StartReceiving()` et le premier
datagramme du pair — sonnerie, handshake ICE/DTLS, pair muet, pair jamais
venu — chaque jambe reçue tourne à ~3 kHz, soit ~7 000 appels `select()` par
seconde et par jambe.

C'est **le seul gaspillage CPU franc** de ce chemin, et il est ailleurs que là
où on le cherchait.

**Mesuré le 2026-08-31, appel JSR-309 B2BUA, quatre jambes reçues.** Le
compteur `RTPSession::CountEmptyGetPacket` donne, sur chacune des quatre jambes
et pendant deux secondes consécutives :

```
attente active : 2231 GetPacket a vide en 1000 ms [cx-15163]
attente active : 2279 GetPacket a vide en 1000 ms [cx-15163]
attente active : 2288 GetPacket a vide en 1000 ms [cx-15163-outbound]
attente active : 2262 GetPacket a vide en 1000 ms [cx-15163-outbound]
```

Soit **~2 250 appels à vide par seconde et par jambe, ~9 000 pour l'appel**,
donc ~18 000 `select()` par seconde. La fenêtre : de `EndpointStartReceiving`
au **premier paquet entrant**, ici 2 à 3 secondes. Pendant l'appel établi, le
compteur retombe à ~27/s.

Ce n'est donc pas un régime permanent mais un **coût par établissement
d'appel**, qui grandit avec le rythme des appels. Un appel MCU dont le pair
émet déjà n'a pas cette fenêtre : c'est pourquoi la même mesure côté MCU n'a
rien montré.

### 1.4 Correction à porter sur l'idée de départ

> « rendre `RTPSession::GetPacket()` bloquant avec une temporisation max pour
> éviter l'attente active des clients »

`GetPacket()` est déjà bloquant, sans temporisation. Ce qui manque n'est pas
le blocage, c'est :

1. une **borne** au blocage, pour que le consommateur relise son drapeau
   d'arrêt sans dépendre d'un `Cancel()` ;
2. un chemin **bloquant** quand le flux n'existe pas encore, au lieu du
   `msleep(100)` qui fait tourner la boucle.

---

## 2. Ce que la boucle `poll()` fait réellement

Avant de déplacer un thread, il faut savoir ce qu'il porte. `RTPSession::Run()`
(`rtpsession.cpp:3005-3204`) ne lit pas seulement du RTP. Un tour de boucle
fait **sept** choses :

| # | Travail | Cadence voulue |
|---|---|---|
| 1 | `ReadRTP()` : RTP, mais aussi STUN et DTLS, qui arrivent sur le même socket | à l'événement |
| 2 | `ReadRTCP()` : RR/SR/NACK/FIR/REMB/TMMBR/RPSI | à l'événement |
| 3 | `SendTransportWideFeedback()` : rapports d'arrivée transport-cc | ≤ 25 ms |
| 4 | `SendNATPrimingPacket()` : rafale d'amorçage NAT | 20 ms |
| 5 | `DriveICEChecks()` : binding requests sortants + backoff | 250 ms |
| 6 | `DriveDTLSClientHandshake()` : ClientHello + retransmissions | 250 ms |
| 7 | `onApplicationTick()` : bat les timers de la pile SCTP d'un data channel | `GetApplicationTickMs()` |
| 8 | Watchdog `onRTPTimeout` | 1 000 ms |

Deux propriétés rendent le regroupement possible, et il faut les nommer parce
qu'elles sont la clé de la faisabilité :

- **Chaque travail périodique porte sa propre horloge.** `DriveICEChecks`
  compare à `iceLastCheck` (`rtpsession.cpp:1471`), le DTLS délègue à
  `dtls.HandleTimeout()` (`:1386`), l'amorçage NAT compare à `natPrimingLast`,
  le tick applicatif à `lastAppTick`, le watchdog à `lastRecv`. Les appeler
  **plus souvent que nécessaire est donc inoffensif** : ils sortent tout seuls.
  C'est ce qui permet à un réacteur de rappeler toutes les sessions du groupe à
  chaque tour, sans réveiller de retransmission parasite.
- **Tout ce chemin est mono-thread par construction.** Le générateur
  transport-cc est écrit et lu « par le seul thread `Run`, donc sans verrou »
  (`rtpsession.h:760-762`), et l'objet SSL du DTLS n'est pas concurrent
  (`rtpsession.h:294-298`). Un réacteur **préserve** cet invariant : il y a
  toujours un seul thread par session. Il en partage juste un entre plusieurs.

### 2.1 Ce qui réveille la boucle depuis l'extérieur

Cinq sites signalent l'`eventfd` du `Wait` hérité, lu comme 3ᵉ `pollfd`
(`rtpsession.h:512`) :

| Site | Pourquoi |
|---|---|
| `SetRemoteSTUNCredentials` (`:715`) | des checks ICE deviennent émissibles |
| `ArmRTPTimeout` (`:1288`) | le watchdog veut une attente bornée |
| `SetDTLSApplicationListener` (`:1319`) | une cadence applicative apparaît |
| `RequestDTLSClientHandshake` (`:1362`) | il y a un ClientHello à pousser |
| `ArmNATPriming` (`:1636`) | il y a une rafale à cadencer |

Plus `WakeUp()` (`rtpsession.h:315`), appelé par le porteur de data channel
quand la pile SCTP a produit des datagrammes à chiffrer.

### 2.2 Deux consommateurs sur une même session

En mode partage de document BFCP, `VideoStream` SLIDES consomme la session de
MAIN, sur un autre SSRC (`rtpparticipant.cpp:613`). Deux threads appellent
donc `GetPacket()` sur **la même** `RTPSession`.

L'invariant « une seule pompe par session » que proposait la conception n°1
était donc déjà violé. Le réacteur le règle par construction : la pompe est le
réacteur, jamais le consommateur.

---

## 3. La cible : un réacteur par groupe

### 3.1 Principe

`RTPSession` cesse d'être une classe active. Elle devient un **handler
d'événements** : elle déclare ses descripteurs et sa prochaine échéance, et
on l'appelle quand quelque chose arrive.

`RtpSessionSet` devient la classe active : un thread, un `poll()`, N sessions.

```
        ┌──────────────── RtpSessionSet (1 thread) ──────────────┐
        │  poll( [rtp,rtcp]×N + eventfd , min(échéances) )       │
        │    ├─ session A : OnPollEvents() puis OnPeriodic()     │
        │    ├─ session B : OnPollEvents() puis OnPeriodic()     │
        │    └─ …                                                │
        └────────────────────────────────────────────────────────┘
```

### 3.2 L'interface de handler

```cpp
// mcu/include/pollhandler.h
class PollHandler
{
public:
	virtual ~PollHandler() = default;

	//Descripteurs à surveiller. Remplit `fds` (au plus `max`), rend le nombre
	//posé. Appelé à chaque reconstruction du tableau du réacteur, pas à chaque
	//tour : un handler qui change de fd doit demander la reconstruction.
	virtual int  GetPollFds(pollfd* fds, int max) = 0;

	//Dans combien de ms ce handler veut-il la main, même sans événement ?
	//-1 = jamais. Le réacteur prend le minimum sur tout le groupe.
	virtual int  GetNextTimeoutMs(QWORD nowUs) = 0;

	//Les `revents` des fds rendus par GetPollFds, dans le MÊME ordre.
	virtual void OnPollEvents(const pollfd* fds, int count, QWORD nowUs) = 0;

	//Travail périodique. Appelé à CHAQUE tour, après OnPollEvents, y compris
	//quand le réveil venait d'une autre session. Chaque travail garde sa propre
	//horloge (cf. §2), donc un appel en trop ne coûte que le test.
	virtual void OnPeriodic(QWORD nowUs) = 0;

	//Le transport est mort (POLLERR/POLLHUP/POLLNVAL). Le réacteur retire le
	//handler après cet appel ; il ne s'arrête pas lui-même.
	virtual void OnPollError(short revents) = 0;
};
```

Le passage de `RTPSession::Run()` à ces cinq méthodes est **mécanique** :
c'est le corps actuel de la boucle, coupé en deux au niveau du `poll()`.

- `GetPollFds` → `simSocket`, `simRtcpSocket`.
- `GetNextTimeoutMs` → le calcul de `waitMs` qui existe déjà
  (`rtpsession.cpp:3067-3095`), tel quel.
- `OnPollEvents` → les branches `ufds[0]`/`ufds[1]` : `lastRecv`,
  `rtpTimedOut=false`, `ReadRTP()`, `ReadRTCP()`.
- `OnPeriodic` → tout le bloc après les lectures : transport-cc, amorçage NAT,
  ICE, DTLS, tick applicatif, watchdog.
- `OnPollError` → le `break` final, qui devient un retrait du groupe.

`RTPSession` n'hérite plus de `Worker`. Effet de bord bienvenu : le piège
documenté en `RTPEndpoint.h:73-79` — une classe dérivée qui déclare un
`Run()` et masque silencieusement la boucle `poll()` — **disparaît**, faute de
`Run()` à masquer.

### 3.3 La boucle du réacteur

```cpp
class RtpSessionSet
{
public:
	//nom : sert aux traces et au recensement de threads
	explicit RtpSessionSet(const char* name);
	~RtpSessionSet();          //Stop() implicite, refuse le join sur soi

	int  Start();
	int  Stop();               //réveille, joint, ne touche à aucun handler

	//Inscription. À appeler APRÈS ouverture des sockets, AVANT tout trafic.
	void Add(PollHandler* h);
	//Retrait SYNCHRONE : au retour, le thread du réacteur ne touche plus `h`
	//et ne poll plus ses fds. L'appelant peut alors fermer ses sockets.
	//Appelé depuis le thread du réacteur, retire sans attendre.
	void Remove(PollHandler* h);

	//Réveil : à appeler quand un handler a du travail hors événement réseau.
	void Wake();

	bool IsReactorThread() const;

private:
	int Run();                 //la boucle
	void RebuildFds();         //sous `lock`
};
```

Un tour de boucle :

```
si (dirty) RebuildFds()                      // sous lock, copie locale
now = getTime()
timeout = min sur les handlers de GetNextTimeoutMs(now)   // -1 si tous -1
nready = poll(fds, nfds, timeout)
si nready<0 : EINTR/EAGAIN -> continue ; sinon trace + sortie
si eventfd lisible : wait.Drain()
now = getTime()
pour chaque handler h :
    h->OnPollEvents(&fds[h.offset], h.count, now)
    h->OnPeriodic(now)
    si un revents de h porte POLLERR|POLLHUP|POLLNVAL :
        h->OnPollError(...)  puis  marquer h pour retrait
traiter les retraits et les inscriptions en attente ; signaler les quiesce
```

Points fixés :

- **Le tableau de `pollfd` est reconstruit hors `poll()`**, jamais pendant.
  `Add`/`Remove` posent `dirty` et réveillent ; la reconstruction se fait au
  début du tour suivant.
- **Un handler ne fait jamais arrêter le réacteur.** Un socket mort retire une
  session, point. C'est un changement de comportement voulu : aujourd'hui un
  `POLLERR` fait sortir la boucle de la session, ce qui la rend sourde en
  silence.
- **Ordre `OnPollEvents` puis `OnPeriodic`**, comme aujourd'hui : les paquets
  du tour sont déjà entrés quand les timers tournent.
- **Le réacteur ne possède aucun handler.** Il ne les détruit pas, ne les
  garde pas en vie. C'est `Remove` qui porte le contrat.

### 3.4 Le point dur : retrait et fermeture des sockets

C'est le seul endroit où le réacteur est plus délicat qu'un thread par
session, et c'est là qu'il faut être précis.

Aujourd'hui, `RTPSession::End()` ferme les sockets après que `StopThread()` a
joint le thread : plus personne ne peut lire un fd fermé. Avec un réacteur
partagé, on ne peut pas joindre le thread (il sert d'autres sessions). Fermer
un fd que le réacteur a dans son tableau ouvre la course classique de
réutilisation de descripteur — celle-là même que documente
`rtpparticipant.cpp:99-101`.

Protocole retenu, un **quiesce** explicite :

```
Remove(h) appelé depuis un AUTRE thread :
    lock
    pending_remove.insert(h) ; dirty = true ; gen_wanted = ++generation
    Wake()
    attendre (condvar) que gen_done >= gen_wanted et h absent du tableau vivant
    unlock
    -> au retour, garanti : le réacteur ne poll plus les fds de h,
       et n'appellera plus aucune méthode de h.

Remove(h) appelé DEPUIS le thread du réacteur (via un callback) :
    retrait immédiat de la liste, pas d'attente (un self-join tuerait le
    processus — même garde-fou que Worker::StopThread).
```

`RTPSession::End()` devient :

```
group->Remove(this);      // bloque jusqu'au quiesce
close(simSocket); close(simRtcpSocket);
DeleteStreams(); ...
```

C'est le seul verrou nouveau du chantier, et il est court : le réacteur ne le
prend qu'entre deux `poll()`.

### 3.5 Pourquoi la conception n°1 était risquée, et celle-ci moins

La conception n°1 supprimait la pompe : plus rien ne lisait les sockets tant
qu'un consommateur ne réclamait pas un paquet. Trois conséquences, qu'elle
identifiait honnêtement sans les résoudre (ses blocages B1, B3, B6) :

| Risque de la n°1 | Statut ici |
|---|---|
| **B1** — le jitter buffer perd son avance de phase : plus personne ne remplit les trous pendant que le consommateur attend l'échéance | **disparaît** : la pompe tourne en continu, exactement comme aujourd'hui |
| **B3** — une session *sendonly* ne traite plus le RTCP entrant (RR, REMB, TMMBR) : le contrôle de débit devient sourd | **disparaît** : le RTCP est lu indépendamment de tout consommateur |
| **B6** — le handshake ICE/DTLS ne démarre pas avant le premier `StartReceiving` | **disparaît** : `Add()` au `Init()`, la pompe amorce comme aujourd'hui |

Le réacteur ne change **pas qui lit les sockets par rapport au jitter
buffer**. Il change seulement **combien de threads** portent ces lectures.
C'est tout l'écart de risque entre les deux conceptions.

Une bonne idée de la n°1 est **conservée** : le déqueue non bloquant
`RTPBuffer::GetDue(DWORD& msUntilDue)`. Elle sert ici à autre chose — dire au
réacteur dans combien de temps un paquet retenu deviendra livrable (§4.2).

### 3.6 Découpage des groupes : deux groupes par jambe — DÉCIDÉ

**Décision : deux réacteurs par jambe**, {audio, texte} et {vidéo MAIN,
vidéo SLIDES}. C'est l'option (a) ci-dessous.

L'idée de départ était « un groupe par participant / Endpoint ». Le bon axe,
à raffiner sur **un** point : quand la livraison passera en push (§4.2), le
travail du consommateur — démultiplexage, décodage, transcodage — tournera
**dans le thread du réacteur**. Tout ce qui est dans le même groupe attend
donc ce travail.

Mesure connue : un encodage VP8 720p coûte ~22 ms par image (fiche mémoire du
chantier transcodeur). À 25 im/s, c'est plus d'un demi-cœur, en blocs de
22 ms. Mettre l'audio dans le même groupe que la vidéo, c'est laisser le
socket audio non poll pendant 22 ms.

Trois découpages possibles :

| Option | Threads / Endpoint | Couplage |
|---|---|---|
| (a) un groupe par classe de média : {audio, texte} et {vidéo MAIN, vidéo SLIDES} | **2** | l'audio n'attend jamais la vidéo |
| (b) un groupe par Endpoint | 1 | l'audio attend le transcodage vidéo |
| (c) un pool de N réacteurs pour tout le serveur, sessions réparties par hachage | N (fixe) | des participants sans rapport se bloquent entre eux |

**Pourquoi (a).** Elle donne 2 threads par jambe au lieu de 5 à 8, sans
coupler un média temps réel serré (audio, 20 ms) à un média coûteux (vidéo).
(b) échangeait un thread contre un risque de gigue audio qu'on ne saurait pas
mesurer avant la recette. (c) n'a de sens qu'une fois le coût réel d'un tour
de réacteur mesuré, et elle sacrifie l'isolation entre appels — mauvais
échange pour un MCU. Elle reste consultable si le nombre de threads redevient
un problème à grande échelle, PAS avant.

Ce que (a) laisse comme couplage, et qui est **assumé** : la vidéo SLIDES est
dans le groupe de la vidéo MAIN, donc elle attend son transcodage. Le partage
de document est à quelques images par seconde.

**Repli obligatoire.** Une session qu'aucun propriétaire n'inscrit doit
marcher quand même — les tests unitaires en créent, `Broadcaster` aussi.
`RTPSession::Init()` s'inscrit alors dans un **groupe par défaut** du
processus, créé à la demande. Un seul chemin de code, pas deux : il n'existe
plus de session portant son propre thread. Contrainte associée : la livraison
en push n'est autorisée que pour une session inscrite dans un groupe
**explicite** ; le groupe par défaut reste en mode tiré (§4.1).

Ce groupe par défaut est aussi **l'étape de migration** : au lot 2, toutes les
sessions y sont, et le serveur entier tourne sur un seul thread de réacteur —
c'est sûr parce qu'aucun travail long n'y tourne encore. Le découpage en deux
groupes par jambe n'arrive qu'au lot 4a, juste avant que le push n'y mette du
décodage.

---

## 4. Le chemin de consommation

Le réacteur enlève les threads de pompe. Les threads consommateurs — 4 par
participant, 1 à 4 par Endpoint — demandent un second geste.

### 4.1 Étape A : borner l'attente et tuer l'attente active

Cible : `GetPacket` ne fait plus tourner personne, et le consommateur garde
son thread. Petit changement, gain immédiat, aucun déplacement de travail.

```cpp
//rtpbuffer.h : borne l'attente. 0 = non bloquant. Sémantique inchangée
//sinon (livraison en séquence, comblement de trou, cancel collant).
RTPPacket* Wait(DWORD timeoutMs);

//rtpsession.h
//timeoutMs borne l'attente. Rend NULL sur expiration, sur annulation, ou si
//le flux demandé n'existe pas encore — dans ce dernier cas APRÈS avoir
//attendu, jamais en tournant.
RTPPacket* GetPacket(DWORD& ssrc, DWORD timeoutMs);
```

Deux corrections dans `GetPacket` :

1. les `msleep(100)` internes (`rtpsession.cpp:3210`, `:3220`) disparaissent ;
2. le cas « pas de flux pour ce SSRC » attend `timeoutMs` sur une condition
   de la session — celle que `SetDefaultStream`/`AddStream` signalent — au
   lieu de rendre NULL tout de suite.

Chez les consommateurs : `GetPacket(ssrc, 200)` et suppression du `msleep`
qui suit. La boucle relit son drapeau toutes les 200 ms au pire ; les
`Cancel()` existants continuent de réveiller immédiatement.

Sites : `audiostream.cpp:336`, `videostream.cpp:790`, `textstream.cpp:453`,
`RTPEndpoint.cpp:387`.

### 4.2 Étape B : livraison poussée, le thread consommateur disparaît

Cible : le réacteur livre les paquets **échus** à un listener, dans son
thread. Plus de `recAudioThread`, `recVideoThread`, `recTextThread`, ni de
`MultiplexLoop`.

```cpp
//rtpsession.h
class MediaListener
{
public:
	virtual ~MediaListener() = default;
	//Appelé dans le thread du réacteur. Le listener prend la propriété du
	//paquet. Il ne doit ni bloquer sur un verrou tenu par un autre thread du
	//groupe, ni appeler End() sur sa propre session (cf. §3.4).
	virtual void onMediaPacket(RTPSession* session, std::unique_ptr<RTPPacket> packet) = 0;
};
//NULL = mode tiré (étape A). Non NULL = mode poussé.
void SetMediaListener(MediaListener* listener);
```

Le jitter buffer reste **entier** — c'est lui qui porte le réordonnancement et
la fenêtre de comblement. Il fournit au réacteur les deux réponses dont il a
besoin :

```cpp
//rtpbuffer.h — extraction non bloquante (idée reprise de la conception n°1)
//  tête échue         -> le paquet, msUntilDue = 0
//  tête retenue       -> NULL, msUntilDue = (arrivée + maxWaitTime) - maintenant
//  file vide/annulée  -> NULL, msUntilDue = 0
RTPPacket* GetDue(DWORD& msUntilDue);
```

Et le câblage dans la session :

- `RTPSession::GetNextTimeoutMs()` intègre le plus petit `msUntilDue` de ses
  flux. Le `poll()` du réacteur attend donc **juste** l'échéance du paquet
  retenu, pendant que les paquets manquants continuent d'arriver et de
  remplir les trous. C'est le comportement d'aujourd'hui, sans thread
  supplémentaire.
- `RTPSession::OnPeriodic()` boucle `GetDue()` sur chaque flux et pousse ce
  qui est échu vers le `MediaListener`.

Côté consommateurs, le corps de la boucle devient le corps du callback :

| Aujourd'hui | Devient |
|---|---|
| `AudioStream::RecAudio` | `AudioStream::onMediaPacket` |
| `VideoStream::RecVideo` | `VideoStream::onMediaPacket` |
| `TextStream::RecText` | `TextStream::onMediaPacket` |
| `RTPEndpoint::MultiplexLoop` | `RTPEndpoint::onMediaPacket` |

Chaque corps garde son état local (`lastSeq`, `lostCount`, `waitIntra`,
`lastFPURequest`, décodeur courant…) : il devient membre au lieu d'être
variable de pile. Transformation mécanique, mais à faire un site à la fois.

Le mode SLIDES/BFCP (§2.2) se règle naturellement : la session a deux flux et
pousse chacun à son listener, par SSRC. Plus de second thread lisant la
session d'un autre flux.

### 4.3 La tête de ligne, honnêtement

En push, le travail du consommateur bloque le `poll()` de son groupe. Deux
constats :

- **Pour la jambe concernée, rien ne change.** Aujourd'hui déjà, pendant que
  `MultiplexLoop` décode et encode, personne n'appelle `GetPacket` : les
  paquets s'accumulent dans le socket puis dans le jitter buffer. Le réacteur
  reproduit ce comportement à l'identique.
- **Le couplage NOUVEAU est entre jambes du même groupe**, et c'est
  exactement ce que le découpage en deux groupes par jambe (§3.6) borne.

Garde-fou à poser : le réacteur mesure la durée de chaque `OnPollEvents` +
`OnPeriodic` et **trace au-delà d'un seuil** (proposition : 50 ms), au plus
une trace par seconde et par groupe, en nommant la session. Sans cette trace,
une régression de gigue serait indiagnosticable.

### 4.4 Les lisseurs d'émission : HORS CHANTIER — DÉCIDÉ

`RTPSmoother::Run()` (`RTPSmoother.cpp:207`) est une boucle purement
temporelle : prendre un paquet de la file, attendre `nextSendUs`, émettre.
Un handler de réacteur ferait cela sans thread, et gagnerait 2 threads par
participant RTP. Même mécanique pour `RTPMultiplexerSmoother` côté JSR-309.

**On n'y touche pas.** Ce lisseur EST le pacer du contrôle de débit : c'est
lui qui décide du temps de passage de chaque paquet sur le fil, et c'est le
pacing de ce qu'on envoie qui fait la croyance de l'estimateur du pair. Le
chantier contrôle de débit a coûté cher à stabiliser (plafond fenêtré,
throttler TMMBR, RPSI) et vient d'être validé en recette. Le déplacer sur le
thread du réacteur, c'est mettre l'émission et la réception sur le même
thread : elles s'attendent, donc la cadence d'émission change — exactement la
variable qu'on ne veut plus faire bouger.

Le chemin d'émission reste donc entier : les threads de lissage restent, et
`SendPacket()` continue d'écrire depuis le thread appelant. À reconsidérer
seulement si le nombre de threads redevient le problème dominant, et alors
comme chantier séparé, avec sa propre séance de mesure BWE.

### 4.5 Gain attendu

Le nombre de threads ne baisse pas de façon monotone, et il faut le dire :
le lot 4a en **rajoute**. C'est voulu, et voici pourquoi.

Au lot 2, un seul réacteur pour tout le processus suffit : rien de long n'y
tourne — seulement lire un datagramme et l'empiler. C'est le plus gros gain
du chantier, pour le plus petit risque.

Au lot 4a, on découpe en groupes par jambe. On remonte donc à 2 threads par
jambe, non pour le plaisir, mais parce que le lot 4b va mettre du travail
long dans ces threads (décodage, transcodage) et qu'on refuse de coupler
l'audio d'un appel à la vidéo d'un autre.

Au lot 4c, les threads consommateurs disparaissent et la courbe redescend
pour de bon. C'est l'état final : les lisseurs d'émission gardent leur thread
(§4.4).

| | Aujourd'hui | Lot 2 : 1 réacteur global | Lot 4a : 2 réacteurs par jambe | Lot 4c : push (final) |
|---|---|---|---|---|
| `RTPParticipant`, 4 sessions | 14 | 10 | 12 | **8** |
| `Endpoint` JSR-309, 4 sessions dont 2 reçues | 6 | 2 | 4 | **2** |
| Appel B2BUA, 2 `Endpoint` | 12 | 4 | 8 | **4** |
| Conférence, 10 participants RTP | 140 | 100 | 120 | **80** |

La colonne « lot 2 » compte en plus **un** thread pour tout le processus. Les
deux dernières comptent 2 threads de réacteur par jambe (§3.6).

Vue autrement : un appel B2BUA transcodé passe de **12 threads à 4**, et une
conférence à 10 participants de **140 à 80**. Sur les 8 threads restants d'un
`RTPParticipant`, 4 sont des threads d'émission et 2 des lisseurs — c'est-à-dire
tout le chemin d'émission, laissé intact à dessein.

## 5. C++17, coroutines : ce qui s'applique vraiment

Question posée : les mécanismes récents du langage aident-ils ici ?

### 5.1 Les coroutines ne sont pas du C++17

Elles sont arrivées en **C++20**. Le projet compile en `-std=gnu++17`
(`mcu/Makefile:28`). GCC 11.5 les accepte, mais sous `-std=gnu++20`, ce qui est
une migration à part entière sur une base mêlant `.c` et `.cpp`, fichiers CRLF
et code de vingt ans.

### 5.2 Et même en C++20, elles ne suppriment pas un thread

C'est le point important. Une coroutine est une **façon d'écrire** du code qui
attend : elle transforme une machine à états en fonction linéaire. Elle
n'exécute rien par elle-même. Il faut toujours quelqu'un pour :

- attendre les événements réseau — un `poll()` ;
- reprendre les coroutines dont l'événement est arrivé — un ordonnanceur.

Autrement dit : **une conception par coroutines a besoin d'un réacteur**. Le
réacteur du §3 n'est pas une alternative aux coroutines, c'en est le
prérequis. Il donne 100 % du gain de threads ; les coroutines n'ajouteraient
que du confort d'écriture.

Et elles ajouteraient un risque propre : une coroutine suspendue est un
`frame` alloué qui capture des références. Un `co_await` sur une session
détruite est un use-after-free, dans un cadre où ni `shared_ptr` ni `Use`
n'aident — ce dépôt vient d'en corriger plusieurs (voir la fiche mémoire du
crash `VideoStream/SetFrameRate`).

**Recommandation : ne pas introduire de coroutines dans ce chantier.** La
conception du §3 laisse la porte ouverte : un handler dont `OnPollEvents` est
la reprise d'une coroutine s'y branche sans rien changer au réacteur. Si le
confort d'écriture devient un besoin, c'est un chantier séparé, après une
migration `gnu++20` elle aussi séparée.

### 5.3 Ce que C++17 apporte, et qui sert ici

| Outil | Usage dans ce chantier |
|---|---|
| `std::scoped_lock` | verrou du réacteur, sans risque d'ordre |
| structured bindings | parcours des maps de flux et de handlers |
| `std::optional` | « pas de prochaine échéance » sans sentinelle `-1` |
| `std::clamp` | calcul du timeout de `poll` |
| `[[nodiscard]]` | sur `Remove()`, dont ignorer le retour serait un bug |

Rien de tout cela ne change l'architecture. C'est de la propreté locale.

Ce que C++17 **n'apporte pas** : aucune primitive d'entrée/sortie asynchrone.
La Networking TS a été abandonnée, et les exécuteurs (`std::execution`) sont
arrivés en C++26. Il n'existe donc rien dans la bibliothèque standard qui
remplace `poll()` — le constat est le même que celui du chantier IPv6 sur
`boost::asio::ip::address`.

### 5.4 Ce que C++20 apporterait, si la migration a lieu un jour

Utile, indépendamment des coroutines :

- `std::jthread` + `std::stop_token` : remplacerait exactement le contrat
  « drapeau + `Wait::Cancel` + `join` » de `worker.h`.
- `std::atomic<T>::wait/notify` : un `Wait` plus léger, sans mutex.
- `std::latch` : dirait le quiesce du §3.4 en trois lignes.
- `std::span` : les parseurs RTP/RTCP, qui manipulent `(BYTE*, DWORD)` partout.

À évaluer comme chantier propre, pas ici.

---

## 6. Ce que ce chantier ne fait pas

- **Le chemin d'émission n'est pas touché du tout.** `SendPacket()` continue
  d'écrire sur le socket depuis le thread appelant, et `RTPSmoother` /
  `RTPMultiplexerSmoother` gardent leur thread. Décision explicite : ce sont
  les pacers du contrôle de débit (§4.4).
- **Le jitter buffer n'est pas retouché** dans sa logique : mêmes conditions
  de livraison, mêmes délais. On lui ajoute deux entrées non bloquantes.
- **Les threads d'encodage et de mixage restent.** Ce sont d'autres chantiers
  (transcodeurs sans thread, mixeurs).
- **Les jambes inutiles restent créées.** Que `Endpoint` ouvre 4 sessions et
  8 ports UDP pour un appel audio est un vrai défaut, mais c'est un chantier
  de cycle de vie, pas de threads. À noter et à traiter à part : avec le
  réacteur, il ne coûte plus de thread, seulement des ports.

---

## 7. Plan d'implémentation

Build vert à chaque lot (`./install.ksh localcompile`) et `cd mcu && make
check` vert à chaque lot.

### Lot 0 — Mesurer avant — code FAIT, mesure live à faire

1. Recensement de référence avec `mcu/tests/tools/thread_census.sh <pid> 60`,
   sur trois scénarios : serveur au repos, un appel JSR-309 audio+vidéo, une
   conférence à 3 participants RTP. Noter les noms de threads, leur nombre,
   le CPU. **À jouer par l'opérateur** : aucun test ne le remplace.
2. Compteur d'attente active : `RTPSession::CountEmptyGetPacket()` compte les
   `GetPacket` rendus à vide et les trace à 1 Hz (`attente active : N GetPacket
   a vide en 1 s`). Placé dans la session, pas dans les quatre consommateurs :
   ils passent tous par là. À retirer au lot 6.
3. `docs/maintenance/recette-reacteur-rtp.md` : la recette, lot par lot, avec
   les nombres de threads attendus et les traces d'alerte à guetter.

**Critère** : aucun changement de comportement, base de comparaison écrite.

### Lot 1 — `RtpSessionSet` et `PollHandler`, sans appelant — FAIT

- `mcu/include/pollhandler.h`, `mcu/include/rtpsessionset.h` +
  `mcu/src/rtpsessionset.cpp`. `RtpSessionSet` dérive de `Worker`.
- Tests (`tests/test_rtpsessionset.cpp`), avec des handlers factices sur
  tube :
  - N handlers, un seul thread, chacun reçoit ses propres événements ;
  - `GetNextTimeoutMs` : le réacteur prend bien le minimum ;
  - `OnPeriodic` est appelé même quand le réveil vient d'un autre handler ;
  - `Add`/`Remove` pendant que le trafic coule, sans perte pour les autres ;
  - **`Remove` est synchrone** : au retour, plus aucun appel au handler
    (le test le prouve en fermant le fd juste après et en vérifiant l'absence
    de `POLLNVAL` observé) ;
  - `Remove` appelé depuis un callback ne bloque pas ;
  - `POLLERR` sur un handler retire ce handler et **laisse vivre** les autres ;
  - `Wake()` réveille un `poll()` d'attente infinie.

**Critère** : la classe est prouvée seule, aucun code de production ne
l'utilise encore. — 10 tests `RtpSessionSet*` verts, suite complète 631 PASS.

Deux écarts par rapport au §3.3, décidés à l'écriture :

- **`Add`/`Remove` ne posent pas de `dirty`.** Le tableau de `pollfd` et
  l'instantané sont reconstruits à **chaque** tour. C'est O(handlers) par tour,
  ce qu'on paie déjà pour `GetNextTimeoutMs`, et cela supprime tout un état à
  tenir cohérent : un handler qui change de descripteur n'a rien à signaler.
- **`OnPollEvents` n'est appelé que si le handler a au moins un `revents`.**
  Avec beaucoup de jambes, un tour sans événement ne doit pas coûter N appels
  à vide. Le travail « à chaque tour » est donc entièrement dans `OnPeriodic`,
  ce qui rend la frontière entre les deux méthodes plus nette.

Trois pièges rencontrés, qui valent d'être connus avant le lot 2 :

- **L'`eventfd` du `Wait` est créé paresseusement** par `GetPollFd()`. Le
  réacteur l'obtient donc dans son constructeur : sinon le réveil dépendrait de
  qui, du thread du réacteur ou d'un appelant de `Wake()`, y touche en premier.
- **`Worker::StartThread()` appelle `wait.Reset()`, qui purge l'`eventfd`.** Un
  `Wake()` émis avant `Start()` est donc perdu. Sans conséquence — l'instantané
  du premier tour prend les handlers déjà inscrits — mais à ne pas confondre
  avec un défaut de réveil.
- **Un `poll()` sans échéance ne produit aucun `OnPeriodic`.** C'est la
  sémantique d'aujourd'hui (`waitMs = -1` quand rien n'est armé), et les tests
  la figent : le réacteur DORT, il ne tourne pas à vide.

### Lot 2 — `RTPSession` devient un handler — FAIT, recette MCU PASSÉE

- `RTPSession` implémente `PollHandler` et n'hérite plus de `Worker`.
- Découpe mécanique de `Run()` en `OnPollEvents` / `OnPeriodic` /
  `GetNextTimeoutMs` / `OnPollError` (§3.2).
- Les 5 `wait.Signal()` (§2.1) et `WakeUp()` deviennent `group->Wake()`.
- `Init()` : `group->Add(this)` au lieu de `Start()`. `End()` :
  `group->Remove(this)` **avant** la fermeture des sockets.
- Groupe par défaut du processus, créé à la demande, pour toute session dont
  personne n'a fixé le groupe.
- `SetPollGroup(RtpSessionSet*)`, à appeler **avant** `Init()`.

**Critère** : suite `make check` verte, y compris `test_rtp_*`,
`test_datachannel`, `test_sctp_loopback`, `test_endpoint_teardown`,
`test_rtp_stream_race`, `test_ipv6`. Recensement de threads : une seule
boucle `poll` pour tout le processus, aucune régression fonctionnelle.
Recette : appel SIP audio, appel WebRTC (ICE + DTLS + SRTP), data channel
T.140.

Tests ajoutés (`tests/test_rtp_reactor.cpp`) : six jambes de plus n'ajoutent
**aucun** thread (mesure dans `/proc/self/task` — c'est LE test du lot),
inscription à `Init` et retrait à `End` comme au destructeur, groupe explicite
respecté, changement de groupe après `Init` refusé, et le réacteur **lit
vraiment** le RTP entrant sans qu'aucun consommateur ne l'ait réclamé.

Ce que le découpage a changé, et qui n'était pas prévu :

- **`RTPSession` n'a plus de `Run()` virtuel**, donc le piège documenté dans
  `RTPEndpoint.h` — une dérivée qui déclare un `Run()` et masque la boucle
  `poll` — n'existe plus. Le commentaire qui le décrivait a été retiré : il
  était devenu faux.
- **Trois prédicats extraits** (`IsDrivingDTLSClient`, `IsDrivingICEChecks`,
  `IsNATPriming`), parce qu'ils servent deux fois par tour : borner l'attente,
  puis décider du travail. La boucle d'avant les calculait une fois et gardait
  la valeur ; les recalculer est sûr, les fonctions `Drive*` revérifiant leurs
  propres préconditions (§2).
- **`GetNextTimeoutMs` doit ignorer une cadence applicative nulle.** Un
  `GetApplicationTickMs()` à 0 signifie « pas de cadence » ; le passer au
  calcul du minimum demanderait une attente de 0 ms, donc un cœur à 100 %.
- **Un descripteur mort détecté en lecture (`ENOTCONN`) retire la session du
  groupe** au lieu de seulement baisser `running`. Le retrait est réentrant,
  donc immédiat : appelé depuis le réacteur, il n'attend pas.

**Séance de recette du 2026-08-31, API MCU, un participant navigateur.** Le
journal donne les quatre preuves attendues :

| Observation | Chiffre |
|---|---|
| `>Run RTPSession` (ancienne boucle par session) | **0** |
| `>RtpSessionSet [default]` | **1**, pour 7 `Init` de session |
| Traces d'alerte (`descripteur invalide`, `handler encore inscrit`, `tour long`, `poll error`) | **aucune** |
| `DeleteParticipant` | ok en **3 ms** |

Deux choses que le scénario a exercées sans qu'on les demande :

- **`Rebind` trois fois** (audio, vidéo, texte reliés sur l'adresse du profil
  `publicv4`, nouveaux ports locaux). C'est la séquence la plus risquée du lot —
  `End` → retrait → `close` → `Init` → inscription — jouée trois fois, propre.
- **Le démontage complet d'un participant** sous le retrait synchrone, sans
  blocage ni trace.

Threads relevés : **14** au repos, **34** en appel, **25** après raccroché. Les
25 ne sont pas une fuite : `/status/general` rend
`conferences: 1, participants: 0` — la conférence survit au départ de son
participant, et ces 11 threads sont ses mixeurs et ses encodeurs.

**Le gain n'est pas mesuré, il est calculé.** La mesure de référence du lot 0
sur `master` n'a pas été prise, donc il n'existe pas de avant/après. À la
lecture du code, les 34 threads en appel auraient été 37 (quatre boucles `poll`
au lieu d'un réacteur) : trois threads gagnés pour un participant. Ce qui EST
directement observé, et qui vaut mieux qu'une soustraction : sept sessions
initialisées, **aucune** boucle `poll` par session, **un** réacteur.

**Séance de recette du 2026-08-31, API JSR-309, appel B2BUA de 32 s.** C'est
elle qui donne le chiffre que le cas MCU ne pouvait pas donner.

| Observation | Chiffre |
|---|---|
| `>Run RTPSession` | **0** |
| `>RtpSessionSet [default]` | **1**, pour **10** `Init` de session |
| Traces d'alerte | **aucune** |
| Threads : repos / en appel / après `MediaSessionDelete` | **13 / 19 / 14** |

**Le gain est mesuré : −7 threads.** Huit sessions vivent pendant l'appel (deux
`Endpoint` × quatre ports, plus deux `Rebind`). Avant ce lot, chacune portait sa
boucle `poll` : l'appel aurait compté 26 threads là où il en compte 19. L'écart
dépasse largement le bruit de ±2 du pool Abyss, contrairement au cas MCU à un
participant.

**Le démontage est complet**, et c'est ce que le cas MCU ne montrait pas : après
deux `EndpointDelete` puis `MediaSessionDelete`, le compte retombe à 14 — le
repos, plus l'unique thread du réacteur par défaut, qui est permanent par
construction (§3.6).

**Séance BWE du 2026-08-31, appel JSR-309 de 20,4 min.** Jouée, dépouillée par
`bwe_report.py`. Zéro trace d'alerte du réacteur, et l'estimateur de
**réception** — celui que le changement de thread touche — passe ses **5
critères** sur 20,3 min : aucun NaN, plus longue plage hors `Normal` 1,0 s
(limite 30 s), aucun écrêtage au plafond, covariance saine.

Deux critères en échec sur l'estimateur d'**émission**, tous deux imputables au
scénario et non au chantier :

| Échec | Cause lue dans le journal |
|---|---|
| 526,6 s au plafond (30 000 kb/s) | aucune congestion — 5 418 `usage=Normal` pour **1** `OverUsing`, lien non dégradé, pas de `netem`. L'estimateur monte librement, et 30 000 est le défaut de l'OUTIL, pas un plafond du serveur |
| 680,2 s au plancher (16 kb/s) | `target=0 delay=0 acked=0 lost=0 sent=0` : aucune entrée. transport-cc n'était activé que sur l'AUTRE patte, et §15 de `RATE-CONTROL.md` dit que sans lui l'estimateur d'émission ne produit rien |

**Et l'appel n'était pas transcodé** : `-Init VideoTranscoder [… bridging:1]`
puis `-VideoTranscoder: switched to bridged mode for codec VP8` sur les deux
pattes, VP8 des deux côtés. Donc aucun encodeur dans le chemin vidéo — ce qui
explique aussi les 0 `Got Intra` — et la cible de l'estimateur d'émission
n'atteint rien.

Comptages sur 20,4 min : **2** TMMBR émis (2 500 000 puis 2 350 536), 2 257
REMB émis, 5 TMMBN reçus, 2 385 `onTargetBitrateRequested`. Aucune trace de
TMMBR bruités.

**Ce qui reste ouvert sur R8** : le pair n'a émis **ni TMMBR ni REMB** vers
nous. La réception de la limite du pair — le chemin précis qui a changé de
thread — n'a donc pas été exercée. Pour la clore il faut trois conditions
réunies : un appel réellement transcodé (codecs différents des deux côtés, donc
un encodeur), une dégradation appliquée (`netem_scenario.sh`), et un pair qui
annonce sa limite (Linphone en `ccm tmmbr`).

**Un point à mesurer, pas à optimiser d'avance.** `OnPollEvents` lit **un**
datagramme par tour, exactement comme la boucle d'avant (le `poll` est à
déclenchement par niveau : s'il en reste, le tour suivant revient aussitôt).
Mais un tour coûte maintenant O(N) — `GetPollFds` + `GetNextTimeoutMs` +
`OnPeriodic` pour chaque session du groupe. Avec le réacteur unique du lot 2,
N est le nombre de sessions du serveur entier : l'ordre de grandeur est ~20 µs
par tour à 80 sessions, soit quelques pourcents d'un cœur à pleine charge.
C'est acceptable, et le lot 4a le fait disparaître en ramenant N à 2. Si la
mesure du §4.3 dit le contraire, la parade est de **drainer** chaque socket
prêt jusqu'à `EWOULDBLOCK` — à ne faire que sur mesure, parce que cela crée de
la tête de ligne à l'intérieur du groupe.

### Lot 3 — Attente bornée, fin de l'attente active

- `RTPBuffer::Wait(DWORD timeoutMs)`, `RTPSession::GetPacket(ssrc, timeoutMs)`
  (§4.1), attente sur condition quand le flux n'existe pas encore.
- Les 4 consommateurs : `GetPacket(ssrc, 200)` et suppression du `msleep`.
- Test : « pendant la sonnerie, une jambe reçue ne consomme pas de CPU » —
  compteur de tours de boucle borné.

**Critère** : le compteur du lot 0 point 2 tombe de ~3 300/s à ~5/s. Aucune
latence ajoutée sur un appel réel.

### Lot 4 — Groupes explicites, puis livraison poussée

Ordre imposé : le regroupement d'abord, le push ensuite. Chacun est
observable seul.

1. **4a — regroupement.** `RTPParticipant` et `Endpoint` créent leurs groupes
   selon le découpage du §3.6 — {audio, texte} et {vidéo MAIN, SLIDES} — et
   les posent avant `Init()`. Instrumenter la durée des tours (§4.3).
2. **4b — push.** `MediaListener`, `RTPBuffer::GetDue`, intégration du
   `msUntilDue` dans `GetNextTimeoutMs`. Basculer **un** site à la fois, dans
   cet ordre : `TextStream` (le plus simple, débit faible), `AudioStream`,
   `RTPEndpoint`, `VideoStream`. Build + recette entre chaque.
3. **4c — retrait du mode tiré.** Une fois les 4 sites basculés, supprimer
   `GetPacket`/`CancelGetPacket` du chemin de réception, et l'attente bornée
   du lot 3 avec. Il ne doit rester **qu'un** motif de livraison.

**Critère** de 4c : plus aucun `recXThread` ni `MultiplexLoop`. Recensement
conforme au tableau du §4.5. Recette : conférence 3 participants, partage de
document BFCP (les deux flux vidéo d'une même session), enregistrement MP4,
appel B2BUA transcodé 20 min sans dérive de gigue ni trame clé parasite.

### Lot 5 — Nettoyage et documentation

- Retrait de l'instrumentation du lot 0 devenue inutile ; **garder** la trace
  de tour long du §4.3, c'est un garde-fou permanent.
- `docs/reference/threads-rtp.md` : le modèle de threads du transport RTP,
  les groupes, le contrat de `Remove`, l'interdit « ne pas bloquer dans un
  callback de réacteur ».
- `CLAUDE.md` : le piège à connaître, et lui seul — « toute session RTP est
  battue par un `RtpSessionSet` ; un callback qui bloque bloque tout son
  groupe ».
- Mise à jour de la fiche mémoire du chantier.
- `SPEC-n1-ecartee.md` : la supprimer. Une fois le chantier fait, garder deux
  conceptions concurrentes du même sujet est un piège pour le prochain
  lecteur. Elle ne survit que le temps du chantier, pour dire ce qu'on a
  écarté et pourquoi.

---

## 8. Risques et arbitrages

| # | Risque | Gravité | Traitement |
|---|---|---|---|
| R1 | Course de fermeture de socket au retrait d'une session | **Haute** | quiesce synchrone du §3.4, prouvé par un test dédié au lot 1 |
| R2 | Tête de ligne entre jambes d'un même groupe | **Haute** | deux groupes par jambe (§3.6) + trace de tour long (§4.3) |
| R3 | Réentrance : un callback appelle `End()` sur sa propre session, ou attend un verrou tenu par un autre thread du groupe | **Haute** | `IsReactorThread()` + `Remove` non bloquant depuis le réacteur ; contrat écrit dans `docs/reference/threads-rtp.md` ; à vérifier sur chaque site du lot 4b |
| R4 | `OnPeriodic` appelé plus souvent qu'aujourd'hui déclenche une retransmission parasite | Basse | vérifié : chaque travail garde son horloge (§2). À re-vérifier à chaque ajout de travail périodique |
| R5 | Le groupe par défaut réunit des sessions sans rapport | Basse | il reste en mode tiré, donc aucun travail long n'y tourne |
| R6 | Un `POLLERR` ne fait plus taire la session mais la retire du groupe | Basse | changement de comportement **voulu** ; à tracer explicitement |
| R7 | Régression de gigue invisible en test, visible en appel | Moyenne | recette live obligatoire aux lots 2 et 4b ; séance de mesure BWE |
| R8 | Le contrôle de débit dérive alors qu'on n'a pas touché à l'émission | Moyenne | l'émission est hors chantier (§4.4), mais la RÉCEPTION du RTCP change de thread : faire une séance de mesure BWE au lot 2 ET au lot 4b, pas seulement à la fin |

### Décisions prises

- **Découpage des groupes** : deux réacteurs par jambe, {audio, texte} et
  {vidéo MAIN, SLIDES} (§3.6).
- **Lisseurs d'émission** : hors chantier, on ne touche pas au pacer du
  contrôle de débit (§4.4).
- **Emplacement** : `docs/conception/RTP-REACTOR/`, clé `RTP-REACTOR`.

### Reste à trancher

Rien. Le chantier peut démarrer au lot 0.
