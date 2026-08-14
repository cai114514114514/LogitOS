#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>

#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef long ssize_t;
#endif

#define alloca(n) __builtin_alloca(n)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
#define MB_CUR_MAX ((size_t)4)      /* UTF-8: see the note in wchar.c */

/* --- allocation ---------------------------------------------------------
 * These four declarations are shared with c/apps/libc/src/malloc.c, which is
 * owned separately; do not change their shape here without changing it there. */
void  *malloc(size_t);
void   free(void *);
void  *realloc(void *, size_t);
void  *calloc(size_t, size_t);
size_t malloc_usable_size(void *);
void  *aligned_alloc(size_t, size_t);
int    posix_memalign(void **, size_t, size_t);

/* --- process control ---------------------------------------------------- */
void   abort(void);
void   exit(int);
void   _Exit(int);
void   quick_exit(int);
int    atexit(void (*)(void));
int    at_quick_exit(void (*)(void));
int    system(const char *);

/* --- integer arithmetic ------------------------------------------------- */
int    abs(int);
long   labs(long);
long long llabs(long long);

typedef struct { int quot, rem; }             div_t;
typedef struct { long quot, rem; }            ldiv_t;
typedef struct { long long quot, rem; }       lldiv_t;
div_t   div(int, int);
ldiv_t  ldiv(long, long);
lldiv_t lldiv(long long, long long);

/* --- string conversion -------------------------------------------------- */
int    atoi(const char *);
long   atol(const char *);
long long atoll(const char *);
double atof(const char *);
double strtod(const char *, char **);
float  strtof(const char *, char **);
long double strtold(const char *, char **);
long   strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);

/* --- searching and sorting ---------------------------------------------- */
void   qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void   qsort_r(void *, size_t, size_t, int (*)(const void *, const void *, void *), void *);
void  *bsearch(const void *, const void *, size_t, size_t,
               int (*)(const void *, const void *));

/* --- pseudo-random ------------------------------------------------------ */
int    rand(void);
void   srand(unsigned);
int    rand_r(unsigned *);
long   random(void);
void   srandom(unsigned);

/* --- environment -------------------------------------------------------- */
extern char **environ;
char  *getenv(const char *);
int    setenv(const char *, const char *, int);
int    unsetenv(const char *);
int    putenv(char *);
int    clearenv(void);

/* --- multibyte / wide (implemented in wchar.c) -------------------------- */
#ifndef __LIBC_WCHAR_T_DEFINED
#define __LIBC_WCHAR_T_DEFINED
typedef __WCHAR_TYPE__ wchar_t;
#endif
int    mblen(const char *, size_t);
int    mbtowc(wchar_t *, const char *, size_t);
int    wctomb(char *, wchar_t);
size_t mbstowcs(wchar_t *, const char *, size_t);
size_t wcstombs(char *, const wchar_t *, size_t);

/* --- temporary names ---------------------------------------------------- */
char  *mktemp(char *);
int    mkstemp(char *);
char  *mkdtemp(char *);       /* c/apps/libc/src/pathx.c */

/* --- path canonicalisation (c/apps/libc/src/pathx.c) --------------------- */
char  *realpath(const char *restrict, char *restrict);

/* --- allocation (c/apps/libc/src/pathx.c; malloc.c owns the other four) -- */
void  *reallocarray(void *, size_t, size_t);

/* --- suboption parsing (c/apps/libc/src/pathx.c) -------------------------
 * getsubopt() is declared here rather than in a header of its own because
 * that is where glibc puts it (<stdlib.h>, not <getopt.h>) and every ported
 * program that uses it #includes <stdlib.h> expecting to find it. */
int    getsubopt(char **, char *const *, char **);

#endif
