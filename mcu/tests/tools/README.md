# Outillage de mesure du contrôle de débit (lot 3)

Ce répertoire porte de quoi **exécuter et dépouiller** la séance de mesure du
lot 3 de [`rate_control_plan.md`](../../../rate_control_plan.md) — le *portillon*
qui décide du GO/NO-GO du lot 6. Il ne contient aucun test unitaire : la suite
gtest du chantier est `mcu/tests/test_rate_control.cpp`, jouée par
`make check-ratecontrol`.

| fichier | rôle |
|---|---|
| `netem_scenario.sh` | applique les trois scénarios de dégradation et **journalise** chaque changement avec son horodatage |
| `bwe_report.py` | lit `mcu.log` + le journal de marqueurs, sort CSV, graphe SVG, et **prononce chaque critère d'acceptation** |
| `exemple/` | un journal **synthétique** et ses marqueurs, pour vérifier l'outillage avant la séance |

Aucune dépendance : bash + `iproute-tc` d'un côté, python 3.6+ de la
bibliothèque standard de l'autre. Ni matplotlib ni gnuplot — le graphe est un
SVG écrit à la main, lisible dans n'importe quel navigateur.

## Vérifier l'outillage (à froid, sans appel)

```sh
cd mcu/tests/tools
./bwe_report.py exemple/mcu-escalier.log --markers exemple/marqueurs-escalier.tsv \
                --stream Alice --out /tmp/verif
```

Attendu : `0 critere(s) en echec`, et `/tmp/verif/bwe.svg` montre un palier à
2000 kb/s, une chute à 500, une re-montée. **Ce journal est fabriqué**, il ne
mesure rien — il ne sert qu'à prouver que la chaîne de dépouillement fonctionne.

## Le montage

```
   ┌──────────┐        ┌───────────────────┐        ┌──────────────┐
   │ pair A   │───────►│ machine en coupure│───────►│ mediaserver  │
   │ (Chrome, │        │   tc netem ici    │        │  -d, mcu.log │
   │  SIP…)   │◄───────│                   │◄───────│              │
   └──────────┘        └───────────────────┘        └──────────────┘
```

**Le sens du trafic n'est pas un détail.** Ce que le lot 3 mesure est
l'estimateur de **réception** : il faut donc dégrader ce qui **arrive** au
mediaserver, et `netem` ne façonne que l'**émission** d'une interface. Deux
montages valides :

- **machine en coupure** (ou le poste client lui-même) : lancer
  `netem_scenario.sh` dessus, sur l'interface qui émet **vers** le mediaserver.
  C'est le montage de référence ;
- **sur le mediaserver**, avec `--ingress` : le script détourne le trafic entrant
  vers une interface `ifb` et lui applique `netem`. Plus simple à monter, mais le
  façonnage frappe alors **tout** ce qui entre par l'interface — à réserver à une
  machine dédiée à l'essai.

### Préparer le mediaserver

Les traces `BWE:` sont des `Debug()` : elles n'existent que si le binaire tourne
avec `-d`.

```sh
# /etc/sysconfig/mediaserver
OPTIONS="-d"
```

```sh
systemctl restart mediaserver
: > /var/log/mcu.log          # repartir d'un journal propre
tail -f /var/log/mcu.log | grep BWE:      # doit défiler dès qu'un appel vidéo est établi
```

Si rien ne défile alors qu'un appel vidéo est en cours, l'estimateur n'est pas
branché sur cette patte — inutile de lancer le scénario, régler cela d'abord.

## Phase 0 — sans quoi la séance ne mesure rien

Deux vérifications à faire **appel établi, avant tout `netem`**. Elles coûtent
deux minutes et évitent une séance à refaire.

**1. Le dialecte négocié.** Le mode de feedback est tracé à la pose des
propriétés :

```sh
grep 'Activated .* bitrate feedback' /var/log/mcu.log
# Activated REMB bitrate feedback on Video stream 0x…      (navigateur, goog-remb)
# Activated TMMBR+REMB bitrate feedback on Video stream 0x… (pair SIP, ccm tmmbr)
```

Aucune ligne = mode `None` : rien ne part vers le pair. Ce n'est pas une panne,
c'est le défaut (arbitrage A2), et c'est même le montage voulu pour la mesure en
**boucle ouverte** ci-dessous — mais il faut le savoir avant, pas après.

