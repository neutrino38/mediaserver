# Notre contrôle de débit face à celui de WebRTC

Ce document compare le contrôle de débit de réception du mediaserver à celui de
la pile WebRTC de référence.

- **Nous** : `mcu/src/remoteratecontrol.cpp`, `mcu/include/remoteratecontrol.h`,
  `mcu/src/remoterateestimator.cpp`.
- **Témoin** : `../webrtc`, commit `e12c39e03c`,
  `modules/remote_bitrate_estimator/` et
  `modules/congestion_controller/goog_cc/link_capacity_estimator.cc`.

Les mesures citées viennent de `mcu/tests/tools/escalier/` (marche d'escalier
3000 → 750 → 3000 kb/s, 90 s par palier, file `-l 100`, boucle **ouverte** :
`mode=0`, aucun feedback ne part). Protocole : `mcu/tests/tools/README.md`.

## Verdict en une phrase

Le filtre de Kalman n'est plus le suspect : il est aligné ligne à ligne sur le
témoin. Ce qui reste diverge se trouve **autour** de lui — le seuil du détecteur,
et surtout la machine AIMD, qui n'a aucun frein temporel entre deux réactions.

## 1. Le filtre lui-même : plus d'écart

`UpdateKalman` (`mcu/src/remoteratecontrol.cpp:113-209`) et
`OveruseEstimator::Update` (`overuse_estimator.cc:31-103`) font la même chose,
dans le même ordre, avec les mêmes constantes.

| point | nous | témoin | état |
|---|---|---|---|
| état initial `E`, `varNoise`, `slope` | `remoteratecontrol.cpp:29-38` | `overuse_estimator.h:55-61` | identiques |
| `processNoise` `[1e-13 ; 1e-3]`, sans mise à l'échelle | `:35-36`, `:120-124` | `overuse_estimator.h:59`, `:46-54` | identiques |
| grandeur filtrée | `:99` — `curDelta`, la première différence | `:37` — `t_delta - ts_delta` | équivalents |
| écrêtage du résidu à ±3σ, signe gardé | `:152-155` | `:64-72` | identiques |
| bruit mesuré en état `Normal` seulement | `:160` | `:62-63`, `:120-122` | identiques |
| facteur d'oubli exponentié par la période d'image minimale sur 60 | `:142-147`, `:172` | `:105-115`, `:132` | identiques |
| bascule d'`alpha` 0,01 → 0,002 | `:166` — 300 images | `:127` — 300 deltas | équivalents |
| plancher `varNoise ≥ 1` | `:176-177` | `:136-138` | identiques |
| temporaires `e00`/`e01`, contrôle de covariance | `:194-205` | `:80-98` | identiques |

L'équivalence de la grandeur filtrée tient à un télescopage : `curDelta`
accumule `(arrivée − horodatage)` paquet par paquet sur toute l'image
(`remoteratecontrol.cpp:86`), et la somme vaut la différence entre le dernier
paquet de l'image et le dernier paquet de l'image précédente — exactement ce que
`InterArrival` produit par groupe (`inter_arrival.cc:56-90`). Un groupe du témoin
est d'ailleurs une image : les paquets d'une même image portent le même
horodatage RTP, donc `NewTimestampGroup` ne coupe pas
(`inter_arrival.cc:126-137`, seuil 5 ms, `remote_bitrate_estimator_single_stream.cc:40`).

## 2. Tableau des écarts, classés par impact

