// Redirige vers la version canonique dans libmedkit.
// UTF8Parser, TextFrame, TextInput et TextOutput sont desormais fournis par
// medkit/text.h (remontes dans libmedkit). Les definitions dupliquees du mcu
// ont ete supprimees pour eviter les collisions ODR a l'edition de liens
// (UTF8Parser etait aussi defini dans amf.o, TextFrame ici).
#include "medkit/text.h"
