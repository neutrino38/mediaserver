# Harnais de test WebSocket

Test de non-régression **autonome** du serveur WebSocket
(`WebSocketServer` / `WebSocketConnection` / `WebSocketTransport`), sans
dépendance à JSR309/RTP/média. Sert de garde-fou pour le refactor décrit dans
`websocket-refactor.md` (Phases 0 → 2).

## Composants

- `mcu/src/wstest.cpp` — serveur d'écho autonome : un `WebSocketServer` avec le
  `TextEchoWebsocketHandler` (fourni par `websocketserver.h`) sur `/echo`.
- `test/websocket/ws_client.py` — client WebSocket minimal en **Python pur**
  (stdlib uniquement) + suite de tests (handshake RFC 6455, écho texte/UTF-8,
  longueur 16 bits, ordre, ping/pong, close, **concurrence de 15 connexions**,
  **isolation d'un client lent** — ces deux derniers valident le mono-thread de
  la Phase 1).
- `test/websocket/run.py` — orchestrateur : lance le serveur, attend le port,
  exécute le client, arrête le serveur, propage le code de sortie.

## Utilisation

```sh
# 1. Construire le binaire de test (depuis la racine du dépôt) :
make -C mcu -f Makefile.rpm wstest      # → bin/debug/wstest

# 2. Lancer le harnais complet :
python3 test/websocket/run.py           # code de sortie != 0 si un test échoue
python3 test/websocket/run.py --verbose # logs serveur en debug (-d)

# (ou piloter séparément :)
bin/debug/wstest 9001 -d &              # serveur d'écho sur le port 9001
python3 test/websocket/ws_client.py --port 9001
```

## WSS (TLS) — Phase 2

`wstest` écoute en WSS avec `--secure --cert <fichier> --key <fichier>`.
`run.py --tls` génère automatiquement un certificat auto-signé (`wstest.crt`/
`wstest.key` via `openssl req`) puis lance le client en `wss://` :

```sh
python3 test/websocket/run.py --tls           # suite complète en WSS
# vérif indépendante :
bin/debug/wstest 9077 --secure --cert test/websocket/wstest.crt --key test/websocket/wstest.key &
openssl s_client -connect 127.0.0.1:9077 -servername localhost
```

Le client Python utilise le module `ssl` standard (vérification de certificat
désactivée) : c'est une pile TLS indépendante qui valide le serveur.

## Bugs révélés par ce harnais (corrigés en Phase 0)

Tous **préexistants** (commit `0969d35` « passage de websocketconnection en
stdlib »), pas introduits par le refactor de transport :

1. **Socketpair de réveil inversée** : on écrivait le signal dans
   `wakeup_socket[1]` mais `poll()` surveillait `wakeup_socket[1]` (au lieu de
   `[0]`) → le `POLLOUT` n'était jamais armé, aucune donnée sortante n'était
   émise (même la réponse 101).
2. **Auto-deadlock de `SendMessage`** : `SendMessage()` tient `mutex` puis
   appelait `SignalWriteNeeded()` qui re-verrouille le même `std::mutex` non
   récursif → thread de connexion figé. Corrigé via une variante
   `SignalWriteNeededUnlocked()` (appelée quand le mutex est déjà tenu).
