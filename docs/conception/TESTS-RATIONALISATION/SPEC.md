# Rationaliser les tests automatisés du mediaserver

Revue des deux suites GoogleTest : `mcu/tests/` et
`third_party/fontventa/libmedikit/tests/`. Objectif : moins de tests, pas de
doublon, une suite plus rapide, **sans perdre un seul garde-fou**.

Mesures faites le 2026-09-12 sur la branche `feat/test-review`, une exécution
de chaque suite, sortie JSON de gtest. Les temps « après » sont des estimations,
sauf mention contraire.

---

## 1. Le constat en chiffres

| Suite | Tests | Temps mur | Temps CPU |
|---|---|---|---|
| `mcu/tests/runtests` | 714 (4 sautés, 1 désactivé) | 96 s | 12,6 s |
| `libmedikit/tests/runtests` | 203 (1 désactivé) | 11,7 s | 6,7 s |

Trois faits structurent tout le reste.

1. **La suite mcu attend 87 % du temps.** 36 tests de plus d'une seconde
   consomment 63 s sur 96. 495 tests tiennent sous 10 ms. Le problème n'est pas
   le nombre de tests, c'est une poignée d'attentes murales.
2. **Le vrai frein du développeur est la compilation.** Modifier un seul fichier
   de test coûte **2 min 51 s** de `make tests` (mesuré). Les 70 sources de test
   sont passées à g++ en une seule commande (`mcu/Makefile:360-363`), donc sans
   objets intermédiaires et sans `-j`. Même règle côté libmedikit
   (`libmedikit/Makefile:350-354`).
3. **Le temps se concentre dans six familles.** Le tableau ci-dessous donne le
   poids de chacune et ce que la revue propose.

| Famille | Tests avant | Tests après | Temps avant | Temps après |
|---|---|---|---|---|
| Transcodeurs, codecs, fichiers média, RTP applicatif | 214 | ~191 | 58,7 s | 12 à 16 s |
| Expiration, data channel, texte, WebSocket, status | 102 | 86 | 31,3 s | 13 s (3,5 s avec deux crochets de production) |
| Threads et primitives d'attente | 129 | 110 | 10,7 s | ~4 s |
| Adressage et IPv6 | 137 | 83 | 3,8 s | ~0,06 s |
| Régulation de débit | 120 | ~95 | 2,7 s | 2,7 s |
| Parseurs et suites adverses | 213 | 160 | 0,7 s | 0,5 s |
| **Total** | **917** | **~725** | **108 s** | **~35 s, ~25 s avec les crochets** |

---

## 2. Six leviers transverses

Ces leviers reviennent dans plusieurs familles. Les appliquer une fois vaut
mieux que corriger test par test.

### 2.1 Compiler les tests par objet, en parallèle

Remplacer la règle unique par une règle motif `$(BUILD)/tests/%.o : tests/%.cpp`
avec `-MMD`, puis un lien. Un fichier modifié recompile en quelques secondes,
et `make -j` s'applique. C'est le levier au meilleur rapport gain/effort de
toute la revue : il ne touche aucun test. Même chose dans libmedikit.

Au passage, `mcu/Makefile:370-374` (commentaire de `check-ipv6`) dit encore
« attendue en échec » alors que la suite est verte, et `check-ratecontrol` /
`check-senderbwe` passent `--gtest_also_run_disabled_tests` alors qu'il ne
reste aucun test désactivé dans ces suites.

### 2.2 Le ClientHello DTLS arrivé trop tôt (9 s) — corrigé en production, voir §5

`test_dcendpoint.cpp:139-140` et `test_textstream_datachannel.cpp:128-129`
appellent `a.Negotiate` (client DTLS) avant `b.Negotiate` (serveur). Le
ClientHello part tout de suite (`rtpsession.cpp:1379-1388`), B n'est pas
initialisé, le paquet est jeté (`dtls.cpp:604-606`), OpenSSL retransmet à 1 s.
Neuf tests sur onze perdent une seconde chacun. Inverser les deux appels dans
les deux `SetUp`. `test_conference_mixed_text.cpp` a déjà le bon ordre.

Note hors périmètre : la production paie la même seconde si le ClientHello d'un
navigateur arrive avant le `SetRemoteCryptoDTLS` du contrôleur.

### 2.3 Une horloge injectable là où la production lit le mur

