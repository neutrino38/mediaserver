# Recette : le réacteur RTP

## À quoi sert cette fiche

Le chantier « réacteur RTP » remplace une boucle `poll()` par session RTP par un
**réacteur partagé** : un thread et un seul `poll()` pour un groupe de sessions.
Puis il retire les threads consommateurs, les paquets étant livrés par callback.

Conception et lots : `docs/conception/RTP-REACTOR/SPEC.md`.

Les tests automatiques prouvent la mécanique du réacteur. Ils ne prouvent pas
qu'un appel réel reste bon : ni la gigue, ni le handshake, ni le contrôle de
débit. Cette fiche donne la recette à jouer en appel réel, lot par lot.

Jouez-la après chaque lot, et après tout changement dans `RTPSession`,
`RtpSessionSet` ou un consommateur de paquets RTP.

## Ce qui est déjà couvert par les tests

Ne rejouez pas ces points à la main.

```sh
cd mcu && make check
```

La suite `RtpSessionSet*` (`tests/test_rtpsessionset.cpp`) fixe : un seul thread
pour N handlers, l'aiguillage de chaque événement au bon handler, le minimum des
échéances, `OnPeriodic` appelé même sur le réveil d'un voisin, le retrait
**synchrone** (le contrat qui autorise l'appelant à fermer ses sockets), le
retrait réentrant non bloquant, et le fait qu'un socket mort ne rende pas sourd
son groupe.

Les suites déjà en place et à surveiller comme garde-fous : `RTPLatching*`,
`RTPRenegotiation*`, `RTPStreamRace*`, `EndpointTeardown*`, `DataChannel*`,
`SctpLoopback*`, `RateControl*`, `IPv6*` (`make check-ipv6`).

## Avant de commencer

1. Binaire déployé selon la convention IVèS :

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release
ln -s /home/<user>/mediaserver/bin/debug/mcu mediaserver
systemctl restart mediaserver
tail -f /var/log/mcu.log
```

2. Un contrôleur, un navigateur WebRTC et un Linphone.

### Par quelle API recetter ?

Le réacteur est le même pour les deux : au lot 2, **toute** `RTPSession` y est,
quelle que soit l'API qui l'a créée. Mais les deux ne couvrent pas les mêmes
chemins, et il faut les deux pour clore un lot.

| | API MCU (`/mcu`, conférences) | API JSR-309 (`/jsr309`) |
|---|---|---|
| Objet porteur | `RTPParticipant` : 4 sessions (audio, vidéo MAIN, vidéo SLIDES, texte) | `Endpoint` : 4 `RTPEndpoint`, `Init()`és même sans négociation |
| Consommateurs | `recAudioThread`, `recVideoThread` ×2, `recTextThread` | un `MultiplexLoop` par jambe reçue |
| Ce qu'elle couvre en propre | mixage, mosaïques, **partage BFCP** (deux consommateurs sur UNE session), enregistrement MP4 | **transcodage B2BUA**, contrôle de débit (séance de mesure BWE), 4 jambes ouvertes pour rien |

**Commencer par l'API MCU.** Elle est la plus simple à piloter et elle exerce le
réacteur de bout en bout.

Elle est aussi la seule à produire le cas de **deux consommateurs sur une même
session** — le partage de document BFCP, où la jambe SLIDES lit la session de
MAIN sur un autre SSRC. Faute de client BFCP disponible, ce cas n'est PAS joué
en recette : il est figé par
`RtpReactor.TwoConsumersOnOneSessionEachGetItsOwnSsrc`
(`mcu/tests/test_rtp_reactor.cpp`), deux threads lisant une même session sur
deux SSRC. C'est la seule preuve qu'on en a — à rejouer, et à ne pas
désactiver.

**Mais deux points ne s'obtiennent que par l'API JSR-309**, et ils ne sont pas
optionnels :

- **La séance de mesure du contrôle de débit** (§ recette du lot 2, point 6) :
  c'est le risque R8, la réception du RTCP qui a changé de thread. Protocole et
  outillage dans `mcu/tests/tools/README.md` ; mécanismes mesurés dans
  `docs/RATE-CONTROL.md`. Se joue sur un transcodeur JSR-309.
- **Le compte de threads d'un `Endpoint`** : il ouvre 4 sessions et 8 ports UDP
  même pour un appel audio seul. C'est là que le gain du lot est le plus gros,
  donc là que son absence se verrait le mieux.

L'API MCU est documentée dans `docs/MCU-API.md`, la JSR-309 dans
`docs/JSR-309-API.md`.

3. L'outil de recensement des threads :

```sh
mcu/tests/tools/thread_census.sh            # une photo
mcu/tests/tools/thread_census.sh "" 60      # 60 s : min, max, moyenne, CPU
```

Les threads ne portent pas de nom dans ce binaire : **c'est le TOTAL qui est le
signal**, pas les noms.

### Deux choses à savoir avant de lire un total

**Le total est bruité de ±2.** Le serveur HTTP XML-RPC (Abyss, xmlrpc-c) gère un
pool de threads qui grandit et rétrécit tout seul. Mesuré le 2026-08-31, à
scénario CONSTANT et avec pour seul trafic quelques `curl /status/general` :
25 → 26 → 24 threads. Un écart de 2 ou 3 sur une photo unique ne prouve donc
rien. Conséquence pratique : **mesurer sur le scénario à 3 participants**, où le
gain attendu dépasse le bruit, et prendre plusieurs photos.

**Le total ne revient pas au repos après un raccroché, et c'est normal.** Une
conférence MCU survit au départ de son dernier participant : `/status/general`
rend alors `conferences: 1, participants: 0`. Ses workers restent — mixeur
audio, mixeur vidéo, `TextEncoder`, `AudioEncoderWorker`, les consommateurs des
pipes texte — et ne s'arrêtent qu'à `DeleteConference`. Pour comparer au repos,
il faut donc supprimer la conférence, ou comparer « conférence vide » à
« conférence vide », pas à « serveur neuf ».

Pour nommer les threads qui restent, quand un doute subsiste :

```sh
sudo gdb -p $(pgrep -x mediaserver) -batch -ex "set pagination off" \
	-ex "thread apply all bt 6" | grep -E "^Thread|::Run|Worker::"
```

## Mesure de référence, à prendre AVANT le lot 2

Trois scénarios, chacun mesuré 60 s, à noter dans le compte rendu de séance :

| Scénario | Ce qu'on note |
|---|---|
| serveur au repos, aucun appel | total de threads, CPU |
| un appel JSR-309 audio + vidéo | total, CPU, delta par rapport au repos |
| une conférence à 3 participants RTP | total, CPU, delta |

Sans ces trois chiffres, aucun lot suivant n'est mesurable.

### L'attente active, à mesurer pendant la SONNERIE

C'est le seul gaspillage CPU franc du chemin actuel : entre `StartReceiving` et
le premier paquet du pair, chaque jambe reçue tourne à ~3 kHz (§1.3 de la
conception). Procédure :

1. Établir un appel et **laisser sonner** sans décrocher, 30 s.
2. Relever le CPU du processus pendant cette phase :

```sh
mcu/tests/tools/thread_census.sh "" 30
```

Attendu avant le lot 3 : une consommation non nulle alors qu'aucun média ne
circule. Attendu après le lot 3 : indiscernable du repos.

## Nombres de threads attendus

Par jambe, deux réacteurs : `{audio, texte}` et `{vidéo MAIN, SLIDES}`.

| | Avant | Lot 2 (1 réacteur global) | Lot 4a (2 par jambe) | Lot 4c (push) |
|---|---|---|---|---|
| `RTPParticipant`, 4 sessions | 14 | 10 | 12 | 8 |
| `Endpoint` JSR-309, 2 jambes reçues | 6 | 2 | 4 | 2 |
| Appel B2BUA | 12 | 4 | 8 | 4 |

Le lot 4a **remonte** le compte : c'est voulu, il prépare le push. Un total qui
ne bouge pas au lot 2 signifie que les sessions ne sont pas inscrites dans le
réacteur — ce n'est pas une bonne nouvelle, c'est un lot qui n'a rien fait.

## Recette par lot

### Lot 2 — la boucle poll devient un réacteur

À jouer, dans cet ordre :

1. **Appel SIP audio.** Son dans les deux sens, pas de coupure sur 5 min.
2. **Appel SIP audio + vidéo.** Image dans les deux sens. Provoquer une perte
   (couper le Wi-Fi 2 s) et vérifier la reprise d'image.
3. **Appel WebRTC.** C'est le test du handshake : ICE puis DTLS puis SRTP. Dans
   le journal, `Got ports`, la mise en place DTLS, le premier paquet reçu.
4. **Data channel T.140.** Le texte doit circuler : la pile SCTP n'a pas de
   thread, c'est le réacteur qui bat ses timers (`onApplicationTick`).
5. **Raccrocher, puis rappeler 5 fois de suite.** Le démontage est le point dur
   du lot : chaque `End()` doit passer par un retrait synchrone avant de fermer
   ses sockets. Aucun `descripteur invalide` ni `handler encore inscrit` dans le
   journal.
6. **Contrôle de débit.** Rejouer une séance de mesure BWE (protocole complet
   dans `mcu/tests/tools/README.md` : binaire lancé avec `-d`,
   `netem_scenario.sh` pour la dégradation, `bwe_report.py` pour le
   dépouillement) et comparer : nombre de TMMBR émis, trames clés par minute,
   débit acquitté. La réception du RTCP a changé de thread — c'est le risque R8.

Signaux d'alerte dans `/var/log/mcu.log` :

| Trace | Ce qu'elle dit |
|---|---|
| `tour long : N ms` | un callback a fait attendre les autres jambes du groupe |
| `descripteur invalide, retrait du handler` | un socket a été fermé sans passer par `Remove` |
| `detruit avec N handler(s) encore inscrit(s)` | un groupe meurt avant ses sessions |
| `evenement socket 0x...` | une jambe est sortie du groupe sur POLLERR/POLLHUP |

### Lot 3 — attente bornée

1. **Sonnerie sans décrocher, 30 s** : CPU indiscernable du repos. Relever la
   trace `attente active : N GetPacket a vide en M ms` sur chaque jambe reçue :
   **5 pour 1000 ms**, pas ~2 250. C'est la borne de 200 ms
   (`RTPSession::ConsumerPollMs`) qu'on lit là, une jambe qui donne autre chose
   est une régression.
2. **Pair muet** : établir l'appel avec du média dans les deux sens, couper
   l'émission du pair, observer. Le watchdog doit rendre `onRTPTimeout` **une
   seule fois** par jambe muette, et le CPU **descendre** puis rester plat.

   **`kill -STOP` du client, jamais `kill -9`.** Tuer le processus ferme aussi sa
   socket SIP : la signalisation le voit immédiatement, le contrôleur raccroche,
   et le média n'a jamais le temps de devenir muet — on remesure alors un
   raccroché ordinaire. Gelé, le client cesse d'émettre du RTP mais sa socket
   reste ouverte. Le mute de Linphone ne convient pas non plus : il continue
   d'émettre.

   **La phase muette ne dure pas 60 s**, et c'est structurel : le chien de garde
   tombe à 10 s (`EndpointStartRTPTimeout`, 10 000 ms posés par le contrôleur) et
   celui-ci raccroche ~5 s après l'`EndpointDisconnectedEvent`. On dispose donc
   de ~15 s de régime muet — assez pour juger la platitude, à condition
   d'échantillonner le CPU **à la seconde** : un min/max/moyenne sur toute la
   fenêtre mélange l'appel vivant, le silence et le démontage, et ne conclut
   rien.
3. `StopReceiving` puis `StartReceiving` sur la même jambe, 5 fois : pas de
   blocage, pas de thread orphelin. Se pilote en XML-RPC sans aucun pair, donc
   **sur une instance à part** (`bin/debug/mcu --http-port 9091 --rtmp-port 1936
   --websocket-port 8101 --min-rtp-port 40000 --max-rtp-port 40999 -d`) : le
   serveur partagé garde son contrôleur, et une erreur de pilotage ne coûte rien.
   Attendu : `StartReceiving` de l'ordre de la milliseconde, et un total de
   threads qui ne monte pas cycle après cycle.

   Attendu : **moins de 2 ms** par `StopReceiving` sur l'audio et le texte, même
   quand la jambe n'a jamais reçu un paquet. La **vidéo reste à ~102 ms**, et
   c'est normal : `VideoStream::StopReceiving` sonde par `msleep(100000)` au lieu
   d'attendre une condition. Un audio ou un texte qui remonte à ~200 ms signale
   que le réveil des attentes de naissance a été perdu
   (`RTPSession::CancelGetPacket` → `OnStreamsChanged`).

   **Et surtout : ce scénario est celui qui a trouvé la course de démarrage.** Un
   `StopReceiving` qui ne rend JAMAIS la main veut dire que le thread
   consommateur a écrasé le `TaskStopping` de son appelant par `TaskRunning` en
   démarrant. Signature : `pthread_join` éternel dans la pile du thread XML-RPC,
   drapeau `receivingX == TaskRunning`, et un processus que SIGTERM ne tue plus.
   Enchaîner Start/Stop sans délai est ce qui la fait sortir.

### Lot 4a — deux groupes par jambe

Le compte de threads doit remonter comme au tableau. Puis vérifier l'isolation :

1. Appel transcodé 720p, qui charge le réacteur vidéo.
2. Pendant ce temps, écouter l'audio. **Aucune gigue audible.** C'est tout
   l'objet du découpage : si l'audio grésille, le découpage n'est pas appliqué.
3. Relever les traces `tour long` : elles doivent viser le groupe vidéo, jamais
   le groupe audio. Le nom du groupe dit lequel : `part-<id du participant>-media`
   ou `-video` côté MCU, `jsr309-<adresse de l'Endpoint>-media` ou `-video` côté
   JSR-309.

### Lot 4b et 4c — livraison poussée

Un site à la fois, recette entre chaque : texte, puis audio, puis
`RTPEndpoint`, puis vidéo.

1. Après chaque bascule : appel bidirectionnel du média concerné, 5 min.
2. **Conférence à 3 participants** : audio mixé propre, mosaïque à jour.
3. **Partage de document BFCP** : les deux flux vidéo d'une même session (MAIN
   et SLIDES) doivent arriver tous les deux. C'est le cas que l'ancien modèle
   servait avec deux threads sur une même session.
4. **Enregistrement MP4** : lire le fichier, vérifier audio, vidéo et sous-titres.
5. **Appel B2BUA transcodé, 20 min** : ni dérive de gigue, ni trame clé
   parasite. Comparer au relevé du lot 2.

## Passe ThreadSanitizer

Le chantier remplace une séparation par threads par une séparation par verrous
et par instantané. ThreadSanitizer voit ce qu'un test fonctionnel rate. À
rejouer après tout changement dans `RtpSessionSet` ou `RTPSession`.

```sh
cd mcu
make mkdirs TAG=tsan                       # une seule fois
make tests TSAN=yes TAG=tsan               # objets isolés du build normal
TSAN_OPTIONS="halt_on_error=0 detect_deadlocks=0" \
	./tests/runtests --gtest_filter='RtpSessionSet*:RTP*:EndpointTeardown*'
```

## Compte rendu de séance

À écrire à chaque séance, dans le commit ou dans la fiche mémoire du chantier :

- le lot joué, le binaire (commit) ;
- les trois totaux de threads, avant et après ;
- les scénarios joués, et lesquels ont échoué ;
- les traces d'alerte relevées, avec leur contexte ;
- pour tout lot touchant la réception RTCP : le verdict de `bwe_report.py`.
