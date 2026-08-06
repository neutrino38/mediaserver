# Négociation des paramètres des codecs (fmtp SDP)

> Statut : **conception figée** (décisions A–E actées, §8). Phases 1-4 FAITES
> (2026-07-15). **Phase 5 partiellement faite (2026-08-05)** : l'ingestion du fmtp
> distant est livrée **pour H.264** (`ParseFmtpParams`,
> `H264Encoder::ResolveNegotiation`, RFC 6184 §8.2.2), et `Endpoint::Port::
> NegotiateReceiving` la branche enfin. **Reste ouvert** : le câblage des
> `effectiveProps` vers l'encodeur réel (§6.3 — elles sont mémorisées, personne ne
> les consomme), l'ingestion AV1 (phase 5b, investiguée et non écrite) et VP8/Opus.
> Phase 6 (MCU) spécifiée, non commencée.
>
> Le média serveur ne parle pas SIP : la signalisation et le SDP sont gérés par
> un **contrôleur SIP** externe (p. ex. le projet elixip), qui pilote le média
> serveur par l'API XML-RPC JSR-309. Dans tout ce document, « contrôleur SIP »
> désigne cet appelant.

## 1. Objectif

- Faire en sorte que chaque `Endpoint` connaisse réellement les codecs supportés
  par le média serveur, média par média.
- Déléguer au média serveur la production des attributs `fmtp` du SDP local
  (offer *ou* answer) : le contrôleur SIP ne devrait pas coder en dur des
  `profile-level-id`, `sprop-parameter-sets`, `useinbandfec`, etc.
- Enrichir le retour de `EndpointStartReceiving` (API JSR-309 XML-RPC) pour
  renvoyer, par média, une liste `payloadType → fmtp` correspondant aux codecs
  **réellement acceptés**.
- À terme : ingérer aussi le `fmtp` **distant** (reçu dans l'offer/answer du pair)
  pour que la négociation soit bidirectionnelle — le média serveur adapte alors
  ses paramètres d'émission aux contraintes du pair (p. ex. son `profile-level-id`
  H.264).

Deux « sens » de `fmtp` circulent, à ne pas confondre :

- **fmtp sortant** : ce que le média serveur publie dans *son* SDP. Décrit
  d'abord ce qu'il **accepte de recevoir**. C'est le retour de `StartReceiving`.
- **fmtp entrant** : ce que le pair a publié dans *son* SDP. Sert à contraindre
  l'**émission** du média serveur. Poussé via une propriété (§5.3, phase 5).

## 2. État des lieux du code (ce qui existe déjà)

Cette section corrige le brouillon initial : plusieurs briques existent, d'autres
sont nommées à tort.

### 2.1 Côté libmedikit (`third_party/fontventa/libmedikit/medkit/`)

- **Enums de codec** : `AudioCodec::Type`, `VideoCodec::Type`, `TextCodec::Type`,
  `AppCodec::Type` (`codecs.h`). Valeur = payload type par défaut / identifiant
  interne. Helpers `GetNameFor(Type)` (tous), `GetCodecFor(const char* name,
  Type&)` (**Audio et Video seulement** ; absent pour Text/App).
- **Factories** : `AudioCodecFactory`, `VideoCodecFactory` exposent
  `CreateDecoder(Type)`, `CreateEncoder(Type)` et
  `CreateEncoder(Type, const Properties&)`. **`TextCodecFactory` n'existe pas** —
  à créer. Le mapping `Type → classe concrète` est un `switch`, qui retourne
  `NULL` sur type inconnu.
- **Détection réelle du support** : à l'ouverture, les classes `Ff*` font
  `avcodec_find_encoder_by_name(name)` (ou `_decoder_by_name`) avec repli sur
  `avcodec_find_encoder(av_codec_id)`. C'est le test à réutiliser pour
  `IsSupported()`.
- **Canal de configuration = `Properties`** (`config.h`) : un
  `std::map<std::string,std::string>` **plat** (aucune hiérarchie native ;
  regroupement par convention de préfixe `codec.`/`h264.` + `substr`). Injecté
  **au constructeur de l'encodeur** via `CreateEncoder(type, props)`. Il n'y a
  **pas** de `SetProperties` a posteriori sur un codec : toute valeur négociée
  doit être connue **avant** `CreateEncoder`. Clés existantes :
  `h264.profile-level-id` (défaut `42801F`), `opus.useinbandfec`, `opus.usedtx`,
  `opus.maxaveragebitrate`, `vp8.max-fr`, `vp8.max-fs`, `av1.preset`…
- **Génération de fmtp DÉJÀ présente** : méthode virtuelle
  `bool GetFmtpInfo(std::string& fmtp, int payloadType)` sur `AudioEncoder`,
  `AudioDecoder`, `VideoEncoder`, `VideoDecoder` (défaut : `false`). Implémentée
  pour **H264, Opus, VP8, AV1**. Elle produit la **ligne SDP entière**
  `a=fmtp:<pt> <params>`. Absente sur les codecs texte et audio « legacy »
  (PCMU/PCMA/G722/GSM/AMR/AAC/Speex/Nelly héritent du défaut → `false`).
- **Sens entrant (parser un fmtp reçu → Properties) : inexistant.** Seul H264
  consomme réellement un `h264.profile-level-id` fourni (il le réinjecte dans le
  SPS encodé). C'est le gros manque.
- `IsSupported()` et `GetSupportedCodecs()` : **inexistants**, à créer.

### 2.2 Côté transport (`mcu/`)

- `RTPMap` (`include/rtp.h`) = `std::map<BYTE,BYTE>` : `payloadType → codec`.
  **Aucun champ fmtp/paramètre.** Une seule lecture utile
  `GetCodecForType(BYTE)`.