Aucune classe du chemin JSR-309 n'a d'horloge injectable : tout passe par
`getTime()`/`getDifTime()`. Quatre sites forcent les tests à dormir :

| Site | Rôle | Tests qui dorment à cause de lui |
|---|---|---|
| `VideoTranscoder.cpp:523` `DueForEncoding()` | borne la cadence de sortie | `VideoEncoderInline.*` (19 s) |
| `VideoTranscoder.cpp:500` `lastFpsApply` | une application de cadence par 5 s | idem |
| `VideoEncoderWorker.cpp:582-583` `MinForcedIntraUs` | une intra forcée par seconde | `UneRafaleDeDemandesDIntra…` (2,7 s) |
| `ffmp4reader.cpp:817` `getDifTime(&startPlaying)` | cadencement de la lecture MP4 | `Mp4ReadOrder`, `Mp4ReadTiti`, `Mp4Transcode` (3,7 s) |

Proposition : un `std::function<QWORD()> nowUs` (défaut `getTime`) sur
`VideoTranscoder` et `VideoEncoderMultiplexerWorker`, posé par
`SetClockForTests` ; un drapeau « sans cadencement » sur
`Mp4FfReader::GetNextFrame`. Le lisseur (pacer) reste réel : il ne coûte que
100 ms par test qui lit sa sortie. Deux autres crochets du même ordre :
`StartSweeper` en millisecondes (`eventqueuesweeper.h:86-92` multiplie des
secondes entières, d'où 16 s de tests d'expiration) et le tick du mixeur texte
(`textmixer.cpp:107`, 200 ms câblés, 3,5 s de tests).

### 2.4 Des délais de retransmission injectables dans StunClient

`StunClientProbe.UnServeurMuetEstUnEchecEtPasUnVerdict` dure **3,5 s** : les
délais `{500, 1000, 2000}` sont figés (`stunclient.cpp:25`). Un setter de test,
ou un paramètre optionnel de `Discover`, ramène le test à 20 ms. Un faux
serveur muet qui compte les requêtes ajouterait la preuve des trois
retransmissions, absente aujourd'hui. `StunClient::Probe` n'a d'ailleurs aucun
appelant en production (`main.cpp:514,526` n'utilise que `ParseServer` et
`Discover`).

### 2.5 Le témoin ordonné remplace l'attente négative

Un test qui prouve « ce paquet est jeté » attend aujourd'hui 400 à 600 ms sans
rien recevoir (`test_rtp_renegotiation.cpp:868`, `test_rtp_endpoint_codec.cpp`,
`test_rtp_latching.cpp:294`, `test_rpsi.cpp`). Sur une session RTP, un seul
socket et un seul thread `Run` traitent les paquets en ordre
(`docs/reference/threads-rtp.md`). Il suffit d'envoyer, derrière le paquet à
jeter, un paquet accepté d'un autre SSRC : quand le témoin arrive, le précédent
a été traité et n'a rien produit. L'attente négative devient positive, ~50 ms.
Huit tests concernés, ~4 s gagnées.

### 2.6 Des tables de cas à la place de N tests jumeaux

Quand N tests exercent la même ligne de production avec des entrées
différentes, une boucle sur un tableau avec `SCOPED_TRACE(nom)` garde le
diagnostic nominatif et divise le nombre par N. Un `TEST_P` ne réduit pas le
nombre affiché par gtest ; la boucle, si. Candidats : les 15 « longueur
menteuse » de `test_rtcp_hardening.cpp`, les 5 rejets d'en-tête RTP, les
drapeaux VP8, les 22 tests de l'amortisseur de débit (§3).

---

## 3. Régulation de débit : rendre les cas visibles

Sept fichiers, 120 tests, 2,7 s. La famille est déjà rapide. Son problème est
la **lisibilité de la couverture** : les tests portent des noms d'incidents et
des dates, quatre helpers différents nourrissent la même classe, et personne ne
peut dire quels cas sont couverts sans relire 1 600 lignes. La revue a bâti la
matrice, et la matrice a trouvé deux cas non couverts qui se comportent mal.

### 3.1 L'amortisseur (`RembThrottler`) : une table de décision

`RembThrottler` (`mcu/include/rembthrottler.h`) est une fonction de décision
pure. Ses entrées :

