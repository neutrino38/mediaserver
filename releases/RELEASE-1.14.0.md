# Medooze mediaserver 1.14.0

- amélioration de l'information retournée par /status/general. Découvert des encodeurs réellement supportés par ffmeeg
- support de RFC 8865 - T.140 Real-Time Text Conversation over WebRTC Data Channels
- refactioring des transcodeurs JSR 309 pour éliminer des threads inutiles. Asservissement en format d'image et en fréquence. Gestion des dépassements
  de temps d'encodage par une décimation de la fréquence entre les deux tronçons. Bref, les nouveaux transcodeurs vidéo sont top.
- utilisation de `std::shared_ptr` pour les trames audio et vidéo encodées
- seconde vague de petites amélioration sur le mixeur audio
- passage de l'encodeur VP8 en mode temps réel + multithread.
- ajustement du contrôle de débit pour éviter un flux constant de TMMBR si la bande passante ne bouge pas.

