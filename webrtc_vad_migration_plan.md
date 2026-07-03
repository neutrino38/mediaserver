# Plan — Migration VAD vers `webrtc-audio-processing-devel`

> Objectif : supprimer la dépendance à l'ancien WebRTC « trunk » (clone `webrtc_stack`
> + `Makefile.vad`) et réimplémenter la classe `VAD` (`mcu/include/vad.h`,
> `mcu/src/vad.cpp`) sur le paquet système `webrtc-audio-processing-devel` (0.3.1, el9).
> Lier `webrtc-audio-processing` en statique si possible.

## 1. Constat / état actuel

| Élément | Aujourd'hui |
|---|---|
| API utilisée | **Bas niveau C** de l'ancien webrtc trunk : `WebRtcVad_InitCore`, `WebRtcVad_set_mode_core`, `WebRtcVad_CalcVad{8,16,32}khz`, `WebRtcVad_ValidRateAndFrameLength`, struct `VadInstT` |
| Fourniture des libs | `install.ksh` → `compile_webrtc` : `git clone webrtc_stack` + `make -f Makefile.vad` → `libvad.a` + `libsignal_processing.a` sous `~/webrtc/trunk/...` |
| Link | `mcu/Makefile.rpm` : `VADLD = -L …/common_audio -lvad -lsignal_processing -lopus` |
| Compile flags | `-DVADWEBRTC -DOPUS_SUPPORT`, `VADINCLUDE = -I~/webrtc/trunk/webrtc/` |

**Le paquet cible n'expose QUE l'API haut niveau C++ `webrtc::AudioProcessing` (APM)**
avec le composant `VoiceDetection`. L'ancienne API C bas niveau n'existe pas →
**réécriture** de `vad.cpp`/`vad.h`, pas un simple changement de `-I`.

**Surface réellement utilisée** de `VAD` (vérifiée) : constructeur + `IsRateSupported()`
+ `CalcVad()`, uniquement dans `mcu/src/pipeaudiooutput.cpp`. `SetMode()` et `GetVAD()`
sont définis mais **jamais appelés** → conservés pour la forme, non contraignants.

`-DOPUS_SUPPORT` n'est référencé **nulle part** dans le code → supprimé avec le reste.

⚠️ **Statique** : le devel ne fournit que `libwebrtc_audio_processing.so` (pas de `.a`).
Lien statique impossible depuis le paquet → recompilation des sources 0.3.1 requise
(voir §4).

## 2. Conception de la nouvelle classe VAD (sur APM)

Interface publique **inchangée** (aucun appelant à modifier) :

```cpp
#ifdef VADWEBRTC
#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>

class VAD {
public:
    typedef enum { QUALITY=0, LOWBITRATE=1, AGGRESSIVE=2, VERYAGGRESIVE=3 } Mode;
    VAD();
    ~VAD();
    bool SetMode(Mode mode);
    int  CalcVad(SWORD* frame, DWORD size, DWORD rate);
    int  GetVAD();
    bool IsRateSupported(DWORD rate) { return (rate==8000||rate==16000||rate==32000); }
private:
    webrtc::AudioProcessing* apm;
    int last;
};
#endif
```

**Implémentation (`vad.cpp`) :**
- **Constructeur** : `apm = webrtc::AudioProcessing::Create();` puis
  `apm->voice_detection()->Enable(true);`, `set_frame_size_ms(10);`,
  `set_likelihood(...)` selon le mode par défaut VERYAGGRESIVE (comportement actuel).
