# Recette : transcodeurs JSR-309 sans thread

## À quoi sert cette fiche

Le chantier « transcodeurs sans thread » a retiré les threads du chemin des
paquets d'un transcodeur JSR-309. Un paquet RTP traverse maintenant décodage,
rééchantillonnage ou redimensionnement, encodage et multiplexage **sur le thread
qui l'a livré**.

Les tests automatiques prouvent que le chemin est bien inline. Ils ne prouvent
pas qu'un appel réel reste bon. Cette fiche donne la recette à jouer en appel
réel, et ce qu'il faut observer.

Jouez-la après chaque changement dans un transcodeur, un encodeur ou un
décodeur JSR-309.

## Ce qui est déjà couvert par les tests

Ne rejouez pas ces points à la main. Ils sont figés :

```sh
cd mcu && make check-transcoder
```

Cette cible joue la caractérisation de sortie, les chemins inline audio et
vidéo, le mode pont, et les scénarios de recette jouables hors appel :
rattachement à une nouvelle source, arrêt de la source, source détruite sans
détachement, renégociation à chaud (`SetCodec` et bornes négociées).

La recette ci-dessous couvre ce qu'un test ne voit pas : le média réel, la
cadence réelle, la charge réelle.

## Avant de commencer

1. Le binaire à recetter est déployé selon la convention IVèS :

```sh
cd /opt/ives/bin/
mv mediaserver mediaserver.release
ln -s /home/<user>/mediaserver/bin/debug/mcu mediaserver
systemctl restart mediaserver
tail -f /var/log/mcu.log
```

2. Un contrôleur capable d'établir un appel JSR-309 (elixip) et deux endpoints :
   un navigateur WebRTC et un Linphone. Il en faut deux différents : c'est ce
   qui force le transcodage.

3. L'outil de recensement est dans le dépôt :

```sh
mcu/tests/tools/thread_census.sh            # une photo
mcu/tests/tools/thread_census.sh "" 30      # 30 s : min, max, moyenne, CPU
```

Les threads ne portent pas de nom dans ce binaire. Le regroupement par nom du
script ne distingue donc que les threads créés par ffmpeg, x264 ou SVT-AV1, qui
se nomment eux-mêmes. **C'est le TOTAL qui est le signal**, pas les noms.

## Passe ThreadSanitizer

Le chantier remplace une séparation par threads par une séparation par verrous
et drapeaux. ThreadSanitizer voit ce qu'un test fonctionnel rate. À rejouer
après tout changement dans un transcodeur.

```sh
cd mcu
make mkdirs TAG=tsan                       # une seule fois
make tests TSAN=yes TAG=tsan               # objets isoles du build normal
TSAN_OPTIONS="halt_on_error=0 detect_deadlocks=0" \
  ./tests/runtests --gtest_filter='TranscoderRecette*:TranscoderCharacterization*:AudioDecoderInline*:VideoDecoderInline*:AudioEncoderInline*:VideoEncoderInline*:AudioBridgingTest*:VideoBridgingTest*'
make tests                                 # RE-BATIR le binaire normal ensuite
```

Le paquet `libtsan` doit être installé. `detect_deadlocks=0` est obligatoire :
le détecteur d'interblocage de TSan tombe sur son propre `CHECK failed` dans les
verrous de libavfilter, qui n'est pas instrumenté.

Trois choses à savoir en lisant le rapport :

- **Un avertissement attendu, et faux.** « double lock of a mutex … Mutex is
  already destroyed » sur `Wait::Cancel` apparaît quand la suite enchaîne
  plusieurs transcodeurs. Ils occupent la même adresse de pile, et TSan garde
  l'état du verrou précédent. Le même test joué seul ne rapporte rien : c'est le
  contrôle à faire avant de conclure.
- **libmedkit et libbfcp ne sont pas instrumentés.** Une course interne à ces
  bibliothèques ne sera pas vue.
- **Le reste du dépôt n'est pas propre.** La suite entière sous TSan rapporte
  environ 630 avertissements, dont 85 dans `src/rtpsession.cpp` — les jambes RTP,
  hors du périmètre de ce chantier. Sur le chemin JSR-309, il n'en reste que deux,
  et aucun sur le chemin des paquets d'un transcodeur. Ne jugez donc le résultat
  que sur le filtre ci-dessus.

## Mesure de référence (« avant »)

Le relevé d'avant chantier n'a jamais été pris. Pour l'obtenir, bâtissez le
commit qui précède le chantier :

```sh
git worktree add /tmp/avant 8ee3229
cd /tmp/avant && ./install.ksh localcompile
```

Puis jouez le scénario 1 sur ce binaire-là, et notez le total de threads.

Ordre de grandeur attendu, pour un appel audio + vidéo transcodé dans les deux
sens, hors threads internes de ffmpeg, x264 et SVT-AV1 :

| | threads du transcodage | détail |
|---|---|---|
| avant | 18 | 8 session et démux + 4 transcodeurs audio + 6 transcodeurs vidéo |
| après | 10 | 8 session et démux + 0 audio + 2 lisseurs vidéo |

