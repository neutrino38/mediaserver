// Redirige vers la version canonique dans libmedkit.
// L'implémentation (audio.o, AudioCodecFactory) vient déjà de libmedkit.a.
// Les classes AudioInput/AudioOutput propres au mcu ont été remontées dans
// medkit/audio.h pour que le mcu se base entièrement sur libmedkit.
#include "medkit/audio.h"