| # | écart | nous | témoin | ce qu'on observe | classe |
|---|---|---|---|---|---|
| É1 | aucun frein entre deux réactions de l'AIMD | `remoterateestimator.cpp:235` (`reactNow` inutilisé), `:570-572`, `remoteratecontrol.cpp:361` | `aimd_rate_control.cc:119-133`, `remote_bitrate_estimator_single_stream.cc:111-121` | 102 réestimations en 5,1 s, une toutes les 12 à 52 ms | **défaut réel** |
| É2 | la descente prend un cliquet sur une fenêtre de 200 ms | `remoterateestimator.cpp:16`, `:297`, `:351-358` | `remote_bitrate_estimator_single_stream.cc:54`, `bwe_defines.h:26`, `aimd_rate_control.cc:277-293`, `:311` | estimation à 233 kb/s pour 720 kb/s reçus | **défaut réel** |
| É3 | seuil de détection fixe | `remoteratecontrol.cpp:42`, `:364-375` | `overuse_detector.cc:79-96`, `overuse_detector.h:45` | 6,0 bascules/min sur le palier contraint | **défaut réel** |
| É4 | gel de l'estimation à 1,5 × l'entrant | `remoterateestimator.cpp:375-381` | `aimd_rate_control.cc:238-239`, `:266` | estimation figée à 3841 kb/s pendant 90 s pour 2465 kb/s reçus | **défaut réel** |
| É5 | le maximum connu est nourri par notre propre estimation | `remoterateestimator.cpp:320`, `:338`, `:364`, `:462-484` | `link_capacity_estimator.cc:39-41`, `aimd_rate_control.cc:232-233` | région `NearMax` auto-entretenue, rampe divisée par deux | **défaut réel** |
| É6 | échelle de la grandeur comparée au seuil | `remoteratecontrol.cpp:211` — `min(fps,30)·offset` | `overuse_detector.cc:44` — `min(deltas,60)·offset` | sensibilité qui suit la cadence de l'encodeur | **défaut réel** |
| É7 | `UnderUsing` relance la montée | `remoterateestimator.cpp:279-291` | `aimd_rate_control.cc:378-379` | relance de la montée pendant la vidange de file | **défaut réel** |
| É8 | la première surutilisation ne descend pas | `remoterateestimator.cpp:271-278` | `aimd_rate_control.cc:373-376` | jusqu'à 1 s de retard à la descente | écart réel, effet faible |
| É9 | forme de la montée : sigmoïde + régions | `remoterateestimator.cpp:421-451` | `aimd_rate_control.cc:342-362` | ×1,048/s mesuré | **choix assumé** |
| É10 | cadence de réestimation périodique | `remoterateestimator.cpp:193` — 1 s | `remote_bitrate_estimator.h:58` — 500 ms, ramenés à 200 ms par `GetFeedbackInterval` | — | choix assumé |
| É11 | hystérésis de bascule | `remoteratecontrol.cpp:232` — 3 images | `overuse_detector.cc:56` — 10 ms **et** 2 échantillons | ≈ 2 images d'écart | sans effet mesurable |
| É12 | taille de paquet comptée | `remoterateestimator.cpp:92` — en-tête compris | `remote_bitrate_estimator_single_stream.cc:94` — utile + bourrage | +2 % sur le débit mesuré | sans effet mesurable |
| É13 | l'échantillon est délimité par le bit `mark` | `remoteratecontrol.cpp:89-105` | `inter_arrival.cc:126-137` | non observé | sans effet mesuré, **à mesurer** |
| É14 | pas de remise à zéro sur saut d'horloge ni réordonnancement | `remoteratecontrol.cpp:73-75` | `inter_arrival.cc:60-84` | non observé | sans effet mesuré |
| É15 | plancher de débit | `remoterateestimator.cpp:23` — 16 kb/s | `bwe_defines.h:24` — 5 kb/s | — | choix assumé (arbitrage A1) |

## 3. Les écarts qui expliquent la mesure

### É1 — l'AIMD n'a aucun frein entre deux réactions

**Nous.** Trois chemins entrent dans la machine AIMD. Le chemin paquet est
protégé par un front : `streamOverusing` ne vaut vrai qu'au passage à
`OverUsing` (`remoterateestimator.cpp:160-166`, `:200-203`). Les deux autres ne
le sont pas. `RemoteRateControl::UpdateLost` et `UpdateRTT` rendent un **niveau**,
pas un front — `return GetUsage()==OverUsing`
(`remoteratecontrol.cpp:361`, `:311`) — et l'appelant relance tout l'AIMD à
chaque fois (`remoterateestimator.cpp:570-572`, `:554-556`). Or `UpdateLost` est
appelé à chaque trou de numéro de séquence (`rtpsession.cpp:4001`). Pendant une
congestion, le trou est la règle.