- `RTPSession` (`rtpsession.h/.cpp`) : setters réels
  `SetReceivingRTPMap(RTPMap&)`, `SetSendingRTPMap(RTPMap&)`,
  `SetSendingCodec(DWORD)` (recherche inverse codec→PT). **N'existent pas** :
  `SetReceivingMap`, `SetSendingMap`, `SetReceivingCodec`, `SetSendingType`,
  `SetReceivingType` (noms du brouillon). Le codec en réception est résolu
  paquet par paquet via `rtpMapIn->GetCodecForType()`.
- **`RTPSession::SetProperties()` interprète des clés transport** (`rtcp-mux`,
  `ssrc`, `tmmbr`, extensions d'en-tête, `rtpTimeout`…) et **ignore les clés
  `codec.*`**. RTPSession ne possède ni encodeur ni catalogue de codecs : c'est
  volontairement une classe **transport**. Côté MCU, les `Properties` de codec
  sont extraites en amont (`VideoStream::SetRTPProperties`, préfixe `codec.`) et
  poussées vers `VideoCodecFactory::CreateEncoder(...)`.

### 2.3 Côté JSR-309 (`mcu/src/jsr309/`)

- `RTPEndpoint` **hérite de** `RTPSession` (+ `Endpoint::Port`). Un par média
  (audio/video/video-slides/text), créé par `Endpoint`. Média fixé à la
  construction.
- Chaîne réception : `EndpointStartReceiving` (xmlrpc) →
  `MediaSession::EndpointStartReceiving` → `Endpoint::StartReceiving` →
  `RTPEndpoint::SetReceivingRTPMap` → `p->StartReceiving()` →
  **retour `GetLocalMediaPort()`**.
- `EndpointStartReceiving` est **le seul handler Endpoint qui renvoie une
  valeur** : `xmlok(env, "(i)", recPort)` → `returnVal = [recPort]`.
- Chaîne des propriétés : `EndpointSetRTPProperties` (xmlrpc) →
  `MediaSession::EndpointSetRTPProperties` → `Endpoint::SetRTPProperties` →
  `RTPEndpoint::SetProperties` = `RTPSession::SetProperties`. **Conséquence
  directe : les propriétés `codec.*` passées par `EndpointSetRTPProperties`
  n'aboutissent nulle part aujourd'hui** (RTPSession les ignore, et aucun
  encodeur n'est branché à ce niveau côté JSR-309). C'est un trou à combler.
- Aucune occurrence de `fmtp` dans `mcu/src` ni `mcu/include`.

## 3. Critique du découpage proposé

1. **La moitié « émission » n'est pas à créer, elle est à canaliser.**
   `GetFmtpInfo()` existe déjà et produit la ligne fmtp. Le travail réel n'est
   pas « ajouter un parseur fmtp dans chaque codec » (phase 5 du brouillon) mais :
   (a) **normaliser** la sortie de `GetFmtpInfo` (aujourd'hui la ligne
   `a=fmtp:<pt> …` complète, alors qu'on veut la **valeur seule** — le contrôleur
   SIP possède déjà le PT et formate la ligne) ;
   (b) rendre le fmtp **calculable au moment de la négociation** (voir point 2) ;
   (c) créer le sens **entrant** (parser), réellement absent.

2. **« Ouvrir tous les codecs pour collecter les fmtp » est coûteux et parfois
   impossible au bon moment.** Instancier + `avcodec_open2` chaque encodeur a des
   effets de bord (ouverture device VAAPI, threads x264…). Surtout : **le fmtp
   H.264 (`sprop-parameter-sets`) dépend des SPS/PPS qui ne sont peuplés
   qu'après l'encodage d'une première trame** — donc indisponibles à
   `StartReceiving`, avant tout flux média. Conséquence de conception : le fmtp
   de négociation doit être **dérivé de la configuration** (`Properties` +
   défauts du codec), pas d'une instance de codec « chaude ». Le
   `profile-level-id` s'en déduit sans encoder ; `sprop-parameter-sets` est au
   mieux « best-effort » dans l'offer et sera précisé plus tard. → On privilégie
   une génération de fmtp **sans ouverture de codec** quand c'est possible ; on
   n'ouvre un décodeur/encodeur que si un codec l'exige vraiment.

3. **Le catalogue de codecs supportés ne doit pas vivre dans `RTPEndpoint`.**
   « Supporté » = « libmedikit/ffmpeg a été compilé avec ce codec ». C'est une
   propriété de **libmedikit**, pas de la couche JSR-309. Le mettre en liste
   statique dans `RTPEndpoint` (proposition du brouillon) le duplique et le rend
   inaccessible au MCU. → `IsSupported()` sur les classes de codec et
   `GetSupportedCodecs()` sur les factories, **dans libmedikit**, calculé une
   fois et mémoïsé.

4. **Réponse à la question RTPEndpoint vs RTPSession : ni l'un ni l'autre.**
   - `RTPSession` est délibérément **transport pur** : y injecter
     l'instanciation de codecs et la collecte de fmtp est une violation de
     couche (et il ignore déjà `codec.*`). À proscrire.
   - `RTPEndpoint` est **spécifique JSR-309** : y mettre l'algorithme priverait
     le MCU du bénéfice.
   - **Recommandation** : un composant de négociation **dans libmedikit**
     (réutilisable par le MCU *et* JSR-309), au-dessus des factories. `RTPEndpoint`
     (et, plus tard, les `*Stream` du MCU) n'en sont que des appelants minces.
     C'est la vraie réponse « pour permettre la négo aux autres apps ».

5. **Différer entièrement l'ingestion du fmtp distant casse la négociation
   entrante.** Pour un appel **entrant**, notre answer doit tenir compte des
   contraintes de l'offer (p. ex. le `profile-level-id` que le pair sait
   décoder). Sans ingestion, on ne fait qu'« annoncer nos capacités », pas
   négocier. → Le **canal** d'entrée du fmtp distant doit être conçu dès
   maintenant (propriété `codec.<codec>.fmtp`), même si le **parsing** par codec
   arrive incrémentalement.

6. **Encodeur ou décodeur comme source du fmtp ?** Le fmtp du SDP local décrit
   surtout la **réception** → sémantiquement, c'est la capacité du **décodeur**.
   Mais `GetFmtpInfo` n'est implémenté côté décodeur que pour VP8. Plutôt que de
   trancher enc/dec au cas par cas, on vise une génération pilotée par
   **configuration** (point 2), la distinction enc/dec devenant un détail interne
   au négociateur.

7. **Filtrage des codecs, pas seulement annotation.** `GetCodecs()` =
   intersection(supportés, `rtpMap` proposée, média). Si un PT proposé n'est pas
   supporté, il doit **disparaître** du retour. Donc `StartReceiving` doit
   renvoyer la **map réellement acceptée** (PT → fmtp), pas seulement des fmtp
   plaqués sur la map d'entrée. Le contrôleur SIP reconstruit la m-line à partir
   des PT survivants. → à acter (§8, décision D).

8. **Corrections de nommage** (à répercuter partout) : `SetReceivingRTPMap` /
   `SetSendingRTPMap` (et non `SetReceivingMap`), `avcodec_find_encoder` /
   `avcodec_find_decoder` (et non `av_find_codec`).

## 4. Architecture retenue

Trois couches, responsabilités strictes.

### 4.1 libmedikit — catalogue de capacités

- **Classes de codec** (`codecs.h`) : ajouter
  ```cpp
  static bool AudioCodec::IsSupported(Type);   // idem Video/Text/App
  ```
  Défaut `true`. Les types adossés à ffmpeg (`Ff*`, H264, VP8, AV1, Opus…)
  surchargent en faisant le même `avcodec_find_encoder(_by_name)` /
  `avcodec_find_decoder` que l'ouverture réelle. Les types « toujours dispo »
  (PCMU/PCMA, T140…) renvoient `true` en dur.

- **Factories** (`AudioCodecFactory`, `VideoCodecFactory`, **nouvelle
  `TextCodecFactory`**) : ajouter
  ```cpp
  static const std::vector<Type>& GetSupportedCodecs();  // calculé 1×, mémoïsé
  ```
  Filtre l'ensemble des `Type` connus par `IsSupported()`. Thread-safe,
  initialisation paresseuse (ou explicite au démarrage).

### 4.2 libmedikit — le négociateur (`CodecNegotiator`, nouveau)

Composant sans dépendance MCU/JSR-309. Point d'entrée unique :

```cpp
struct NegotiatedCodec {
    BYTE        payloadType;   // PT sur le fil (clé de la RTPMap)
    int         codec;         // AudioCodec::Type / VideoCodec::Type / ...
    std::string fmtp;          // paramètres SEULS (sans "a=fmtp:<pt> ")
    Properties  effectiveProps;// props résolues à donner plus tard à l'encodeur
};

struct NegotiationResult {
    RTPMap                       acceptedMap;  // sous-ensemble accepté
    std::vector<NegotiatedCodec> codecs;       // détail par PT, ordre = priorité
};

class CodecNegotiator {
public:
    // media : Audio/Video/Text. proposed : la RTPMap venant du contrôleur SIP.
    // localProps : props locales (config + défauts). remoteFmtp : fmtp distant
    //   déjà ingéré (nullable ; renseigné surtout en appel entrant / answer).
    static bool Negotiate(MediaFrame::Type media,
                          const RTPMap& proposed,
                          const Properties& localProps,
                          const Properties* remoteFmtp,
                          NegotiationResult& out);
};
```

Rôle : (1) intersecter `proposed` avec `GetSupportedCodecs(media)` ;
(2) pour chaque PT retenu, calculer le fmtp local à partir de `localProps` +
défauts codec (via `GetFmtpInfo`, cf. §4.4) et, si `remoteFmtp` présent, adapter
les `effectiveProps` aux contraintes distantes ; (3) remplir `NegotiationResult`.

### 4.3 JSR-309 — orchestration et remontée

- `Endpoint` (ou `RTPEndpoint`) appelle `CodecNegotiator::Negotiate` dans le
  flux `StartReceiving`, applique `acceptedMap` via `SetReceivingRTPMap`,
  **mémorise** le résultat par média (pour le retour XML-RPC et pour brancher
  plus tard l'encodeur avec `effectiveProps`).
- `EndpointSetRTPProperties` doit, côté JSR-309, **router les clés `codec.*`
  vers le stockage de propriétés de l'endpoint** (pour le négociateur), au lieu
  de les laisser filer vers `RTPSession::SetProperties` qui les jette. Les clés
  transport continuent vers `RTPSession`.
- `xmlrpcjsr309.cpp` : `EndpointStartReceiving` renvoie le retour enrichi (§5.2).

`RTPSession` n'est **pas** modifié dans son rôle. `RTPEndpoint` reste mince.

### 4.4 Normalisation de `GetFmtpInfo`

`GetFmtpInfo` renvoie aujourd'hui `a=fmtp:<pt> <params>`. Deux options (décision
§8-E). Recommandé : ajouter une surcharge/param qui renvoie **`<params>` seuls**
et rend l'entête indépendant du PT (le contrôleur SIP ré-associe). Le négociateur
consomme cette forme. La forme « ligne complète » peut rester pour l'usage MCU
historique.

## 5. Modèle de données et format d'échange

### 5.1 Ce qui reste inchangé

`rtpMap` en **entrée** de `EndpointStartReceiving` : struct XML-RPC
`{ "<pt>": <codecInt> }`. On ne change pas la signature d'entrée en phase 1-4.

### 5.2 Retour enrichi de `EndpointStartReceiving`

Compatibilité ascendante calquée sur le « gap sourceName » (§6.1 de
`xmlrpc_jsr309_api.md`) : `returnVal[0]` reste le port ; on **ajoute** un second
élément.

```
returnVal = [
  int   recvPort,                       // inchangé (clients existants OK)
  struct {                              // nouveau : fmtp par PT accepté
    "<pt>": "<paramètres fmtp>",        // ex. "96": "profile-level-id=42801f;packetization-mode=1"
    ...
  }
]
```

- Clé = payload type (chaîne), valeur = **paramètres fmtp seuls**.
- Un PT proposé mais non supporté **n'apparaît pas** (il a été filtré). Le
  contrôleur SIP déduit de `returnVal[1]` les PT réellement acceptés (décision D).
- Un codec **sans** fmtp (PCMU, T140…) est **présent avec une valeur vide `""`**.
  La **présence de la clé est le signal d'acceptation** ; l'absence signale
  « filtré ». C'est ce que fait le code livré (`NegotiateReceiving` pousse tout
  `result.codecs`, fmtp vide inclus — `Endpoint.cpp:551-555`) et ce dont dépend le
  contrôleur SIP (`accepted_pts/2` côté elixip, plan
  `mendooze_sdp_delegation_plan.md` §4 « Option A »).

  > **Correction 2026-08-05.** Ce paragraphe disait l'inverse (« absent de la
  > struct »), et la décision §8-E avec lui. Le code n'a jamais suivi cette règle
  > et a bien fait : si on la lui appliquait, tout codec sans fmtp — PCMU, PCMA,
  > T140 — disparaîtrait silencieusement des SDP du contrôleur, puisqu'il n'a pas
  > d'autre source pour l'ensemble accepté. Le commentaire placé juste au-dessus de
  > la boucle de sérialisation (`xmlrpcjsr309.cpp:1201-1203`) répète encore la
  > règle périmée et doit être corrigé avec le prochain passage sur ce fichier.

