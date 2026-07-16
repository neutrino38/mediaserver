# Plan de refactor WebSocket (serveur mono-thread + TLS/WSS + certificats en ligne de commande)

Statut : **CONCEPTION** — non implémenté. Date : 2026-07-16.

Fichiers concernés :
- `mcu/include/websocketserver.h` / `mcu/src/websocketserver.cpp`
- `mcu/include/websocketconnection.h` / `mcu/src/websocketconnection.cpp`
- `mcu/include/websockets.h` (interface `WebSocket` inchangée)
- `mcu/src/main.cpp` (CLI + init)
- Consommateurs : `mcu/src/jsr309/WSEndpoint.cpp` (via `WebSocket*`), `JSR309Manager` (handler)

---

## 1. Objectifs

1. **Une seule `poll()` et une seule thread** (celle de `WebSocketServer`) gèrent *toutes* les
   connexions : socket d'écoute + tous les sockets clients + réveil inter-thread.
   Suppression du thread et de la `poll()` par-connexion.
2. **WebSocket sécurisé (WSS)** : ajout de TLS côté serveur.
3. **Paramétrage des certificats en ligne de commande** (fichier crt/key, activation TLS).

---

## 2. État actuel (rappel de l'architecture)

- `WebSocketServer::Run()` : thread unique, `poll()` sur le seul socket d'écoute ; à chaque
  `accept()`, crée un `WebSocketConnection` **qui démarre son propre thread**.