La fonction appelée reçoit bien un paramètre `reactNow`
(`remoterateestimator.cpp:235`) — mais il n'est **jamais lu**. Le frein n'existe
pas, même sous forme d'intention.

**Témoin.** Même test de niveau, mais gardé :

```c++
if (estimator.detector.State() == BandwidthUsage::kBwOverusing) {
  if (incoming_bitrate.has_value() &&
      (prior_state != BandwidthUsage::kBwOverusing ||
       remote_rate_.TimeToReduceFurther(now, *incoming_bitrate))) {
    UpdateEstimate(now);
```
(`remote_bitrate_estimator_single_stream.cc:111-121`)

`TimeToReduceFurther` (`aimd_rate_control.cc:119-133`) laisse passer soit le
premier front, soit une réaction espacée d'au moins `clamp(rtt, 10 ms, 200 ms)`,
soit un débit tombé sous la moitié de l'estimation. Le témoin ne descend jamais
plus de dix fois par seconde, et en pratique cinq.

**Observable.** Sur la marche basse, entre 147,8 s et 152,9 s, le journal porte
**102 lignes `BWE: estimation`**, espacées de 12 à 52 ms. L'estimation descend de
664 à 233 kb/s pendant que l'entrant tient 720 kb/s.

**À mesurer pour confirmer.** Compter les lignes `BWE: estimation` par seconde
pendant une phase de perte, et vérifier que le nombre tombe à ≤ 5/s une fois un
frein posé. Le rapport le donne déjà : `events.csv`, densité des lignes `rate`.

**Test proposé.** `RateControlEstimator, DeuxRapportsDePerteRapprochesNeDescendentQuUneFois` :
poser une congestion, appeler `UpdateLost` dix fois en 100 ms, vérifier qu'au
plus deux estimations sont publiées au listener. Le harnais existe déjà
(`BitrateCapture`, `mcu/tests/test_rate_control.cpp:58`).

### É2 — la descente prend un cliquet sur une fenêtre de 200 ms

**Nous.** La descente vise `beta × débit entrant` (`remoterateestimator.cpp:351`),
et ne remonte jamais tant que l'état reste `Decrease` :

```c++
current = (DWORD) (beta * incomingBitRate + 0.5);
if (current > currentBitRate) { … current = fmin(current, currentBitRate); }
```
(`remoterateestimator.cpp:351-358`)

Le débit entrant vient de `bitrateAcu`, dont la fenêtre est de **200 ms**
(`remoterateestimator.cpp:16`, lu en `:297`). Combiné à É1, l'estimation ne
retient pas `beta × débit`, mais **le minimum de `beta × débit` sur toutes les
fenêtres de 200 ms de l'épisode**. C'est un quantile bas, pas une moyenne.

**Témoin.** Même règle de non-remontée (`aimd_rate_control.cc:291-293`), mais
trois protections l'encadrent : la fenêtre de mesure vaut **1 s**
(`bwe_defines.h:26`, `remote_bitrate_estimator_single_stream.cc:54`), l'état
passe en `Hold` juste après la descente — *« Stay on hold until the pipes are
cleared »*, `aimd_rate_control.cc:310-311` — et É1 limite le nombre de descentes.
Le minimum porte sur un seul échantillon.

**Observable.** Palier 750 kb/s, hors transitoire : médiane de l'estimation
517 kb/s pour 651 kb/s entrants, soit **−20 %**. Au creux de l'épisode,
233 kb/s pour 720 kb/s, soit **−68 %**.

**À mesurer pour confirmer.** Rejouer le même journal en remplaçant la fenêtre de
`bitrateAcu` par 1 s : le rapport `estimation / entrant` du palier contraint doit
remonter vers `beta`. La mesure est possible hors ligne, `events.csv` porte les
deux séries.

