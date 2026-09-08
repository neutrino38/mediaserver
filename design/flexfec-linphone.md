# FlexFEC dans Linphone SDK 6 — analyse de l'implémentation oRTP

> Analyse sur pièces (2026-08-15), demandée en marge du chantier contrôle de
> débit ([`../docs/RATE-CONTROL.md`](../docs/RATE-CONTROL.md), « hors périmètre »).
> Références, commit `be0395f9` du monorepo linphone-sdk :
> - `ortp/src/fecstream/` — 16 fichiers, ~118 Ko (encodeur, paquets, params,
>   cluster de réception, stats, overhead) ;
> - `ortp/src/fecstream/fec-encoder.cpp`, `packet-api.cpp`, `fec-params.cpp`,
>   `fecstream.cc`, `receive-cluster.cpp` ;
> - `liblinphone/tester/call_flexfec_tester.cpp` ;
> - `ortp/src/avprofile.c` (définition du payload) ;
> - texte normatif : [RFC 8627](https://datatracker.ietf.org/doc/rfc8627/)
>   (§4.2.2.2 pour le mode L/D, §4.2.1 pour le CSRC, §5.1/§7.1 pour le SDP).

## 1. Verdict de format : RFC 8627 finale, mode lignes/colonnes — PAS flexfec-03

Trois preuves convergentes dans le code :

1. **Le payload s'appelle `flexfec`** (`avprofile.c`) : MIME `flexfec`, clock
   90000, `fmtp repair-window=200000` (µs, soit 200 ms), type vidéo, PT
   dynamique. WebRTC négocie `flexfec-03` — **les deux noms ne se répondent
   même pas en SDP**.
2. **L'en-tête réparateur fait 12 octets** (`packet-api.cpp`) : bitstring de
   8 octets (XOR des 16 premiers bits d'en-tête + timestamp + longueur — les
   champs *recovery*) puis `SN base` (2) + `L` (1) + `D` (1). C'est **octet
   pour octet** la Figure 13 de la RFC 8627 §4.2.2.2 (mode R=0/F=1) : 2 octets
   R|F|P|X|CC|M|PT recovery, 2 de length recovery, 4 de TS recovery, SN base,
   L, D. Sémantique de L/D (§4.2.2.2, Figure 14) : `L>0,D=0` = FEC de ligne
   seule (1D) ; `L>0,D=1` = ligne, colonnes à suivre (2D) ; `L>0,D>1` = FEC de
   colonne (un paquet sur L, D fois — d'où le `step=L` du
   `createSequenceNumberList`). Le flexfec-03 de WebRTC utilise, lui, des
   **masques** de 15/46/109 bits.
3. **Le SSRC protégé voyage en CSRC** du paquet réparateur
   (`getProtectedSsrc()` = `rtp_get_csrc(mPacket, 0)`) — signature RFC 8627.

**Conséquence interop : il existe deux mondes FEC disjoints.** Les navigateurs
émettent ULPFEC (flexfec-03 derrière un field trial jamais activé) ;
Linphone 6 émet FlexFEC RFC 8627. Aucun pont possible
au niveau du fil : un serveur qui veut réparer les deux parcs doit décoder les
deux formats. Notre décodeur ULPFEC couvre le premier ; le second est à écrire.

## 2. La mécanique, côté fil

- **Flux réparateur = session RTP dédiée** (`fecstream.cc`, `mFecSession`) :
  SSRC et numéros de séquence propres, pas de jitter buffer, multiplexée avec
  le média — **tous les tests activent `enable_rtp_bundle(TRUE)`**, le bundle
  est manifestement le prérequis de transport.
- **Protection matricielle L×D** (`fec-encoder.cpp`) : chaque paquet source
  entre dans un réparateur de ligne (`mRowRepair[i]`) et, en mode 2D, un de
  colonne (`mColRepair[j]`). La liste protégée d'un réparateur est
  `seqnumBase + i×step` (step=1 pour une ligne, step=L pour une colonne).
- **Réparation multi-passes** (`receive-cluster.cpp`) : `repair2D()` alterne
  passes lignes et colonnes tant qu'une passe récupère quelque chose ;
  `repairOne()` reconstruit un paquet quand il est l'unique absent de sa liste
  (XOR des bitstrings + payloads présents). Le paquet reconstruit récupère
  numéro de séquence, SSRC, timestamp et payload complet.
- **Fenêtre de rétention** : `mRepairWindow` (le `repair-window` négocié,
  200 ms) borne cluster source et réparateurs (`cleanSource`/`cleanRepair`).

## 3. L'adaptation — et son lien avec notre chantier contrôle de débit

`fec-params.cpp` choisit un **niveau de protection** (tableaux `mLvalues`,
`mDvalues`, `mIs2Dvalues`) à partir de trois entrées : **taux de perte**,
**bande passante disponible**, **overhead courant**. L'overhead théorique est
`1/L + 1/D` (2D) ou `1/L` (1D), plafonné par zone de bande passante
(`mMaxOverheadList[0..2]` selon basse/moyenne/haute) ; un `mMaxLossRate`
coupe la FEC quand ça perd trop pour qu'elle serve. Pas d'hystérésis
explicite ; l'overhead réellement émis est mesuré (`getMeasuredOverhead()`).

C'est exactement le raisonnement « la FEC s'achète sur le budget média »,
en plus simple que le `fec_controller` de libwebrtc :
la redondance est une fonction de (perte, bande passante) plafonnée — pas un
pourcentage fixe. Les scénarios du testeur le confirment : variation
5 Mb/s → 300 kb/s → 5 Mb/s, pertes 5-8 %, VP8 et AV1, avec SRTP/DTLS/ZRTP/ICE.

## 4. Conséquences pour le mediaserver

1. **Pas d'urgence interop, mais un plafond de robustesse.** Sans bundle chez
   nous, un Linphone 6 face au mcu ne négociera vraisemblablement pas sa FEC
   (dégradation propre : la vidéo passe, sans protection). Le parc Linphone —
   central en conversation totale — perd donc sa protection de perte dès que
   le mcu est dans le chemin.
2. **Chemin relayé Linphone↔Linphone** : pour que la FEC *traverse*, il faut
   transporter le payload `flexfec` et le SSRC réparateur de bout en bout —
   affaire de négociation elixip + relais mcu, pas de décodage. C'est l'option
   la moins chère et elle protège le cas le plus fréquent (`:avoid` relaie).
3. **Chemin transcodé** : là il faut *décoder* la réparation en réception.
   Le module est compact (le cœur Linphone — packet-api + receive-cluster —
   fait ~16 Ko de source) et notre `fecdecoder` ULPFEC donne le gabarit.
   Prérequis structurel : accepter un second SSRC (réparation) dans la session
   vidéo — et la RFC allège ce point : l'association réparation→source peut
   être **entièrement in-band** (§7.1.1), le paquet réparateur portant en CSRC
   le SSRC qu'il protège. Un décodeur n'a donc besoin que de reconnaître le
   payload `flexfec` ; il n'a pas structurellement besoin du `ssrc-group` ni
   du bundle pour savoir quoi réparer.
   Écart Linphone à noter : la RFC déclare `rate` **et** `repair-window`
   obligatoires en fmtp (§5.1) ; le profil oRTP ne porte que
   `repair-window=200000` — à confirmer sur pcap.
4. **Ordre des chantiers inchangé** : l'arbitrage FEC reste conditionné à une
   mesure de perte fiable (plan, « hors périmètre »), mais quand il s'ouvrira,
   la cible de *décodage* prioritaire est désormais double — ULPFEC (acquis,
   navigateurs) **et** FlexFEC RFC 8627 lignes/colonnes (Linphone) — et la
   cible d'*émission* dépendra du pair, comme pour le feedback.

## 5. À vérifier sur un pcap réel (Linphone 6 ↔ Linphone 6)

- La **forme SDP exacte** : m-line séparée avec `mid` dans le groupe BUNDLE,
  ou payload `flexfec` dans la m-line vidéo avec `a=ssrc-group:FEC-FR` ?
  (le code vu ici ne tranche pas ; un pcap le fait en une capture).
- Le `step` effectif des colonnes et les valeurs des tableaux
  `mLvalues`/`mDvalues` par niveau (dans `fec-params.h`, non lu).
- Le comportement quand l'answer retire `flexfec` (le testeur n'a aucun cas
  asymétrique — à provoquer nous-mêmes, c'est notre cas nominal aujourd'hui).
