# Durcissement des parseurs réseau (chantier 5 de la rénovation C++)

Dernier chantier de fond de la rénovation C++ (les chantiers 1 à 4 — C++17,
`-Werror=return-type`, `delete-non-virtual-dtor`, `int-to-pointer-cast`, verrous
`std::mutex`, primitives `Wait`/`Worker` — sont clos). Objet : **tout ce qui
décode des octets venus du réseau** dans le mcu, à l'image du durcissement déjà
fait dans libmedikit (`h264.h`, `red.cpp`).

## Le principe

Un parseur réseau ne doit **jamais** croire une longueur qu'il vient de lire.
Trois fautes reviennent partout dans cette base :

1. **La longueur annoncée n'est pas confrontée à la longueur reçue** — le
   paquet dit « 40 octets » dans un datagramme de 8, et le code lit 40.
2. **Une soustraction de tailles non signées** (`DWORD`) passe sous zéro et
   devient un nombre gigantesque, qui sert ensuite de taille de `malloc`, de
   `memcpy` ou de borne de boucle.
3. **Un compteur d'éléments du paquet** (`count` RTCP, blocs RED, attributs)
   sert de borne de boucle sans être confronté à la taille disponible.

La règle appliquée ici : **une taille lue dans le paquet est une intention,
pas un fait** ; elle est confrontée à la taille réellement reçue avant tout
déréférencement, et un parseur qui n'y arrive pas rend 0/NULL au lieu de
deviner.

## Comment on le prouve

Les tests adverses vivent dans `mcu/tests/` et jouent deux registres :

- **Fonctionnel** : le parseur *refuse* proprement l'entrée malformée (retour
  0/NULL, champs non renseignés), au lieu de rendre un objet à moitié rempli.
- **Page de garde** (`tests/guardedbuffer.h`) : le paquet est posé en fin de
  page, la page suivante est `PROT_NONE`. Toute lecture d'un seul octet au-delà
  de la fin du paquet déclenche un SIGSEGV. Le parsing tourne dans un
  `EXPECT_EXIT` (fork), donc le crash fait échouer *ce* test sans emporter la
  suite. C'est ce qui distingue « ça n'a pas planté chez moi » de « ça ne
  déborde pas ».

## Les lots

| Lot | Périmètre | Fichiers |
|-----|-----------|----------|
| 1 | RTCP (rapports, feedback, SDES, BYE, APP) | `src/rtp.cpp`, `include/rtp.h` |
| 2 | En-tête RTP et extensions | `include/rtp.h`, `src/rtp.cpp`, `src/rtpsession.cpp` |
| 3 | RED / ULPFEC | `src/rtp.cpp`, `include/fecdecoder.h`, `src/fecdecoder.cpp` |
| 4 | RTMP (chunk, message, AMF) | `src/rtmpconnection.cpp`, `src/rtmpmessage.cpp`, `src/rtmpchunk.cpp` |
| 5 | HTTP / WebSocket | `src/httpparser.cpp`, `src/websocketconnection.cpp`, `include/websocketconnection.h` |

STUN a déjà été relu au passage (le parseur unique de `libmedikit`,
`stunmessage.cpp`, et le client `src/stunclient.cpp` écrit pendant le chantier
IPv6) : ses bornes sont correctes.

## État

Voir la section « Journal » en fin de fichier ; chaque lot y est daté avec les
défauts corrigés et les tests qui les tiennent.