**2. Le débit que la source sait produire.** Laisser tourner 60 s sans `netem` et
lire le débit entrant :

```sh
grep 'BWE: estimation' /var/log/mcu.log | tail -20
```

Le champ `incoming=` (en kb/s) est le débit **réellement reçu**. Le lien injecté
ensuite (`-r`) doit être **franchement en dessous** : un palier à 2 000 kb/s face
à une source qui n'émet que 700 kb/s ne contraint rien, et le rapport le compte
en `KO` (« -65 % du lien ») alors que la boucle a raison. Régler la source haut
(720p ou plus) ou baisser `-r` en conséquence. En cas de doute, la colonne
`(entrant …)` du verdict tranche : si l'entrant colle au palier, le lien mordait ;
s'il est très en dessous, le palier n'était pas contraignant et le verdict ne
vaut rien.

## Boucle ouverte, boucle fermée : deux mesures différentes

Face à un navigateur, **deux boucles agissent en même temps** : la nôtre (notre
REMB fait ralentir le pair) et celle du pair (son propre contrôle de congestion
voit la perte et ralentit tout seul). Le débit entrant baisse alors sans rien
prouver de notre estimateur.

- **boucle ouverte** — elixip ne pose ni `remb` ni `tmmbr` : rien ne part, la
  source continue d'émettre, la file se remplit vraiment. C'est la mesure de
  **l'estimateur seul**, celle qui exerce le détecteur de délai réparé au lot 1 ;
- **boucle fermée** — le dialecte est négocié : c'est la mesure du **système**,
  telle qu'elle se comportera en production.

Faire au moins la marche d'escalier dans les deux configurations, et **dire
laquelle** dans l'annexe D pour chaque série : la même courbe n'y raconte pas la
même chose.

## Les trois scénarios

Un appel réel établi via elixip (1:1 vidéo, le pair A émettant vers le
mediaserver), puis :

```sh
# marche d'escalier : 2 Mb/s -> 500 kb/s -> 2 Mb/s, 60 s par palier
sudo ./netem_scenario.sh -i eth0 -s escalier -m escalier.tsv

# pertes : sain -> 2 % -> 10 % -> sain
sudo ./netem_scenario.sh -i eth0 -s pertes -m pertes.tsv

# gigue : sain -> 50 ms ± 30 ms (2 phases) -> sain
sudo ./netem_scenario.sh -i eth0 -s gigue -m gigue.tsv
```

Options utiles : `-r` (débit du lien sain, kb/s, défaut 2000), `-d` (durée d'une
phase, défaut 60 s), `-l` (profondeur de la file netem, défaut 40 paquets),
`--ingress` (cf. plus haut). Le script **restaure les qdisc à la sortie**, y
compris sur Ctrl-C ou `kill`.

### La profondeur de file n'est pas un détail

`netem` garde par défaut **1000 paquets**, soit **19 s de backlog à 500 kb/s** : le
lien injecté se comporte alors en entrepôt, pas en lien. Ce que ça produit, mesuré
le 2026-08-18 : au relâchement de la marche basse, le débit entrant est resté 8 s
à l'ancien plafond, puis a bursté 4 s à 1789 kb/s le temps que la file se vide —
donc 8 s de faux retard sur le chrono de re-montée, et une dispersion qui mesurait
la vidange.

Un lien d'accès réel tient 100 à 300 ms. Le défaut `-l 40` donne ~200 ms à
2 Mb/s et ~800 ms à 500 kb/s. Le baisser davantage transforme la contrainte de
capacité en pertes : c'est alors le scénario `pertes` qu'on joue, pas
`escalier`. **Dire la valeur retenue en annexe D** : elle change ce que le
détecteur de délai voit.

Le critère « 10 minutes sans NaN, sans gel d'hypothèse, sans écrêtage » se juge
sur la durée **cumulée** de la séance : enchaîner les trois scénarios sans couper
l'appel et dépouiller le journal entier une fois de plus, sans `--markers`,
donne cette lecture-là.

## Dépouiller

```sh
./bwe_report.py /var/log/mcu.log --markers escalier.tsv --stream 'Alice' \
                --out ./escalier --markdown
```

