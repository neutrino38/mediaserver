# La détection de voix (VAD)

## À quoi elle sert

La VAD (*voice activity detection*) désigne **qui parle**. Elle ne coupe aucun
audio.

Son résultat suit trois étapes :

| Étape | Fichier | Ce qui se passe |
|---|---|---|
| Décision | `mcu/src/vad.cpp` | `VAD::CalcVad()` découpe en trames de 10 ms et rend 0 ou 1 |
| Accumulation | `mcu/src/pipeaudiooutput.cpp` | le 0/1 est multiplié par le nombre d'échantillons, puis cumulé |
| Usage | `mcu/src/audiomixer.cpp`, `mcu/src/videomixer.cpp` | le cumul choisit le locuteur affiché en mosaïque |

## D'où vient le moteur

Le détecteur est **libfvad**, le moteur VAD de WebRTC extrait en bibliothèque C
autonome. Il arrive par le sous-module `third_party/libvad`
(`https://github.com/aks-tel/libvad`).

Ce dépôt porte deux choses. Nous n'en prenons qu'une.

| Ce que le dépôt porte | Bâti ? |
|---|---|
| les 12 sources de libfvad (`fvad.c`, `vad/`, `signal_processing/`) | **oui** |
| la couche `sivr-vad` (machine à états issue de FreeSWITCH) | **non** |

Deux raisons de laisser `sivr-vad` de côté :

1. Elle ne compile pas hors FreeSWITCH. Elle emploie `int16_t`, `malloc`,
   `free`, `memset`, `strcmp` et `abs` sans inclure leurs en-têtes : c'est
   `switch.h` qui les fournissait.
2. Elle rend un état à hystérésis (*start talking* / *stop talking*), pas le 0/1
   par trame que `PipeAudioOutput` cumule.

Le mediaserver appelle donc `fvad_*` en direct, comme le fait `sivr-vad`
elle-même.

## Comment elle se bâtit

`compile_libvad` (dans `install.ksh`) appelle le `Makefile` du sous-module en
surchargeant trois variables **en ligne de commande**. Aucun fichier n'est écrit
dans le sous-module : IVèS ne forke pas ce dépôt, donc tout ce qui nous est
propre reste chez nous.

```sh
make -C third_party/libvad/sources \
    CFLAGS="-g -O2 -fPIC -I./sources -I./sources/signal_processing" \
    LIB_SRC='$(FVAD_SRC)' \
    ST_LIB=libfvad.a
```

`LIB_SRC='$(FVAD_SRC)'` n'est pas une liste de fichiers écrite à la main :
`FVAD_SRC` est la variable du `Makefile` amont, réévaluée chez lui. Un fichier
ajouté en amont est donc pris automatiquement. C'est ce qui évite une liste
parallèle, qui dériverait.

`mcu/Makefile` consomme le résultat par `VADDIR` : `-I$(VADDIR)/sources` pour
`fvad.h`, et l'archive `$(VADDIR)/libfvad.a` au lien.

## Le comportement, mesuré

Relevé sur le commit épinglé du sous-module, et couvert par
`mcu/tests/test_vad.cpp`.

- **Quatre fréquences** : 8000, 16000, 32000 et 48000 Hz. `IsRateSupported()` et
  `CalcVad()` s'accordent sur cette liste. Toute autre fréquence rend 0, avec une
  trace d'erreur.
- **La fréquence est portée par l'instance**, pas par l'appel. `CalcVad()` la
  réécrit quand elle change. Sans cela, une trame de 480 échantillons serait lue
  comme 60 ms de 8 kHz — longueur invalide, décision perdue en silence.
- **Quatre modes d'agressivité** : `QUALITY`, `LOWBITRATE`, `AGGRESSIVE`,
  `VERYAGGRESIVE`. Mêmes valeurs et mêmes noms que les modes fvad. Le
  mediaserver ouvre en `VERYAGGRESIVE`.
- **Traîne** : après de la parole, le détecteur déclare encore voisé un silence
  numérique pendant 100 ms en mode agressif, 150 ms en mode qualité. C'est du
  lissage : en mosaïque, la fenêtre du locuteur ne clignote pas entre deux
  syllabes.

## Licence

Le code embarqué est du WebRTC dérivé, sous licence BSD. Le sous-module
**ne porte ni `LICENSE`, ni `PATENTS`, ni `AUTHORS`**, alors que ses en-têtes y
renvoient.

Ces textes sont donc repris dans `LICENSE.libfvad`, à la racine du dépôt, et les
deux paquets les installent, sous `/usr/share/licenses/mcumediaserver-<version>/`
pour le RPM et `/usr/share/doc/mcumediaserver/` pour le `.deb`.
