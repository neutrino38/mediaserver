# Revue de code du mixeur audio — améliorations possibles

Revue du 2026-08-04 (branche `feat/av-frame`). Périmètre : `mcu/src/audiomixer.cpp`
(865 l.), `mcu/include/audiomixer.h`, `mcu/src/sidebar.cpp`, `mcu/include/sidebar.h`,
avec un œil sur `pipeaudiooutput.cpp`/`pipeaudioinput.cpp` (déjà modernisés :
mutex std, resampler libswresample).

Architecture en place — saine dans son principe :
- un thread mixeur cadencé à 10 ms (`MixAudio`) ; le nombre d'échantillons par
  tick suit l'horloge réelle (pas de dérive cumulative) ;
- première passe : lecture des fifos participants + VAD ; somme de chaque
  participant dans les buffers des sidebars auxquels il appartient ;
- seconde passe : « mix-minus » — chaque participant reçoit le mix de SON
  sidebar moins sa propre voix (soustraction SSE2), les non-membres reçoivent
  le mix tel quel ;
- verrou global `Use` (lecteurs `IncUse`, écrivain exclusif `WaitUnusedAndLock`),
  pipes en co-propriété `shared_ptr` avec les streams (Point 1 / C-4 du chantier
  smart pointers).

---

## 1. Défauts réels, par ordre d'importance

### 1.1 Pas de saturation dans le mixage (qualité audio) — PRIORITAIRE

`Sidebar::Update` (sidebar.cpp:41-60) additionne les échantillons en 16 bits
avec `_mm_add_epi16` (et `+=` en scalaire) ; le mix-minus
(audiomixer.cpp:181-201) soustrait avec `_mm_sub_epi16`. C'est de
l'arithmétique **modulo**, pas saturée : deux locuteurs forts simultanés font
déborder le 16 bits et le signal *wrap* (passe de +32767 à −32768) →
craquements violents, bien pires qu'un simple clipping.

- **Correctif quasi gratuit** : `_mm_adds_epi16` / `_mm_subs_epi16`
  (variantes **saturantes**, strictement même coût CPU) ; côté scalaire,
  accumuler en `int` et borner à [−32768, 32767].
- **Correctif propre (optionnel, plus tard)** : accumuler le mix de chaque
  sidebar en 32 bits (`int32_t mixer_buffer[]`, +16 Ko par sidebar), faire le
  mix-minus en 32 bits et ne saturer qu'à la restitution. Rend le mix-minus
  exact même quand la somme sature (avec la saturation 16 bits, retirer sa voix
  d'un mix saturé reste approximatif — inhérent au 16 bits).

### 1.2 Alignement SSE fragile (bombe à retardement)

`_mm_load_si128`/`_mm_store_si128` exigent un alignement de 16 octets.
`Sidebar::mixer_buffer` est alloué aligné 32 (malloc32) ✓, mais
`AudioSource::buffer` (audiomixer.h:58) est un simple tableau membre à
l'offset 32 d'une struct allouée par `new` : l'alignement 16 tombe juste **par
chance** (glibc x86-64). Ajouter un membre avant `buffer` dans la struct →
SIGSEGV en production, loin du site fautif.

- **Correctif** : `alignas(16)` sur le membre `buffer` (une ligne), ou passer
  aux variantes non alignées `_mm_loadu/_mm_storeu_si128` (coût nul sur les
  CPU modernes). Les deux sites : sidebar.cpp:48-58 et audiomixer.cpp:189-200.

### 1.3 Mutations d'état sous verrou LECTEUR (data race entre appels XML-RPC)

Le patron `Use` réserve `IncUse` aux lecteurs. Or plusieurs méthodes
**modifient** l'état sous simple `IncUse` :
- `CreateSidebar` (audiomixer.cpp:746-761) : insertion dans la map `sidebars`
  — et `numSidebars++` carrément **hors verrou** ;
- `AddSidebarParticipant` / `RemoveSidebarParticipant` (:681-744) : mutation
  du `std::set` du sidebar ;
- `SetMixerSidebar` (:593-634) et `InitMixer` (:370-418) : écriture de
  `audio->sidebar`, `AddParticipant` sur le sidebar par défaut.

Vis-à-vis du thread mixeur c'est sûr (lui prend l'exclusif), mais **deux
requêtes XML-RPC concurrentes** (workers Abyss multiples sur la même
conférence) peuvent muter la même map/set en parallèle → corruption mémoire.

