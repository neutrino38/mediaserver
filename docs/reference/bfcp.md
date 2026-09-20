# BFCP dans le mediaserver

BFCP (Binary Floor Control Protocol, RFC 4582) est le protocole de contrôle de
parole. Il désigne qui a le droit d'émettre sur un flux partagé — dans le
mediaserver, le partage de document ou d'écran.

Le dépôt contient **deux implémentations BFCP distinctes**. Une seule est
compilée.

## Celle qui est en service : le sous-module `libbfcp`

- Code : `third_party/libbfcp` (sous-module git), en C.
- Entrée : `bfcp_server.h`, classe `BFCP_Server`.
- Transport : **TCP, ou TLS**. C'est le BFCP des endpoints SIP (RFC 4583,
  `m=application … TCP/BFCP`).
- Threads : les siens (`bfcp_threads.h`).
- Bâtie par : `install.ksh libbfcp`, qui produit `libbfcp{dbg,rel}.a`.
  `mcu/Makefile` lie l'archive par chemin complet.
- Seul appelant dans le mcu : `mcu/src/shareddocmixer.cpp`. Il crée le
  `BFCP_Server` d'une conférence, ouvre son écoute TCP, et répond aux demandes
  de parole (`FloorRequestRespons`).

## Celle qui attend : `mcu/src/bfcp/` et `mcu/include/bfcp/`

Une pile BFCP en C++, orientée objet : un fichier par attribut
(`BFCPAttr*`), un par message (`BFCPMsg*`), plus `BFCPFloorControlServer`,
`BFCPUser` et `BFCPFloorRequest`.

**Elle n'est nommée dans aucune cible de `mcu/Makefile`.** Le compilateur ne la
voit jamais.

**Piège : modifier ces fichiers ne change pas le binaire.** Rien ne le signale —
ni erreur, ni avertissement. Un chantier transversal (verrous, threads,
pointeurs intelligents) qui balaie le dépôt par `grep` les touchera pour rien.

Son transport n'est **pas** celui de `libbfcp` : elle attend une connexion
WebSocket (`BFCPFloorControlServer::UserConnected(int, WebSocket*)`), héritage
du BFCP « web » de Medooze. Ce n'est donc pas un substitut direct.

## Ce que demanderait la réinternalisation

Remplacer `libbfcp` par la pile interne suppose, au minimum :

1. Un transport TCP et TLS pour `BFCPFloorControlServer`, qui n'en a pas
   aujourd'hui. C'est le vrai travail : les endpoints SIP n'offrent pas de
   WebSocket.
2. Une entrée dans `mcu/Makefile` (les 52 fichiers, plus les sous-répertoires
   `attributes/` et `messages/` dans `VPATH`).
3. Une réécriture de `shareddocmixer.cpp`, qui parle aujourd'hui l'API C de
   `BFCP_Server` et implémente son interface de rappel.
4. Une suite de tests. La pile interne n'en a aucune, et son parseur de
   messages lit du réseau : c'est exactement la surface que le durcissement des
   parseurs couvre ailleurs (`mcu/tests/`).
5. Une machine à états. `libbfcp` en porte une (`BFCP_fsm`) ; la pile interne
   traite les messages un par un.

Le gain attendu est de sortir un sous-module du build et de tenir tout le BFCP
en C++ testable. Aucune décision n'est prise.
