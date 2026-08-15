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

## Journal

**Les cinq lots sont faits (2026-08-15).** La suite passe de 335 à 379 tests
verts ; aucun test existant n'a changé de verdict.

### Lot 1 — RTCP (`test_rtcp_hardening.cpp`, 20 tests)

- Le paquet composé lisait l'en-tête des sous-paquets suivants sans vérifier
  qu'il restait quatre octets (`IsRTCP` ne valide que le premier).
- SR / RR / APP / RTPFB / PSFB / FIR / NACK lisaient leur corps sans exiger que
  la longueur annoncée le contienne : jusqu'à 24 octets hors datagramme pour un
  SR, et pour APP une soustraction non signée sous zéro — donc un `malloc` puis
  un `memcpy` de ~4 Go.
- Les compteurs du paquet (blocs de rapport, SSRC du BYE, gigues, descriptions
  SDES) servaient de borne de boucle sans être confrontés à la place restante ;
  la longueur de la raison du BYE (un octet, jusqu'à 255) non plus.
- Deux décalages faux : le BLP du NACK relu à l'offset 2 alors qu'il est écrit
  en 6 ; le `pictureId` du champ SLI cherché dans un cinquième octet d'un champ
  qui en fait quatre.
- `RTCPCompoundPacket::GetSize()` écrasait son accumulateur (`=` au lieu de
  `+=`) : la taille annoncée était celle du dernier sous-paquet, et `Serialize`
  acceptait donc un tampon trop petit pour le compound entier.
- Fuites sur entrée malformée : champ de feedback et description SDES non
  ajoutés à leur liste et jamais libérés.

### Lot 2 — En-tête RTP (`test_rtp_header_hardening.cpp`, 8 tests)

`RTPSession` ne vérifiait que `size >= 12`, alors que l'en-tête décrit sa propre
longueur (`cc` CSRC de 4 octets, extension annoncée en mots de 32 bits, jusqu'à
262 140 octets). `SetData` confronte désormais l'annonce à la taille reçue et
marque le paquet invalide ; `RTPSession` le jette, `ProcessExtensions` ne
parcourt rien sur un paquet invalide, et `GetExtensionLength` passe en `DWORD`
(tronquée en `WORD`, la valeur vérifiée différait de la valeur utilisée).

### Lot 3 — RED / ULPFEC (`test_red_fec_hardening.cpp`, 8 tests)

- La lecture des en-têtes RED n'avait aucune borne : sur un paquet dont tous les
  octets portent le bit « un autre bloc suit », elle sortait du paquet.
- La taille du bloc primaire (`taille - en-têtes - blocs`) passait sous zéro dès
  que les longueurs annoncées dépassaient le paquet ; le cumul des blocs tenait
  dans un `WORD` qu'une suite de blocs longs fait déborder ; et la longueur de
  bloc (10 bits) mélangeait ses deux moitiés (décalage de 6 au lieu de 8).
- `FECData` copiait la charge utile dans un tampon de MTU octets sans vérifier
  qu'elle y tenait (un paquet RTP en porte jusqu'à 1700), et sa « longueur de
  protection » (16 bits, annoncée) servait de taille de copie vers un tampon de
  pile.
- La reconstruction déréférençait un pointeur nul (`operator[]` sur une map
  *insère* une entrée nulle), nettoyait ses tables en testant la fin de
  l'itérateur après l'avoir déréférencé, et pouvait renvoyer sur le réseau des
  octets de pile jamais écrits.

### Lot 4 — RTMP (`test_rtmp_hardening.cpp`, 6 tests)

Message AMF3 de longueur nulle : l'octet de saut était lu hors du tampon et la
taille transmise au parseur AMF (`len - 1`) valait ~4 Go ; même faute sur une
trame vidéo vide. Un flux de chunks sans message ouvert déréférençait
franchement un pointeur nul. Côté sortie : tampon `malloc` rendu par `delete`,
messages retirés d'une file sans être détruits, et copie de chunk sans regarder
la taille du tampon de l'appelant.

### Lot 5 — HTTP / WebSocket (`test_websocket_http_hardening.cpp`, 2 tests)

Le parseur HTTP embarqué (Node.js/nginx) est éprouvé et n'a pas été touché. Le
défaut était dans **nos callbacks** : ils remplaçaient au lieu d'accumuler,
alors que le parseur rend ses chaînes par morceaux (autant que TCP en a
découpé). Une clé `Sec-WebSocket-Key` coupée en deux donnait une réponse
d'acceptation fausse, et `on_url` créait une requête neuve — donc en fuyait une
— à chaque fragment d'URL. Un client provoque les deux en écrivant octet par
octet.

## Ce qui n'a pas été touché, et pourquoi

- **`httpparser.cpp`** est le parseur HTTP de Node.js/nginx, largement audité en
  amont : le durcissement utile était côté appelant (lot 5), pas dans ses états.
- **STUN** (`libmedikit/stunmessage.cpp`, `mcu/src/stunclient.cpp`) : relu, ses
  bornes d'attributs sont correctes.
- **AMF** au-delà du point d'entrée : `test_amf.cpp` le couvre déjà en
  round-trip, et deux défauts y avaient été corrigés lors de sa création.
- Les limites de **taille de message RTMP** (16 Mo annoncés par le client, cinq
  flux de chunks autorisés) : c'est une question de politique de ressources, pas
  de sûreté mémoire — à traiter avec les autres quotas si le besoin vient.
