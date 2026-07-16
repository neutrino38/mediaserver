// Redirige vers la version canonique dans libmedkit.
// L'implémentation (video.o, VideoCodecFactory) vient déjà de libmedkit.a.
// Les classes VideoInput/VideoOutput propres au mcu ont été remontées dans
// medkit/video.h pour que le mcu se base entièrement sur libmedkit.
#include "medkit/video.h"
