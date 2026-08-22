# Medooze mediaserver 1.13.0

Contrôle de débit adaptatif, IPv6 de bout en bout, durcissement des parseurs réseau

## Contrôle de débit

- L'estimateur de débit côté récepteur (REMB) mesure enfin ce qu'il croit mesurer : deux inversions d'arguments qui le figeaient sur une valeur constante sont corrigées
- Estimateur de débit côté émetteur ajouté (sender-side BWE sur transport-cc), en complément du REMB
- Boucle de feedback sortant fermée : REMB/TMMBR partent selon ce que le pair a négocié, plus seulement en SIP classique
- Lisseur d'émission (pacer) à budget continu, avec régime auto-limité et réouverture par paliers
- Fenêtre glissante de 5 s pour le plafond entrant, hausse TMMBR limitée à une fois par 5 s ou à un pas d'au moins 20 % : corrige l'oscillation constatée face à Linphone, validé en appel réel
- VP8 : dépacketizer RTP ajouté à libmedikit

## IPv6

- Le mediaserver écoute et parle IPv6 de bout en bout : RTP/RTCP, STUN/ICE, BFCP, SDP, DNS AAAA
- Les profils d'adressage distinguent l'adresse liée et l'adresse annoncée sur chaque famille
- La couche Java (jsr309impl, XmlRpcMcuClient) ne suppose plus qu'une adresse tient sur 32 bits

## Durcissement

- Les parseurs RTP, RTCP, RED/ULPFEC, RTMP et WebSocket ne font plus confiance aux longueurs qu'ils lisent
- Correction d'un SIGSEGV du TextMixer sur un worker déjà détruit
