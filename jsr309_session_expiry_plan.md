# Expiration des sessions JSR309 et des conférences MCU par inactivité — conception

> **RÉVISION 2026-08-11 — nouvelle direction retenue : solution (c), « vitalité
> par event queue »** (idée mainteneur), décrite au §7. Elle rend inutiles le
> ping applicatif et l'armement opt-in de la solution (a) ci-dessous, qui reste
> documentée comme historique de conception (sa mécanique de balayeur, §3.3,
> est reprise telle quelle par la solution (c)).
>
> **IMPLÉMENTÉE le 2026-08-11** (branche `fix/c++-renovation`) : arbitrage des
> questions ouvertes du §7.4 rendu par le mainteneur — expiration **active par
> défaut à 60 s**, réglable par `--event-queue-expires <s>` (0 = désactivée),
> et **solution (a) NON retenue** (aucune méthode XML-RPC ni type d'événement
> ajouté, donc contrat de fil `/jsr309` inchangé et rien à répercuter dans les
> protobuf MOTELI v2). État de livraison détaillé au §7.6.
>
> **ÉTENDUE À L'API MCU le 2026-08-11** (même session) : la politique est
> factorisée dans `EventQueueSweeper` (`mcu/include/eventqueuesweeper.h`), dont
> `JSR309Manager` (ses `MediaSession`) ET `MCU` (ses conférences) héritent. Deux
> précisions du mainteneur à cette occasion :
>
> 1. **`EventQueueDelete` ARME le délai de grâce, il ne détruit plus rien
>    immédiatement** — « pour laisser une chance à la reconnexion ». Cela révise
>    la « cascade immédiate » esquissée au §7.3 et livrée le matin même, et
>    comble du même coup le trou du `queueId` pendouillant.
> 2. **La portée du nettoyage est la responsabilité du contrôleur** : c'est à
>    lui d'assumer les conséquences de son découpage de files. kelixip passera à
>    une file par conférence ; mcuGold n'est plus utilisé.

## 7. Solution (c) — vitalité par event queue (direction retenue, 2026-08-11)

### 7.1 L'idée

Associer la durée de vie d'une session à celle de son event queue : le
long-poll du client sur `/events/jsr309/<queueId>` est un **heartbeat gratuit**
(elixip se reconnecte en ≤ 1 s après une coupure, et le serveur émet un
keep-alive toutes les ~30 s). Si plus personne ne polle la queue d'une session
pendant **60 s**, le client est mort → grand nettoyage : destruction des
sessions liées à la queue, puis de la queue.

Avantages sur la solution (a) : zéro changement côté elixip (pas de
`MediaSessionPing`), zéro RPC de plus, pas de faux positif sur les conférences
« calmes » côté contrôle (le poller, lui, est toujours là), pas d'armement
opt-in à oublier.

### 7.2 Faits vérifiés (code elixip + mcu, 2026-08-11)

- **elixip ouvre bien une queue PAR session** (voie mendooze/JSR309) : chaque
  scénario d'appel fait `media_connect()` → un GenServer `MediaServer.Mendooze`
  PAR APPEL → `EventQueueCreate` à l'init (une queue par connexion=par appel) →
  `MediaSessionCreate(sess_tag, queue_id)` avec CETTE queue
  (`MediaServerMendoozeConn.ex:344`). kelixip choisit même le MCU du pool par
  appel (`mediaserver_instance`).
- **L'association existe DÉJÀ côté mcu** : `MediaSessionEntry.queueId`
  (`JSR309Manager.h:65`, posé par `CreateMediaSession(tag,queueId)`,
  `JSR309Manager.cpp:143`). Tous les événements de la session partent vers
  cette queue (`DeliverEvent` → `eventMngr->AddEvent(entry.queueId, event)`).
  L'association est UNIDIRECTIONNELLE (la queue ne connaît pas ses sessions —
  le balayeur fera le scan inverse sur la map sessions) et NON VALIDÉE
  (`MediaSessionCreate` ne vérifie pas que la queue existe).
