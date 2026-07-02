// Redirige vers la version canonique dans libmedkit.
// RTPRedundantPayload (et RTPRedundantEncoder, requis par mp4reader) sont
// desormais fournis par medkit/red.h. La definition dupliquee du mcu a ete
// supprimee pour eviter la double definition avec red.o de libmedkit.
// Note : la version libmedkit corrige aussi le decodage du champ block length
// (10 bits) qui etait errone dans l'ancien red.cpp du mcu.
#include "medkit/red.h"