- **Mapping Mode → `VoiceDetection::Likelihood`** (échelle APM *inverse* de l'agressivité) :
  - `QUALITY` → `kHighLikelihood`
  - `LOWBITRATE` → `kModerateLikelihood`
  - `AGGRESSIVE` → `kLowLikelihood`
  - `VERYAGGRESIVE` → `kVeryLowLikelihood`
- **`CalcVad(buf, size, rate)`** : l'APM impose des trames de **10 ms exactement** à un
  *native rate* (8/16/32/48 kHz), int16 mono. Découper `buf` en blocs de `rate/100`
  échantillons, remplir un `webrtc::AudioFrame` (`UpdateFrame(...)` ou champs publics
  `data_`/`samples_per_channel_`/`sample_rate_hz_`/`num_channels_=1`), appeler
  `apm->ProcessStream(&frame)` puis cumuler `voice_detection()->stream_has_voice()`
  (OU logique). Retour **0/1** comme avant, mémorisé dans `last`. Reliquat < 10 ms ignoré
  (équivalent au clipping 10/20 ms de l'ancien code).
- **`GetVAD()`** : renvoie `last`.
- **Destructeur** : `delete apm;` (`Create()` transfère la propriété).
- Le bloc `#else` (VAD no-op pour `VADWEBRTC=no`) reste **inchangé**.

La logique d'agrégation aval (`acu` dans `PipeAudioOutput`, VADProxy/`AudioMixer::GetVAD`,
sélection du locuteur dans `videomixer`) n'est **pas touchée** : elle consomme toujours
un 0/1 par trame.

## 3. Modifications `mcu/Makefile.rpm`

- Bloc `ifeq ($(VADWEBRTC),yes)` → pkg-config :
  - `VADINCLUDE = $(shell pkg-config --cflags webrtc-audio-processing)`
    (fournit `-I/usr/include/webrtc_audio_processing -DWEBRTC_POSIX -DWEBRTC_AUDIO_PROCESSING_ONLY_BUILD`)
  - `VADLD = $(shell pkg-config --libs webrtc-audio-processing)` (→ `-lwebrtc_audio_processing`)
  - `OPTS += -DVADWEBRTC` (retirer `-DOPUS_SUPPORT`)
- Supprimer les variables mortes `WEBRTCINCLUDE`, `WEBRTDIROBJ` (l.23-24).
- Corriger la ligne 33 (`FEWSTATICDEPS`) : retirer `-I/usr/include/webrtc` obsolète
  (remplacé par `VADINCLUDE`).

## 4. Lien statique (variable make `WEBRTCSTATIC`)

- **Défaut — dynamique (recommandé)** : `-lwebrtc_audio_processing` via pkg-config.
  Cohérent avec le mode `FEWSTATICDEPS` d'AlmaLinux 9 (ffmpeg/x264 déjà dynamiques).
  `Requires: webrtc-audio-processing` dans le .spec.
- **`WEBRTCSTATIC=yes`** : fonction `install.ksh` télécharge les sources **0.3.1**
  (freedesktop, autotools), `./configure --enable-static --disable-shared --prefix=$PWD/staticdeps`,
  `make && make install` → `staticdeps/lib/libwebrtc_audio_processing.a`, puis
  `VADLD = -Wl,-Bstatic -lwebrtc_audio_processing -Wl,-Bdynamic` depuis `staticdeps`.
  À confirmer avant engagement : système de build exact de 0.3.1 et dépendances de lien
  (pthread, stdc++).

## 5. Modifications `install.ksh`

- **Supprimer** : `compile_webrtc_from_google`, `compile_webrtc`, `archive_webrtc`,
  l'appel `compile_webrtc;` dans `local_compile` (l.389), l'entrée `"webrtc")` du dispatch
  (l.500-501) et sa ligne d'aide (l.521).
- **Ajouter** dans `local_compile` un contrôle de prérequis :
  `rpm -q webrtc-audio-processing-devel` + présence de `pkg-config`.
- (Variante statique) fonction `compile_webrtc_ap_static` + entrée dispatch.

## 6. `mcumediaserver.spec`

- `BuildRequires: webrtc-audio-processing-devel`
- `Requires: webrtc-audio-processing` (mode dynamique uniquement).

## 7. Validation

1. `cd mcu && make -f Makefile.rpm mcu` → build vert.
2. `ldd bin/debug/mcu | grep webrtc` → présent (dynamique) / absent (statique).
3. Test fonctionnel : conférence avec bascule VAD (`--vad-period`), vérifier la sélection
   du locuteur actif dans la mosaïque en réaction à la parole.

## Fichiers touchés

`mcu/include/vad.h` · `mcu/src/vad.cpp` (réécriture) · `mcu/Makefile.rpm` ·
`install.ksh` · `mcumediaserver.spec`. **Aucun appelant modifié.**

## Décisions en attente

- Stratégie de lien : dynamique / statique / switch (défaut proposé : **switch**,
  dynamique par défaut + `WEBRTCSTATIC=yes`).