### 5.3 Canal du fmtp distant (phase 5)

Deux clés, selon ce que le contrôleur a à dire. La **résolution est par payload
type** ; la clé par nom de codec n'est qu'un raccourci valable quand il n'y a qu'un
PT par codec.

```
pt.<pt>.fmtp        = "<paramètres fmtp reçus du pair pour CE payload type>"
   ex. pt.109.fmtp = "profile-level-id=42e01f;packetization-mode=1"

codec.<nomCodec>.fmtp = "<paramètres fmtp reçus du pair>"     (raccourci historique)
   ex. codec.h264.fmtp = "profile-level-id=42e01f;packetization-mode=1"
```

- **`pt.<pt>.fmtp`** est alimentée par le paramètre `offer` de `StartReceiving`
  (chemin MCU, cf. MCU-API §`StartReceiving`), une entrée par PT de l'offre.
- **`codec.<nomCodec>.fmtp`** est alimentée par `EndpointSetRTPProperties`
  (chemin JSR-309), routée (§4.3) vers le stockage de l'endpoint, où
  `StoreCodecProperties` la range débarrassée du préfixe `codec.`.
- La clé par PT gagne quand les deux sont présentes, ce qui rend la migration d'un
  contrôleur sans coordination possible.

Le négociateur parse la valeur, en tire le fmtp **annoncé** pour ce PT et les
`effectiveProps` (ex. `h264.profile-level-id`, `h264.packetization-mode`) qui
bornent l'encodeur d'émission.