**Test proposé.** `RateControlEstimator, LaDescenteNeRetientPasLePireEchantillon` :
injecter un débit régulier de 1000 kb/s modulé par des rafales d'image clé,
déclarer une congestion, vérifier que l'estimation reste au-dessus de
`0,8 × 1000` kb/s.

### É3 — le seuil du détecteur ne s'adapte pas

**Nous.** `threshold = 25` est posé au constructeur
(`remoteratecontrol.cpp:42`) et plus rien ne le touche : `SetRateControlRegion`
est vide depuis le lot 1bis (`:364-375`). Le seuil est le même sur un lien propre
et sur un lien qui gigue.

**Témoin.** Le seuil est une variable d'état, mise à jour à **chaque** détection :

```c++
const double k = fabs(modified_offset) < threshold_ ? kDown : kUp;
threshold_ += k * (fabs(modified_offset) - threshold_) * time_delta_ms;
threshold_ = SafeClamp(threshold_, 6.f, 600.f);
```
(`overuse_detector.cc:90-94`, constantes `:23-27`, départ 12,5 en
`overuse_detector.h:45`)

Il monte lentement (`kUp = 0,0087`) quand le signal dépasse, redescend vite
(`kDown = 0,039`) quand il repasse dessous, et **ignore les pointes** de plus de
15 ms au-dessus de lui (`:83-88`) pour ne pas s'ajuster à une chute de capacité.

L'intention est écrite noir sur blanc dans le test `ThresholdAdapts`
(`overuse_detector_unittest.cc:552-620`) : un offset qui déclenche une
surutilisation ne doit **plus** la déclencher après que le seuil a monté, puis la
déclencher à nouveau après qu'il est redescendu. Et le barème est sévère :
`Run100000Samples` avec 10 ms d'écart-type de gigue doit rendre **zéro**
surutilisation, à 3 im/s comme à 30 im/s
(`overuse_detector_unittest.cc:282-295`, `:522-535`).

**Observable.** 6,0 bascules `Increase`↔`Decrease` par minute sur le palier
contraint de la séance dépouillée, 6,5 à 11,3 sur la campagne. La trace de
détection donne la marge : `t=33,77 th=25,00` — un dépassement de 35 %, soit un offset
d'au moins 1,13 ms (le multiplicateur vaut au plus 30, cf. É6). Un seuil adaptatif serait monté au-dessus dès la deuxième
occurrence.

**À mesurer pour confirmer.** Ajouter le seuil courant à la trace `detect`, puis
compter les bascules sur la phase gigue. Aujourd'hui `th=` est constant par
construction, donc la trace ne prouve rien.

**Test proposé.** `RateControlThreshold, LeSeuilSAdapteAuBruitDuLien` : rejouer
deux fois la même dérive faible ; la deuxième ne doit plus déclencher. C'est la
transposition directe de `ThresholdAdapts`. Le générateur déterministe existe
(`Jitter`, `mcu/tests/test_rate_control.cpp:816`).

### É4 — un gel là où le témoin met un plafond

**Nous.**

```c++
if (!recovery && (incomingBitRate > 100000 || current > 150000) && current > 1.5 * incomingBitRate)
{
	current = currentBitRate;
	lastBitRateChange = now;
}
```
(`remoterateestimator.cpp:375-381`)

Au-delà de 1,5 × l'entrant, l'estimation ne bouge **plus du tout**. Elle ne monte
plus, elle ne descend pas non plus, et `lastBitRateChange` continue d'avancer.

**Témoin.** La même idée, mais en plafond glissant :

```c++
DataRate increase_limit = 1.5 * estimated_throughput + DataRate::KilobitsPerSec(10);
…
new_bitrate = std::min(increased_bitrate, increase_limit);
```
(`aimd_rate_control.cc:238-239`, `:266`)

L'estimation colle au plafond et **suit** le débit entrant. Elle n'est jamais
figée.

**Observable.** Palier haut, boucle ouverte : estimation constante à 3841 kb/s
pendant 90 s pour 2465 kb/s entrants, soit un rapport de 1,56 — juste au-dessus
du seuil de gel, atteint puis jamais quitté. C'est ce gel qui rend la re-montée
**non mesurable en boucle fermée**, déjà constaté sur la patte `cx-120`.

