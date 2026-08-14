# Plan — savoir demander au serveur quels codecs il porte

**Le principe d'abord** (posé dans `CLAUDE.md`) : ce que le serveur sait de
lui-même, c'est à lui qu'on le demande. Il est la source de vérité sur ses propres
capacités et ses propres verdicts ; un contrôleur le pilote, il ne le modélise
pas. Le dépôt portait déjà cette règle pour l'adresse annoncée dans le SDP — une
seule source, lue par les deux API de contrôle, jamais recopiée dans la
configuration d'un contrôleur.

Les codecs portés en sont le second cas, et il a coûté un appel : le 2026-08-12,
un appel AV1 ↔ AV1 est mort en 488 parce que le contrôleur offrait H.264/VP8. Non
parce qu'il avait tort de le croire, mais parce que **personne ne pouvait poser la
question** — alors trois couches y ont répondu seules, avec trois listes écrites à
la main, dont aucune n'était vraie.

Ce plan ne corrige donc pas une liste : il rend la question posable, ce qui est la
seule façon de ne pas la reposer dans six mois. Corollaire à retenir : **une
capacité qui existe dans le code mais qu'aucune API ne permet d'interroger est un
défaut** — c'est ce qui force le contrôleur à deviner.

## 1. L'état des lieux

| étage | ce qu'il déclare | la réalité |
|---|---|---|
| `GetSupportedCodecs` (API MCU, `mcu/src/xmlrpcmcu.cpp:2320`) | tableau **figé** : `PCMA, PCMU, GSM, SPEEX16, G722, AMR, NELLY8, NELLY11` | il **manque OPUS, AMRWB, AAC** |
| — vidéo / texte | `// Impleent video and text` → *media not supported* | encodeur et décodeur existent pour 7 codecs vidéo |
| API JSR309 (`mcu/src/jsr309/xmlrpcjsr309.cpp`) | la méthode n'est **pas exposée** | — |
| elixip `@default_video_codecs` | `["H264", "VP8"]`, AV1 ajouté à la main en 6d6f75b | emplâtre : la liste recommencera à dériver |

La source de vérité existe déjà, et c'est la seule qui ne peut pas mentir — les
`switch` des factories :

```
AudioCodecFactory::CreateEncoder   PCMA PCMU G722 AAC AMR AMRWB NELLY8 NELLY11 GSM SPEEX16 OPUS
AudioCodecFactory::CreateDecoder   G722 AMR AMRWB PCMA PCMU NELLY8 NELLY11 GSM SPEEX16 OPUS AAC
VideoCodecFactory::CreateEncoder   SORENSON H263_1998 H263_1996 MPEG4 H264 VP8 AV1
VideoCodecFactory::CreateDecoder   SORENSON H263_1998 H263_1996 MPEG4 H264 VP6 VP8 AV1
```

**Les deux directions ne coïncident pas** : VP6 se décode et ne s'encode pas. Une
réponse qui les confond ferait offrir du VP6 à un pair, qui l'accepterait, et rien
ne sortirait. La capacité se déclare donc **par média ET par direction**.

## 2. Ce qu'il faut faire, côté serveur

**2.1 Dériver, ne plus déclarer.** Une fonction de capacité à côté de chaque
factory, dans le même fichier, de sorte qu'un `case` ajouté sans elle se voie :

```cpp
// libmedikit/audio.cpp, à côté des deux switch
const std::vector<AudioCodec::Type>& AudioCodecFactory::SupportedEncoders();
const std::vector<AudioCodec::Type>& AudioCodecFactory::SupportedDecoders();
// idem VideoCodecFactory dans video.cpp
```

Un test qui, pour chaque entrée annoncée, appelle réellement la factory et exige
un pointeur non nul — et, pour chaque codec de l'énumération **absent** de la
liste, exige `NULL`. C'est ce double sens qui empêche la liste de dériver dans
l'un ou l'autre sens.

**2.2 Réparer `GetSupportedCodecs`.** Le tableau figé disparaît au profit des
listes dérivées. **OPUS entre par là** — il n'a jamais été absent du serveur, il
était absent de la déclaration. La signature gagne la direction :

```
GetSupportedCodecs(i media, i direction) -> [ (i codecId, s codecName) ]
    direction : 0 = émission (encodeur), 1 = réception (décodeur)
```

Compatibilité : l'arité `(i media)` historique reste acceptée et répond
*encodeur*, ce que ses deux appelants attendaient de fait. Vidéo et texte cessent
d'être une erreur.

**2.3 Exposer sur `/jsr309`.** Même implémentation, seconde entrée dans la table
du dispatcher JSR309 — c'est l'API que le B2BUA parle, et il n'a aujourd'hui
aucun moyen de demander.

> ⚠️ **Règle de maintenance** : c'est un changement d'API XML-RPC, donc les
> `moteli_*.proto` sont mis à jour **dans le même lot** (cf. CLAUDE.md, §3.1 du
> document MOTELI).

## 3. Ce qu'il faut faire, côté elixip

Supprimer `@default_audio_codecs`, `@default_video_codecs`, `@default_text_codecs`
de `MediaServerMendoozeConn.ex` et les remplacer par ce que le serveur répond,
demandé une fois par connexion de contrôle et gardé en cache — la capacité d'un
serveur ne change pas en cours de vie.

Les deux chemins d'offre s'en servent différemment, et c'est pour cela qu'aucun
des deux ne suffit seul :

- **B2BUA sortant** : `ce que la jambe entrante a négocié ∩ ce que le serveur
  porte`. C'est la moitié « offre » du cross-leg de `mediagw_b2bua_jsr309.md` §5,
  celle qui reste à écrire ; sans elle on offre au callee des codecs que l'appelant
  ne sait pas produire.
- **Conférence sortante** : `ce que le serveur porte`, filtré par `medias` et le
  profil de la conférence. Il n'y a **pas d'autre jambe** dont dériver — c'est
  précisément le cas que le cross-leg ne couvre pas.

Ce second point n'est pas hypothétique : « une patte de conférence répond
toujours » est ce qui met aujourd'hui le module MCU à l'abri de toute cette
classe de bugs, et mcuGold avait une fonction d'appel sortant. Le jour où le
module offre, il n'a **aucune** liste — P8a les a retirées exprès au profit de
`Sdp.propose_all/2` — donc soit il réintroduit la liste statique qu'on vient de
payer, soit la question a une réponse.

## 4. Ordre

1. §2.1 les listes dérivées + leur test (autonome, aucun appelant touché) ;
2. §2.2 `GetSupportedCodecs` réparé, OPUS inclus, vidéo et texte servis ;
3. §2.3 exposition JSR309 + protos moteli dans le même lot ;
4. §3 elixip consomme et supprime ses listes ;
5. la moitié « offre » du cross-leg B2BUA, qui devient alors exprimable.

Les étapes 1 à 3 sont sans effet observable : rien n'appelle encore. Le
changement de comportement arrive en 4, et c'est là qu'il faut une passe de
trafic réel.