> **Pourquoi par PT — corrigé le 2026-08-06.** La clé par nom de codec, seule, ne
> peut porter qu'UNE résolution pour tous les PT d'un même codec. Une offre
> navigateur énumère le même H.264 sous six ou sept PT, précisément pour décrire
> autant de couples (profil, `packetization-mode`) : Chrome 138 en offre sept.
> `RTPParticipant::StartReceiving` écrivait `h264.fmtp` dans une boucle sur les PT
> — le dernier itéré gagnait — et le négociateur servait ce profil aux sept PT
> acceptés. Six réponses décrivaient donc un codec que l'appelant n'avait pas
> offert : libwebrtc refuse la réponse entière et l'application raccroche juste
> après l'ACK. `H264Encoder::ResolveNegotiation` était correct — il annonce bien le
> profil du pair — on lui passait simplement le mauvais pair.
>
> Le `packetization-mode` annoncé, codé en dur à `1`, relevait de la même erreur :
> le mode fait partie de l'**identité** du payload type côté pair. Il est désormais
> celui du pair pour ce PT, dans les deux jeux de propriétés (annoncé et effectif).
> Reste à faire : le packetiseur n'honore pas encore le mode 0 (il émet du FU-A),
> ce que `ResolveNegotiation` signale par un `Log`.

## 6. Dynamique d'appel (les deux sens)

### 6.0 Sémantique SDP offer/answer (fondement de tout le reste)

Un SDP décrit **les capacités de celui qui l'émet, en réception**. Trois
conséquences qui gouvernent la négociation :

1. **L'offer (dans l'INVITE) = ce que le distant sait/accepte de RECEVOIR.**
   Ses payload types et ses `a=fmtp` décrivent ce que l'offreur peut **décoder**.
2. **L'answer (notre 200 OK) = ce que NOUS savons/acceptons de RECEVOIR.**
3. Les flux sont donc croisés, avec un espace de PT **par direction** :
   ```
   Flux A : distant ──envoie──▶ local (on REÇOIT)   → régi par NOTRE answer
   Flux B : local  ──envoie──▶ distant (on ÉMET)    → régi par SON offer
   ```

