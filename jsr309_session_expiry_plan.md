# Expiration des MediaSession JSR309 par inactivité — conception (solution a)

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
chaque étape (`make -f mcu/Makefile.rpm mcu`).

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
