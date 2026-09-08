# Durée de vie des trames encodées

> Référence du contrat mémoire entre un encodeur de `libmedikit` et celui qui
> consomme sa sortie. Conception : `docs/conception/FRAME-PTR/SPEC.md`.

## 1. Les deux objets

| Objet | Contenu | Type partagé | Qui alloue |
|---|---|---|---|
| `Pict` | image brute (`AVFrame`) | `PictPtr = shared_ptr<Pict>` | décodeur, mixeur, graphe avfilter |
| `Samples` | échantillons bruts (`AVFrame`) | `SamplesPtr = shared_ptr<Samples>` | décodeur, mixeur |
| `VideoFrame` | trame vidéo **encodée** (bitstream + packetisation RTP) | `VideoFramePtr = shared_ptr<VideoFrame>` | l'encodeur, une neuve par image |
| `AudioFrame` | trame audio **encodée** | `AudioFramePtr = shared_ptr<AudioFrame>` | l'encodeur, une neuve par trame |

Les quatre se partagent de la même façon : par `shared_ptr`, jamais par copie de
l'objet.

## 2. Ce que l'encodeur promet

```cpp
VideoFramePtr VideoEncoder::EncodeFrame(PictPtr pic);
AudioFramePtr AudioEncoder::EncodeFrame(SamplesPtr samples);
```

- La trame rendue est **neuve**. L'encodeur n'en garde aucune référence.
- Elle vit tant qu'un `shared_ptr` la tient. Rien de ce qui arrive ensuite à
  l'encodeur ne l'invalide : `SetFrameRate`, `SetSize`, réouverture du codec,
  `EncodeFrame` suivant, destruction de l'encodeur.
- `nullptr` veut dire « pas de trame cette fois » : encodeur fermé, image mise en
  attente par le codec, fifo audio trop courte.
- Un encodeur audio peut produire plusieurs trames pour un seul appel. On boucle
  jusqu'au `nullptr`, `samples = nullptr` pour purger :

```cpp
for (AudioFramePtr f = enc->EncodeFrame(samples); f; f = enc->EncodeFrame(nullptr))
    ...
```

Test qui tient ce contrat : `libmedikit/tests/test_encoder_frame_lifetime.cpp`.
Il garde une trame, force une réouverture, relit la trame. À jouer sous
`ASAN=yes` (Makefile de libmedikit) pour qu'une lecture après libération soit
une erreur nette.

## 3. Ce que le consommateur doit savoir

- Il garde la trame aussi longtemps qu'il veut : il suffit de garder le
  `shared_ptr`.
- Les fonctions qui prennent encore un `MediaFrame*` ou une `MediaFrame&`
  (`RTPSmoother::SendFrame`, `mp4writer::ProcessFrame`,
  `MediaFrame::Listener::onMediaFrame`) **copient pendant l'appel** et ne
  gardent pas le pointeur. On leur passe `frame.get()` ou `*frame`. Une telle
  fonction qui garderait le pointeur au-delà de l'appel violerait ce contrat :
  elle doit alors prendre le `shared_ptr`.
- Le chemin de **réception** (dépaquetiseurs RTP, `mp4track`, `Clone()`) n'est
  pas couvert : il manipule encore des `MediaFrame*` bruts avec leurs propres
  règles.

## 4. Pas de pool

Les trames sont allouées par `std::make_shared`, sans pool. glibc recycle déjà
une taille fixe demandée et rendue en boucle ; le smoother alloue par ailleurs
un objet par paquet RTP. Critère de révision : sur un appel long sous
`perf record`, `malloc` + `free` au-dessus de 1 % du temps CPU, ou un RSS qui
dérive. La réponse serait alors un stockage `AVPacket` avec `av_buffer_pool`,
pas un pool maison.