| Dimension | Valeurs |
|---|---|
| source de la décision | mesure locale (`OnEstimateChanged`) / plafond externe (`SetMaxBitrate`) |
| dialecte | REMB (période 200 ms, baisse franche 3 %) / TMMBR (pas de période, hausse franche 20 %, baisse franche 10 %) |
| variation par rapport à la dernière annonce | baisse franche / baisse dans le bruit / identique / hausse dans le bruit / hausse franche |
| temps depuis la dernière annonce | dans la période / après (REMB seulement) |
| congestion mesurée | oui / non (mesure locale seulement) |
| ce que le pair émet | inconnu / respecte la limite / la dépasse |
| plafond externe actif | non / oui, qui borne / oui, qui ne borne pas |

Les 22 tests actuels de `RateControlThrottler` sont des lignes éparses de
cette table. Chacun construit un amortisseur, fait une première annonce, puis
un ou deux appels. Ils se remplacent par **un test à table** (une ligne =
état de départ, appel, résultat attendu) plus **trois scénarios** à séquence :
`LePlafondSurvitAUneMesureBasse`, `UnPlafondIdentiqueNeRepartPasEnTMMBR`
(la boucle de 232 s) et `ApresUnSuiviLaRemonteeSeJugeContreLaDerniereAnnonce`.

Recouvrements déjà visibles dans l'existant :

- `EnDialecteTMMBRUneBaisseFranchePartToujoursImmediatement` (−15 %) est
  contenu dans `EnDialecteTMMBRUneBaisseDansLeBruitNEmetPas` (−5 % non,
  −10 % oui) ;
- `LeDialecteREMBGardeSesSeuils` contient `UneHausseAttendLaPeriode` et
  `UneBaisseFranchePartImmediatement` ;
- `UnPlafondIdentiqueNeRepartPasEnTMMBR` et
  `EnTMMBRLePlafondSuitLaMemeAsymetrieQueLaMesure` sont deux lignes d'une même
  table « plafond externe en TMMBR ».

Cases de la matrice **sans aucun test** aujourd'hui, et ce qu'elles donnent :

| Cas | Résultat | Statut |
|---|---|---|
| TMMBR, mesure locale, hausse franche (+30 %), plafond externe actif qui borne | **émet un TMMBR de valeur identique au plafond** (exécuté : `emis=1 out=500000`). Le test `step` compare la mesure à `lastSent`, puis `Compose` rend le plafond inchangé. À chaque pas de +20 % de la mesure locale sous plafond, un TMMBR redondant part. C'est le défaut que le correctif du relais a fermé pour `SetMaxBitrate` et pas pour `OnEstimateChanged` | **défaut confirmé** |
| TMMBR, plafond externe relevé (500 → 700 kb/s), pair qui émet 800 kb/s | émis (exécuté). `RaiseIsInformative` compare le débit du pair à la **mesure** (`lastSent`, 2 Mb/s), pas à la valeur **annoncée** (500 kb/s). La règle écrite dit « un pair qui dépasse déjà la limite annoncée n'apprend rien » | **contredit la doc, à trancher** |
| REMB, plafond externe qui ne mord pas, après la période | par lecture : émet la mesure inchangée | à vérifier |
| `SetMaxBitrate` avant toute annonce (`hasSent` faux) | par lecture : émet le plafond seul | à vérifier |
| Levée du plafond (`NoLimit`) en TMMBR : la hausse vers la mesure est-elle un pas franc ? | non testé (couvert en REMB seulement) | à couvrir |
| Frontière de tolérance du pair (`PeerOverLimitPercent` = 105) : pair à 1,05 × et 1,06 × la limite | non testé | à couvrir |

Les deux premiers cas ont été reproduits par un programme de dix lignes contre
`rembthrottler.h`, hors gtest. Aucun correctif n'a été appliqué.

### 3.2 Le détecteur (`RemoteRateControl`) : un scénario, des phases

Dix-neuf tests répartis dans quatre suites (`RateControlDetector`,
`RateControlThreshold`, `RateControlJitter`, `RateControlLoss`) nourrissent la
même classe par **quatre helpers différents** : boucles brutes,
`OveruseCounter::Feed`, `FeedJittered`, `FeedLossPhase`. Un seul constructeur
de scénario, fait de phases (régulier, dérive, à-coup, gigue, rafale GOP,
rapports de perte, RTT) et observé par `OveruseCounter`, les remplace. Le test
devient une table : suite de phases, région, résultat attendu (épisodes,
hypothèse finale).

Doublons trouvés :