- `WebSocketConnection::Run()` : thread propre, `poll(ufds, 2, -1)` sur `[socket, wakeup_socket[1]]`.
  - Lit → `ProcessData()` (parse HTTP upgrade puis trames WS) → callbacks `WebSocket::Listener`
    exécutés **sur ce thread**.
  - Écrit les `Frame*` en attente (file `frames` protégée par `mutex`) quand `POLLOUT`.
  - `SignalWriteNeeded()` (appelé depuis n'importe quel thread) écrit un octet dans
    `wakeup_socket[1]` pour réarmer `POLLOUT`.
- Déconnexion : `Run()` sort → `listener->onDisconnected(this)` → le serveur pousse dans
  `zombies` → `CleanZombies()` (thread serveur) fait `delete con` (qui `join()` le thread mort).
- Cross-thread : `WSEndpoint::_ws` garde un `WebSocket*` brut et appelle `_ws->SendMessage()` /
  `_ws->Close()` depuis le chemin média. `onClose()` doit remettre `_ws` à NULL.

**Problème central du refactor** : aujourd'hui chaque connexion est isolée sur son thread ; en
mono-thread, (a) un callback lent bloque *toutes* les connexions, (b) la sécurité de durée de vie
change radicalement (le serveur possède et détruit les connexions, mais des threads externes
détiennent encore un `WebSocket*`).

---

## 3. Architecture cible

### 3.1 Boucle unique dans `WebSocketServer::Run()`

Une seule `poll()` sur un tableau `pollfd` dynamique :

| index | fd | rôle |
|-------|-----|------|
| 0 | `server` | socket d'écoute (POLLIN = nouvelle connexion) |
| 1 | `wakeup[0]` | réveil inter-thread (eventfd ou pipe) |
| 2..N | socket de chaque connexion | POLLIN toujours ; POLLOUT si écriture en attente |

- Le tableau `pollfd` est **reconstruit à chaque tour** à partir de la map des connexions
  (simple et sûr ; le volume de connexions WS d'un MCU est faible — pas besoin d'epoll).
  Alternative epoll notée en §7 si le nombre de connexions explose.
- Chaque connexion expose au serveur : `GetFd()`, `GetPollEvents()` (POLLIN | POLLOUT si sortie
  pendante), `OnReadable()`, `OnWritable()`, `OnError()`, `IsClosed()`.
- `WebSocketConnection` **n'a plus** : `thread`, `run()`, `Start()`, `Stop()`, `Run()`,
  `wakeup_socket[2]`, `ufds[2]`. Elle devient une **machine à état passive** pilotée par le serveur.

### 3.2 Réveil inter-thread unique + file de commandes

Remplacer les N `socketpair` par **un seul** mécanisme de réveil au niveau serveur :
`int wakeup[2]` (pipe) **ou** `eventfd`. Préférence : `eventfd(0, EFD_NONBLOCK)` (1 fd, plus léger) ;
repli `pipe2()` si portabilité.

Les threads externes ne touchent plus directement au socket. Ils déposent une **commande** dans une
file protégée par un mutex serveur, puis écrivent dans `wakeup` :

```
struct Command {
    enum { Send, Close } type;
    uint64_t connId;          // identifiant stable (voir 3.3), PAS le fd
    std::shared_ptr<Frame> ...; // ou payload à encoder
};
```

Au réveil, le thread serveur draine la file :
- `Send` → pousse la/les `Frame` dans la file de sortie de la connexion visée (si encore vivante)
  et arme POLLOUT.
- `Close` → marque la connexion pour fermeture propre (envoi trame Close puis destruction).

> Rationale : centraliser *toute* mutation d'état de connexion sur le thread serveur élimine les
> data-races sans multiplier les verrous. Les callbacks entrants tournent aussi sur ce thread.

### 3.3 Cycle de vie et sécurité mémoire (shared_ptr)

Aligné sur la migration smart-pointers du projet (cf. `smart_pointers_plan.md`).

- Le serveur possède : `std::map<uint64_t /*connId*/, std::shared_ptr<WebSocketConnection>>`.
  `connId` est un compteur monotone (pas le fd, qui est réutilisable).
- `Accept()` continue de fournir un `WebSocket*` au `Listener`. Pour la sécurité, l'appel externe
  `SendMessage`/`Close` ne déréférence **jamais** l'objet directement : il passe par la file de
  commandes du serveur (le `WebSocket*` sert uniquement de clé → il faut exposer `connId` via le
  handle, ou router par pointeur avec validation sous verrou).
  - **Décision proposée (A)** : `WebSocketConnection::SendMessage()`/`Close()` restent l'API
    publique, mais leur implémentation **poste une commande** au serveur au lieu d'écrire.
    L'objet garde un pointeur/back-ref vers le serveur + son `connId`. C'est thread-safe et ne
    change pas les appelants (`WSEndpoint` inchangé).
- Destruction : quand le pair ferme / erreur / commande Close traitée, le thread serveur :
  1. émet `onClose`/`onDisconnected` (sur le thread serveur),
  2. `erase` l'entrée de la map (plus aucune nouvelle commande ne pourra la cibler — la commande
     référence `connId`, lookup échoue proprement),
  3. le dernier `shared_ptr` détruit l'objet. Plus de liste `zombies` ni de `join()`.
- **Piège** : `WSEndpoint::_ws` reste un `WebSocket*` brut ; `onClose()` doit impérativement
  remettre `_ws=NULL`. Comme `onClose` et les `SendMessage` externes sont désormais sérialisés par
  le serveur (callbacks sur thread serveur, sends via file), la fenêtre d'usage-après-libération
  est fermée : une commande `Send` postée après `erase` est simplement ignorée (connId absent).

### 3.4 Abstraction Transport (plain vs TLS)

Introduire une couche transport par-connexion pour isoler la lecture/écriture socket du TLS :

```
class Transport {                    // interface
  virtual int  Handshake();          // >0 ok/continue, 0 en cours, <0 erreur (plain: no-op ok)
  virtual int  Recv(BYTE* buf, DWORD size);   // renvoie octets clairs dispo (0 = WANT), <0 erreur
  virtual int  Send(const BYTE* buf, DWORD size); // consomme clair, <0 erreur
  virtual bool WantsWrite();         // vrai s'il reste des octets chiffrés à pousser sur le socket
  virtual bool WantsRead();          // pour handshake TLS (WANT_READ)
};
```