**La règle qui débloque la négociation entrante (RFC 3264) : l'answer doit être
un sous-ensemble de l'offer.** Même si l'offer exprime la capacité de réception
*du distant*, il joue pour nous le rôle de **menu** : c'est la liste dans
laquelle nous piochons les codecs que **nous** accepterons de recevoir. Par
convention RTP symétrique, on **réutilise les numéros de PT de l'offer** pour les
codecs retenus (un même numéro = un même codec dans les deux sens).

**Direction du fmtp** (à ne pas inverser) :

- `profile-level-id` de l'**offer** = ce que le distant sait **décoder** → c'est
  le **plafond de NOTRE encodeur** (flux B, émission). C'est l'usage principal de
  l'ingestion du fmtp distant.
- `profile-level-id` de notre **answer** = ce que **nous** savons décoder →
  vient de **notre config**, il est fondamentalement le nôtre. Le refléter sur
  l'offer n'est qu'une **politique d'interop optionnelle**, pas une contrainte de
  la négo. Le couplage *obligatoire* est `fmtp offer → notre encodeur`.
- Exception H.264 : `sprop-parameter-sets` décrit ce que **l'émetteur du SDP va
  envoyer** (ses SPS/PPS) — le seul attribut « de réception » qui parle en fait
  d'émission, et justement celui qu'on ne peut pas produire avant d'avoir encodé.

### 6.1 Appel sortant (média serveur = UAC, on génère l'offer)

`StartReceiving` d'abord, **aucun fmtp distant connu** — négociation en deux temps.

```
contrôleur SIP ──INVITE + SDP offer──▶ Pair
contrôleur SIP ◀──200 OK + SDP answer── Pair ; ──ACK──▶ Pair

 contrôleur SIP                          mediaserver
   │ 1. EndpointStartReceiving(video,       │──▶ Negotiate(proposed={97:H264,…},
   │      {97:H264, …ce qu'on veut offrir})  │       localProps, remoteFmtp=∅)
   │                                        │    • intersection supportés∩proposé
   │                                        │    • fmtp LOCAL = capacités pures
   │  ◀── [recvPort,                         │      (config+défauts, ex.42801f)
   │        {"97":"profile-level-id=42801f;  │    • sprop-parameter-sets omis
   │              packetization-mode=1"}]    │      (pas encore encodé)
   │ 2. construit l'INVITE offer ───▶ Pair   │
   │ 3. reçoit 200 OK : PT97 profile=42e01f  │
   │ 4. EndpointSetRTPProperties(           │──▶ stocké dans l'endpoint
   │      codec.h264.fmtp="…42e01f…")         │
   │ 5. EndpointStartSending(video, ip, port,│──▶ encodeur créé avec
   │      {97:H264}) ◀ map réduite de l'answer│    effectiveProps (≤ profil pair)
   │ 6. ACK ; 7. EndpointStartRTPTimeout(…)  │
```

Le fmtp local (étape 1) est notre **capacité brute** ; la contrainte du pair
n'arrive qu'à l'étape 4, d'où l'ingestion **avant** `StartSending`.

### 6.2 Appel entrant (média serveur = UAS, on génère l'answer)

`StartReceiving` précède toujours `StartSending`, mais **le fmtp distant est déjà
dans l'offer** → le contrôleur SIP peut le pousser **avant** `StartReceiving`, et
la négociation est complète en un seul passage.

```
Pair ──INVITE + SDP offer──▶ contrôleur SIP ──▶ mediaserver
                             contrôleur SIP ◀── 200 OK (construit ici)

 contrôleur SIP                          mediaserver
   │  parse l'offer : 97=H264 profile=42e01f │
   │                  96=Opus  useinbandfec=1 │
   │ 1. EndpointSetRTPProperties(           │──▶ stocké (PAS dans RTPSession)
   │      codec.h264.fmtp="…42e01f…", …)      │
   │ 2. EndpointStartReceiving(video,       │──▶ Negotiate(proposed={97:H264},
   │      {97:H264})                          │       localProps, remoteFmtp=✔)
   │                                        │    • intersection supportés∩offer
   │                                        │    • fmtp LOCAL (notre décodage)
   │  ◀── [recvPort,                         │    • effectiveProps encodeur
   │        {"97":"profile-level-id=…;        │      = plafonné par profil pair
   │              packetization-mode=1"}]     │
   │ 3. construit l'answer (m-line = PT       │
   │    retenus, a=fmtp depuis returnVal[1])  │
   │ 4. envoie 200 OK ───────────▶ Pair       │
   │ 5. EndpointStartSending(video, ip, port, │──▶ encodeur avec effectiveProps
   │      {97:H264})                          │    (respecte le pair d'emblée)
   │ 6. EndpointStartRTPTimeout(…)            │
```

> Correction du brouillon : « appel entrant = StartSending en premier » est
> **inexact**. `StartReceiving` reste avant `StartSending` dans les deux cas
> (cf. `xmlrpc_jsr309_api.md` §9). Le vrai distinguo est la **disponibilité du
> fmtp distant** au moment du `StartReceiving` : connue dès l'offer en entrant,
> seulement à l'answer en sortant.

### 6.3 Où vit l'encodeur qui applique `effectiveProps` (point délicat)

Dans JSR-309, un `RTPEndpoint` **relaie** des paquets ; il ne contient pas
l'encodeur du flux sortant. Le flux émis vers le pair provient de ce qui est
**attaché** à l'endpoint : un `VideoTranscoder`
(`VideoTranscoderSetCodec(..., Properties)`), un `VideoMixerPort`, ou — en pont
B2B sans transcodage — la source directe (relais de paquets, aucun encodeur).

