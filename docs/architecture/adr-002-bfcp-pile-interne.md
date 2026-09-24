# ADR 002 — BFCP : pile interne, TCP et UDP, pour la MCU seule

Statut : proposé.

Conception associée : [BFCP-INTERNE](../conception/BFCP-INTERNE/SPEC.md).

## Contexte

Le partage de document de la MCU repose sur BFCP. Jusqu'ici, le protocole
venait d'un sous-module en C, `third_party/libbfcp`, bâti dans l'arbre et lié
en archive statique.

Le dépôt portait **aussi** une seconde pile BFCP, en C++, héritée de Medooze :
`mcu/src/bfcp/`. Elle n'était dans aucune cible du Makefile. Elle contenait un
modèle fidèle à la RFC 4582 et un serveur de contrôle de parole complet, mais
ne parlait que JSON pour un transport WebSocket que personne ne demandait, et
un de ses fichiers incluait un en-tête absent du dépôt : l'arbre ne compilait
pas.

Trois choses rendaient la situation coûteuse :

1. **Deux piles, dont une invisible.** Modifier `mcu/src/bfcp/` ne changeait
   pas le binaire, et rien ne le signalait. Un chantier transversal qui balaie
   le dépôt les touchait pour rien.
2. **Le sous-module n'a aucun test**, et son BFCP n'était couvert par aucune
   des suites du dépôt.
3. **Des défauts constatés dans le sous-module**, qu'on ne pouvait corriger
   qu'en forkant : le surplus d'une lecture TCP jeté, une écoute liée en IPv4
   seule parce que le mcu lui passait `"0.0.0.0"`, et un UDP hors RFC 8855.

## Décision 1 — Le BFCP du mediaserver est la pile interne

`mcu/src/bfcp/` devient le BFCP en service : codec binaire RFC 4582 et
RFC 8855, contrôle de parole, transports TCP et UDP. Le sous-module
`third_party/libbfcp` sort du dépôt.

### Écarté : garder libbfcp et la corriger

Il aurait fallu forker le sous-module pour chaque correctif, et les défauts
trouvés touchent son découpage de flux, son adressage et son format filaire —
c'est-à-dire l'essentiel de ce qu'elle fait. Le dépôt aurait gardé deux piles.

### Écarté : prendre une autre bibliothèque

Aucune bibliothèque BFCP n'est empaquetée sur AlmaLinux 9. En vendre une
ramènerait exactement le problème qu'on retire.

### Écarté : supprimer `mcu/src/bfcp/` et rester sur libbfcp

C'était l'option qui ne coûtait rien à court terme. Elle laissait le BFCP hors
de portée des tests, et un code C sans couverture au milieu d'un chemin réseau.

### Conséquences

- Le BFCP est en C++, dans l'arbre, et couvert par cinq fichiers de tests dont
  une suite adverse du parseur derrière une page de garde.
- Un sous-module de moins à initialiser, bâtir et nettoyer.
- Les transports sont battus par le réacteur `RtpSessionSet` au lieu de deux
  threads `pthread` par conférence.
- Trois défauts du sous-module ne sont pas reconduits, et chacun a un test :
  les messages TCP collés, l'écoute IPv4 seule, et le port tiré par une socket
  sonde puis lié plus tard.
- Le format filaire est à notre charge, y compris la fiabilité UDP que le
  sous-module portait.

## Décision 2 — TCP et UDP, pas TLS

Les deux transports servis sont TCP et UDP. Une offre `TLS/BFCP` est refusée.

### Écarté : porter TLS

Le sous-module l'annonçait. Aucun déploiement ne l'utilise, et le coût n'est
pas le chiffrement lui-même mais ce qu'il traîne : certificats, révocation,
et un troisième transport à tester.

### Conséquences

- Un contrôleur qui offrirait `TLS/BFCP` reçoit un refus explicite, au lieu
  d'une négociation qui aboutit sur un transport muet.
- Rouvrir la question demandera un transport de plus, pas une option.

## Décision 3 — La MCU seule

BFCP reste une capacité de l'API XML-RPC `/mcu`. L'API `/jsr309` ne l'expose
pas.

### Écarté : ouvrir BFCP à JSR-309 dans le même chantier

JSR-309 n'a jamais eu de BFCP. L'ajouter aurait demandé des méthodes, des
événements et leur miroir dans les schémas protobuf MOTELI, pour un besoin que
personne n'a formulé.

### Conséquences

- Le contrat du contrôleur ne change pas : mêmes méthodes, mêmes paramètres,
  mêmes événements. Les schémas MOTELI n'ont rien à suivre.
- Un futur BFCP JSR-309 se poserait sur la même pile, avec sa propre API.

## Décision 4 — La version émise sur UDP est celle du pair

En réception, les versions 1 et 2 sont acceptées. En émission, le mediaserver
répond dans la version du dernier message reçu de ce pair.

### Écarté : n'émettre que la version 2

C'est ce que demande la RFC 8855 sur transport non fiable. Les endpoints en
service parlent la version 1, parce que c'est tout ce que le sous-module
émettait, et jettent en silence ce qui arrive en version 2.

### Écarté : n'émettre que la version 1

Cela reconduirait indéfiniment le comportement hors RFC du sous-module, et
fermerait la porte à un endpoint conforme.

### Conséquences

- Les endpoints déployés continuent de fonctionner sans rien changer chez eux.
- Un endpoint conforme est servi selon la RFC.
- Le mediaserver porte les deux versions, et ne peut pas les oublier : la
  suite de tests les éprouve toutes les deux.