Premier relevé « après », le 2026-08-29 à 17:09 (Linphone ↔ Linphone, Alice
VP8 720p → x264, Bob H.264 640×480 → VP8, 2 cœurs) : **26 à 28 threads** pour
le processus entier, contre **11 au repos**, soit 15 à 17 pour l'appel — les 10
du tableau plus les threads internes de x264 et de libvpx. CPU : **94 % d'un
cœur**, la moitié de la machine, pour deux transcodages vidéo simultanés.
Le relevé « avant » reste à prendre sur le binaire `8ee3229`.

## Les scénarios

Chaque scénario donne ce qu'il faut faire, puis ce qui doit être vrai. Notez le
résultat dans la feuille de relevé en fin de fiche.

### 1. Les quatre combinaisons

Établissez quatre appels, l'un après l'autre :

| # | média | mode |
|---|---|---|
| 1a | audio | pont (les deux pattes parlent le même codec) |
| 1b | audio | transcodage (Opus d'un côté, PCMU de l'autre) |
| 1c | vidéo | pont (H.264 des deux côtés) |
| 1d | vidéo | transcodage (VP8 d'un côté, H.264 de l'autre) |

Pour chacun :

- le son est audible et sans coupure, l'image est fluide et sans artefact ;
- `mcu/tests/tools/thread_census.sh "" 30` pendant l'appel : notez le total et
  le CPU ;
- dans `/var/log/mcu.log`, le mode choisi est tracé :
  `-VideoTranscoder: switched to bridged mode for codec …` en pont,
  `-VideoTranscoder: transcoding … -> …` en transcodage.

**Critère** : média correct dans les quatre cas, et total de threads conforme au
tableau ci-dessus pour 1b et 1d.

**Regardez la FORME de l'image, pas seulement sa netteté.** Une image déformée
se voit mal quand on cherche autre chose. Le journal donne la réponse sans
ambiguïté : comparez la taille native de la source à celle de l'encodeur.

```
-VideoEncoder: taille native de la source 640 x 360 [useInputSize 1]
-Created H264 video encoder [640x360@20, 2500 kbps, intra 200]
```

Les deux ratios doivent coïncider. S'ils diffèrent, l'image sort étirée : le
redimensionnement ne conserve pas le ratio, il remplit le cadre demandé. Le
transcodeur suit la source par défaut (`useInputSize` armé à la création,
`MediaSession::VideoTranscoderCreate`) ; un `useInputSize=0` passé par le
contrôleur dans `VideoTranscoderSetCodec` réimpose sa géométrie, et c'est alors
à lui de choisir un `mode` du bon ratio.

**Regardez si l'encodeur tient la cadence.** Depuis le lot 5, un encodeur trop
lent pour sa source ne bloque plus le thread de démux : le transcodeur saute
des images, une sur k, et le dit dans le journal :

```
-VideoTranscoder: encodeur trop lent [video transcoder inbound] : 96 ms par image pour un budget de 50 ms (source 20 im/s) -> 1 image sur 3 encodee, encodeur recale a 6 im/s
-VideoTranscoder: encodeur de nouveau dans les temps [video transcoder inbound] : 28 ms par image pour un budget de 50 ms (source 20 im/s) -> toutes les images encodees
```

Le rappel revient toutes les 30 s tant que des images sont sautées. Ce que ça
veut dire pour la recette :

- **aucune ligne** : l'encodeur suit, c'est le cas attendu en CIF ;
- **une ligne puis un retour « dans les temps »** : une charge passagère, pas
  un défaut ;
- **des montées et descentes qui alternent** (`trop lent` puis `de nouveau dans
  les temps`, encore et encore) : le pas bat. Il ne doit plus, depuis que
  montée et descente ont deux seuils distincts (4/5 et 7/10 du budget) ; si
  vous le revoyez, notez le coût affiché — il est probablement entre les deux ;
- **un rappel toutes les 30 s** : la machine ne tient pas ce transcodage à cette
  taille. La sortie est fluide mais à cadence réduite. Ce n'est pas une
  régression du chantier, c'est le coût de la géométrie suivie. Vérifiez
  d'abord que l'encodeur tient le temps réel sur cette machine :
  `make -C third_party/fontventa/libmedikit check` doit donner `Vp8Realtime`
  vert. Sinon, réduisez la résolution de la source ;
- **« PAS MAXIMAL »** : même une image sur 15 ne suffit pas. L'encodeur ne
  convient pas du tout à cette machine.

Le symptôme d'AVANT ce mécanisme, à ne plus jamais voir ensemble : des lignes
`-RTPBuffer: file trop profonde (>500 ms), N paquet(s) jete(s)` chaque seconde
sur la jambe source **et** des `-Requesting FPU lost N` / `-SendFIR` au même
rythme. C'est le thread de démux qui n'arrive plus à consommer parce qu'il
encode : le serveur jette lui-même les paquets, puis fait payer une trame clé
par seconde au pair. Si vous le revoyez, la décimation n'a pas suffi ou n'a pas
agi : notez le pas en vigueur dans le journal.