**À mesurer pour confirmer.** Dans `events.csv`, la variance de `estimation_kbps`
sur une fenêtre où `state=Increase` : elle est nulle sur toute la durée du gel.
Vérification faite ici, à refaire sur chaque séance.

**Test proposé.** `RateControlEstimator, LEstimationSuitLePlafondAuLieuDeGeler` :
source à 1000 kb/s, laisser monter, vérifier que l'estimation se stabilise à
`1,5 × 1000 + 10` kb/s **et** qu'elle redescend quand la source tombe à 500 kb/s.

### É5 — le maximum connu est nourri par notre propre estimation

**Nous.** `UpdateMaxBitRateEstimate` reçoit `fmax(currentBitRate, incomingBitRate)`
sur ses trois sites d'appel (`remoterateestimator.cpp:320`, `:338`, `:364`).
Quand l'estimation dépasse le débit reçu — ce qui est le cas dès que É4 mord —
c'est **notre estimation** qui devient la mémoire du maximum du lien. La région se
calcule ensuite contre cette mémoire (`:315-327`), et la région pilote `beta`
(`:519-541`) et le facteur de montée (`:441-448`).

**Témoin.** `LinkCapacityEstimator::OnOveruseDetected(acknowledged_rate)`
(`link_capacity_estimator.cc:39-41`) ne reçoit **que** le débit acquitté, jamais
la consigne. Et l'estimation de capacité est **effacée** dès que le débit mesuré
dépasse sa borne haute (`aimd_rate_control.cc:232-233`,
`link_capacity_estimator.cc:21-26`), ce qui rend la rampe rapide jusqu'à la
prochaine surutilisation. Le lissage est aussi plus lent : `alpha = 0,05` contre
0,10 chez nous (`link_capacity_estimator.cc:40`, `remoterateestimator.cpp:464`).

**Observable.** Sur le retour à 3000 kb/s, la région alterne `NearMax`/`BelowMax`
sans jamais tenir `MaxUnknown` : chaque passage par `MaxUnknown` appelle
`UpdateMaxBitRateEstimate` dans la foulée (`:319-320`), ce qui ramène le maximum
vers l'entrant et referme la région au tick suivant. La rampe rapide dure un tick.

**À mesurer pour confirmer.** Compter la durée cumulée passée en `MaxUnknown`
dans `events.csv` (lignes `region`) sur une re-montée : elle doit être de l'ordre
de la seconde, alors que le témoin resterait sans estimation de capacité pendant
toute la remontée.

**Test proposé.** `RateControlEstimator, LeMaximumConnuNeVientQueDuDebitRecu` :
forcer l'estimation au-dessus du débit reçu, déclencher une descente, vérifier
que la consigne obtenue vaut `beta × débit reçu` et non `beta × estimation`.

### É6 — l'échelle de la grandeur comparée au seuil

**Nous.** `const double T = min(fps, 30) · offset` (`remoteratecontrol.cpp:211`).
Le multiplicateur est une **cadence**, mesurée sur la dernière seconde.

**Témoin.** `const double T = min(num_of_deltas, 60) · offset`
(`overuse_detector.cc:44`), où `num_of_deltas` est un **compteur** d'échantillons,
plafonné à 1000 côté estimateur (`overuse_estimator.cc:41-43`) et à 60 côté
détecteur.

Deux conséquences. D'abord un facteur deux permanent : à 30 im/s, notre `T` vaut
la moitié du sien, donc notre seuil de 25 correspond à un offset de 0,83 ms quand
le sien, à 12,5 au départ, correspond à 0,21 ms. Ensuite, et c'est le vrai
problème, **notre sensibilité suit la cadence de la source** : un partage de
document à 5 im/s exige un offset de 5 ms pour déclencher, six fois plus qu'à
30 im/s. Le témoin, lui, sature son compteur en deux secondes quelle que soit la
cadence — ses tests couvrent explicitement 3, 5, 10 et 30 im/s avec le même
barème (`overuse_detector_unittest.cc:252-535`).