Conséquence : les `effectiveProps` calculées au niveau de l'endpoint doivent
**atteindre le producteur réel** du flux sortant. Le véhicule existe déjà
(`VideoTranscoderSetCodec` / `*MixerPortSetCodec` acceptent des `Properties`),
mais **le câblage endpoint→producteur reste à faire** — c'est la vraie
difficulté de la phase 5, à traiter H.264 en premier. En relais B2B pur, les
`effectiveProps` sont sans objet : ce sont alors les deux pattes qui doivent
avoir négocié des profils compatibles (orchestration côté contrôleur SIP).

C'est pourquoi le phasage isole le sens réception (phases 1-4, autonome, valeur
immédiate) du sens émission (phase 5, dépendant de ce câblage).

## 7. Phasage révisé

### Phase 1 — libmedikit : capacités
- `IsSupported()` sur `AudioCodec/VideoCodec/TextCodec/AppCodec`.
- `GetSupportedCodecs()` sur les factories ; **création de `TextCodecFactory`**.

### Phase 2 — libmedikit : normalisation fmtp
- Surcharge de `GetFmtpInfo` renvoyant les **paramètres seuls** (sans
  `a=fmtp:<pt> `).
- Rendre le fmtp calculable **sans codec ouvert** (dérivation depuis
  `Properties` + défauts) pour H264/Opus/VP8/AV1 ; défaut vide pour les autres.
- fmtp texte : T140RED (`a=fmtp:<red_pt> <t140_pt>/<t140_pt>/…`, RFC 4103).

### Phase 3 — libmedikit : négociateur — **FAIT (2026-07-15)**
- `CodecNegotiator::Negotiate` (intersection + fmtp local, `remoteFmtp` encore
  ignoré). Tests unitaires de l'intersection et du format fmtp.
- Livré : `medkit/negotiator.h` + `negotiator.cpp` (branchés `OBJS` du Makefile).
  `NegotiatedCodec`/`NegotiationResult` comme §4.2 ; `RTPMap` matérialisée en
  `std::map<int,int>` (PT→Type) pour rester agnostique du `RTPMap` MCU (BYTE,BYTE),
  conversion reportée en phase 4. Dispatch `IsSupported`/fmtp par média→Type vers
  le catalogue (codecs.h) et les statiques phase 2
  (`OPUS/H264/VP8/AV1Encoder::GetFmtpParams`, `TextCodec::GetT140RedFmtpParams`) —
  aucun codec ouvert. Intersection dans l'ordre des PT proposés ; PT non supporté
  filtré (décision D) ; `effectiveProps = localProps` (remoteFmtp phase 5) ;
  `Application` → `false`. T140RED : fmtp ne référence le PT du T140 que si ce
  dernier est proposé+supporté, sinon vide. Probe : filtrage + formats fmtp
  H264/Opus/AV1/T140RED validés (build vert).