- **`PlainTransport`** : `read()`/`write()` directs (comportement actuel).
- **`TlsTransport`** : calqué sur `dtls.cpp` (BIO mémoire), adapté à `TLS_server_method()` et à une
  boucle non-bloquante :
  - `read_bio` / `write_bio` = `BIO_new(BIO_s_mem())`.
  - `OnReadable` serveur : `read()` socket → `BIO_write(read_bio, ...)`. Puis :
    - si handshake non fini : `SSL_accept()` ; gérer `SSL_ERROR_WANT_READ/WRITE` ; drainer
      `write_bio` → socket.
    - sinon : `SSL_read()` en boucle → octets clairs → `ProcessData()`.
  - `Send(clair)` : `SSL_write()` → `BIO_read(write_bio)` → file de sortie chiffrée → socket sur
    POLLOUT.
  - POLLOUT armé dès qu'il reste des octets dans `write_bio` (handshake **ou** données).
  - Fermeture : `SSL_shutdown()` best-effort.
- `SSL_CTX` **unique et statique** (comme `DTLSConnection::ssl_ctx`), créé une fois à
  l'initialisation du serveur avec `TLS_server_method()`, `SSL_CTX_set_min_proto_version(TLS1_2)`,
  `SSL_CTX_use_certificate_chain_file()` + `SSL_CTX_use_PrivateKey_file()` + check. **Pas** de
  `SSL_VERIFY_PEER` (client browser sans cert) — contrairement au DTLS/SRTP.

### 3.5 Activation TLS + certificats (CLI)

- Nouvelles options `main.cpp` :
  - `--websocket-secure` : active WSS (sinon WS clair, défaut).
  - `--websocket-cert <fichier>` : chaîne de certificats PEM (défaut : réutiliser `crtfile`
    `/etc/mediaserver/mcu.crt`).
  - `--websocket-key <fichier>` : clé privée PEM (défaut : `keyfile` `/etc/mediaserver/mcu.key`).
  - **Décision proposée (B)** : si `--websocket-cert`/`--websocket-key` sont fournis, activer TLS
    automatiquement même sans `--websocket-secure` (ergonomie), tout en gardant le flag explicite.
- API serveur : `WebSocketServer::SetCertificate(const char* crt, const char* key)` (statique,
  façon DTLS) **ou** paramètres passés à `Init(port, secure, crt, key)`. Préférence : signature
  `Init(int port)` inchangée + `SetSecure(bool)` / `SetCertificate(...)` appelés avant `Init`,
  pour rester cohérent avec le style DTLS et minimiser la diff d'appel.
- Mettre à jour le texte d'aide (`--help`) et documenter dans le README/`CLAUDE.md` si besoin.

---

## 4. Impacts sur `WebSocketConnection`

À **supprimer** : `std::thread thread`, `run()`, `Run()`, `Start()`, `Stop()`,
`wakeup_socket[2]`, `pollfd ufds[2]`, `signal(SIGIO,...)`, `blocksignals()` local.

À **ajouter/adapter** :
- `int fd` + `uint64_t connId` + back-ref `WebSocketServer* server`.
- `std::unique_ptr<Transport> transport`.
- `short GetPollEvents()` : `POLLIN | POLLERR | POLLHUP` + `POLLOUT` si (`response` pendante ||
  `frames` non vide || `transport->WantsWrite()` || handshake en cours).
- `void OnReadable()` : ex-branche `POLLIN` de l'ancien `Run()` (via `transport->Recv`).
- `void OnWritable()` : ex-branche `POLLOUT` (sérialise `response`, sinon `GetNextFrame()` →
  `transport->Send`).
- `void OnError()` / `bool IsClosed()`.
- `SendMessage()`/`Close()`/`SignalWriteNeeded()` : **reroutés vers la file de commandes serveur**
  (§3.2) au lieu d'écrire dans un socketpair.