**À mesurer pour confirmer.** Une séance gigue sur une source à faible cadence
(partage d'écran, 5 im/s) : la part d'échantillons hors `Normal` doit s'effondrer
par rapport à la même gigue à 30 im/s. Le critère « faux positifs » du rapport
donne le chiffre.

**Test proposé.** `RateControlDetector, LeVerdictNeDependPasDeLaCadence` :
même dérive de délai par image à 30 im/s et à 5 im/s, même nombre d'images avant
détection. Le générateur de `RateControlThreshold` sait déjà cadencer.

### É7 — `UnderUsing` relance la montée

**Nous.** `UnderUsing` conduit à `Increase`, sauf en région `NearMax`
(`remoterateestimator.cpp:279-291`).

**Témoin.** `case BandwidthUsage::kBwUnderusing: rate_control_state_ = kRcHold;`
(`aimd_rate_control.cc:378-379`). Sans exception.

`UnderUsing` veut dire que le délai **diminue** : la file se vide. C'est l'instant
qui suit une congestion. Relancer la montée à ce moment-là, c'est la remplir à
nouveau.

**Observable.** Entre 150 s et 156 s, la trace alterne `Normal` et `UnderUsing`
plusieurs fois par 100 ms (`events.csv`, lignes `detect`), au sortir de l'épisode
de perte. Chaque `UnderUsing` hors `NearMax` repasse en `Increase`.

**À mesurer pour confirmer.** Compter les transitions `→ Increase` dont la cause
est `usage=UnderUsing` dans la minute qui suit une descente.

**Test proposé.** `RateControlEstimator, LaVidangeDeFileNeRelancePasLaMontee` :
après une congestion, injecter une suite de délais décroissants, vérifier que
l'état reste `Hold` jusqu'au retour à `Normal`.

## 4. Les écarts sans effet mesurable

Ils sont réels mais je ne peux les relier à aucun comportement observé. Ils ne
sont pas des défauts.

- **É11, hystérésis.** Nous exigeons trois images au-dessus du seuil
  (`remoteratecontrol.cpp:232`) ; le témoin exige 10 ms cumulés **et** deux
  échantillons (`overuse_detector.cc:56`), ce qui revient à deux images de 33 ms.
  Nous sommes plus prudents d'environ deux images, soit 66 ms. Le critère mesuré
  (descente sous 3 s) laisse cent fois cette marge.
- **É12, taille comptée.** `GetSize()` inclut l'en-tête RTP
  (`mcu/include/rtp.h:244`), le témoin compte utile + bourrage
  (`remote_bitrate_estimator_single_stream.cc:94`). L'écart vaut environ 2 % sur
  un paquet de 1100 octets, et il joue **vers le haut** : il ne peut pas
  expliquer une sous-estimation.
- **É14, robustesse d'horloge.** Le témoin réinitialise sur un saut d'horloge de
  3 s ou après trois groupes réordonnés (`inter_arrival.cc:60-84`). Nous jetons
  simplement les paquets hors séquence (`remoteratecontrol.cpp:73-75`). Aucun
  saut d'horloge n'apparaît dans les journaux dépouillés.
- **É10 et É15** sont des choix de déploiement, pas des divergences
  d'algorithme : cadence de réestimation d'une seconde, plancher à 16 kb/s
  (arbitrage A1).

## 5. Un écart à mesurer avant de le classer

**É13 — l'échantillon est délimité par le bit `mark`.** Nous ne filtrons qu'à la
fin d'une image (`remoteratecontrol.cpp:89-105`) ; le témoin découpe par groupe
d'horodatage, sans dépendre d'un bit de l'émetteur
(`inter_arrival.cc:126-137`). Deux conséquences possibles, aucune observée :

1. une source qui ne pose pas `mark` ne produit **aucun** échantillon, donc
   aucune détection ;
2. la perte du paquet marqué fusionne deux images, ce qui double `deltaTime` et
   `deltaSize` — et cela arrive précisément pendant les épisodes de perte, là où
   nous mesurons déjà le pire comportement.

**À mesurer.** Compter les images vues par le filtre
(`fpsCalc.GetAcumulated()`) et les comparer au nombre d'images réellement reçues
pendant une phase de perte. Un écart notable confirme le point 2.

**Test proposé.** `RateControlDetector, UneImageSansMarqueurNeFaussePasLeFiltre` :
rejouer une suite d'images régulières en supprimant un bit `mark` sur dix,
vérifier qu'aucune surutilisation n'est déclarée.

## 6. Ce que la mesure demandait d'expliquer

**L'estimation est 20 à 30 % sous le débit reçu.** Expliqué par É1 combiné à É2 :
chaque trou de séquence relance l'AIMD, chaque relance rejoue une descente, et
la descente ne remonte jamais dans l'épisode. L'estimation retient donc le pire
échantillon de 200 ms de l'épisode, pas sa moyenne. Mesuré : −20 % en médiane sur
le palier contraint, −68 % au creux.

Une précision s'impose sur l'énoncé : **le biais n'est pas uniforme**. Quand le
lien ne contraint pas, la mesure dit l'inverse — l'estimation est 56 % **au-dessus**
du débit reçu, figée par É4. Les deux symptômes ont des causes distinctes, et
c'est É4, pas la sous-estimation, qui rend la re-montée non mesurable.

**La montée vaut ×1,048 par seconde.** C'est la valeur nominale de notre formule,
pas une anomalie. `RateIncreaseFactor` (`remoterateestimator.cpp:421-451`) rend
environ 1,012 avant modulation par la région, puis la région multiplie l'écart à 1
par 0,5 en `NearMax`, par 3 en `BelowMax`, par 5 en `MaxUnknown` — soit 1,006,
1,037 et 1,062. La séance alterne `NearMax` et `BelowMax` : 1,048 est la moyenne
attendue.

**La comparaison au ×1,08 de l'annexe B est trompeuse et doit être corrigée.**
Le ×1,08 du témoin (`aimd_rate_control.cc:346`) n'est pas sa rampe de régime :
c'est sa rampe de **découverte**, celle qu'il n'emploie que sans estimation de
capacité de lien (`:259-265`). Dès qu'une surutilisation a eu lieu, il passe en
montée **additive** (`:256-258`, `:191-207`), qui vaut environ +14 kb/s par
seconde à 1 Mb/s, soit ×1,014 — **moins vite que nous**. La divergence de fond
n'est donc pas la valeur du facteur, c'est É5 : le témoin sait retrouver sa rampe
rapide en effaçant sa mémoire de capacité, nous ne le savons pas parce que notre
mémoire de capacité se nourrit de nos propres consignes.

**Le détecteur bascule 6,5 à 11,3 fois par minute.** Expliqué par É3, aggravé par
É7. Le témoin tient un seuil qui monte avec le bruit du lien, et son barème de
non-régression exige zéro fausse détection sous 10 ms de gigue, sur 100 000
images. Nous comparons un signal bruité à une constante.

**La descente est excellente (0,6 à 1,8 s).** Cohérent : É8 nous coûte au plus un
tick d'une seconde, et É1 nous fait descendre bien plus souvent que le témoin.
Notre point fort est l'effet de bord de nos deux principaux défauts.

## 7. Ordre de traitement recommandé

1. **É1** — poser le frein temporel de `TimeToReduceFurther`. C'est le
   changement le plus court et celui qui débloque la mesure de tout le reste.
2. **É4** — remplacer le gel par le plafond glissant. Sans lui, aucune séance en
   boucle fermée ne mesure la re-montée.
3. **É2** — fenêtre de mesure à 1 s et passage en `Hold` après descente.
4. **É3** — seuil adaptatif.
5. **É5**, **É6**, **É7** — ensuite, dans cet ordre.

É9 ne se traite qu'après É5 : tant que la mémoire de capacité est polluée, changer
la forme de la rampe ne change rien d'observable.
