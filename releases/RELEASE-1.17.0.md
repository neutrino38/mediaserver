# Medooze mediaserver 1.17.0

Plusieurs flux vidéo par participant, BUNDLE côté conférence et JSR-309, partage de document BFCP vu comme une source de mosaïque, écran noir corrigé sur changement de SSRC

## Mise à jour : ce qui change pour l'exploitant

- Le paquet RPM déclare sa licence exacte : `GPL-3.0-only AND BSD-3-Clause`. Le texte de la GPL (`LICENSE.md`) est installé avec le paquet
- Aucune option de ligne de commande ne change

## Plusieurs flux vidéo par participant

Un participant peut envoyer plusieurs vidéos : sa caméra, un partage d'écran, une seconde caméra. Chaque flux devient une source du mixeur. Il prend donc sa place dans une mosaïque comme n'importe quel participant.

- API MCU : `CreateVideoStream`, `DeleteVideoStream`, `SetVideoStreamMosaic`, `GetVideoStreamSource`. Un rôle à partir de 2 reçoit sa propre jambe et son propre `sourceId`
- API JSR-309 : `EndpointCreateVideoStream` et `EndpointDeleteVideoStream`. Les 18 méthodes qui agissent sur une jambe prennent un rôle optionnel en dernier paramètre. Un B2BUA peut ainsi relayer le partage d'écran d'un navigateur à côté de sa caméra
- Le client Java XML-RPC (`XmlRpcMcuClient`) est aligné sur l'API
- Documentation : `docs/MCU-API.md` §3, §4, §6.3 et §6.5. Conception : `docs/conception/UNIFIED-PLAN/SPEC.md`

## BUNDLE

BUNDLE fait passer tous les médias d'un appel par un seul transport (un seul port, une seule négociation DTLS). Un navigateur offre sa seconde vidéo en « bundle-only » : sans BUNDLE, cette vidéo était perdue.

- `CreateParticipant(bundle=1)` (MCU) et `EndpointCreate(bundle)` (JSR-309) rattachent la vidéo et le texte à la jambe audio
- La jambe audio porte le transport. Elle aiguille chaque paquet RTP par son MID, puis par son SSRC, puis par son payload type
- L'extension d'en-tête RTP MID est lue sur chaque paquet. Elle est écrite quand elle est négociée
- Décision : `docs/architecture/adr-004-bundle-session-porteuse.md` (session porteuse plutôt que découpage de `RTPSession`)

## Partage de document (BFCP)

- Le document partagé devient une source du mixeur, comme un flux vidéo. Le navigateur peut donc le voir dans une case de mosaïque
- La prise de parole BFCP place la source du présentateur dans la mosaïque partagée et démarre le flux SLIDES. Le relâchement la retire et arrête SLIDES
- L'événement `ParticipantRequestDocSharing` porte le `sourceId`
- Un flux SLIDES arrêté revient quand une nouvelle présentation commence. Avant, il échouait toujours avec « No video »
- Après un changement de SSRC du correspondant, l'audio n'est plus pris pour du contenu BFCP. Avant, l'audio devenait muet

## Corrections RTP et DTLS

- **Écran noir de plusieurs secondes sur changement de SSRC.** Un consommateur gardait le verrou des flux pendant toute son attente (200 ms). Le réacteur RTP attendait autant pour renommer un SSRC. Le consommateur attend maintenant hors du verrou
- Une nouvelle offre DTLS identique garde l'association en cours. Avant, elle la recréait, et le data channel SCTP qu'elle porte tombait
- Le serveur demande une image clé (FIR/PLI) dès le premier paquet vidéo reçu. Un partage d'écran Chrome restait vide jusqu'à sa prochaine image clé spontanée
- `ConfigureMediaConnection` refuse le WebSocket pour la vidéo. Avant, le processus mourait sur une exception non rattrapée

## Tests

- Nouvelles suites : MID, multi-vidéo (MCU et JSR-309), partage de document sur un vrai floor BFCP, verrou des flux RTP
- Les traces du serveur vont dans `tests/test.log` au lieu de noyer le rapport gtest. La variable `GTEST_MCU_LOG` choisit le fichier (`-` = sortie standard)

# Limitations

- Le multi-vidéo, le BFCP sur ce modèle et BUNDLE sont validés par les tests seulement. Au moment de cette version, la SPEC UNIFIED-PLAN marque leurs recettes en appel réel « à faire »
- Le branchement côté contrôleur (elixip, lot 4 de la SPEC) n'est pas complet