- **Correctif** : basculer ces méthodes sur `WaitUnusedAndLock` (elles sont
  courtes ; la contention avec le tick de 10 ms restera négligeable).
  Vérifier au passage qu'aucun appelant ne tient déjà le verrou.

### 1.4 `DeleteSidebar` du sidebar par défaut = use-after-free différé

`DeleteSidebar(0)` (audiomixer.cpp:763-804) détruit le sidebar par défaut mais
laisse `defaultSidebar` pendouiller ; le `InitMixer` suivant écrit dedans. Le
contrôleur ne le fait probablement jamais — raison de plus pour le refuser
explicitement (garde d'une ligne en tête de fonction).

### 1.5 Bug de format dans `GetMixerSidebar`

audiomixer.cpp:639 : `Log(">GetMixerSidebar [id:%d,sidebar:%d]\n", id)` —
deux `%d`, un seul argument → lecture de pile (UB), affichage poubelle.

---

## 2. Nettoyages et améliorations secondaires

- **Code mort VAD sidebar** : bloc `#if 0` (audiomixer.cpp:135-142),
  `Sidebar::UpdatePartVad` et le membre `avgVad` — jamais lu, jamais remis à
  zéro, et **jamais initialisé** dans le constructeur. À supprimer ensemble
  (ou initialiser si on compte s'en resservir).
- **Passe 2 inutile pour un participant muet** : si `audio->len == 0`, la
  boucle de mix-minus calcule `mixed − 0` échantillon par échantillon ; un
  `PutSamples(mixed, numSamples)` direct suffit (micro-optimisation gratuite).
- **Messages** : `SetMixerSidebar` logge « Sidebar %d for participant found,
  will be send only » — il manque le « No » (présent dans `InitMixer`).
  `Init` : « Musr be 8000, 16000 or 32000 Hz » — typo, et le 48000 accepté
  n'est pas cité.
- `DumpMixerInfo` : `sprintf` dans un tampon de 40 octets — passe aujourd'hui,
  `snprintf` par principe.
- **Cadence** : `msleep` relatif introduit une petite gigue de tick ; sans
  conséquence (numSamples suit l'horloge réelle), mais
  `clock_nanosleep(TIMER_ABSTIME)` donnerait un tick régulier — utile si on
  mesure un jour la latence de bout en bout.
- **Modernisation** (cohérent avec le chantier smart pointers) : `audios` et
  `sidebars` sont des maps de pointeurs bruts avec `delete` manuel →
  candidates `unique_ptr` ; `mixingAudio` est un `int` lu par le thread
  mixeur sans atomicité → `std::atomic<bool>` (inoffensif en pratique
  aujourd'hui, mais c'est le réflexe attendu).

---

## 3. Points vérifiés SANS problème (pour mémoire)

- Le remplissage silence des fifos courtes est correct : la première passe
  memset le reste du buffer participant, donc le mix-minus retire bien 0
  au-delà de `len` et la boucle SSE qui arrondit à 8 échantillons ne lit que
  du buffer dimensionné `MIXER_BUFFER_SIZE`.
- `DeleteMixer` : l'entrée est retirée de la map sous verrou exclusif puis le
  `delete` se fait hors verrou — le thread mixeur ne peut plus la voir, et les
  pipes survivent chez les streams via `shared_ptr` (documenté).
- L'ajout systématique au sidebar par défaut dans `InitMixer` (le participant
  CONTRIBUE au défaut, ÉCOUTE `audio->sidebar`) est un choix de conception
  (défaut = « tous »), pas un bug — mais mérite un commentaire dans le code.

---

## 4. Ordre d'attaque suggéré

1. §1.1 saturation (adds/subs saturants) + §1.2 alignas — audibles/critiques,
   4 lignes en tout, zéro risque.
2. §1.5 format log + §1.4 garde DeleteSidebar + nettoyages §2 — triviaux.
3. §1.3 verrouillage des mutations — petite passe sur les appelants d'abord.
4. Optionnel, plus tard : accumulation 32 bits par sidebar (§1.1 propre),
   unique_ptr/atomic (§2).

Chaque étape est indépendante et testable par la suite gtest existante
(`make -C mcu -f Makefile.rpm check`) + un appel réel pour la partie audible.
