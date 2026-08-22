# MP4 : baser `MP4Recorder` (mcu) sur libmedkit — renommage `mp4recorder` → `mp4writer`

> Conception pour aligner l'enregistrement MP4 du mcu sur le même patron que le
> streaming, désormais bâti sur libmedkit.
>
> Rédigé le 2026-07-02, **après** la migration de `mp4streamer` sur le lecteur
> libmedkit `mp4reader` (commit `50a5fe9` « refactoring mp4streamer basé sur
> libmedkit »).
>
> **Complète et actualise** `mp4_libmedkit.md` (analyse antérieure au chantier
> mp4reader). Ce document décrit la stratégie *réellement applicable* maintenant
> que le précédent lecture/écriture est établi.

---

## 1. Le précédent : ce que la migration `mp4streamer` a établi

La migration du streaming a posé un patron clair qu'il faut simplement
répliquer pour l'enregistrement :

| Rôle | Côté libmedkit (moteur bas niveau) | Côté mcu (pilote / adaptateur) |
|---|---|---|
| **Lecture** | `mp4reader` — pur producteur de `MediaFrame`, indépendant d'Asterisk | `MP4Streamer` — thread de lecture + `Listener`, ouvre le fichier |
| **Écriture** | `mp4recorder` → **à renommer `mp4writer`** — pur consommateur de `MediaFrame` | `MP4Recorder` — implémente `RecorderControl` + `MediaFrame::Listener` |

Points structurants observés dans `mcu/src/mp4streamer.cpp` :

1. **Le mcu possède le `MP4FileHandle`, pas libmedkit.** `MP4Streamer::Open`
   appelle `MP4Read(filename)` lui-même, puis `new mp4reader(NULL, handle)`. Le
   lecteur libmedkit **n'ouvre ni ne ferme** le fichier ; `MP4Streamer::Close`
   fait le `MP4Close`. (`mp4streamer.cpp:57-67`)
2. **`ctxdata` = `NULL`** côté mcu (pas de binding Asterisk). (`mp4streamer.cpp:67`)
3. **Le header libmedkit porte un nom distinct** (`medkit/mp4reader.h`), déclaré
   en *forward declaration* dans `mp4streamer.h` pour ne pas propager `<mp4v2/…>`
   à tous les consommateurs. (`mp4streamer.h:24`)
4. **Ordre d'inclusion** : les en-têtes mcu (`media/audio/video/codecs/
   avcdescriptor`) sont inclus **avant** `medkit/mp4reader.h` pour que les gardes
   d'inclusion partagées soient fixées par le mcu. (`mp4streamer.cpp:6-18`)

Conséquence directe pour l'enregistrement : **la même recette s'applique
mot pour mot**, en remplaçant lecture par écriture.

---

## 2. Pourquoi renommer `mp4recorder` → `mp4writer` (et pas seulement l'esthétique)

### 2.1 Symétrie de nommage

`mp4reader` lit, `mp4writer` écrit. Le nom actuel `mp4recorder` casse la
symétrie et, surtout, entre en **collision** avec la classe mcu `MP4Recorder`
(seule la casse les distingue) — source de confusion permanente dès qu'on
travaille sur les deux à la fois.

### 2.2 Raison technique : collision de garde d'inclusion (bloquante)

C'est l'argument décisif, pas un simple confort :

- `mcu/include/mp4recorder.h` ouvre avec `#ifndef _MP4RECORDER_H_`.
- `third_party/fontventa/libmedikit/medkit/mp4recorder.h` ouvre **avec la même**
  garde `#ifndef _MP4RECORDER_H_`.

