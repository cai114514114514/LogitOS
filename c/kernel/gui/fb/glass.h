/* Compatibility include: the one material implementation belongs to OpenLogit. */
/* Bare name, not a path: c/lib/gfx moved its public headers into
 * c/lib/gfx/include/ and this spelling pointed at where they used to be, so
 * the include was dead before this file moved. Spelled as a path rather than a bare name so the
 * narrow -I sets the host gates pass keep working without each of them
 * growing a -Ic/lib/gfx/include of its own. */
#include "../../../lib/gfx/include/openlogit_glass.h"
