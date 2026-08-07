#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>

/* x86-64 ELF is LP64: int64_t == long, so the 64-bit length modifier is "l"
 * and intptr_t/size_t/ptrdiff_t are all 64-bit too. */

typedef struct { intmax_t quot, rem; } imaxdiv_t;

intmax_t  imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t  strtoimax(const char *, char **, int);
uintmax_t strtoumax(const char *, char **, int);
#ifndef __LIBC_WCHAR_T_DEFINED
#define __LIBC_WCHAR_T_DEFINED
typedef __WCHAR_TYPE__ wchar_t;
#endif
intmax_t  wcstoimax(const wchar_t *, wchar_t **, int);
uintmax_t wcstoumax(const wchar_t *, wchar_t **, int);

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIdLEAST8  "d"
#define PRIdLEAST16 "d"
#define PRIdLEAST32 "d"
#define PRIdLEAST64 "ld"
#define PRIdFAST8  "d"
#define PRIdFAST16 "ld"
#define PRIdFAST32 "ld"
#define PRIdFAST64 "ld"
#define PRIdMAX "ld"
#define PRIdPTR "ld"

#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIiMAX "li"
#define PRIiPTR "li"

#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIuLEAST8  "u"
#define PRIuLEAST16 "u"
#define PRIuLEAST32 "u"
#define PRIuLEAST64 "lu"
#define PRIuFAST8  "u"
#define PRIuFAST16 "lu"
#define PRIuFAST32 "lu"
#define PRIuFAST64 "lu"
#define PRIuMAX "lu"
#define PRIuPTR "lu"

#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIoMAX "lo"
#define PRIoPTR "lo"

#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIxMAX "lx"
#define PRIxPTR "lx"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIXMAX "lX"
#define PRIXPTR "lX"

#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNdMAX "ld"
#define SCNdPTR "ld"
#define SCNi8  "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 "li"
#define SCNiMAX "li"
#define SCNiPTR "li"
#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNuMAX "lu"
#define SCNuPTR "lu"
#define SCNo8  "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 "lo"
#define SCNoMAX "lo"
#define SCNoPTR "lo"
#define SCNx8  "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 "lx"
#define SCNxMAX "lx"
#define SCNxPTR "lx"

#endif