- **La présence d'un poller est déjà observable** : le handler long-poll
  (`XmlStreamingHandler::ProcessRequest`) fait `queue->IncUse()/DecUse()`
  autour de sa boucle — il suffit d'horodater le dernier détachement.
- **Queues NON associées à une session — c'est normal, trois familles** :
  1. les sondes keepalive du pool elixip (`Kelix.MediaPool`, purpose
     `:health_check`) : à chaque cycle, `EventQueueCreate` → poll bref →
     `EventQueueDelete` → déconnexion, JAMAIS de session (elles sondent la
     chaîne XML-RPC + long-poll de bout en bout) ;
  2. fenêtre transitoire de tout appel normal : la queue naît à `connect()`,
     la session au premier usage média — une queue d'appel est « nue » entre
     les deux (et le reste si l'appel échoue avant le média) ;
  3. pathologiques : session supprimée sans sa queue (queue orpheline jusqu'à
     l'`EventQueueDelete`), ou queue supprimée avant la session (queueId
     pendouillant : les événements partent dans le vide — et FUITENT,
     cf. §3.3 : `AddEvent` inconnu retourne 0 sans détruire l'événement,
     défaut préexistant à corriger).

### 7.3 Mécanique (esquisse)

- `XmlEventQueue` : horodater (`steady_clock`) le dernier détachement de
  poller (DecUse du handler) ; « pollée » = IncUse actif OU détachement récent.
- Balayeur périodique (10 s — reprendre la mécanique §3.3, aujourd'hui en
  `Worker`) : toute queue **sans poller depuis > 60 s** → détruire les
  sessions dont `entry.queueId` pointe dessus (extraction sous lock,
  `End()` hors lock, exactement §3.3) puis la queue elle-même. Couvre du même
  coup les queues nues abandonnées (sondes crashées, appels avortés).
- `DeleteEventQueue` explicite (chemin propre d'elixip) : déclencher le MÊME
  nettoyage immédiatement (c'est la « cascade EventQueueDelete » du §6).
- La reconnexion du poller annule le compte à rebours d'elle-même (re-IncUse).
  Cohérence des seuils : retry elixip 1 s, décrochage elixip 90 s, keep-alive
  serveur 30 s → 60 s serveur = deux keep-alives manqués, aucun faux positif.
- L'événement 7 (`MediaSessionExpiredEvent`, §2.1) devient à peu près inutile
  (la queue est morte, personne ne le lirait) — log serveur seulement.

### 7.4 Points arbitrés (2026-08-11, mainteneur)

- Contrat pour les clients à queue PARTAGÉE (jsr309impl Java : UNE queue pour
  toutes ses sessions) : 60 s sans poll y emporte TOUT — correct si le client
  est mort. **Documenté** comme changement de contrat (`xmlrpc_jsr309_api.md`
  §5 « Le long-poll est la preuve de vie du client », `readme.md`).
- Le 60 s : **configurable**, `--event-queue-expires <s>`, 0 = désactivé,
  **actif par défaut** (`XmlEventQueue::DefaultExpiresSecs`).
- Armement (a) en complément : **NON retenu** — pas de `MediaSessionPing`, pas
  de `MediaSessionSetTimeout`, pas d'événement d'expiration. Un déploiement
  sans long-poll permanent doit désarmer le mécanisme (`0`).
- **`EventQueueDelete` explicite : ARMEMENT du délai, pas destruction** (2e
  arbitrage du 2026-08-11) — les objets rattachés à une file détruite ne partent
  qu'à l'échéance du délai de grâce, pour laisser une chance à la reconnexion.
  C'est le « signal 2 » de `eventqueuesweeper.h`, et il couvre AUSSI le cas d'un
  `queueId` > 0 qui n'a jamais désigné de file.
- **Extension à l'API MCU : oui, même mécanisme, même option, même délai.** La
  portée (une conférence, ou toutes celles d'une file partagée) est la
  responsabilité du contrôleur, qui choisit son découpage de files ; `MCU-API.md`
  §5 et §7.1 recommandent désormais une file par conférence. Un `queueId` ≤ 0
  (défaut du transport MOTELI) reste hors d'atteinte du balayeur.

### 7.5 « Plan walking dead » — équivalent pour Medooze 2.0 / MOTELI (RabbitMQ)

Le transport RabbitMQ (elixip 2.0, `moteli_*.proto`) n'a pas de long-poll :
il faut un équivalent de la preuve de vie pour tuer les sessions zombies.
Pistes à instruire (aucune décidée) :

1. **Publication `mandatory` sur la clé d'événements de la session** : si la
   queue AMQP du contrôleur n'existe plus (connexion elixip morte → queues
   `exclusive`/`auto-delete` supprimées par le broker), le broker renvoie un
   `basic.return` → le mcu arme le temporisateur 60 s. Le broker fait office
   de détecteur de vie — c'est l'analogue le plus direct du long-poll.
2. **Keepalive applicatif** : message `SessionPing` périodique dans le schéma
   moteli (équivaut au `MediaSessionPing` de la solution (a) — simple,
   transport-agnostique, mais du trafic et du code elixip en plus).
3. **TTL broker** : queue d'événements déclarée avec `x-expires` côté
   contrôleur ; le mcu vérifie périodiquement son existence
   (`queue.declare passive`) — dépend des droits et de la topologie déclarée
   par elixip.

Contrainte CLAUDE.md à respecter le moment venu : tout ajout au contrat
`/jsr309` (ou `/mcu`) doit être répercuté dans les schémas protobuf MOTELI v2
d'elixip dans le même jeu de changements.

### 7.6 État de livraison (2026-08-11)

Fichiers touchés :

| Fichier | Changement |
|---|---|
| `mcu/include/xmlstreaminghandler.h`, `mcu/src/xmlstreaminghandler.cpp` | `XmlEventQueue` : `DefaultExpiresSecs` (60, délai commun à toutes les API), `AttachPoller`/`DetachPoller`/`IsPolled(idle)` (compteur de pollers + horodatage `steady_clock` du dernier détachement, sous le verrou de `::Wait` via `Locked`) ; horodatage initialisé à la CRÉATION (les files nues expirent aussi) ; `GetIdleQueues(idle)` (rend des **ids** seulement) et `HasQueue(id)`, tous deux sous `listUse.IncUse()` ; `ProcessRequest` encadre sa boucle de long-poll par `AttachPoller`/`DetachPoller` |
| `mcu/include/eventqueuesweeper.h` **(nouveau)** | `EventQueueSweeper` : toute la politique, en-tête seul (aucun `.o` à ajouter aux `OBJS`). `private Worker` → tick annulable `wait.WaitSignal`, période = `min(grâce, 10 s)`. **Signal 1** : `GetIdleQueues` → `DeleteQueueOwners` → `DestroyEventQueue`. **Signal 2** : `CollectQueueIds` → pour un `queueId` que `HasQueue` ne connaît plus, ARMEMENT (map `orphans`, horodatée) puis destruction à l'échéance ; purge des `orphans` que plus aucun objet ne référence. Contrat documenté : `StartSweeper`/`StopSweeper` hors verrou du service, `StopSweeper` en premier dans `End()` |
| `mcu/src/jsr309/JSR309Manager.{h,cpp}` | `private EventQueueSweeper` + `CollectQueueIds`/`DeleteQueueOwners` (extraction sous verrou, `End()` + destruction HORS verrou, mécanique du §3.3) ; `Init(eventMngr, queueExpiresSecs=60)` arme le balayeur hors verrou ; `End()` fait `StopSweeper()` en PREMIER et hors verrou ; `DeleteEventQueue` ne détruit plus les sessions (armement par le balayeur) ; fuite historique corrigée dans `DeliverEvent` (retour d'`AddEvent` non testé) |
| `mcu/include/mcu.h`, `mcu/src/mcu.cpp` | même greffe pour les conférences : `private EventQueueSweeper`, `Init(eventMngr, queueExpiresSecs=60)`, `CollectQueueIds`/`DeleteQueueOwners` (qui **libère aussi le `tag`**, sans quoi il bloquerait la map des tags par laquelle passent tous les événements de participant) ; `DeleteEventQueue` en armement ; `MCU::End()` réaligné sur le motif « extraction sous verrou, `End()` dehors » (il terminait les conférences SOUS le mutex, alors qu'un `conf->End()` joint des threads qui le veulent — p.ex. `onParticipantMediaTimeout`) et vide `tags` ; `inited` enfin initialisé ; 4 fuites d'événements corrigées (retour d'`AddEvent` non testé dans les handlers FPU / doc sharing / media timeout / media connected) |
| `mcu/src/main.cpp` | option `--event-queue-expires <s>` (défaut `XmlEventQueue::DefaultExpiresSecs`), passée à `jsr309Manager.Init` **et** `mcu.Init`, documentée dans `--help` |
| `mcu/tests/test_jsr309_session_expiry.cpp` | 13 tests : vitalité de la file (file neuve dans la grâce, jamais pollée → expire, poller attaché → protège indéfiniment, détachement → relance le compte à rebours, ré-attache → l'annule, dernier poller gagne), recensement `GetIdleQueues`, balayeur nominal, **référence en vol qui survit** (shared_ptr), `EventQueueDelete` → armement sans destruction, destruction à l'échéance, session à `queueId` 0 jamais balayée, délai 0 = désarmé, `End()` joint le balayeur |
| `mcu/tests/test_mcu_conference_expiry.cpp` **(nouveau)** | 7 tests, mêmes propriétés côté conférences + **libération du `tag`** (nominal et à l'arrêt) |
| `xmlrpc_jsr309_api.md`, `MCU-API.md`, `readme.md`, `CLAUDE.md` | contrat côté client des deux API (§5, §6.1/6.2, §9.6 pour JSR-309 ; §5, §6.1, §7.1 pour MCU — où le montage « file partagée » est désormais signalé comme point de défaillance unique), option CLI + traces de log, invariant dans les conventions |

Écarts assumés par rapport à l'esquisse du §7.3 :

- **pas d'événement d'expiration** (l'ancien `MediaSessionExpiredEvent` du §2.1
  n'existe pas ; le code 7 est de toute façon pris par `EndpointConnectedEvent`) :
  log serveur seulement, comme le prévoyait le §7.3 ;
- **`DeleteEventQueue` arme au lieu de détruire** (arbitrage du §7.4) : le §7.3
  parlait de « déclencher le MÊME nettoyage immédiatement » ;
- **balayeur bâti sur `Worker`** (`worker.h`) plutôt que sur un `std::thread` +
  `condition_variable` maison comme au §3.3 — même sémantique (tick annulable,
  join hors verrou), motif déjà factorisé par le chantier `wait.h` — et
  **politique factorisée** entre les deux services (`EventQueueSweeper`) plutôt
  que recopiée.

Trou résiduel connu, réduit : un objet créé avec un `queueId` **≤ 0** n'est
rattaché à aucune file et échappe au balayeur — comme il échappe déjà à
l'émission de ses événements. C'est le cas par défaut du transport MOTELI
(`eventListenerId` absent → 0, `rabbitmqmcu.cpp`), donc **le nettoyage des
conférences pilotées par RabbitMQ reste entièrement à faire** (§7.5). En
revanche un `queueId` > 0 qui ne désigne aucune file EST désormais traité (le
signal 2 l'arme puis le détruit) : le contrôleur doit passer un `queueId` réel.

Validation manuelle restante : recette en appel réel (coupure du long-poll →
nettoyage constaté ; reconnexion sous 1 s → aucun faux positif ; côté MCU,
kelixip doit d'abord passer à une file par conférence).

---

# Solution (a) historique — expiration par inactivité XML-RPC

> Objectif : empêcher la fuite des `MediaSession` JSR309 (endpoints, mixers, threads
> d'encodage/décodage, ports RTP) quand le client de contrôle (elixip) meurt ou perd la trace
> d'une session. Aujourd'hui **rien ne fait le ménage** : les seuls chemins de destruction sont
> l'appel explicite `MediaSessionDelete` (`JSR309Manager.cpp:185`) et l'arrêt global du serveur
> (`JSR309Manager::End`, `JSR309Manager.cpp:59`). Une session orpheline vit jusqu'au redémarrage.
>
> Solution retenue : **garbage collector côté serveur, basé sur un horodatage « dernière commande
> XML-RPC » par session** (plan de contrôle), avec balayeur périodique dans `JSR309Manager`.
> C'est l'option (a) discutée le 2026-07-06 ; l'option (b) — réagir au watchdog RTP quand tous les
> endpoints sont morts — reste possible en complément mais ne couvre pas le cas du client mort
> avant tout trafic média.
>
> Conception réalisée le 2026-07-06 sur la branche `feat/alma_linux9`, après les Phases 0+1 de la
> migration smart pointers (sessions en `shared_ptr`, mutex interne `MediaSession`). Toutes les
> références `fichier:ligne` correspondent à cet état du code.

---

## 1. Sémantique choisie

### 1.1 Ce que « inactivité » veut dire ici

**Inactivité du plan de contrôle uniquement** : une session expire si aucune commande XML-RPC ne
l'a touchée depuis `timeout` millisecondes. Le trafic média (RTP) ne compte **pas** comme
activité :

- c'est ce qui rend le mécanisme simple (un seul point de passage, aucun coût sur le chemin
  média) ;
- le but est de détecter un **client mort**, pas un média silencieux — le silence média est déjà
  couvert par le watchdog RTP (gap 5, `EndpointStartRTPTimeout`, `xmlrpcjsr309.cpp:1072`).

Conséquence assumée : une conférence saine mais « calme » côté contrôle (média qui coule, aucun
ordre XML-RPC pendant des heures) expirerait si le timeout est armé. D'où :

- le mécanisme est **opt-in par session** (défaut : désarmé, aucun changement de comportement) ;
- une commande **`MediaSessionPing`** triviale permet au client de rafraîchir périodiquement les
  sessions qu'il veut garder (un ping par session, période conseillée ≤ `timeout/3`) ;
- le timeout doit être choisi grand devant le silence normal du plan de contrôle (ordre de
  grandeur : minutes, pas secondes).

### 1.2 Ce qui rafraîchit l'horodatage (« touch »)

| Chemin | Touch ? | Justification |
|---|---|---|
| `CreateMediaSession` (`JSR309Manager.cpp:116`) | oui (init) | naissance de la session |
| `GetMediaSessionRef` (`JSR309Manager.cpp:162`) | **oui** | point de passage unique : *tous* les handlers XML-RPC à portée session le traversent (`PlayerCreate`, `EndpointCreate`, `*Attach*`, …) — aucun handler à modifier |
| Nouveau RPC `MediaSessionPing` | oui | c'est un `GetMediaSessionRef` + ok |
| Nouveau RPC `MediaSessionSetTimeout` | oui | passe par `GetMediaSessionRef` sémantiquement (armement = preuve de vie) |
| `PostEvent`/`DeliverEvent` (`JSR309Manager.cpp:222,264`) | **non** | événements générés par le **serveur** (fin de player, arrêt recorder, timeout RTP) : ce n'est pas une preuve que le client est vivant. Sinon une session avec player en boucle ne mourrait jamais |
| Long-poll de la file d'événements (`XmlStreamingHandler`) | non (v1) | la file est par `queueId`, pas par session ; propager le touch traverserait les couches. Extension possible (§6) |

### 1.3 Expiration = suppression dure + événement

À expiration, le serveur fait l'équivalent d'un `MediaSessionDelete` (retrait de la map, `End()`,
destruction par le dernier `shared_ptr`) **et** publie un événement `MediaSessionExpiredEvent`
dans la file du client (best effort : si la file n'existe plus — client mort — l'événement est
détruit). Un client vivant mais distrait apprend donc la disparition au lieu d'accumuler des
erreurs « Media session not found ».

---

## 2. Contrat de fil (nouveautés côté API XML-RPC)

⚠️ Comme pour l'énumération `JSR309Event::Events` (`JSR309Event.h:31-39`), ces valeurs sont un
contrat partagé avec elixip : ne jamais les réutiliser ni les réordonner.

### 2.1 Nouvel événement

```
MediaSessionExpiredEvent = 7
```

Ajouté à `JSR309Event::Events` (`JSR309Event.h`), après `EndpointDisconnectedEvent = 6`.

Sérialisation `(is)` = `{type=7, sessionTag}` — même mécanique UTF-8 que
`EndpointDisconnectedEvent::GetXmlValue` (`RTPEndpoint.cpp:415-423`) mais sans contexte joinable
(l'événement porte sur la session entière ; `joinableId`/`media`/`role` n'ont pas de sens ici).
Le `sessionTag` suffit au client pour retrouver la session ; il est copié dans l'événement avant
suppression de l'entrée (pas de référence vers la map après le retrait).

### 2.2 Nouvelles méthodes XML-RPC (`jsr309CmdList`, `xmlrpcjsr309.cpp:2755`)

```
MediaSessionSetTimeout (sessionId:int, timeoutMs:int) -> ok
    timeoutMs > 0 : arme (ou re-arme) l'expiration par inactivité de la session.
    timeoutMs = 0 : désarme (comportement historique : la session ne expire jamais).
    Erreur si la session n'existe pas.

MediaSessionPing (sessionId:int) -> ok
    Rafraîchit l'horodatage d'activité. Erreur si la session n'existe pas
    (le client apprend ainsi qu'elle a expiré, même s'il a raté l'événement 7).
```

`MediaSessionCreate` garde sa signature `(tag, queueId)` : compatibilité totale, une session naît
désarmée. (Extension possible : défaut global serveur, §6.)

---

## 3. Conception détaillée côté `JSR309Manager`

### 3.1 État par session (`JSR309Manager.h:62-68`)

```cpp
struct MediaSessionEntry
{
    int id;
    int queueId;
    std::wstring tag;
    std::shared_ptr<MediaSession> sess;
    // --- nouveau ---
    std::chrono::steady_clock::time_point lastActivity; // maj sous mutex manager
    std::chrono::milliseconds timeout{0};               // 0 = jamais d'expiration
};
```

- **`steady_clock`** obligatoire (immunisé contre NTP/`settimeofday`) — pas de `gettimeofday`
  comme dans `Init` (`JSR309Manager.cpp:43`).
- Les deux champs sont lus/écrits **exclusivement sous `JSR309Manager::mutex`**, déjà pris par
  tous les accesseurs — aucun atomique nécessaire, aucun nouveau verrou.

### 3.2 Touch

- `CreateMediaSession` : `entry.lastActivity = steady_clock::now();` (le bloc sous lock existe
  déjà, `JSR309Manager.cpp:146-151`).
- `GetMediaSessionRef` : sous le lock existant (`JSR309Manager.cpp:165`), après le `find` réussi :
  `it->second.lastActivity = steady_clock::now();`. Coût : une lecture d'horloge par commande
  XML-RPC — négligeable devant le coût d'un appel XML-RPC.
- Nouvelle méthode `int SetMediaSessionTimeout(int id, DWORD timeoutMs)` : sous lock, `find`,
  maj `timeout` + `lastActivity`, log. Retourne erreur si absent.

### 3.3 Balayeur (thread membre du manager)

Nouveaux membres :

```cpp
std::thread                 sweeper;
std::condition_variable     sweeperWakeup;   // attend sur 'mutex' existant
bool                        stopping = false;
static constexpr std::chrono::seconds SweepPeriod{10};
```

- **Démarrage** dans `Init` (`JSR309Manager.cpp:33`), après `inited = true`.
- **Arrêt** dans `End` (`JSR309Manager.cpp:59`) : positionner `stopping = true` sous lock,
  `notify_all`, puis **`sweeper.join()` hors de tout mutex** — même discipline que le piège déjà
  documenté pour les threads de session (le join ne doit jamais se faire sous un verrou que le
  thread peut vouloir prendre). Le join se fait **avant** la boucle des `sess->End()` existante,
  pour qu'aucun balayage ne coure pendant la vidange.
- **Boucle** (période fixe `SweepPeriod` = 10 s ; largement assez fin pour des timeouts de
  l'ordre de la minute, et le réveil immédiat à l'arrêt est garanti par la CV) :

```cpp
void JSR309Manager::SweepLoop()
{
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping)
    {
        sweeperWakeup.wait_for(lock, SweepPeriod);
        if (stopping) break;

        // 1) Collecte des expirées SOUS lock : on sort l'entrée complète de la map
        //    (même principe que DeleteMediaSession : une fois hors map, personne ne
        //    peut plus obtenir de nouvelle référence).
        auto now = std::chrono::steady_clock::now();
        std::vector<MediaSessionEntry> expired;
        for (auto it = sessions.begin(); it != sessions.end(); )
        {
            if (it->second.timeout.count() > 0 &&
                now - it->second.lastActivity > it->second.timeout)
            {
                expired.push_back(std::move(it->second));
                it = sessions.erase(it);
            } else
                ++it;
        }
        XmlStreamingHandler *mngr = eventMngr;

        // 2) Terminaison + événement HORS lock (convention C-3 : jamais d'appel de
        //    méthode de session sous le mutex manager — End() joint des threads).
        if (!expired.empty())
        {
            lock.unlock();
            for (auto &e : expired)
            {
                Log("-JSR309Manager::SweepLoop : session %d expirée par inactivité, suppression\n", e.id);
                e.sess->End();
                if (mngr)
                {
                    MediaSessionExpiredEvent *event = new MediaSessionExpiredEvent();
                    event->SetSessionTag(e.tag);
                    if (!mngr->AddEvent(e.queueId, event))
                        delete event;   // file détruite (client mort) : pas de fuite
                }
            }
            expired.clear();            // dernier shared_ptr → destruction hors lock
            lock.lock();
        }
    }
}
```

Points de sûreté (alignés sur l'état post-Phases 0+1 de `smart_pointers_plan.md`) :

- **Handler concurrent tenant déjà un `shared_ptr`** sur une session qui expire : sûr par
  construction — c'est exactement le scénario couvert par le passage aux `shared_ptr`
  (`JSR309Manager.h:42-45`). La session survit à l'`End()` tant que le handler la tient ;
  `MediaSession::End()` est idempotent (déjà requis par `DeleteMediaSession`,
  `JSR309Manager.cpp:211-214`).
- **`DeleteMediaSession` concurrent** : l'un des deux trouve l'entrée dans la map, l'autre ne la
  trouve plus — pas de double `End()` possible sur le même chemin d'extraction.
- **`AddEvent` sur file disparue** : `XmlStreamingHandler::AddEvent` retourne 0 **sans détruire
  l'événement** (`xmlstreaminghandler.cpp:212-239`) → le balayeur fait le `delete`. NB : ce même
  défaut existe déjà dans `DeliverEvent` (`JSR309Manager.cpp:291-295`, le retour n'est pas testé) —
  fuite préexistante à corriger au passage (une ligne).
- Pas d'appel à `Date`/horloge murale : tout en `steady_clock`.

### 3.4 Pourquoi ne pas réutiliser `DeleteMediaSession` dans le balayeur

`DeleteMediaSession(id)` ne rend ni `tag` ni `queueId`, nécessaires à l'événement d'expiration, et
re-verrouillerait le mutex par session. La collecte groupée sous un seul lock + terminaison hors
lock est à la fois plus simple et identique dans sa mécanique d'extraction (`JSR309Manager.cpp:193-209`).

---

## 4. Plan d'implémentation (fichiers touchés)

| # | Fichier | Changement |
|---|---|---|
| 1 | `mcu/src/jsr309/JSR309Event.h` | `MediaSessionExpiredEvent = 7` dans l'énum ; classe `MediaSessionExpiredEvent : public JSR309Event` (comme `EndpointDisconnectedEvent`, `RTPEndpoint.h:99-108`, mais déclarée ici : l'événement n'appartient à aucun endpoint) |
| 2 | `mcu/src/jsr309/JSR309Event.cpp` | `GetXmlValue` : sérialisation `(is)` `{7, sessionTag}` (copier la mécanique UTF8 de `RTPEndpoint.cpp:415-423`) |
| 3 | `mcu/src/jsr309/JSR309Manager.h` | champs `lastActivity`/`timeout` dans `MediaSessionEntry` ; membres `sweeper`/`sweeperWakeup`/`stopping` ; déclarations `SetMediaSessionTimeout`, `SweepLoop` ; includes `<thread>`, `<condition_variable>`, `<chrono>`, `<vector>` |
| 4 | `mcu/src/jsr309/JSR309Manager.cpp` | init `lastActivity` dans `CreateMediaSession` ; touch dans `GetMediaSessionRef` ; `SetMediaSessionTimeout` ; `SweepLoop` (§3.3) ; démarrage du thread dans `Init`, arrêt+join dans `End` ; correction du retour d'`AddEvent` non testé dans `DeliverEvent` |
| 5 | `mcu/src/jsr309/xmlrpcjsr309.cpp` | handlers `MediaSessionSetTimeout` et `MediaSessionPing` + 2 entrées dans `jsr309CmdList` (`xmlrpcjsr309.cpp:2755`) |
| 6 | `xmlrpc_jsr309_api.md` | documenter les 2 méthodes et l'événement 7 |
| 7 | `jsr309impl` / elixip (hors repo C++) | consommer l'événement 7 ; côté `jsr309impl` Java, mapping dans `MSControlFactoryImpl` comme pour l'événement 6 (`MSControlFactoryImpl.java:189`) — peut suivre dans un second temps, l'événement est simplement ignoré sinon |

Aucun changement de `MediaSession.*` : le mécanisme vit entièrement dans le manager (la session
n'a pas besoin de connaître son horodatage).

Ordre de commit suggéré : (1+2) événement, (3+4) manager+balayeur, (5+6) API. Build vert exigé à
chaque étape (`make -C mcu mcu`).

---

## 5. Validation manuelle (pas de suite de tests automatisée)

1. **Expiration nominale** : `EventQueueCreate` → `MediaSessionCreate` →
   `MediaSessionSetTimeout(sess, 5000)` → ne plus rien envoyer → sous ~15 s (timeout + période de
   balayage) : événement `(7, tag)` dans la file, log `session expirée`, puis
   `MediaSessionPing(sess)` retourne « Media session not found ».
2. **Ping maintient en vie** : même montage, pinger toutes les 2 s pendant 30 s → pas
   d'expiration ; arrêter les pings → expiration.
3. **Toute commande vaut ping** : remplacer le ping par `EndpointCreate`/`PlayerCreate` → même
   effet (preuve que le touch dans `GetMediaSessionRef` couvre tout).
4. **Désarmement** : `MediaSessionSetTimeout(sess, 0)` → plus d'expiration.
5. **Client mort** : `EventQueueDelete` puis silence → expiration, log, pas de crash, pas de
   fuite d'événement (valgrind ou log).
6. **Arrêt propre** : sessions armées en attente d'expiration + arrêt du serveur → `End()` joint
   le balayeur sans deadlock ni double `End()` de session.
7. **Concurrence** : boucle de commandes sur une session pendant qu'elle expire (timeout court +
   marteau XML-RPC) → jamais de crash ; au pire « Media session not found » après expiration.

---

## 6. Extensions possibles (hors périmètre v1)

- **Défaut global serveur** : option de `main.cpp` (`--jsr309-session-expires <s>`) appliquée à la
  création — utile pour se protéger des clients qui n'armeront jamais le timeout. Défaut 0 pour
  conserver le comportement actuel.
- **Long-poll = preuve de vie** : rafraîchir toutes les sessions liées à un `queueId` quand le
  client interroge la file (`XmlStreamingHandler` → callback vers le manager). Dispense le client
  de pinger, mais couple les couches ; à ne faire que si le ping s'avère pénible côté elixip.
- **Cascade `EventQueueDelete`** : supprimer (ou armer avec un timeout court) les sessions liées
  à une file détruite. Aujourd'hui `DeleteEventQueue` (`JSR309Manager.cpp:100`) ne touche pas aux
  sessions et leurs événements partent dans le vide.
- **Option (b) complémentaire** : auto-suppression quand *tous* les endpoints d'une session sont
  en `EndpointDisconnectedEvent` depuis N secondes — couvre le cas « client vivant mais média
  mort », symétrique de celui-ci.
