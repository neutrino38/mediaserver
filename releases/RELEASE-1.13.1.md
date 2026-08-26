# Medooze mediaserver 1.13.1

Corrections de stabilité sur la reprise d'appel après mise en attente, paramétrage H.264 adaptatif

## Corrections de stabilité

- La reprise d'un appel après une mise en attente ne fige plus la vidéo : un mixeur temporairement sans image (décodeur du participant en resynchronisation, changement de résolution en cours de flux) est traité comme un état transitoire, plus comme une fin d'émission
- La reprise d'un appel après une mise en attente ne fait plus planter le serveur : le thread d'émission vidéo est systématiquement rejoint avant d'être réaffecté, quel que soit l'état où la mise en attente l'a laissé
- Le transcodeur vidéo JSR-309 ne fuit plus de thread et ne référence plus un objet déjà détruit en fin d'encodage
- La session de pont média (`mediabridgesession`) ferme son thread d'enregistrement dans tous les cas, y compris quand l'arrêt survient pendant son démarrage

## Vidéo

- Le CRF de l'encodeur H.264 (libx264) s'ajuste au débit disponible par pixel et par image, ce qui évite le pompage de qualité quand la bande passante est limitée

## Nettoyage

- Suppression de `mcu/include/videotranscoder.h`, un en-tête mort qui décrivait l'ancien modèle de threads et faussait les audits