- `RateControlLoss.UnDelaiSainNEffacePasLAccumulationDesPertes` est un
  sous-ensemble strict de `UnePerteMassiveEstUneCongestion` : mêmes arrivées
  exactes, mêmes rapports de 60 pertes toutes les 30 images, seule la longueur
  diffère ;
- la dérive de 2 ms par image est assertée quatre fois (`UneDeriveDeDelai…`,
  `UneVraieCongestionResteVueEnRegionNearMax`, et comme prérequis de
  `LeCalmeRevient…` et `LeBruitEstGele…`) ;
- `QuelquesPertesRaresNeSontPasUneCongestion` (0,3 %) est couvert par la phase
  à 1 % de `LeSeuilDePerteEstFranchiDansLesDeuxSens`.

Les trois tests qui lisent l'intérieur du filtre de Kalman (`GetNoise`,
`CovarianceIsPositiveSemiDefinite`) restent à part : ils gardent des invariants
numériques, pas un scénario.

Trou de couverture : le critère RTT (« précédent > 40 ms et nouveau > 1,5 × »)
et son expiration ne sont testés nulle part directement. `ForceOveruseViaRtt`
n'est qu'un outil des tests de l'estimateur.

### 3.3 L'estimateur (`RemoteRateEstimator`) et le frein

- `LEstimationSuitUnFluxRegulier`, `LEstimationSuitLeCheminDeProduction` et
  `LEstimationArriveDansLesPremieresSecondes` assertent la même fourchette.
  Garder les deux derniers (chemin par paquet, contrainte de délai), retirer
  le premier.