### 2. Renégociation en cours d'appel

Appel vidéo transcodé établi et stable. Depuis le contrôleur, changez le codec
de sortie (`VideoTranscoderSetCodec`), puis les bornes négociées de la patte
émettrice (une nouvelle offre ou réponse SDP).

**Critère** :

- l'image se rétablit en moins d'une seconde, sans gel durable ;
- le log montre `-VideoEncoder: applied renegotiated properties [<l>x<h>@<fps>]`
  puis `-Created <codec> video encoder […]` ;
- le récepteur voit un **SSRC neuf** : un encodeur rouvert est une nouvelle base
  de temps ;
- **aucun gel du serveur**. C'est le point clé : le plan de contrôle ne joint
  plus aucun thread (le gel du 2026-08-13 venait de là).

Refaites la même chose côté audio avec `AudioTranscoderSetCodec`.

### 3. Détachement puis rattachement

Appel établi. `VideoTranscoderDettach` puis `EndpointAttachToVideoTranscoder`
vers une autre patte, sans détruire le transcodeur.

**Critère** : le flux repart sur la nouvelle patte, l'ancienne ne reçoit plus
rien, le total de threads est inchangé.

### 4. Arrêt de la source pendant l'appel

Appel vidéo transcodé établi. Coupez brutalement l'endpoint source (fermez
l'onglet du navigateur, tuez Linphone).

**Critère** :

- le serveur ne plante pas, et le log ne montre ni `SIGSEGV` ni trace d'arrêt ;
- le puits **cesse de recevoir**. C'est un changement de comportement assumé :
  l'encodeur ne duplique plus la dernière image quand la source se tait ;
- la destruction de la session (`MediaSessionDelete`) rend la main normalement.

### 5. Source lente : Linphone à 15 im/s vers un puits négocié à 30

Établissez un appel vidéo transcodé où la source envoie 15 im/s et où la sortie
est négociée à 30 im/s, `intraPeriod` à 300.

**Critère**, après 2 s d'appel :

- le log montre
  `-VideoTranscoder: cadence source mesuree 15 im/s, appliquee a l'encodeur …`
  **une seule fois**, pas en boucle ;
- puis `-VideoEncoder: cadence effective 30 -> 15 im/s, periode intra 300 -> 150
  images [consigne 30 im/s]` : la période intra suit, pour rester constante en
  secondes ;
- le débit sortant mesuré sur 5 s est à ±15 % de la consigne négociée. Sans
  cette reconfiguration il tomberait à la moitié ;
- une **seule** trame clé accompagne le changement.

### 6. Mute vidéo de 5 s puis reprise

Appel vidéo transcodé à 30 im/s. Coupez la vidéo côté source pendant 5 s, puis
reprenez à la même cadence.

**Le bouton « mute vidéo » de Linphone ne convient pas** : il remplace la caméra
par une image fixe et continue d'émettre. Le serveur ne voit aucune pause (la
mesure se déclenche sur un trou de plus d'une seconde), donc il n'y a rien à
observer. Coupez vraiment : désactivez la caméra dans les réglages, ou passez
l'appel en audio seul puis remettez la vidéo.

**Critère** :

- pendant la coupure, le puits ne reçoit rien, et la cadence mesurée **ne tombe
  pas** : une pause de plus d'une seconde est ignorée par la mesure ;
- à la reprise, **aucune** ligne `cadence effective` et **aucune** trame clé
  supplémentaire ne doivent apparaître si la cadence est la même ;
- si la source reprend à 15 im/s, la reconfiguration arrive après une fenêtre
  pleine (30 écarts), une seule fois.

## Feuille de relevé

À recopier et à remplir. Datez, et notez la version du binaire
(`curl 'http://<hôte>:8080/status/general?format=text'`).

| Scénario | Résultat | Threads | CPU | Remarque |
|---|---|---|---|---|
| 1a audio pont | | | | |
| 1b audio transcodé | | | | |
| 1c vidéo pont | | | | |
| 1d vidéo transcodé | | | | |
| 2 renégociation | | — | — | |
| 3 détachement/rattachement | | | — | |
| 4 arrêt de la source | | | — | |
| 5 source lente 15→30 | | | | |
| 6 mute 5 s | | | — | |

## Si un scénario échoue

Le log est la première source. Les lignes utiles portent toutes un préfixe :
`-VideoTranscoder:`, `-VideoEncoder:`, `-AudioEncoder:`.

Pour une image gelée ou absente, regardez d'abord le mode retenu : un pont
établi alors que le puits ne sait pas décoder le codec entrant donne exactement
ce symptôme.

Pour un gel du serveur, prenez la pile de tous les threads avant de redémarrer :

```sh
gdb -p $(pgrep -x mediaserver) -batch -ex 'thread apply all bt' > /tmp/mcu-gel.txt
```