Aujourd'hui il n'y a pas de casse car aucune unité de compilation n'inclut les
deux. Mais la coquille mcu (`mcu/src/mp4recorder.cpp`) devra inclure **son
propre** `mp4recorder.h` (pour `class MP4Recorder`) **et** le header libmedkit
(pour le moteur d'écriture). Avec deux gardes identiques, le second `#include`
est purement ignoré → erreur de compilation impossible à diagnostiquer.

Deux basenames identiques (`mp4recorder.h`) posent aussi un risque selon l'ordre
des `-I`. Le renommage en `medkit/mp4writer.h` (garde `_MP4WRITER_H_`) **élimine
les deux problèmes d'un coup** — exactement la raison pour laquelle le lecteur
s'appelle `mp4reader.h` et non `mp4player.h`/`mp4streamer.h`.

### 2.3 Périmètre du renommage (mesuré)

`grep` sur le symbole `mp4recorder` dans `third_party/fontventa/libmedikit/` :
**41 occurrences**, dont la grande majorité sont des chaînes de log
(`"-mp4recorder: …"`) cosmétiques. Les références de *code* réelles :

| Fichier | Nature | Action |
|---|---|---|
| `medkit/mp4recorder.h` | déclaration de classe + garde | → `medkit/mp4writer.h`, garde `_MP4WRITER_H_`, `class mp4writer` |
| `mp4recorder.cpp` | ctor/dtor + méthodes + `Mp4RecoderVideoCb` | → `mp4writer.cpp`, `mp4writer::…`, `#include "medkit/mp4writer.h"` |
| `astmedkit/mp4format.h` | `class AstMp4Recorder : public mp4recorder` + `using mp4recorder::ProcessFrame` + `#include <medkit/mp4recorder.h>` | changer la base en `mp4writer` et l'include |
| `Makefile` | cible `mp4recorder.o` + dépendances | → `mp4writer.o`, mettre à jour `OBJS` (l.99) et règles (l.146, 148) |
| `mp4format.cpp`, `mp4track.cpp` | uniquement chaînes de log `"-mp4recorder: …"` | facultatif (cosmétique) ; peut rester tel quel ou passer à `mp4writer:` |

**`AstMp4Recorder` garde son nom** (c'est le binding Asterisk, la sémantique
« recorder » y est correcte du point de vue applicatif) ; seule sa classe de
base et son include changent. Idem pour l'API C `Mp4RecorderCreate/…` :
inchangée.

> ⚠️ Le renommage touche `astmedkit/`, donc le **binding Asterisk** de fontventa.
> C'est un renommage transverse mcu ↔ Asterisk, à faire d'un bloc et à compiler
> des deux côtés. Mais il n'y a **aucun changement de comportement** — c'est un
> pur rename mécanique.

---

## 3. Stratégie retenue : renommage pur + coquille mcu

### 3.1 Ce qui change dans libmedkit : **rien d'autre que le nom**

Le point le plus important, et la principale correction par rapport à
`mp4_libmedkit.md` §6 (qui proposait d'ajouter un constructeur autonome +
`Record()`/`Stop()` dans libmedkit) :

> **Le précédent `mp4reader` prouve qu'aucun enrichissement de libmedkit n'est
> nécessaire.** Le moteur reste piloté par un handle fourni de l'extérieur ; le
> cycle de vie (`Create`/`Record`/`Stop`/`Close`) et le fichier vivent dans la
> coquille mcu. `mp4writer` = `mp4recorder` renommé, **à l'identique**.

Le constructeur existant convient tel quel :

```cpp
// medkit/mp4writer.h  (ex-mp4recorder.h)
class mp4writer            // ex-mp4recorder
{
public:
    mp4writer(void * ctxdata, MP4FileHandle mp4, bool waitVideo);   // inchangé
    virtual ~mp4writer();                                           // inchangé
    int AddTrack(AudioCodec::Type, DWORD samplerate, const char * trackName);
    int AddTrack(VideoCodec::Type, DWORD w, DWORD h, DWORD br, const char *, bool secondary=false);
    int AddTrack(TextCodec::Type, const char * trackName, int textfile);
    int ProcessFrame(const MediaFrame * f, bool secondary=false);   // le cœur
    void SetParticipantName(const char *);
    void SetInitialDelay(unsigned long);
    int  IsVideoStarted();
    void SetWaitForVideo(bool);
    void Flush();
    // … reste strictement inchangé
};
```

Pas de nouveau constructeur, pas de `Record()`/`Stop()`, pas de gestion de
fichier. La modification libmedkit est un renommage sans risque fonctionnel.

### 3.2 Ce qui change dans le mcu : `mp4recorder.cpp` réécrit en coquille

`mcu/include/mp4recorder.h` : on **supprime** la classe interne `mp4track`
(elle fait double emploi avec `Mp4Basetrack`/`Mp4AudioTrack`/… de libmedkit) et
`MP4Recorder` devient une fine coquille. L'interface publique
(`RecorderControl` + `MediaFrame::Listener`) est **inchangée** — les
consommateurs (§4) ne voient aucune différence.

```cpp
// mcu/include/mp4recorder.h
#ifndef _MP4RECORDER_H_
#define _MP4RECORDER_H_

#include <mp4v2/mp4v2.h>
#include "codecs.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "media.h"
#include "recordercontrol.h"

// Forward declaration : n'impose pas medkit/mp4writer.h (ni mp4v2) aux
// consommateurs, comme mp4streamer.h le fait pour mp4reader.
class mp4writer;

class MP4Recorder :
    public RecorderControl,
    public MediaFrame::Listener
{
public:
    MP4Recorder();
    ~MP4Recorder();

    // Interface RecorderControl (inchangée)
    virtual bool Create(const char *filename);
    virtual bool Record();
    virtual bool Stop();
    virtual bool Close();
    virtual RecorderControl::Type GetType() { return RecorderControl::MP4; }

    // Interface MediaFrame::Listener (inchangée)
    virtual void onMediaFrame(MediaFrame &frame);

private:
    MP4FileHandle   mp4;
    mp4writer*      writer;      // moteur d'écriture libmedkit
    bool            recording;
    pthread_mutex_t mutex;       // sérialise onMediaFrame vs Close
};
#endif
```

```cpp
// mcu/src/mp4recorder.cpp  (nouvelle version, ~80 lignes au lieu de 640)
// En-têtes mcu d'abord (fixent les gardes partagées), puis le moteur libmedkit.
#include "log.h"
#include "codecs.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "mp4recorder.h"
#include "medkit/mp4writer.h"
#include <mp4v2/mp4v2.h>

MP4Recorder::MP4Recorder() {
    mp4 = MP4_INVALID_FILE_HANDLE;
    writer = NULL;
    recording = false;
    pthread_mutex_init(&mutex, 0);
}

MP4Recorder::~MP4Recorder() {
    Close();
    pthread_mutex_destroy(&mutex);
}

bool MP4Recorder::Create(const char *filename) {
    Log("-Opening record [%s]\n", filename);
    if (mp4 != MP4_INVALID_FILE_HANDLE) Close();

    // Le mcu possède le handle (comme MP4Streamer::Open avec MP4Read).
    mp4 = MP4Create(filename, 0);
    if (mp4 == MP4_INVALID_FILE_HANDLE)
        return Error("-Error opening mp4 file for recording\n");

    // waitVideo=true : on attend la première I-frame (comportement historique).
    // ctxdata=NULL : pas de transcodage vidéo interne (cf. risque #1).
    writer = new mp4writer(NULL, mp4, true);
    return true;
}

bool MP4Recorder::Record() {
    if (mp4 == MP4_INVALID_FILE_HANDLE)
        return Error("No MP4 file opened for recording\n");
    recording = true;
    return true;
}

bool MP4Recorder::Stop() {
    recording = false;
    return true;
}

bool MP4Recorder::Close() {
    pthread_mutex_lock(&mutex);
    if (mp4 == MP4_INVALID_FILE_HANDLE) { pthread_mutex_unlock(&mutex); return false; }
    recording = false;
    if (writer) { writer->Flush(); delete writer; writer = NULL; }  // dtor écrit les tags
    MP4Close(mp4);
    mp4 = MP4_INVALID_FILE_HANDLE;
    pthread_mutex_unlock(&mutex);
    return true;
}

void MP4Recorder::onMediaFrame(MediaFrame &frame) {
    pthread_mutex_lock(&mutex);
    if (recording && writer)
        writer->ProcessFrame(&frame);   // libmedkit gère waitVideo, timing, pistes
    pthread_mutex_unlock(&mutex);
}
```

**La logique de timing / waitVideo / création de pistes disparaît du mcu** :
elle est déjà présente et plus riche dans `mp4writer::ProcessFrame`
(auto-création de piste audio, attente d'I-frame, délai initial, prologue
vidéo, hint tracks, sous-titres). C'est exactement le gain visé.

---

## 4. Consommateurs de `MP4Recorder` (mcu) — impact

L'interface publique de `MP4Recorder` ne change pas, donc **aucun** de ces
appelants n'est modifié :

| Consommateur | Usage | Impact |
|---|---|---|
| `mcu/include/rtpparticipant.h:75` | membre `MP4Recorder recorder;` | aucun |
| `mcu/src/jsr309/Recorder.h:18` | `class Recorder : public MP4Recorder` + `onRTPPacket` → `onMediaFrame` | aucun (héritage préservé) |
| `mcu/include/multiconf.h`, `mediabridgesession.h` | inclusion transitive | aucun |
| `mcu/lib/mediamixer.cpp` | wrappers C `MP4RecorderCreate/Record/Stop/…` | **à vérifier** : appelle `Record(filename)` et `End()` qui ne sont pas dans l'interface actuelle → ce fichier semble déjà désynchronisé ; confirmer s'il est réellement compilé dans la cible `mcu` avant de s'en soucier |

> Point de vigilance `mediamixer.cpp` : il appelle `recorder->Record(filename)`
> (un seul argument) et `recorder->End()`, absents de l'interface `MP4Recorder`
> actuelle. Soit ce fichier appartient à une autre cible (MediaMixer autonome),
> soit il est déjà cassé. À trancher avant l'implémentation, mais **hors du
> périmètre** du renommage lui-même.

`jsr309/Recorder` reste pleinement compatible : il ne fait qu'appeler
`onMediaFrame` (hérité) et les méthodes `RecorderControl` — toutes conservées.

---

## 5. Impact Makefile

### 5.1 libmedkit (`third_party/fontventa/libmedikit/Makefile`)

```make
# l.99 : renommer l'objet
OBJS+=mp4track.o logo.o picturestreamer.o mp4writer.o mp4reader.o   # ex mp4recorder.o

# l.146 : renommer la règle de dépendance
mp4writer.o: medkit/mp4writer.h medkit/media.h                     # ex mp4recorder.o/.h

# l.148 : mettre à jour la dépendance de mp4format.o
mp4format.o: astmedkit/mp4format.h medkit/mp4writer.h medkit/mp4reader.h medkit/media.h
```

Renommer physiquement les fichiers : `git mv mp4recorder.cpp mp4writer.cpp` et
`git mv medkit/mp4recorder.h medkit/mp4writer.h`.

### 5.2 mcu (`mcu/Makefile`)

**Aucun changement de liste d'objets nécessaire.** `mp4recorder.o` reste dans
`OBJS` (l.87) — c'est toujours `mcu/src/mp4recorder.cpp` qui est compilé, mais
son contenu est réécrit. `libmedkit.a` (déjà lié, l.157/160) fournit désormais
`mp4writer.o`. `-I$(MEDKITDIR)` (l.156) est déjà en place.

> Note : `mp4player.o` et `mp4streamer.o` restent dans `OBJS` (l.87). La
> suppression de `mp4player.cpp`/`mp4streamer.cpp` est un autre chantier
> (cf. `mp4_libmedkit.md` §2.3 et palier C). Ce document ne traite **que** le
> recorder.

---

## 6. Différences de comportement à valider

Le remplacement du moteur d'écriture change la mécanique interne. Points à
vérifier en test manuel (pas de suite automatisée — cf. CLAUDE.md) :

| # | Sujet | Ancien mcu | Nouveau (mp4writer) | Vérification |
|---|---|---|---|---|
| A | **Rebasage timestamp** | rebase sur la 1ʳᵉ I-frame via `getDifTime(&first)` | `SetInitialDelay` + `firstframets`, gère aussi silences/frames noires | Contrôler la synchro A/V d'un enregistrement ; pas de dérive |
| B | **Attente vidéo** | `waitVideo`, ne démarre qu'à l'I-frame | `waitVideo` dans ctor + logique `IsVideoStarted`/relance FIR | Enregistrement audio-seul ET audio+vidéo |
| C | **Codecs supportés** | PCMU/PCMA/AAC + H263/H264 | + SLIN, AMR-NB, GSM, OPUS… (sur-ensemble) | Vérifier codecs réellement produits par le mixer |
| D | **Piste texte** | sous-titres bruts | T140/T140RED + sauvegarde en commentaire MP4 (`saveTxtInComment`) | Enregistrement RTT si utilisé |
| E | **Tags MP4** | aucun | dtor écrit `Artist`=partName, `Comments`=texte | `ffprobe` sur le fichier produit |
| F | **Nom participant** | absent | `SetParticipantName` dispo (optionnel à câbler) | facultatif |

---

## 7. Risques et points ouverts

| # | Risque | Proposition | Statut |
|---|---|---|---|
| 1 | `ctxdata=NULL` → crash si `mp4writer` invoque le callback vidéo `Mp4RecoderVideoCb` (`mp4recorder.cpp:411`, qui caste `ctxdata` en `mp4writer*`) | Vérifier que le callback n'est armé que si transcodage vidéo actif ; le mcu ne l'active pas. Sinon passer `this` ou un shim non-NULL | ☐ |
| 2 | Création paresseuse des pistes (compatible mp4v2, pas libavformat) | TODO palier B (`supp_mp4v2.md`) — hors périmètre | ☐ |
| 3 | Le renommage touche `astmedkit/` (binding Asterisk fontventa) | Faire le rename d'un bloc, compiler mcu **et** Asterisk | ☐ |
| 4 | `mediamixer.cpp` appelle `Record(filename)`/`End()` absents de l'interface | Vérifier la cible de compilation ; corriger séparément si nécessaire | ☐ |
| 5 | Comportement de timing différent (§6.A) | Test A/V manuel avant merge | ☐ |
| 6 | `SetParticipantName`/`SetInitialDelay` non câblés dans la coquille | Optionnel ; ajouter des setters sur `MP4Recorder` si le multiconf veut les exposer | ☐ |

---

## 8. Plan d'implémentation

1. **Renommage libmedkit** (mécanique, ~30 min)
   - `git mv mp4recorder.cpp mp4writer.cpp` ; `git mv medkit/mp4recorder.h medkit/mp4writer.h`
   - Sed `mp4recorder` → `mp4writer` sur : la classe, ctor/dtor, méthodes, garde
     `_MP4RECORDER_H_` → `_MP4WRITER_H_`, l'include dans `mp4writer.cpp`
   - `astmedkit/mp4format.h` : base de `AstMp4Recorder` + include
   - `Makefile` : `OBJS` l.99, règles l.146/148
   - Chaînes de log `"-mp4recorder:"` : laisser ou renommer (cosmétique)
   - Compiler `libmedkit.a` seul → vert
2. **Réécriture coquille mcu** (~1 h)
   - `mcu/include/mp4recorder.h` : supprimer `class mp4track`, forward-declarer
     `mp4writer`, réduire `MP4Recorder`
   - `mcu/src/mp4recorder.cpp` : version §3.2
   - `make -C mcu mcu` → vert
3. **Vérifications comportementales** (§6) — test manuel : enregistrer une
   conférence, `ffprobe` + lecture VLC (audio, vidéo, synchro, tags).
4. **Trancher `mediamixer.cpp`** (risque #4) si concerné par la cible.

Estimation : ~2 h + tests. Aucune dépendance sur le chantier `supp_mp4v2.md`
(palier B) : mp4v2 reste lié, `mp4writer` continue de l'utiliser en interne.

---

## 9. Références

| Fichier | Rôle |
|---|---|
| `mcu/src/mp4streamer.cpp` / `mcu/include/mp4streamer.h` | **Le patron à répliquer** (pilote sur `mp4reader`) |
| `third_party/fontventa/libmedikit/medkit/mp4reader.h` | Précédent lecture — nommage `reader` |
| `mcu/src/mp4recorder.cpp` / `mcu/include/mp4recorder.h` | Recorder mcu actuel (à réécrire en coquille) |
| `third_party/fontventa/libmedikit/medkit/mp4recorder.h` | Moteur d'écriture libmedkit (à renommer `mp4writer`) |
| `third_party/fontventa/libmedikit/mp4recorder.cpp` | Implémentation (`ProcessFrame`, `AddTrack`, `Flush`, callback vidéo) |
| `third_party/fontventa/libmedikit/mp4track.h` | Hiérarchie `Mp4Basetrack`/`Mp4AudioTrack`/… (remplace le `mp4track` interne mcu) |
| `third_party/fontventa/libmedikit/astmedkit/mp4format.h` | `AstMp4Recorder` — binding Asterisk, base à mettre à jour |
| `mcu/src/jsr309/Recorder.h` | Consommateur (`: public MP4Recorder`) — non impacté |
| `mp4_libmedkit.md` | Analyse d'ensemble antérieure (partiellement dépassée par ce doc) |
| `third_party/fontventa/libmedikit/supp_mp4v2.md` | Palier B (mp4v2 → libavformat), ultérieur |
</content>
</invoke>
