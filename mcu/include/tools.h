// Redirige vers la version canonique dans libmedkit.
// Les helpers propres au mcu (getTimeMS, setZeroThread, PC, BitPrint et les
// defines NULL/TRUE/FALSE) ont ete remontes dans medkit/tools.h afin que
// libmedkit soit la source unique et d'eviter la collision de garde _TOOLS_H_
// entre les deux tools.h (qui masquait BitPrint/PC requis par log.h).
#include "medkit/tools.h"