- Le trio « plafond fenêtré » (`UnTrouDEmission…`, `UneBaisseDurable…`,
  `UneCoupureDePlusDe5s…`) partage 65 s de préparation : une table de trois
  continuations. `RateControlBrake.LEstimationSuitLePlafondAuLieuDeGeler`
  affirme la même propriété que `UneBaisseDurable…` (une source qui baisse
  durablement fait suivre l'estimation) : fusionner.
- `DesRapportsDePerteRapprochesNeDescendentQuUneFois` et
  `LeFreinLaissePasserLaReactionSuivante` ne diffèrent que par l'instant du
  rapport : une table à deux lignes (10 ms, 500 ms).
- Les deux tests de cycle de vie des listeners (`UnListenerPeutInterroger…`,
  `RemoveListenerAttendLaNotificationEnVol`) sont une autre préoccupation :
  suite `RateControlListener` à part. Le second dort 800 ms
  (`usleep(300000)` + `usleep(500000)`) ; 100 + 150 ms suffisent.

### 3.4 Les autres fichiers de la famille

`test_sender_bwe.cpp`, `test_bitrate_probe.cpp`, `test_transport_feedback.cpp`,
`test_transport_feedback_generator.cpp` sont déjà en horloge simulée, courts et
sans doublon notable. Fusionner les deux fichiers transport-feedback (format et
générateur partagent `WireRoundTrip`, copié deux fois). `test_rtp_pacer.cpp`
(1,1 s) et `TransportCCWiring.SansNegociationAucunRapport` (640 ms, attente
négative) relèvent du témoin ordonné (§2.5).

### 3.5 Organisation cible

| Fichier | Contenu | Tests |
|---|---|---|
| `test_remb_throttler.cpp` | table de décision + 3 scénarios | 22 → ~5 tests (30 lignes de table) |
| `test_overuse_detector.cpp` | scénarios par phases + 3 invariants Kalman | 19 → ~8 |
| `test_remote_rate_estimator.cpp` | AIMD, plafond fenêtré, frein, listeners | 15 → ~11 |
| `test_sender_bwe.cpp` | inchangé | 17 |
| `test_transport_feedback.cpp` | format + générateur + historique | 28 → 28 |
| `test_transport_cc_wiring.cpp`, `test_bitrate_probe.cpp`, `test_rtp_pacer.cpp` | inchangés | 17 |
| **Total** | | **120 → ~95** |

`docs/RATE-CONTROL.md` §14 et §17 citent les fichiers par nom : à mettre à jour
dans le même jeu de changements.

---

## 4. Les autres familles, en bref

Le détail (noms exacts, lignes de production, verdict doublon strict ou
partiel) est dans les rapports de revue ; cette section garde ce qui décide.

### 4.1 Transcodeurs, codecs, fichiers média (214 tests, 58,7 s)

- **Supprimer `test_transcoder_characterization.cpp`** : son en-tête dit que sa
  seule raison d'être était de comparer les lots 1 à 4 du chantier, qui sont
  faits. Trois tests sont des doublons des tests inline ; le seul unique
  (`VideoLossRequestsAnIntraFromTheSource`) part dans
  `test_video_decoder_inline.cpp`. −3,2 s.
- **`VideoEncoderInline` (19 s)** : `UnePauseNeFaitPasTomberLaCadence` ne
  pouvait plus échouer depuis la tenue de 20 s ajoutée le 2026-09-02 (une pause
  de 5 s n'atteint jamais `FpsDropHoldTicks`) ; **réécrit** avec une pause de
  25 s par saut de pts, vérifié par mutation (3 s → ~0,3 s). `ApresUnePauseLaFenetreDoitSeRemplirAvantDAgir` est
  pour moitié masqué par la même règle, pour moitié identique à
  `UneSourceLenteAbaisseLaCadence…`, lui-même recouvert par la recette
  `VideoUneBaisseDeCadenceNEstAppliqueeQueSiElleDure`. Six tests de cadence
  deviennent quatre, tous par pts.
- **Recette vidéo** : deux images par phase au lieu de cinq (le SSRC neuf est
  visible dès la première image du nouveau run) ; fusionner adaptatif et non
  adaptatif. 9,2 s → ~2,5 s.
- **`RtpLatching`** : le premier des deux tests de caractérisation est
  **corrigé et inversé** (`AMovedPeerIsFollowedEvenWhenSendingContinues`, §5) ;
  `PortOnlyMappingChangeIsNotFollowed` reste caractérisé, décision du
  mainteneur. `kDenyTimeoutMs` 600 → 300 reste à faire.
- **libmedikit MP4** : `Mp4Prologue` écrit **le même fichier deux fois** avec
  les mêmes arguments (`WriteWithDelay(path, true, 10, 0)` lignes 608 et 708)
  et dort 330 ms par test ; `SetInitialDelay(330)` remplace le `usleep`, un
  `SetUpTestSuite` partage le fichier. Quatre fichiers de lecture
  (`mp4_read`, `mp4_read_titi`, `mp4_read_order`, `mp4_roundtrip`) ouvrent
  les mêmes fixtures : un seul `test_mp4_reader.cpp`.
- `Vp8Realtime.UneImage720pCouteMoinsDe33ms` (1,3 s) est une mesure de
  performance dépendante de la machine : vérifier les options posées sur le
  contexte, ou la sortir dans une cible `check-perf`.
- `MosaicComposition.EveryCompositionTypeComposes` (1,1 s) ne vérifie que
  taille et format : une toile CIF suffit, la géométrie HD reste couverte par
  `SlotsAlwaysFitInsideTheComposite`.

### 4.2 Expiration, data channel, texte (102 tests, 31,3 s)

- Ordre DTLS (§2.2) : −9 s sans toucher la production.
- **Expiration** : `McuConferenceExpiry` rejoue la politique entière du
  balayeur (7 tests miroir de `JSR309SessionExpiry`) alors que le MCU n'y
  apporte qu'une ligne de filtre et l'absence de cascade. Garder côté MCU les
  trois tests qui touchent `mcu.cpp`. Les tests négatifs par sommeil
  (`*WithoutQueueIsNeverSwept` 2,5 s, `*ZeroGrace…` 1,5 s) deviennent
  différentiels : une session à `queueId` 0 qui survit pendant que l'autre
  part. Avec une grâce en millisecondes (§2.3), chaque test tombe à 100-300 ms.
- **Data channel** : `LUTF8MultiOctetsArriveIntact` est joué trois fois (SCTP,
  DCEndpoint, TextStream) pour une couche qui copie des octets ; les tests
  « le canal s'ouvre » sont contenus dans « le texte traverse ». 6 → 3 et
  5 → 3.
- `WebSocketHandshake.UneRequeteDUnSeulTenantEstComprise` envoie la même
  requête que l'écho : reporter la vérification de `Sec-WebSocket-Accept` dans
  l'écho et supprimer.
- `TEST.md` et `mcu/tests/README.md` ne citent aucun des six fichiers data
  channel.

### 4.3 Threads et primitives d'attente (129 tests, 10,7 s)

- `WaitQueue`, `XmlEventQueue` et `VideoPipe` héritent tous de `::Wait` :
  leurs tests de réveil, cancel et timeout rejouent `wait.h:118-165`. Neuf
  doublons stricts, dont « cancel est collant » testé cinq fois.
- Trois tests `WaitSignal` sont **fragiles sous charge** : le signaleur dort
  50 ms puis signale ; si le waiter n'est pas encore entré, le signal est perdu
  par conception (`wait.h:66-70`). Le signaleur doit boucler jusqu'au succès.
- `WorkerBase.StartRunsAndStopJoins` dort 150 ms pour vérifier qu'un thread
  déjà joint ne compte plus : tautologique.
- Les tests de course qui n'ont de valeur que sous ASan ou TSan
  (`RTPStreamRace`, `EndpointTeardown`, `Rtmp*Race`) restent : ils ont chacun
  trouvé un défaut réel.
- Scinder `test_wait_primitives.cpp` (1 150 lignes) : `RTPBuffer` n'est pas
  bâti sur `Wait` et mérite son fichier.
- Erreur factuelle dans la production : `worker.h:47-49` affirme qu'aucun site
  n'est converti alors que 16 classes dérivent de `Worker`. Les en-têtes de
  `test_wait_primitives.cpp`, `test_wait_sites.cpp`, `test_use.cpp` et
  `test_worker.cpp` disent encore « avant la migration ».

### 4.4 Adressage et IPv6 (137 tests, 3,8 s)

- `test_ipv6.cpp` a été écrit avant l'implémentation. Il teste aujourd'hui la
  brique `IPAddress` une seconde fois, à travers trois lignes de `RTPSession`
  (`rtpsession.cpp:1177`, `:198`, `:1561`). Ce qui vaut d'être conservé est le
  **câblage** : une preuve par appelant que la brique est appelée. 41 → 13.
- Trois tests tautologiques : `IPv6Dns.UnHoteDoublePileChoisitDeFaconDeterministe`
  compare deux lectures de la même chaîne ; `IPv6Mapped.UneAdresseMappeeEstEgale…`
  n'asserte aucune égalité ; `IPv6Dns.LAutodetectionVoitLesEnregistrementsAAAA`
  n'appelle jamais l'autodétection.
- Deux tests sautés peuvent tourner partout : `AddPublic` n'exige pas
  l'attachement, un littéral documentaire `2001:db8::1` suffit.
- Garder les deux garde-fous IPv4 que `CLAUDE.md` nomme.

### 4.5 Parseurs et suites adverses (213 tests, 0,7 s)

- Les 22 `EXPECT_EXIT` coûtent 125 ms au total : ce n'est pas un levier.
- `RtpAdversarial` (écrit avant les `*_hardening`) : deux doublons stricts et
  une tautologie (`WrongVersionDetectable` asserte un getter ; aucun code de
  production ne vérifie la version RTP). `RtcpAdversarial` garde une valeur
  propre : `IsRTCP` et la garde de niveau compound.
- RED : deux implémentations distinctes. Celle de libmedikit (`red.cpp`) n'est
  consommée que par `mp4format.cpp`, bâti seulement avec `ASTERISK=yes`. Pas
  un doublon ; mais la copie est en retard sur celle du mcu (voir §5).
- `Amf.NumberZeroDecodeQuirk` caractérise un bug toujours présent (`amf.cpp:491-503`),
  dont le correctif tient en une ligne : corriger et retourner le test.
- Fusions : `test_rtp_rtcp` + `test_rtp_header_hardening` → `test_rtp.cpp` ;
  les `Rtcp*` → `test_rtcp.cpp` ; les trois RTMP → `test_rtmp.cpp`.

---

## 5. Défauts de production relevés au passage

Décision du mainteneur (2026-09-12) : corriger. État :

| Où | Quoi | État |
|---|---|---|
| `rembthrottler.h` `OnEstimateChanged` | sous plafond externe qui borne, une hausse franche de la mesure locale réémettait un TMMBR de valeur identique | corrigé ; en dialecte collant une annonce identique ne part pas, la mesure est retenue pour la levée du plafond. Test `EnTMMBRUneHausseLocaleSousPlafondNeReditPasLePlafond` |
| `rembthrottler.h` `RaiseIsInformative` | comparait le débit du pair à la mesure locale, pas à la valeur annoncée | corrigé ; comparaison à `lastAnnounced`. Test `UneHausseDuPlafondQueLePairDepasseDejaNEmetPas` |
| `rtpsession.cpp` (réception DTLS) | un ClientHello reçu avant `SetRemoteCryptoDTLS` était jeté, le pair retransmettait à 1 s | corrigé ; le datagramme est gardé et rejoué après `dtls.Init()`. Preuve : les tests data channel qui négocient l'actif avant le passif passent de ~1 s à ~60 ms sans changer les fixtures |
| `rtpsession.cpp` `SetRemotePort` | l'observation périmée brûlait le one-shot après re-INVITE | corrigé ; observation oubliée avec la réouverture du droit. Test inversé et renommé `AMovedPeerIsFollowedEvenWhenSendingContinues`. Le changement de port seul (`PortOnlyMappingChangeIsNotFollowed`) reste caractérisé, décision du mainteneur |
| `amf.cpp` | `AMFNumber::GetNumber()` rendait 2^-1023 pour zéro | corrigé ; test `Amf.NumberZeroRoundTrip` (deux signes) |
| `libmedikit/red.cpp` | `skip` en `WORD` débordable ; `headers` non vidé sur longueur mensongère | corrigé ; `Red.TailleRedondanceMensongere` asserte `GetRedundantCount()==0` |
| `stunclient.cpp` | un littéral IPv6 finissait en « sans adresse IPv4 » | corrigé ; refus explicite « IPv6 non pris en charge », testé sur `[2001:db8::1]:3478` et `::1` |
| `worker.h` | commentaire « aucun site converti » | réécrit |

Le §2.2 (ordre DTLS dans les fixtures) n'est plus nécessaire : la production
absorbe l'ordre inverse. Les fixtures restent telles quelles et servent de
garde-fou au correctif.

## 6. Plan d'exécution recommandé

Chaque étape est un jeu de changements autonome, testable seul. L'ordre suit
le rapport gain/risque.

1. **Compilation par objet** (§2.1), les deux Makefiles. Aucun test touché.
   Gain : 2 min 51 s → quelques secondes par itération.
2. **Ordre DTLS** dans deux fixtures (§2.2). −9 s.
3. **Attentes courtes sans changement de sens** : `kDenyTimeoutMs`, `Drain`,
   sommeils « pour laisser démarrer » ramenés à 3-5 ticks du mécanisme, images
   par phase dans la recette, toile CIF, `SetInitialDelay` dans `Mp4Prologue`,
   attentes inutiles retirées (`RpsiWiring`, `StartRunsAndStopJoins`). −15 s
   environ.
4. **Témoin ordonné** sur les huit attentes négatives RTP (§2.5). −4 s.
5. **Suppressions strictes et tautologies**, famille par famille, un commit par
   fichier pour garder le `blame`.
6. **Tables de cas** : amortisseur de débit (§3.1) d'abord, avec les six cas
   manquants ajoutés ; puis détecteur (§3.2), RTCP, en-tête RTP, VP8.
7. **Crochets de production** (§2.3, §2.4) : horloge injectable JSR-309, grâce
   en millisecondes, tick du mixeur texte, délais STUN, lecture MP4 sans
   cadencement. À faire après le reste : chacun est une modification de
   production, petite mais à relire.
8. **Fusions de fichiers** et en-têtes à réécrire (`test_ipv6.cpp`,
   primitives d'attente, Makefile, `TEST.md`, `mcu/tests/README.md`,
   `docs/RATE-CONTROL.md` §14 et §17).

Pour toute suppression : la règle de `TEST.md` s'applique, un garde-fou dont
le pouvoir de détection a été vérifié par mutation ne se supprime pas, et
chaque fusion conserve les assertions des deux côtés.

---

## 7. Ce qui ne bouge pas

- Les garde-fous listés dans `TEST.md` « Vérification par mutation » :
  `MosaicGeometry.*`, `MosaicFactory.*`, les 11 `RtpLatching.*` de la
  politique NAT et les trois tests ICE.
- Les deux garde-fous IPv4 nommés par `CLAUDE.md`.
- Les tests de course à valeur ASan/TSan et les tests qui ont chacun un défaut
  réel derrière eux (listés par famille dans les rapports).
- Les cinq tests « sémantique piège » de `test_wait_sites.cpp` que
  `videopipe.h:16` et `xmlstreaminghandler.cpp:45-51` déclarent figés.
- `RtcpAdversarial.ParseRejectsLengthOverflow` (seul sur la garde compound et
  sur le paquet de la fuite corrigée).
