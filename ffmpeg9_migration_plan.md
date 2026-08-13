# Portage ffmpeg 9 + SVT-AV1 moderne — actions sur libmedikit

Fiche ouverte le 2026-08-13, à la suite du crash SVT-AV1 analysé le même jour.
Elle inventorie **ce qu'il faudra faire dans `third_party/fontventa/libmedikit`
(et ses appelants)** le jour où la compilation ffmpeg 9 + AV1 moderne remplacera
les paquets AlmaLinux 9 actuels (`ffmpeg-libs 5.1.9` de RPM Fusion,
`svt-av1-libs 0.9.0` d'EPEL).

Ce n'est pas un plan de portage de ffmpeg : c'est la liste des points du dépôt
qui **changent d'état** à ce moment-là. Chaque entrée est vérifiée dans l'arbre,
pas supposée.

---

## 0. Pourquoi cette fiche existe

`svt-av1` est figé à **0.9.0** dans EPEL 9 (build de janvier 2022) et aucun
dépôt activé n'en propose de plus récent. Cette version tient son inventaire de
processeurs dans un pointeur **global de processus**, `lp_group`
(`Source/Lib/Encoder/Globals/EbEncHandle.c`), que `svt_av1_enc_deinit_handle()`
libère inconditionnellement — même quand d'autres instances d'encodeur sont
vivantes. Deux pattes qui ouvrent/ferment leur encodeur AV1 simultanément (ce
que fait tout ré-INVITE) déréférencent NULL : SIGSEGV, serveur mort en pleine
communication. Constaté en trafic réel le 2026-08-13.

Deux correctifs ont été posés **de notre côté**, en attendant :

| Correctif | Où | Sort au moment du portage |
|---|---|---|
| Sérialisation des ouvertures/destructions de contexte d'encodage | `medkit/ffcodeclock.h` + 5 sites dans `ffvideocodec.cpp` | **À retirer** (§2) |
| Création paresseuse de l'encodeur (rien en mode pont) | `mcu/src/jsr309/VideoEncoderWorker.cpp` | **À garder** (§2) |

---

## 1. Ce qui bloque la compilation

Vérifié par inventaire des symboles dépréciés/supprimés réellement utilisés.

### 1.1 `AVFrame::key_frame` — 3 sites vivants, bloquant

Le champ est déprécié depuis ffmpeg 6.1 et **supprimé en 8.0** au profit du
drapeau `AV_FRAME_FLAG_KEY` dans `AVFrame::flags`.

- `ffvideocodec.cpp:603` — `frameToSend->key_frame = 1;`
- `ffvideocodec.cpp:609` — `frameToSend->key_frame = 0;`
- `ffvideocodec.h:134` — `FfVideoDecoder::IsKeyFrame()`, lecture du champ

Remplacement : écrire/lire `frame->flags & AV_FRAME_FLAG_KEY`.

### 1.2 Code mort portant des API supprimées — à supprimer

`h264/h264decoder.cpp` porte un `H264Decoder::Decode()` **entièrement commenté**
(à partir de la ligne ~505) qui utilise `av_init_packet()` (l.511) et
`avcodec_decode_video2()` (l.514), tous deux retirés de ffmpeg depuis longtemps.
Ce n'est pas un bloqueur — le bloc n'est pas compilé — mais il fera perdre du
temps au prochain qui cherchera les API à porter. À supprimer dans le lot.

### 1.3 Aucune garde de version

`grep 'LIBAVCODEC_VERSION\|FF_API_'` sur `libmedikit` et `mcu/src` ne rend
**rien** : le dépôt compile contre exactement une version majeure de ffmpeg, il
n'y a pas de double support. Le portage est donc un **basculement franc**, pas
une compatibilité à maintenir. Décider explicitement si on veut le garder ainsi
(recommandé : oui, une seule cible) ou introduire des gardes.

---

## 2. Le contournement à retirer — et celui à garder

### 2.1 À RETIRER : `medkit/ffcodeclock.h`

**Précondition à vérifier avant de retirer**, dans le source de la version
SVT-AV1 retenue :

1. `lp_group` (ou son successeur) n'est plus un global de processus, **ou**
2. `svt_av1_enc_deinit_handle()` ne le libère plus inconditionnellement (compteur
   de références, `once`, ou état déplacé dans l'instance).

Tant qu'aucun des deux n'est vrai, **garder le verrou** : le coût est nul
(une prise par établissement de patte, aucune par trame).

Une fois la précondition tenue, retirer :

- le fichier `third_party/fontventa/libmedikit/medkit/ffcodeclock.h` ;
- son `#include` en tête de `ffvideocodec.cpp` ;
- les 5 portées `std::lock_guard<std::mutex> lock(FfCodecOpenLock())` dans
  `ffvideocodec.cpp` : `SelectCodec` (~l.141), `CloseCodec` (~l.236),
  `FallbackToSoftware` (~l.274), `~FfVideoEncoder` (~l.295), et `OpenCodec`
  (~l.439, où il faut aussi remettre `avcodec_open2()` directement dans le `if`
  et supprimer la variable intermédiaire `openErr`).

Le `Makefile` n'a rien à désapprendre : le suivi de dépendances est en
`-MMD -MP`, il n'y a pas de liste d'en-têtes à tenir.

### 2.2 À GARDER : la création paresseuse de l'encodeur

`VideoEncoderMultiplexerWorker::Encode()` ne crée plus l'encodeur avant la
boucle mais à la **première image réellement capturée**. Ce changement est
indépendant de la version de SVT-AV1 :

- en mode pont (`VideoTranscoder::onRTPPacket`, `state == 2`) les paquets sont
  relayés tels quels, le pipe ne reçoit jamais d'image, et aucun encodeur n'est
  instancié — c'est un encodeur logiciel en moins par patte pontée, sur une
  machine à 2 cœurs sans VAAPI ;
- le mode n'étant arbitré qu'au premier paquet RTP reçu, donc après le démarrage
  du thread d'encodage, la présence d'une image dans le pipe est le seul signal
  disponible au bon moment.

Ne pas le défaire en même temps que §2.1 : ce sont deux sujets distincts qui se
trouvent avoir eu le même symptôme.

---

## 3. AV1 : ce qui est à re-régler, pas juste à recompiler

### 3.1 Le preset

`av1/av1codec.cpp:121` lit `av1.preset` (défaut **10**) et le pose en l.135 via
`av_opt_set_int(ctx->priv_data, "preset", ...)`. Le commentaire l.116 fige
l'échelle « presets 0-13 » de SVT-AV1 0.9.0.

Les versions récentes ont élargi l'échelle (jusqu'à `-1`) **et modifié le
rapport vitesse/qualité de chaque cran** : le 10 d'aujourd'hui n'est pas le 10
de demain. À re-mesurer sur la cible réelle (352×288, 20 fps, 2 cœurs, pas de
VAAPI) et à réajuster, plutôt qu'à reporter tel quel.

### 3.2 Le forçage de backend

`av1/av1codec.cpp:118` force `"libsvtav1"` parce que ffmpeg choisirait
`"libaom-av1"`, trop lent pour du direct. Vérifier que le nom résout toujours
dans le ffmpeg 9 bâti, et que `libaom-av1` reste bien le défaut à contourner —
sinon le commentaire devient faux et le forçage inutile.

Idem l.395 pour `"libdav1d"` en décodage.

### 3.3 L'écrêtage au niveau AV1

`AV1Encoder::ClampToLevel` (appelé depuis `VideoEncoderWorker.cpp:253`) écrête
taille et cadence au niveau AV1 déclaré par le pair. À revalider contre la table
de niveaux de la version retenue.

### 3.4 La question des capacités, qui revient

Un AV1 moderne change ce que le serveur sait faire — et
`codec_capabilities_plan.md` documente déjà que **personne ne sait le lui
demander** : `GetSupportedCodecs` (`mcu/src/xmlrpcmcu.cpp`) est un tableau écrit
à la main, sans OPUS, muet sur la vidéo, et l'API JSR-309 ne l'expose pas. Le
portage est l'occasion de fermer ce trou plutôt que de le franchir une fois de
plus : si elixip doit re-deviner ce que porte le nouveau serveur, on rejoue
exactement l'incident du 2026-08-12 (appel AV1 ↔ AV1 mort en 488).

---

## 4. Points d'accroche build et packaging

### 4.1 libmedikit — **fait**

`third_party/fontventa/libmedikit/Makefile` demande désormais ses flags ffmpeg à
`pkg-config` (`libavcodec libswscale libavformat libavutil libswresample`), avec
repli sur les chemins historiques et avertissement si pkg-config ignore ffmpeg.
`install.ksh` n'écrase plus `INCLUDE` : tous les chemins d'en-têtes sont dans le
Makefile du sous-module, et `FFMPEGINC` / `MP4V2INC` y restent utilisables.

Le sous-module génère aussi un **`libmedkit.pc`** (non versionné, chemins absolus
vers le clone, motif « uninstalled ») qui déclare ffmpeg en `Requires:` — c'est
une dépendance publique, `ffvideocodec.h` incluant `<libavcodec/avcodec.h>` :

```sh
PKG_CONFIG_PATH=third_party/fontventa/libmedikit pkg-config --cflags --libs libmedkit
# -> -I<clone>/libmedikit -I/usr/include/ffmpeg -L<clone>/libmedikit -lmedkit -lavcodec …
```

Bâtir contre un autre ffmpeg ne demande donc plus qu'un `PKG_CONFIG_PATH`.

### 4.2 mcu — **reste à faire**

| Fichier | Ligne | Contenu |
|---|---|---|
| `mcu/Makefile.rpm` | 34, 156 | `-I/usr/include/ffmpeg` — **en dur** (deux fois) |
| `mcu/Makefile.rpm` | 186 | branche `else` (AlmaLinux 9) : `-lavcodec -lswscale -lavfilter …` |

Action : consommer `libmedkit.pc` (`PKG_CONFIG_PATH=$(MEDKITDIR)`), qui rend déjà
les `-I` de ffmpeg par la chaîne `Requires:`, et compléter par `pkg-config` pour
les modules que le mcu utilise en propre et que libmedikit n'a pas —
`libavfilter`, `libavdevice`, `libpostproc`. C'est l'idiome du fichier, qui le
fait déjà pour `Magick++`, `webrtc-audio-processing` et `gtest`.

Sans ça, un ffmpeg installé sous un autre préfixe est silencieusement ignoré au
profit des en-têtes système, avec des symboles qui ne correspondent plus à la
bibliothèque liée — et le mcu, lui, se lie bien à la nouvelle.

Packaging, à mettre à jour dans le même lot :

- `mcumediaserver.spec:12` — `Requires: … ffmpeg …`
- `mcumediaserver.spec:15` — `BuildRequires: ffmpeg-devel`
- `install.ksh:132` — `rpm -q ffmpeg-devel` (contrôle de prérequis)
- `install.ksh:368` — la ligne `yum install` des prérequis

Et **ajouter la dépendance SVT-AV1**, aujourd'hui absente du spec : elle
n'arrive que par transitivité de `ffmpeg`. Avec un ffmpeg bâti maison la
transitivité disparaît — c'est exactement le genre d'oubli qui produit un
serveur qui démarre et ne sait pas encoder.

---

## 5. Ordre de bataille proposé

1. **Avant tout** : lire le source SVT-AV1 retenu et trancher la précondition
   §2.1 (`lp_group` toujours global ? libéré inconditionnellement ?). C'est ce
   qui décide si le verrou part ou reste — et c'est cinq minutes de lecture.
2. Paramétrer les chemins ffmpeg du **mcu** (§4.2) — libmedikit est déjà fait
   (§4.1). Indépendant du reste, faisable tout de suite, et sans quoi on ne peut
   même pas compiler contre la nouvelle version.
3. Porter `key_frame` (§1.1) et supprimer le code mort (§1.2).
4. Compiler, faire passer les deux suites (§6).
5. Re-régler AV1 (§3.1–3.3) **sur trafic réel**, pas sur banc.
6. Retirer le verrou (§2.1) si et seulement si l'étape 1 l'autorise.
7. Mettre à jour le packaging (§4) et le `Requires` SVT-AV1.

---

## 6. Vérification

```sh
# Les deux suites, vertes avant et après (état de référence au 2026-08-13 :
# mcu 199/199, libmedikit 121/121)
cd mcu && make -f Makefile.rpm check
make -C third_party/fontventa/libmedikit check
```

Puis, en trafic, le scénario qui a produit le crash — c'est lui le juge :

- Alice (Linphone) appelle Bob (Linphone), **vidéo AV1 des deux côtés** ;
- laisser le ré-INVITE se produire (il redémarre les deux encodeurs à quelques
  dizaines de ms d'intervalle : c'est la fenêtre de course) ;
- vérifier qu'aucun core n'apparaît dans `/var/crash`, que la vidéo passe dans
  les deux sens, et qu'aucun `-CreateVideoEncoder[110,AV1]` n'est journalisé
  pour une patte restée en mode pont (`VideoTranscoder: switched to bridged
  mode for codec AV1`).

Traces de référence de l'incident d'origine : `/home/ebuu/elixip3.log`,
`/home/ebuu/mcu3.log`, core `core.mediaserver.1578438.*` (2026-08-13 07:11:37).
