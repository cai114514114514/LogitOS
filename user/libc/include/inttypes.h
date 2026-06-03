#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>

/* x86-64 ELF is LP64: int64_t == long, so the 64-bit length modifier is "l". */
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIo64 "lo"

#endif