- `--stream` **n'est pas optionnel en pratique** : un appel a plusieurs pattes
  (les deux participants, audio et vidéo), un seul lien est dégradé, et juger les
  autres pattes contre les marqueurs produit des `KO` qui ne veulent rien dire.
  Le nom est celui que `BWE: estimation stream=…` porte : le tag du participant
  (MCU) ou le nom de l'endpoint (JSR-309). Lancer une première fois sans filtre
  pour lire la liste des pattes.
- `--markdown` ajoute le bloc de tableaux à coller tel quel dans l'annexe D.
- Sorties dans `--out` : `bwe.csv` (la série d'estimation), `events.csv`
  (détections, feedback émis, pertes, RTT, changements d'état), `bwe.svg`.
- Code de retour : `0` si aucun critère n'échoue, `1` si un critère échoue ou si
  la mesure est inexploitable, `2` si le journal ne contient aucune trace BWE
  (traces de debug non activées, le cas le plus fréquent).

**Une mesure vide n'est pas un succès.** Les critères se partagent en deux
familles. Les critères de **fond** se jugent contre les marqueurs : eux seuls
attestent que le scénario a été mesuré. Les critères de **stabilité** (NaN,
écrêtage, covariance) sont vrais de n'importe quel journal. Quand aucun critère
de fond n'a pu être jugé, le script écrit `MESURE INEXPLOITABLE` et sort en `1`,
même si tous les critères de stabilité passent. Il nomme alors la patte dont les
échantillons couvrent la fenêtre des marqueurs : c'est presque toujours celle
qu'il fallait donner à `--stream`. Si aucune ne la couvre, le journal et les
marqueurs ne viennent pas de la même séance.

### Ce que le script prononce

| critère | source | seuil |
|---|---|---|
| régime établi | médiane de l'estimation sur le palier, hors transitoire, rapportée à l'entrant médian | ±25 % |
| réaction à la baisse | 1er échantillon sous 1,25 × le nouveau lien | < 3 s |
| re-montée | 1er échantillon ≥ 80 % du lien, **compté depuis la libération observée** | < 30 s |
| pas d'oscillation | alternances de direction des ticks `rate` (`Increase`↔`Decrease`) et coef. de variation, hors transitoire | ≤ 6/min, ≤ 0,20 |
| pertes : pas d'effondrement | médiane rapportée à la phase saine précédente | ≥ 25 % |
| gigue : faux positifs | part des échantillons hors `Normal`, rapportée à la phase saine précédente | ≤ +10 points |
| pas de NaN | toute valeur imprimée `nan` | 0 |
| hypothèse non gelée | plus longue plage continue hors `Normal` | ≤ 30 s |
| pas d'écrêtage | temps cumulé à 30 000 kb/s (plafond du lot 1) | ≤ 5 s |
| covariance | avertissements `no longer positive semi-definite` | 0 |

**Pourquoi l'entrant et pas le lien `-r`.** `tc` façonne le paquet entier,
en-têtes IP/UDP/Ethernet compris ; l'estimateur ne voit que la charge utile RTP,
qui plafonne 15 à 20 % plus bas. Jugé contre le nominal, tout palier était KO
d'autant. Le débit entrant médian est la seule référence que l'estimateur peut
atteindre — le nominal reste affiché entre parenthèses, comme contexte. Un
palier qui ne mord pas se trahit du même coup : l'estimation s'installe à
1,5 × l'entrant (le plafond glissant), soit +50 %.

**Où commence un régime établi.** Une marche laisse l'estimation grimper ou
chuter de façon strictement monotone pendant tout le transitoire : y calculer une
dispersion mesure la pente, pas une oscillation. La fenêtre de jugement démarre
donc au premier renversement de pente, sans jamais commencer avant la garde
`--settle` — celle-ci reste le plancher. Le motif retenu est imprimé entre
crochets à côté de chaque verdict (`garde 15s`, `transitoire ecarte jusqu a
+49.4 s`) : une fenêtre choisie en silence rend le verdict illisible.

Le transitoire finit aussi quand l'estimation cesse de bouger : une valeur figée
plus de 3 s est le régime le plus établi qui soit, et l'écarter du jugement
laisse passer exactement ce qu'on cherche à mesurer. Un palier au milieu d'une
rampe ne compte pas : si la rampe repart dans le même sens, ce n'était pas un
régime établi. Le verdict reste `--` seulement si l'estimation rampe sans jamais
s'établir.

**D'où part le chrono de re-montée.** Du marqueur `tc`, **sauf** si une file
profonde a retardé la libération. Ce retard se reconnaît à son *plateau* : le débit
entrant reste dans une bande autour de l'ancien plafond (80 % à 115 %) pendant au
moins 2 s, puis en sort par le haut au burst de rattrapage. Sans plateau, il n'y a
rien à retrancher — la rampe de la source est du signal, pas de la vidange, et la
retrancher flatterait notre boucle. Le verdict affiche **les deux** chiffres, le
délai retenu et le délai brut. Un plateau détecté est le signe qu'il faut baisser
`-l`.

**Un taux par minute ne se prononce pas sur 4 secondes.** Sous 30 s de fenêtre, le
verdict d'oscillation ne juge que le coefficient de variation et **affiche le
nombre brut de bascules sans l'extrapoler** : une seule bascule vue sur 4 s donne
« 15/min », ce qui ne veut rien dire. Une fenêtre courte est elle-même le
renseignement — elle dit que le transitoire a mangé la phase, et qu'il faut
allonger `-d`.

Le plan chiffre les quatre premiers. Les autres seuils sont **posés ici** faute
d'être chiffrés ailleurs : `--settle`, `--max-kbps` et `--min-kbps` s'ajustent en
ligne de commande, les autres en tête de `bwe_report.py`. Toute valeur retenue
autrement doit être **dite en annexe D** — un critère déplacé après coup pour
faire passer une mesure ne mesure plus rien.

## Traces lues

Toutes viennent de `mcu/src/remoterateestimator.cpp`,
`mcu/src/remoteratecontrol.cpp` et `mcu/src/rtpsession.cpp` :

```
BWE: estimation stream=… state=… region=… usage=… currentBitRate=… current=… incoming=… min=… max=…
BWE: Overusing bitrate:… T:…,threshold:…      BWE: Overusing candidate n/3 …
BWE:  Normal  bitrate:…                        BWE:  UnderUsing bitrate:…
BWE: ChangeState from:… to:…                   BWE: Change region to:…
BWE: Increase|Decrease rate to current = … kbps
BWE: UpdateLost lost:… hipothesis:…,packets:…,lost:…      BWE: UpdateRTT rtt:…ms hipothesis:…
BWE: covariance no longer positive semi-definite
-RTPSession::onTargetBitrateRequested() mode … bitrate […] -> […] send … for … stream …
-RTPSession::SendReceiverEstimatedMaxBitrate […] on … stream
-RTPSession::SendTempMaxMediaStreamBitrateRequest […] on … stream
-RemoteRateEstimator::SetTemporalMaxLimit() …
```

Le regroupement par patte se fait sur l'**identifiant de thread** du préfixe de
trace (une `RTPSession` = un thread), et l'étiquette lisible vient du champ
`stream=` de la ligne d'estimation. Un journal capturé **avant** l'ajout de ce
champ reste lisible : les pattes s'appellent alors `tid 0x…`.

## Au passage : la recette pcap du lot 2

Le lot 2 (feedback négocié) attend une preuve au fil, qui se prend dans la même
séance. Sur le mediaserver, avant de lancer les scénarios :

```sh
tcpdump -i eth0 -n -s0 -w feedback.pcap "udp and host <IP du pair>"
```

À lire ensuite : RTCP sur un port dynamique n'est pas reconnu tout seul, il faut
le dire à l'analyseur.

```sh
tshark -r feedback.pcap -d udp.port==<port RTCP de la patte>,rtcp -Y rtcp
```

Attendu, face à un navigateur : des paquets **PSFB / Application Layer Feedback
(REMB)** qui partent **et dont la valeur varie** avec les scénarios ; aucun TMMBR.
Face à un pair SIP `ccm tmmbr` : le comportement historique, TMMBR + TMMBN.
Les valeurs elles-mêmes se lisent plus commodément dans `events.csv`
(colonne `value_kbps`, lignes `REMB`/`TMMBR`) — le pcap sert à prouver le
**dialecte** et le fait que ça sort de la machine.

## Livrable

Les mesures et la décision GO/NO-GO vont en **annexe D de
[`rate-control.md`](../../../rate-control.md)** : le graphe, le bloc `--markdown`
de chaque scénario, et la liste des alignements « lot 1bis » que la mesure
réclame le cas échéant (arbitrage A3 du plan).