- `mutex` par-connexion : conservé uniquement si la file `frames` est encore alimentée hors thread
  serveur ; avec la file de commandes centralisée, `frames` devient **thread-serveur-only** → le
  mutex peut disparaître (à confirmer à l'implémentation).

Le parseur HTTP upgrade + parseur de trames WS (`WebSocketFrameHeader::Parser`, `ProcessData`,
`Accept`, handshake Sec-WebSocket-Accept) restent **inchangés** — seule la source des octets change
(passe par `Transport::Recv`).

---

## 5. Phases d'implémentation

### Phase 0 — Préparation (sans changement de comportement) — FAIT 2026-07-16 (BUILD VERT)
- Extraire l'abstraction `Transport` + `PlainTransport` qui encapsule le `read`/`write` actuel.
- Faire passer `WebSocketConnection` par `transport->Recv/Send` tout en gardant son thread.
- **But** : valider la couche transport isolément. BUILD VERT + rejeu WS clair inchangé.

Réalisé :
- Nouveau `mcu/include/websockettransport.h` (header-only, aucun impact Makefile/link) :
  interface `WebSocketTransport` (`Init/Handshake/Recv/Send/WantsWrite/Shutdown/GetFd`) +
  `WebSocketPlainTransport` (relais `read()`/`write()`, setup socket non-bloquant+`TCP_NODELAY`
  déplacé de `Run()` vers `Init()`, `Shutdown()` idempotent). `Handshake()`/`WantsWrite()` déjà
  au contrat pour le TlsTransport (Phase 2), triviaux ici, non sollicités par la connexion.
- `websocketconnection.h/.cpp` : membre `int socket` → `std::unique_ptr<WebSocketTransport>
  transport` ; `Init()` crée le transport ; `Run()` utilise `transport->GetFd()/Recv/Send` ;
  `Stop()` → `transport->Shutdown()`. Thread par-connexion + socketpair de réveil CONSERVÉS
  (comportement WS clair strictement identique).
- Aucune modif de `main.cpp`, `websocketserver.cpp`, `WSEndpoint`, JSR309.

**Harnais de test ajouté (`test/websocket/`, cf. §8)** — validation runtime FAITE, **7/7 PASS** :
- `mcu/src/wstest.cpp` (serveur d'écho autonome, target Makefile `wstest`, hors `TARGETS` par
  défaut) + `ws_client.py` (client WS Python pur) + `run.py` (orchestrateur).
- **2 bugs préexistants (commit `0969d35`, PAS dus au refactor transport) découverts et corrigés
  en Phase 0** :
  1. Socketpair de réveil inversée : `poll()` surveillait `wakeup_socket[1]` alors que le signal
     est écrit dans `[1]` (donc lisible sur `[0]`) → `POLLOUT` jamais armé, aucune sortie émise
     (même la réponse 101). Corrigé : `ufds[1].fd = wakeup_socket[0]`.
  2. Auto-deadlock : `SendMessage()` tient `mutex` puis appelait `SignalWriteNeeded()` qui
     re-verrouille le `std::mutex` non récursif. Corrigé via `SignalWriteNeededUnlocked()`.
- `mcu` (binaire principal) rebâti vert après ces corrections.

### Phase 1 — Passage mono-thread (WS clair) — FAIT 2026-07-16 (BUILD VERT, harnais 9/9)
- Ajouter `eventfd`/pipe de réveil + file de commandes dans `WebSocketServer`.
- Migrer la map de connexions en `map<connId, shared_ptr<WebSocketConnection>>`.
- Réécrire `WebSocketServer::Run()` : `poll()` unique sur écoute + réveil + tous les fds ;
  dispatch `OnReadable`/`OnWritable`/`OnError` ; drain de la file de commandes.
- Supprimer thread/Run/Start/Stop/socketpair de `WebSocketConnection` ; rerouter
  `SendMessage`/`Close` vers la file.
- Supprimer `zombies`/`CleanZombies` (remplacé par erase de la map).
- Vérifier `WSEndpoint` (`onClose` → `_ws=NULL`, sends cross-thread OK via file).
- **But** : parité fonctionnelle WS clair, un seul thread. Rejeu JSR309/WSEndpoint.

Réalisé :
- `WebSocketServer` : `eventfd(EFD_NONBLOCK)` de réveil + `poll()` unique reconstruit à chaque tour
  sur `[écoute, wakeup, toutes les connexions]`. Map possédante `map<uint64_t connId,
  shared_ptr<WebSocketConnection>>` (thread-serveur-only). `zombies`/`CleanZombies`/`sessionMutex`
  supprimés ; fermeture via `CloseConnection()` (NotifyClose→onClose, End, erase) avec grâce d'un
  tour via `recentlyClosed` (équivalent à l'ancien deferral zombies). `Start()`/`Stop()`/
  `onDisconnected` retirés. Décision de conception retenue = **pas de file de commandes formelle** :
  chaque connexion porte `frames` (protégé par `framesMutex`) + un flag atomique `closeRequested` ;
  l'`eventfd` sert juste à faire sortir `poll()`, qui reconstruit les events (`GetPollEvents()`
  renvoie `POLLOUT` si sortie en attente). Plus simple qu'une file, même sûreté.
- `WebSocketConnection` : machine à état passive. Supprimés : `std::thread`, `Run/run/Start/Stop`,
  `wakeup_socket`, `ufds`, `mutex`/`SignalWriteNeeded(Unlocked)`, bookkeeping bande passante.
  Ajoutés : `GetFd/GetPollEvents/OnReadable/OnWritable/IsFinished/NotifyClose`, `connId`,
  `framesMutex`, `std::atomic<bool> closeRequested`, `closeAfterFlush` (Reject : ferme après
  écoulement de la réponse → le 400/404 est réellement émis, léger mieux qu'avant). `SendMessage`/
  `Close` (appelables depuis un autre thread) poussent sous `framesMutex` puis `onWakeupNeeded()`.
- **R2 traité** : borne `WebSocketConnection::MaxFramePayload = 16 Mo` sur la longueur déclarée dans
  l'en-tête de trame → fermeture si dépassée (protège le thread unique de l'OOM).
- `WSEndpoint` inchangé (API `SendMessage`/`Close` préservée) ; contrat `onClose`→`_ws=NULL` vérifié
  OK (l'identité par pointeur/connId protège le cas remplacement de connexion).
- Harnais `test/websocket/` étendu (T8 concurrence 15 connexions, T9 isolation client lent) →
  **9/9 PASS**, stable sur 5 exécutions. `mcu` principal vert.
- ~~**Limite connue** : `WSEndpoint` détient un `WebSocket*` brut → race étroite~~ **FERMÉE en
  Phase 4** (voir ci-dessous) : `WSEndpoint::_ws` est passé en `std::weak_ptr<WebSocket>`.

### Phase 2 — TLS / WSS — FAIT 2026-07-16 (BUILD VERT, harnais clair+TLS 9/9)
- Créer `SSL_CTX` statique serveur (`TLS_server_method`), `SetCertificate`.
- Implémenter `TlsTransport` (BIO mémoire, `SSL_accept` non-bloquant, `SSL_read`/`SSL_write`).
- `CreateConnection` choisit `PlainTransport` ou `TlsTransport` selon le mode serveur.
- Gérer POLLOUT piloté par `WantsWrite()` (handshake + données chiffrées).
- **But** : WSS fonctionnel (test `wss://` navigateur + `openssl s_client`).

Réalisé :
- Nouveau `mcu/src/websockettransport.cpp` (ajouté à `OBJS`) : `SSL_CTX` serveur unique
  (file-scope `g_ssl_ctx`), `TLS_server_method`, TLS 1.2 min, `use_certificate_chain_file` +
  `use_PrivateKey_file`, `SSL_SESS_CACHE_OFF`, **pas de `SSL_VERIFY_PEER`** (clients navigateur).
  Façade `WebSocketTlsTransport::{ClassInit,IsAvailable,Create}` déclarée dans le header →
  **OpenSSL reste hors des en-têtes WebSocket** (impl concrète `TlsTransportImpl` en namespace
  anonyme dans le .cpp).
- `TlsTransportImpl` = BIO mémoire (`read_bio`/`write_bio`), pilotés depuis la boucle poll() non
  bloquante : `Recv` = FillReadBio(socket→read_bio) + `SSL_accept` tant que handshake non fini +
  `SSL_read` ; `Send` = `SSL_write`(→write_bio, ne bloque jamais) + `Flush` ; `Flush` draine
  `write_bio` dans `pendingOut` puis écrit sur le socket (gère les écritures partielles) ;
  `WantsWrite` = octets chiffrés en attente. Shutdown = `SSL_shutdown`+`SSL_free`(libère les BIO).
- **Contrat `Recv` modifié** (transport-agnostique) : `>0` octets / `0` rien maintenant
  (would-block ou handshake) / `<0` fermé. `WebSocketConnection::OnReadable` boucle jusqu'à `0`
  (draine les enregistrements TLS bufferisés) ; `OnWritable` appelle `Flush()` d'abord puis, si
  `WantsWrite()`, attend le prochain POLLOUT. `PlainTransport` suit le même contrat (inchangé
  fonctionnellement).
- `WebSocketConnection::Init(fd, unique_ptr<WebSocketTransport>)` : le serveur fournit le transport.
  `WebSocketServer::SetSecure(secure,cert,key)` + `CreateConnection` choisit Plain/TLS ; `Init`
  appelle `WebSocketTlsTransport::ClassInit`.
- Harnais : `wstest` accepte `--secure --cert --key` ; `run.py --tls` génère un cert auto-signé
  (`openssl req`) et lance le client `ws_client.py --tls` (module `ssl`, pile TLS indépendante).
- Validé : harnais **9/9 en clair ET en TLS** (stable 3×), + `openssl s_client` négocie TLSv1.3
  (AES-256-GCM). `mcu` principal vert.

### Phase 2bis (reste) — validation navigateur `wss://` réelle + scénario JSR309/WSEndpoint en WSS.

### Phase 3 — CLI certificats — FAIT 2026-07-16 (BUILD VERT, validé sur le vrai mcu)
- Options `--websocket-secure`, `--websocket-cert`, `--websocket-key` dans `main.cpp` + `--help`.
- Défauts sur `mcu.crt`/`mcu.key` ; activation auto si cert/key fournis (décision B).
- Log du mode (WS/WSS) et des fichiers de cert au démarrage.
- **But** : configuration complète par ligne de commande.

Réalisé :
- `main.cpp` : variables `wsSecure`/`wsCrtFile`/`wsKeyFile` ; parsing des 3 options ; aide mise à
  jour. Avant `wsServer.Init` : si `--websocket-cert`/`--key` fourni → `wsSecure=true` (décision B) ;
  défauts = `crtfile`/`keyfile` (les mêmes `/etc/mediaserver/mcu.crt`/`.key` que DTLS) ;
  `wsServer.SetSecure(...)` + log du mode.
- Validé sur le binaire `mcu` réel : `--websocket-cert/--key` active bien wss:// ; `openssl
  s_client` négocie TLSv1.3 (AES-256-GCM) sur le port WebSocket de mcu ; logs « secure mode
  enabled » / « TLS server context ready ». Test navigateur possible sur `wss://host:9090/jsr309/…`.

### Phase 4 — Nettoyage & durcissement — FAIT 2026-07-16 (BUILD VERT, harnais clair+TLS 10/10)
- Retirer le `mutex` par-connexion si `frames` est devenu thread-serveur-only.
- Gestion propre EINTR / POLLNVAL / erreurs poll (cf. `rtpsession-run-poll-robustness`).
- Timeouts (handshake TLS lent, connexion idle) — optionnel.

Réalisé :
- **Durée de vie `WSEndpoint` (correctness, la vraie dette)** : `WebSocket` expose désormais
  `GetWeakPtr()` (défaut vide ; surchargé par `WebSocketConnection` via `enable_shared_from_this`
  → `shared_from_this()`). `WSEndpoint::_ws` : `WebSocket*` → **`std::weak_ptr<WebSocket>`** ;
  chaque `SendMessage`/`Close` (thread RTP) fait `_ws.lock()` d'abord → soit un `shared_ptr` valide
  qui maintient la connexion en vie le temps de l'appel, soit `nullptr`. `weak_ptr::lock()` étant
  atomique, **plus d'usage-après-libération**. `onClose` ne réinitialise que si `lock().get()==ws`
  (préserve le cas remplacement de connexion). ⚠️ ajouter une méthode virtuelle à `WebSocket`
  change sa vtable → **rebuild complet requis** (rm de tous les `.o`).
- **Écritures partielles** : `WebSocketPlainTransport` a désormais un tampon `pendingOut` (comme le
  TLS) ; `Send` copie puis `Flush` écoule (gère `write()` partiel/`EAGAIN`), `WantsWrite` reflète le
  reste. Plus de perte de données ; backpressure naturelle (POLLOUT reste armé tant que non écoulé).
  `OnWritable` (déjà `Flush()` puis garde `WantsWrite()`) fonctionne uniformément clair/TLS.
- **Nettoyage** : méthode `Handshake()` retirée de l'interface `WebSocketTransport` (morte — le TLS
  pilote le handshake depuis `Recv`).
- **`framesMutex` : GARDÉ** — `SendMessage` reste appelé depuis le thread RTP, donc `frames` n'est
  pas thread-serveur-only. Item du plan clos = on garde.
- Harnais étendu (T10 backpressure : 100×500o en rafale sans lecture → écho intègre et ordonné) :
  **10/10 clair ET TLS**, stable.

**Choix délibérés (hors périmètre) :**
- **Pas de timeout idle** : les connexions JSR309 sont légitimement longues et souvent inactives
  (attente de saisie texte) ; un timeout idle les tuerait. Écarté.
- **Timeout de handshake/upgrade** (fermer les connexions à moitié ouvertes qui n'« upgradent »
  jamais — anti-slowloris) : nécessiterait un `poll()` à timeout fini (balayage périodique) au lieu
  de `-1`. Non implémenté (non bloquant : les sockets sont non bloquants, une connexion collée ne
  gèle pas le thread, elle occupe juste un slot). À faire si besoin de robustesse anti-DoS.

---

## 6. Points de vigilance

### 6.0 Audit non-bloquant de WSEndpoint (FAIT 2026-07-16)

**Verdict : le chemin de réception WSEndpoint est non bloquant et borné → compatible mono-thread.**

Chaîne tracée : `onMessageData` = `media->AppendMedia()` (memcpy) ; `onMessageEnd` → encode
T140/RED → `RTPMultiplexer::Multiplex()` (verrou **par-endpoint**, boucle synchrone, aucune I/O sous
verrou) → `RTPEndpoint::onRTPPacket` → `RTPSession::SendPacket` (`srtp_protect` CPU + `rtxUse` lock
uniquement si NACK — jamais pour du texte — + `sendto()` UDP qui ne bloque pas en attente du pair).
Le steady-state ne prend jamais `MediaSession::mutex`. Un seul handler WS est enregistré
(`/jsr309` → `JSR309Manager`). Le point « callback bloquant = blocage global » est donc **levé pour
le hot path** ; il se réduit aux 3 risques résiduels ci-dessous.

- **R1 — Contention verrou à l'upgrade** : `JSR309Manager::onWebSocketConnection` →
  `MediaSession::onNewMediaConnection` prend `MediaSession::mutex`, **partagé avec les handlers
  XML-RPC** (coarse-grained). En mono-thread, un thread XML-RPC tenant ce verrou (setup RTP/codec,
  qq ms) fait attendre le thread serveur WS → toutes les connexions. Vérifié : aucun
  `sleep`/`join`/I/O réseau tenu sous ce mutex → latence bornée au handshake, pas de deadlock ni
  stall indéfini. Acceptable ; à re-vérifier si `MediaSession` gagne des sections critiques longues.
- **R2 — `media->Alloc(length)` non borné** : `onMessageStart` alloue selon la longueur déclarée
  dans l'en-tête de trame WS (jusqu'à 64 bits), contrôlée par le pair. Isolé aujourd'hui sur le
  thread par-connexion ; en mono-thread une longueur énorme peut stresser/OOM tout le serveur.
  **Action recommandée (Phase 1) : borner `length` avant `Alloc` (rejet trame > seuil).**