### Phase 4 — JSR-309 : remontée — **FAIT (2026-07-15)**
- `Endpoint::Port::NegotiateReceiving` appelle `CodecNegotiator::Negotiate` dans
  le flux `Endpoint::StartReceiving` (branche RTP), applique la map filtrée via
  `SetReceivingRTPMap` et **mémorise** `negotiatedFmtp` (PT→params) + `negotiatedProps`
  (effectiveProps par PT, réservées à l'encodeur phase 5). Conversion RTPMap
  (BYTE,BYTE) ↔ map<int,int> du négociateur. Repli sur la map proposée si le
  média n'est pas négociable.
- `EndpointStartReceiving` : retour enrichi `[recPort, {"<pt>":"<params>"}]` (§5.2),
  ascendant-compatible ; struct construite dans `xmlrpcjsr309.cpp`. Le fmtp est
  remonté sous le verrou via `MediaSession::EndpointStartReceiving` (out-param) +
  `Endpoint::GetNegotiatedFmtp`.
- `EndpointSetRTPProperties` (`Endpoint::SetRTPProperties`) route les clés `codec.*`
  (préfixe retiré) vers `Port::StoreCodecProperties` (stockage `codecProperties`
  du négociateur) ; les clés transport continuent vers `RTPSession` (qui ignore
  codec.*). Même convention que `VideoStream::SetRTPProperties` côté MCU.
- Doc : `xmlrpc_jsr309_api.md` (§6.7, cycle de vie, §9.0/§9.1/§9.2) mis à jour et
  **`CODECS.md` créé** (clés `codec.*` + défauts par codec, limites, support).
- Build vert (`install.ksh localcompile`, rm des .o car headers non suivis).

### Phase 5 — négociation entrante réelle
- Parsing du fmtp distant (`codec.<x>.fmtp`) par codec « qui le mérite »
  (H264 en premier : `profile-level-id` distant → borne notre émission).
- Câblage endpoint → producteur (transcodeur/mixer) des `effectiveProps` (§6.3).
- Passage du fmtp distant via propriété (§5.3), documenté dans `CODECS.md`.

### Phase 5 — précision sur H.264 (décidée le 2026-08-05)

L'ingestion doit implémenter **RFC 6184 §8.2.2**, pas un simple reflet. Les deux
choses que le contrôleur confondait jusqu'ici sont séparées :

- ce qu'on **annonce** = ce que *nous* savons décoder : même profil que l'offre
  (profile_idc + flags de contrainte), et niveau **le nôtre** seulement si
  `level-asymmetry-allowed=1` est présent dans l'offre *et* dans notre réponse ;
  sinon (absent, ou `=0`) le niveau **de l'offre**.

  **Sans asymétrie, on renvoie donc le `profile-level-id` de l'offre tel quel** :
  les deux premiers octets sont fixés par la règle du profil, le troisième par celle
  du niveau, il ne reste rien à choisir. Deux corollaires. **Si on ne sait pas décoder
  le niveau offert, on annonce notre niveau MAXIMUM et on loggue** (`warning`, en
  nommant le niveau offert, celui annoncé et le participant) — on ne retire **pas** le
  PT. Écart assumé à la RFC 6184 §8.2.2, qui ne laisse que « refléter ou retirer » :
  refuser la vidéo parce qu'un appelant a offert du niveau 5.1 est un échec plus dur
  qu'annoncer 3.1 et fonctionner, et annoncer *en dessous* de l'offre est justement ce
  dont un pair correct a besoin pour encoder à notre portée. Le log est la seule trace
  qui relie « pas de vidéo sur cette patte » à sa cause. Cet assouplissement vaut pour
  ce qu'on **annonce** (capacité de réception : sous-estimer est sans risque) et **pas**
  pour ce qu'on **émet** — le niveau et le mode de paquetisation déclarés par le pair
  sont des bornes dures de notre encodeur. Second corollaire :
  `packetization-mode` n'est **pas** régi par cette règle — il est
  aujourd'hui **codé en dur à `1`** (`h264encoder.cpp:322`), donc un appelant qui ne
  gère que le mode 0 reçoit `packetization-mode=1`. À corriger ici, puisque c'est la
  phase qui rend le fmtp distant lisible : le mode qu'on **émet** doit être un mode
  que le pair a déclaré, celui qu'on **annonce** est le nôtre ;
- ce qui **borne notre encodeur** = `min(notre capacité, le niveau déclaré par le
  pair)` → `effectiveProps`. Le pair a dit ce qu'il sait décoder ; émettre au-dessus
  produit un flux négocié avec succès et décodé par personne.

Nous émettons `level-asymmetry-allowed=1` : un mixeur transcode dans les deux sens,
donc le cas même pour lequel ce paramètre existe est le nôtre. `GetFmtpParams` gagne
la logique de niveau et d'asymétrie ; `CODECS.md` gagne la clé. Conséquence utile :
sur une offre **sans** `level-asymmetry-allowed` — le cas courant des postes SIP — la
règle redonne le niveau de l'offre, donc la réponse reste identique à aujourd'hui,
octet pour octet. Ce qui change ne concerne que les pattes WebRTC et gateway.
- Point plus simple ici que côté JSR-309 : le producteur du flux sortant est le
  `VideoStream` du participant, dont `SetVideoCodec` alimente directement
  l'encodeur — le câblage difficile de §6.3 n'existe pas. Attention en revanche :
  `SetRTPProperties` **fusionne** dans `videoProperties` alors que `SetVideoCodec`
  **remplace** la map entière, donc les `negotiatedProps` doivent être appliquées du
  côté `SetVideoCodec` ou réappliquées après lui.
- Le contrôleur transmet aussi son intention côté feedback RTCP par les clés
  transport existantes (`useNACK`, `useRtcpFIR`, `tmmbr`) : rien à ajouter.
- `MCU-API.md` §6.7 à mettre à jour **avec le code**, pas avant : la doc de l'API
  décrit le serveur tel qu'il est.

### Phase 5b — AV1 : la négociation y est plus simple que H.264 (investigué le 2026-08-05)

Source : [AV1 RTP Payload Format](https://aomediacodec.github.io/av1-rtp-spec/) (AOMedia),
type média enregistré **`video/AV1`**, et l'annexe A.3 de la spec bitstream pour les
niveaux. Trois paramètres fmtp, et **rien d'autre** :

| Paramètre | Sens | Défaut si absent |
|---|---|---|
| `profile` | `seq_profile` le plus élevé que l'émetteur du SDP sait **décoder** | `0` (Main) |
| `level-idx` | `seq_level_idx` le plus élevé qu'il sait décoder | `5` (niveau 3.1) |
| `tier` | `seq_tier` le plus élevé qu'il sait décoder | `0` (Main) |

`seq_level_idx` → niveau : `major = 2 + (idx >> 2)`, `minor = idx & 3`. Donc `5` = 3.1,
`8` = 4.0, `9` = 4.1. `tier = 1` (High) n'existe qu'à partir du niveau 4.0 et ne change
que les débits admissibles.

**La différence de fond avec H.264 : l'asymétrie est le défaut, sans rien à signaler.**
La spec dit que ces paramètres « *are asymmetrical and the answerer MAY declare its own
media configuration if the answerer receiving capabilities are different from the
offerer* ». Il n'y a **pas** d'équivalent de `level-asymmetry-allowed`, donc **aucune
règle de reflet** : on annonce **toujours notre propre capacité**. Refléter les valeurs
de l'appelant, comme le faisait le contrôleur en H.264, serait ici franchement faux. Et
comme on ne reflète jamais, **l'échappatoire pragmatique du H.264 n'a pas d'équivalent
AV1** : le cas « niveau offert non décodable » ne peut pas se produire.

**Ce qui est normatif, en revanche, c'est le sens émission** : « *The AV1 stream sent by
either the offerer or the answerer MUST be encoded with a profile, level and tier, lesser
or equal to the values of the level-idx, profile and tier declared in the SDP by the
receiving agent.* » D'où `effectiveProps` = **minimum composante par composante** entre
notre capacité et celle déclarée par le pair.

**Le piège, et il est courant.** Les défauts s'appliquent quand le pair n'envoie **pas**
de fmtp : il déclare alors `level-idx=5`, soit le niveau 3.1, et c'est une contrainte
**réelle**, pas une absence de contrainte. Or 3.1 plafonne à `MaxDisplayRate` 31 950 720
échantillons/s : il couvre du 1280×720@30 (27 648 000) mais **pas** du 720p@60
(55 296 000, qui exige 4.0) ni du 1080p. Un mixeur en 720p60 ou 1080p qui n'ingère rien
émet donc au-dessus de ce que le pair a déclaré — exactement ce que le MUST ci-dessus
interdit.

**État du code.** `AV1Encoder::GetFmtpParams` (`av1/av1codec.cpp:269`) émet déjà nos
trois paramètres depuis `av1.profile` / `av1.level-idx` / `av1.tier`, avec les défauts
`0/5/0` — **conformes à la spec**. Le côté *annoncé* est donc déjà juste dans sa forme.
Ce qui manque est **entièrement** le côté émission : aucune ingestion, donc aucun
écrêtage. Le point d'accroche est prêt (`ResolveVideo`, branche `VideoCodec::AV1` dans
`negotiator.cpp`) ; le resolver AV1 serait plus court que celui de H.264 puisqu'il n'a
ni règle de profil à recopier ni asymétrie à arbitrer.

**Deux décisions à prendre avant de l'écrire :**

1. **`av1.level-idx` reste-t-il une config statique ?** Le défaut `5` (3.1) est cohérent
   avec la conférence par défaut (HD720p à 15 i/s), et devient un **mensonge** dès qu'un
   opérateur passe le mixeur en 720p60 ou 1080p : on annoncerait 3.1 en encodant du 4.0.
   C'est mot pour mot l'incident H.264 du `profile-level-id`, transposé. L'alternative
   est de **dériver** `level-idx` de la taille et du débit d'images réels du mixeur.
2. **Que fait-on quand le pair déclare moins que ce que la mosaïque produit ?** Écrêter
   l'encodeur pour cette patte (niveau par participant, l'analogue pragmatique du choix
   H.264) ou refuser AV1 pour elle. L'écrêtage suppose que l'encodeur de sortie sait
   descendre par participant — à vérifier avant de s'y engager.

