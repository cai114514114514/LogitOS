/* MB_LEN_MAX must hold the longest multibyte character this libc produces.
 * clang's own <limits.h> defines MB_LEN_MAX as 1 and the mini-libc override was
 * behind an #ifndef, so it never applied. Compile-time only (-fsyntax-only). */
#include <limits.h>
#include <stdlib.h>
#include <wchar.h>

_Static_assert(MB_LEN_MAX >= MB_CUR_MAX, "MB_LEN_MAX is smaller than MB_CUR_MAX");
_Static_assert(MB_LEN_MAX >= 4, "MB_LEN_MAX cannot hold a four-byte UTF-8 sequence");

int libc_mb_len_max_ok;
