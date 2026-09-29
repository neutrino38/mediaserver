# ADR 002 — La sonde GPU tourne hors du processus serveur

Statut : proposé.
Conception associée : [HWACCEL-SONDE](../conception/HWACCEL-SONDE/SPEC.md).

## Contexte

Au démarrage, le serveur doit vérifier que chaque capacité GPU donne une image
juste avant de s'en servir. La vérification encode, décode et compose de vraies
images avec le driver VAAPI.

Cette vérification peut **tuer le processus qui la fait**. Un encodeur VAAPI
qui refuse ses surfaces remplit son DPB, puis libavcodec meurt sur une
assertion (`pic->nb_dpb_pics < 16`). `avcodec_send_frame` sur un `h264_vaapi`
jamais alimenté segfaute. Un driver défaillant peut aussi bloquer un appel sans
jamais rendre la main.

Si la sonde tue le serveur, systemd le relance, la sonde le tue de nouveau :
le service ne démarre jamais, et c'est précisément sur le poste au driver
défaillant.

## Décision — un processus séparé : le binaire relancé avec `--hwprobe`

`main()` lance `/proc/self/exe --hwprobe`, avant de créer le moindre thread ou
codec. Il lit une ligne par capacité jugée, avec un délai global. Si le
processus de sonde meurt ou dépasse le délai, les capacités non jugées sont en
échec, et le serveur démarre sans elles.

Deux avantages :

- un processus neuf, sans rien hérité du père : ni threads, ni état libva, ni
  descripteurs ;
- une commande de diagnostic gratuite pour l'exploitant : `mediaserver --hwprobe`.

Le coût : un `exec` au démarrage, et un protocole de lignes à tenir stable
entre le père et le processus de sonde. Les deux sont le même binaire, donc ce
protocole ne peut pas diverger d'une version à l'autre.

### Écarté : la sonde dans le processus serveur

C'est la voie la plus simple et la plus rapide. Mais une assertion dans
libavcodec tue le serveur, et un appel bloqué dans le driver le bloque. Le
poste où la sonde sert le plus est celui où elle rend le service indémarrable.

### Écarté : `fork()` sans `exec`

Le fils hérite de l'état du père. Un `fork()` fait après l'initialisation de
bibliothèques qui ont des threads ou des verrous internes (libva, le driver
iHD, ffmpeg) peut bloquer le fils sur un verrou tenu par un thread qui n'existe
plus chez lui. Le moment sûr pour forker est très tôt dans `main()`, avant tout
cela. Or `exec` ne coûte rien de plus et ne dépend pas de ce moment.

### Écarté : un binaire d'outil séparé (`mediaserver-hwprobe`)

Il faudrait l'installer, le versionner et le livrer dans le RPM et le `.deb`
en même temps que le serveur. Un écart de version entre les deux ferait sonder
une autre bibliothèque que celle que le serveur charge. Le même binaire, relancé,
exclut cet écart.

## Conséquences

- La sonde n'a pas accès à l'état du serveur, et n'en a pas besoin : elle ne
  teste que le couple driver + ffmpeg.
- Le délai global borne le temps de démarrage, même face à un driver bloqué.
- Un verdict « non testée » après la mort du processus de sonde est prudent :
  une capacité qui aurait marché est perdue jusqu'au prochain démarrage.