### Phase 6 — MCU : la même délégation sur l'API conférence
Demandée le 2026-08-05 par le contrôleur kelixip, dont la conception détaillée est
`docs/design/mcu_module.md` §16.3 (dépôt elixip) — à lire avant de commencer, elle
fixe le contrat de fil.

- `StartReceiving` (`xmlrpcmcu.cpp:2012`) gagne un **7e paramètre optionnel**
  `offer` (struct `{"fmtp": {"<pt>": "<params>"}}`) et un **3e élément de retour**
  `fmtpByPt` — `returnVal[0]` reste le port, `returnVal[1]` l'IP annoncée (S4), donc
  `XmlRpcMcuClient` (mcuGold) et les contrôleurs actuels sont inchangés. Repli de
  signature comme pour `role`/`proto` : `(iiiSiiS)` → `(iiiSii)` → `(iiiSi)`.
- `MultiConf::StartReceiving` → `RTPParticipant` : appeler
  `CodecNegotiator::Negotiate`, installer `acceptedMap` (map filtrée) au lieu de la
  map proposée, mémoriser `negotiatedFmtp` + `negotiatedProps` par média. Décalque
  de `Endpoint::Port::NegotiateReceiving`.
- **Le contrôleur envoie `SetRTPProperties(codec.*)` AVANT `StartReceiving`**
  (décidé le 2026-08-05, `mcu_module.md` §16.3.4 (a)), les clés transport après.
  Sans cela le négociateur travaillerait sur une map de propriétés **vide** et
  `H264Encoder::GetFmtpParams` annoncerait son défaut `42801F` — un bug de surcroît
  intermittent, puisqu'un re-INVITE trouve la map peuplée par le cycle précédent.
  C'est la même convention d'amorçage que `EndpointSetRTPProperties` côté JSR-309
  (décisions A et C), donc **une seule convention d'entrée pour les deux API**.
- **Découpage** : la tuyauterie (paramètre, retour, appel au négociateur, map
  filtrée) est livrable **sans** la phase 5 — le contrôleur a un repli. L'ingestion
  du `remoteFmtp` suit.

## 8. Décisions

- **A. ACTÉ.** Dans les deux sens, `StartReceiving` reste avant `StartSending`.
  Le fmtp distant entrant arrive via `EndpointSetRTPProperties` **avant**
  `StartReceiving` (cas entrant/UAS) et **avant** `StartSending` (cas
  sortant/UAC). Fondé sur la sémantique SDP §6.0 (l'answer ⊆ offer ; l'offer =
  capacité de réception du distant, qui sert de menu à notre propre réception).
- **B. ACTÉ — négociateur dans libmedikit.** `IsSupported()`/`GetSupportedCodecs()`
  sur les classes de codec et les factories, `CodecNegotiator` au-dessus.
  Réutilisable MCU + JSR-309. `RTPSession` reste transport pur ; `RTPEndpoint`
  est un appelant mince.
- **C. ACTÉ — canal du fmtp distant conçu maintenant.** Clé `codec.<x>.fmtp` via
  `EndpointSetRTPProperties` dès la phase 4 (routage) ; parsing par codec livré
  incrémentalement en phase 5 (H.264 en premier).
- **D. ACTÉ — retour filtré.** `StartReceiving` renvoie la **map réellement
  acceptée** : un PT proposé non supporté disparaît. Le contrôleur SIP
  reconstruit la m-line et les `a=fmtp` depuis `returnVal[1]`. Le média serveur
  est l'autorité sur ce qu'il accepte.
- **E. ACTÉ — paramètres seuls.** La struct renvoie `"<pt>": "<params>"` (sans
  `a=fmtp:<pt> `). Le contrôleur SIP formate la ligne SDP. Un codec sans fmtp est
  **présent avec une valeur vide** : la présence de la clé est le signal
  d'acceptation, l'absence signale « filtré » (corrigé le 2026-08-05, §5.2 — la
  formulation initiale « absent de la struct » contredisait le code livré et
  aurait effacé PCMU/PCMA/T140 des SDP du contrôleur).

## Instructions pour CLAUDE

- Sois critique, remets en cause les choix hasardeux.
- Fais préciser les points ambigus.
- Modifie ce document directement, sans historique ; privilégie la clarté.
- L'archi, la conception et les rôles sont fixés (§8 résolu) : l'implémentation
  peut démarrer, phase par phase.
