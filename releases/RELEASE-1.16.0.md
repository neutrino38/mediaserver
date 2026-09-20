# Medooze mediaserver 1.16.0

Enregistrement JSR-309 en `.mkv` et `.mp4` sans transcodage inutile, panne du texte sur WebSocket corrigée, réacteur RTP qui ne reste plus bloqué sur un changement de SSRC

## Enregistrement JSR-309

- Le Recorder JSR-309 supporte désormais le format .mkv.

## Correction du texte sur WebSocket 

Un appel avec Recorder perdait tout son média environ 15 s après le décroché : plus d'audio, plus de texte, vidéo figée. Un seul défaut, quatre maillons.

Correction de l'envoi des BOM keepalive pour le renvoyer avec un numéro de séquence croissant. Correction d'un bug de décodage RED qui entrainait une boucle de 4,28 Md
de caractère de remplacement. Supppression des traces contenant le contenu des sous-titres texte à l'enregistrement.

Correction: un profil d'adresse demandé sur un Endpoint texte sur Websocket est accetpé. 

## Crrection `VideoDecoderJoinableWorker::DecodePacket`

Correction d'un bug similaire au décodage RED.

## RTP et changement de SSRC

Amélioration de la gestion de changement de SSRC.


## Couche RTP, DTLS et STUN

- Un `ClientHello` DTLS reçu **avant** `SetRemoteCryptoDTLS` est conservé, puis rejoué et répondu dès l'initialisation. Le pair n'attend plus sa retransmission, à une seconde de là
- Latching NAT : une nouvelle cible posée par le plan de contrôle efface la source observée. Un pair qui se déplace est suivi, même si l'émission n'a pas cessé
- Un serveur STUN donné en littéral IPv6 est refusé avec un message explicite. Le NAT servi par ce produit est IPv4 seulement
- Amortisseur de débit : sous un plafond externe, seule la valeur **annoncée** compte. Une hausse de la mesure locale qui laisse ce plafond inchangé ne réémet plus de TMMBR. La mesure est retenue et sert à la levée du plafond
- AMF : le zéro fait un aller-retour exact, signe compris, au lieu de rendre un dénormal

## Tests et documentation

- Revue de rationalisation des tests automatisés : constat chiffré, leviers, matrice de cas de la régulation de débit, plan en huit étapes (`docs/conception/TESTS-RATIONALISATION/SPEC.md`)
- Nouvelles suites : fichiers produits par le Recorder JSR-309, profil d'adressage d'un endpoint, décodeur de redondance texte, famine de l'écrivain dans `Use`
- Le test de pause vidéo joue 25 s d'horodatages au lieu de dormir 3 s
- libmedikit : `ParseRed` n'expose plus un en-tête dont la longueur ment, et le writer de fichiers a sa propre suite
- État mesuré avant publication : mcu 728 tests, 724 passés et 4 sautés faute d'adresse IPv6 sur la machine de build ; libmedikit 202 tests, tous passés

## Sous-module libmedikit

- `FfMediaFileWriter` : écriture `.mp4` et `.mkv` par libavformat, avec annonce de piste (`ExpectTrack`)
- Transcodeur remanié, `framescaler` supprimé
- Durcissement du parseur RED

# Limitations

- La correction de la panne du texte sur WebSocket est **validée par les tests seulement**. La recette en appel de bout en bout reste à faire : il faut dépasser deux keepalives BOM, soit plus de 30 s après le décroché
- Le mediaserver ne lit **aucun SR** de la jambe Asterisk 1.4 : ce pair émet ses compounds RTCP avec un octet de queue en trop, et le parseur jette alors le datagramme entier. Donc ni RTT, ni pertes rapportées de ce côté. Non corrigé
- Le contrôle de débit (rate control) n'est toujours pas satisfaisant. Le chantier reste reporté à une prochaine version
- Un flux vidéo VP8 ne s'enregistre toujours pas en MP4 : le conteneur ne porte pas ce codec, et le transcodage VP8 vers H.264 n'est pas fait. Enregistrer en `.mkv` est désormais la réponse