- **R3 — Logs synchrones** : `Multiplex`/`SendPacket` appellent `Log()`. Écriture disque synchrone =
  latence par message sur le thread partagé. Mineur ; surveiller sous forte charge.

### 6.1 Autres points
- **Réentrance `Close()` dans un callback** : `WSEndpoint::onOpen` appelle `_ws->Close()` (ligne 39)
  et `onClose`/`Close` s'entrecroisent. En mono-thread, `Close()` devient une commande postée →
  traitée après le callback courant, donc pas de destruction pendant qu'on itère. Ne pas détruire
  une connexion pendant qu'on tient une référence dans la boucle de dispatch (utiliser une liste
  différée d'`erase` en fin de tour).
- **Reconstruction du `pollfd`** : ne pas invalider le tableau en pleine itération si une connexion
  est fermée pendant le dispatch — collecter les fermetures et appliquer après la boucle.
- **`SSL_ERROR_WANT_WRITE` en lecture** : une renégociation TLS peut exiger d'écrire pendant un
  `SSL_read` → toujours drainer `write_bio` après read *et* write.
- **`SIGPIPE`** : déjà ignoré globalement dans `main.cpp` (`signal(SIGPIPE, SIG_IGN)`), OK.
- **Cohérence smart-pointers** : suivre le modèle « map possède `shared_ptr`, handlers via copie,
  erase sous verrou puis destruction hors verrou » du projet.
- **Piège Makefile** : `mcu/Makefile.rpm` ne suit pas les headers → `rm` les `.o` concernés avant
  rebuild.

## 7. Alternatives écartées / notes
- **epoll** au lieu de reconstruire `pollfd` : plus scalable mais surdimensionné pour le volume WS
  d'un MCU ; à envisager seulement si le nombre de connexions devient grand. `poll()` garde la
  cohérence avec `rtmpserver`/`rtpsession`.
- **TLS via `BIO_new_socket`** (au lieu de BIO mémoire) : plus simple mais gère mal le non-bloquant
  multiplexé ; l'approche BIO mémoire est déjà éprouvée dans `dtls.cpp` → cohérence retenue.
- **Thread pool** (garder N threads) : rejeté, contredit l'objectif 1.

## 8. Tests / validation

**Harnais automatisé `test/websocket/` (FAIT en Phase 0, 7/7 PASS)** — voir `test/websocket/README.md` :
- `mcu/src/wstest.cpp` : serveur d'écho autonome (target Makefile `wstest`, hors `TARGETS` par
  défaut, `make -C mcu -f Makefile.rpm wstest`). Réutilise `TextEchoWebsocketHandler`, **aucune
  dépendance JSR309/RTP** → exerce purement `WebSocketServer/Connection/Transport`.
- `ws_client.py` : client WS en Python pur (stdlib) — handshake RFC 6455, écho texte/UTF-8/16 bits,
  ordre de 3 messages, ping/pong, close. Accepte déjà `--tls` pour la Phase 2.
- `run.py` : orchestrateur (lance le serveur, attend le port, exécute le client, teardown).
- Lancer : `python3 test/websocket/run.py` (code de sortie != 0 si échec).

**À compléter aux phases suivantes** :
- Phase 1 : relancer ce harnais (doit rester 7/7) + ajouter un test de charge (N connexions
  simultanées, un client lent ne doit pas bloquer les autres) — clé pour valider le mono-thread.
- Phase 2 : ajouter l'écoute WSS à `wstest` + option `--tls` à `run.py` ; `wss://` navigateur,
  `openssl s_client -connect host:port`, capture pcap chiffrée.
- Scénario bout-en-bout JSR309/WSEndpoint (hors harnais autonome) : rejeu réel.
- Fermeture : pair qui coupe brutalement, `Close()` interne, arrêt serveur (`End()`) sans fuite ni
  crash (valgrind/asan si possible).
