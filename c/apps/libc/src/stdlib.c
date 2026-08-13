#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "libc_internal.h"

/* errno is defined in io.c (the single syscall TU); declared via <errno.h>. */

#define SYS_EXIT 2
static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

int printf(const char *, ...);

/* ---------------------------------------------------------------------- */
/* exit / atexit                                                          */
/* ---------------------------------------------------------------------- */
/* C11 requires at least 32 registrations for each of atexit and
 * at_quick_exit; handlers run in reverse order of registration. */
#define NATEXIT 64
static void (*g_atexit[NATEXIT])(void);
static int g_natexit;
static void (*g_quick[NATEXIT])(void);
static int g_nquick;

int atexit(void (*fn)(void))
{ if (!fn || g_natexit >= NATEXIT) return -1; g_atexit[g_natexit++] = fn; return 0; }
int at_quick_exit(void (*fn)(void))
{ if (!fn || g_nquick >= NATEXIT) return -1; g_quick[g_nquick++] = fn; return 0; }

void exit(int code)
{
    while (g_natexit > 0) g_atexit[--g_natexit]();
    __libc_flush_all();
    sys(SYS_EXIT, code, 0, 0);
    for (;;) {}
}
/* _Exit and quick_exit deliberately skip the atexit handlers and (for _Exit)
 * the stream flush -- that is the whole point of them. */
void _Exit(int code) { sys(SYS_EXIT, code, 0, 0); for (;;) {} }
void quick_exit(int code)
{
    while (g_nquick > 0) g_quick[--g_nquick]();
    sys(SYS_EXIT, code, 0, 0);
    for (;;) {}
}

/* abort(): C says whether streams are flushed is implementation-defined. We do
 * flush, because on this machine the serial log IS the crash report and losing
 * the last printf before an abort makes every abort look identical. The exit
 * status mirrors the shell's 128+SIGABRT convention. */
void abort(void)
{
    __libc_flush_all();
    printf("\n[libc] abort()\n");
    /* Through the signal, now that there is one. POSIX: abort() raises SIGABRT,
     * so a program that installed a handler to dump its own state before dying
     * gets that one last look -- which is the entire reason abort is specified
     * in terms of a signal rather than as an exit. And if the handler RETURNS
     * (or the signal was ignored, or blocked), abort must still terminate, so
     * the exit below is not a fallback, it is the specified behaviour. */
    raise(SIGABRT);
    sys(SYS_EXIT, 134, 0, 0);
    for (;;) {}
}

/* ---------------------------------------------------------------------- */
/* integer helpers                                                        */
/* ---------------------------------------------------------------------- */
int  abs(int x)        { return x < 0 ? (int)-(unsigned)x : x; }   /* unsigned negate: no UB at INT_MIN */
long labs(long x)      { return x < 0 ? (long)-(unsigned long)x : x; }
long long llabs(long long x) { return x < 0 ? (long long)-(unsigned long long)x : x; }
intmax_t imaxabs(intmax_t x) { return x < 0 ? (intmax_t)-(uintmax_t)x : x; }

div_t   div(int n, int d)               { div_t r;   r.quot = n / d; r.rem = n % d; return r; }
ldiv_t  ldiv(long n, long d)            { ldiv_t r;  r.quot = n / d; r.rem = n % d; return r; }
lldiv_t lldiv(long long n, long long d) { lldiv_t r; r.quot = n / d; r.rem = n % d; return r; }
imaxdiv_t imaxdiv(intmax_t n, intmax_t d) { imaxdiv_t r; r.quot = n / d; r.rem = n % d; return r; }

/* ---------------------------------------------------------------------- */
/* environment                                                            */
/* ---------------------------------------------------------------------- */
/* THE INITIAL ENVIRONMENT IS REAL NOW, and the way it got that way is the
 * interesting part.
 *
 * It used to be permanently empty. The kernel has always built a complete SysV
 * stack on execve -- argv, envp and fourteen auxv pairs (c/kernel/exec/exec.c)
 * -- and crt0 read argc and argv off it and threw the envp pointer away. So
 * getenv("HOME"), getenv("PATH") and getenv("TERM") returned NULL in every
 * program on this machine. That gap survived a long time because this file was
 * SELF-CONSISTENT: setenv then getenv worked, so every unit test passed, and
 * only a value that came from OUTSIDE the process could have exposed it.
 *
 * BOTH SYMBOLS ARE DEFINED IN crt0, not here, and that is not a style choice.
 * The same crt0_cli.asm is linked by ~30 coreutils through CLI_RULE, and those
 * programs do not link this library at all (they use c/apps/clib.h inline
 * syscalls). An `extern` in crt0 would be an undefined symbol in every one of
 * them, so the storage has to sit on the crt0 side and the library takes the
 * extern. c/apps/crt0.asm defines the pair too and leaves both NULL, which is
 * the truth for a GUI app: wm_launch gives it no envp at all.
 *
 * `environ` IS SET BY crt0, BEFORE main, on purpose. env_init() below is lazy
 * -- it runs on the first getenv/setenv -- and `for (char **p = environ; *p;
 * p++)` is a completely ordinary thing to write before either of those is
 * called. Leaving `environ` NULL until something happened to touch the
 * environment made that idiom read nothing, which is a worse failure than an
 * empty environment because it looks like the program's own bug. Both vectors
 * are valid and NULL-terminated: crt0's points into the exec-time stack, and
 * env_sync() swaps in the malloc'd copy the moment one exists. */
extern char **environ;               /* defined in crt0, and VALID BEFORE main */
extern char **__libc_environ_hook;   /* defined in crt0; the exec-time envp */

static char **g_env;             /* our own vector (malloc'd) */
static int g_env_n, g_env_cap;
static char *g_env_owned[64];    /* strings we allocated, to free on replace */
static int g_env_owned_n;

static void env_sync(void) { environ = g_env; }

static int env_find(const char *name, size_t nlen)
{
    for (int i = 0; i < g_env_n; i++)
        if (!strncmp(g_env[i], name, nlen) && g_env[i][nlen] == '=') return i;
    return -1;
}

static int env_grow(void)
{
    int cap = g_env_cap ? g_env_cap * 2 : 16;
    char **v = realloc(g_env, (size_t)(cap + 1) * sizeof *v);
    if (!v) { errno = ENOMEM; return -1; }
    g_env = v; g_env_cap = cap; env_sync();
    return 0;
}

/* Adopt a vector crt0 handed us, once, on first use. */
static void env_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    if (!__libc_environ_hook) { if (env_grow() == 0) { g_env[0] = NULL; } return; }
    for (char **p = __libc_environ_hook; *p; p++) {
        if (g_env_n + 1 > g_env_cap && env_grow() < 0) break;
        g_env[g_env_n++] = *p;
    }
    if (g_env_n + 1 > g_env_cap) (void)env_grow();
    if (g_env) g_env[g_env_n] = NULL;
    env_sync();
}

char *getenv(const char *name)
{
    if (!name || !*name || strchr(name, '=')) return NULL;
    env_init();
    if (!g_env) return NULL;
    size_t n = strlen(name);
    int i = env_find(name, n);
    return i < 0 ? NULL : g_env[i] + n + 1;
}

int setenv(const char *name, const char *value, int overwrite)
{
    env_init();
    if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
    size_t nl = strlen(name), vl = strlen(value);
    int i = env_find(name, nl);
    if (i >= 0 && !overwrite) return 0;
    char *s = malloc(nl + vl + 2);
    if (!s) { errno = ENOMEM; return -1; }
    memcpy(s, name, nl); s[nl] = '=';
    memcpy(s + nl + 1, value, vl + 1);
    if (i >= 0) { g_env[i] = s; }
    else {
        if (g_env_n + 1 > g_env_cap && env_grow() < 0) { free(s); return -1; }
        g_env[g_env_n++] = s; g_env[g_env_n] = NULL;
    }
    if (g_env_owned_n < (int)(sizeof g_env_owned / sizeof *g_env_owned))
        g_env_owned[g_env_owned_n++] = s;
    env_sync();
    return 0;
}

int unsetenv(const char *name)
{
    env_init();
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    size_t nl = strlen(name);
    int i;
    while ((i = env_find(name, nl)) >= 0) {
        for (int k = i; k < g_env_n; k++) g_env[k] = g_env[k + 1];
        g_env_n--;
    }
    if (g_env) g_env[g_env_n] = NULL;
    return 0;
}

/* putenv takes ownership of the caller's string, by contract. */
int putenv(char *s)
{
    env_init();
    if (!s) { errno = EINVAL; return -1; }
    char *eq = strchr(s, '=');
    if (!eq) return unsetenv(s);
    size_t nl = (size_t)(eq - s);
    int i = env_find(s, nl);
    if (i >= 0) { g_env[i] = s; env_sync(); return 0; }
    if (g_env_n + 1 > g_env_cap && env_grow() < 0) return -1;
    g_env[g_env_n++] = s; g_env[g_env_n] = NULL;
    env_sync();
    return 0;
}

int clearenv(void) { g_env_n = 0; if (g_env) g_env[0] = NULL; env_sync(); return 0; }

/* ---------------------------------------------------------------------- */
/* pseudo-random                                                          */
/* ---------------------------------------------------------------------- */
static unsigned long long rng = 1;
static int rand_step(unsigned long long *st)
{
    *st = *st * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((*st >> 33) & 0x7fffffff);
}
int  rand(void)          { return rand_step(&rng); }
void srand(unsigned s)   { rng = s; }
int  rand_r(unsigned *s) { unsigned long long t = *s; int r = rand_step(&t); *s = (unsigned)t; return r; }
long random(void)        { return rand_step(&rng); }
void srandom(unsigned s) { rng = s; }

/* ---------------------------------------------------------------------- */
/* string -> number                                                       */
/* ---------------------------------------------------------------------- */
static int isspace_(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int digit(int c, int base)
{
    int v = (c >= '0' && c <= '9') ? c - '0'
          : (c >= 'a' && c <= 'z') ? c - 'a' + 10
          : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
    return v < base ? v : -1;
}

/* One parser for the whole strto[u]l[l] family.
 *
 * The old one got two edge cases wrong, both of which real code hits: a base-16
 * "0x" with no hex digit after it, and a base-0 "08". C says the subject
 * sequence is the LONGEST INITIAL SUBSEQUENCE that is of the expected form --
 * for both of those the answer is the single character "0", with endptr on the
 * character after it. The old code decided nothing had been converted and
 * pointed endptr back at the start, so a caller stepping through "08:30" with
 * strtol looped forever. */
static unsigned long long strto_uint(const char *s, char **end, int base,
                                     int *negp, int *ovfp)
{
    const char *p = s;
    int neg = 0, any = 0, ovf = 0, d;
    unsigned long long acc = 0;

    *negp = 0; *ovfp = 0;
    if (base != 0 && (base < 2 || base > 36)) { errno = EINVAL; if (end) *end = (char *)s; return 0; }

    while (isspace_((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit((unsigned char)p[2], 16) >= 0) {
        p += 2; base = 16;
    } else if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')
               && digit((unsigned char)p[2], 2) >= 0) {
        /* C23 added the 0b/0B prefix for bases 0 and 2; current glibc accepts
         * it, so code being ported already relies on it. */
        p += 2; base = 2;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    unsigned long long cutoff = ~0ULL / (unsigned)base;
    int cutlim = (int)(~0ULL % (unsigned)base);
    while ((d = digit((unsigned char)*p, base)) >= 0) {
        if (ovf || acc > cutoff || (acc == cutoff && d > cutlim)) ovf = 1;
        else acc = acc * (unsigned)base + (unsigned)d;
        p++; any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    *negp = neg; *ovfp = ovf;
    return acc;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, ovf;
    unsigned long long v = strto_uint(s, end, base, &neg, &ovf);
    unsigned long long lim = neg ? (unsigned long long)LLONG_MAX + 1ULL
                                 : (unsigned long long)LLONG_MAX;
    if (ovf || v > lim) { errno = ERANGE; return neg ? LLONG_MIN : LLONG_MAX; }
    return neg ? (long long)(~v + 1ULL) : (long long)v;
}
unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, ovf;
    unsigned long long v = strto_uint(s, end, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? ~v + 1ULL : v;          /* C: a leading '-' negates mod 2^64 */
}
/* x86-64 is LP64: long == long long, so no second range clamp is needed. If
 * this library is ever built for a 32-bit target these need real clamping. */
long strtol(const char *s, char **e, int b)            { return (long)strtoll(s, e, b); }
unsigned long strtoul(const char *s, char **e, int b)  { return (unsigned long)strtoull(s, e, b); }
intmax_t  strtoimax(const char *s, char **e, int b)    { return strtoll(s, e, b); }
uintmax_t strtoumax(const char *s, char **e, int b)    { return strtoull(s, e, b); }

int  atoi(const char *s)  { return (int)strtoll(s, NULL, 10); }
long atol(const char *s)  { return (long)strtoll(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

double strtod(const char *s, char **end) { return __libc_strtox(s, end, 64); }
float  strtof(const char *s, char **end) { return (float)__libc_strtox(s, end, 32); }
/* DEGENERATE CASE: long double is x87 80-bit on this target, but every value
 * here is computed at double precision, so strtold has 53 bits of mantissa, not
 * 64. It is correctly rounded to double and then widened -- never wrong by more
 * than a double's ulp, and never silently a different answer than strtod. */
long double strtold(const char *s, char **end) { return (long double)__libc_strtox(s, end, 64); }
double atof(const char *s) { return strtod(s, NULL); }

/* ---------------------------------------------------------------------- */
/* search / sort                                                          */
/* ---------------------------------------------------------------------- */
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *))
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *p = (const char *)base + mid * sz;
        int c = cmp(key, p);
        if (c < 0) hi = mid; else if (c > 0) lo = mid + 1; else return (void *)p;
    }
    return NULL;
}

/* qsort is introsort, not plain quicksort.
 *
 * The comparator is the caller's code -- for the browser it is a JavaScript
 * function reached through QuickJS's Array.prototype.sort. A median-of-one
 * quicksort with an attacker-chosen comparator is O(n^2), which on a page's
 * data is a hang rather than a slow sort. Median-of-three picks a sane pivot
 * for the common near-sorted input; a recursion-depth cap of 2*log2(n) then
 * bounds the pathological case by switching that subrange to heapsort, which is
 * O(n log n) no matter what the comparator does. */
typedef int (*cmp_r_fn)(const void *, const void *, void *);

static void swap_bytes(char *a, char *b, size_t n)
{ while (n--) { char t = *a; *a++ = *b; *b++ = t; } }

static void sift(char *base, size_t n, size_t sz, size_t root, cmp_r_fn cmp, void *arg)
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && cmp(base + child * sz, base + (child + 1) * sz, arg) < 0) child++;
        if (cmp(base + root * sz, base + child * sz, arg) >= 0) return;
        swap_bytes(base + root * sz, base + child * sz, sz);
        root = child;
    }
}
static void heapsort_r(char *base, size_t n, size_t sz, cmp_r_fn cmp, void *arg)
{
    if (n < 2) return;
    for (size_t i = n / 2; i-- > 0; ) sift(base, n, sz, i, cmp, arg);
    for (size_t i = n; i-- > 1; ) { swap_bytes(base, base + i * sz, sz); sift(base, i, sz, 0, cmp, arg); }
}

static void med3(char *base, size_t n, size_t sz, cmp_r_fn cmp, void *arg)
{
    char *a = base, *b = base + (n / 2) * sz, *c = base + (n - 1) * sz;
    if (cmp(b, a, arg) < 0) swap_bytes(a, b, sz);
    if (cmp(c, b, arg) < 0) { swap_bytes(b, c, sz); if (cmp(b, a, arg) < 0) swap_bytes(a, b, sz); }
    swap_bytes(b, base + (n - 1) * sz, sz);        /* pivot to the end */
}

static void intro(char *base, size_t n, size_t sz, cmp_r_fn cmp, void *arg, int depth)
{
    while (n > 1) {
        if (n < 12) {
            for (size_t i = 1; i < n; i++)
                for (size_t j = i; j > 0 && cmp(base + j * sz, base + (j - 1) * sz, arg) < 0; j--)
                    swap_bytes(base + j * sz, base + (j - 1) * sz, sz);
            return;
        }
        if (depth-- <= 0) { heapsort_r(base, n, sz, cmp, arg); return; }
        med3(base, n, sz, cmp, arg);
        char *pivot = base + (n - 1) * sz;
        size_t i = 0;
        for (size_t j = 0; j < n - 1; j++)
            if (cmp(base + j * sz, pivot, arg) < 0) { swap_bytes(base + j * sz, base + i * sz, sz); i++; }
        swap_bytes(base + i * sz, pivot, sz);
        size_t left = i, right = n - i - 1;
        /* Recurse on the smaller side, loop on the larger: O(log n) stack. */
        if (left < right) { intro(base, left, sz, cmp, arg, depth); base += (i + 1) * sz; n = right; }
        else { intro(base + (i + 1) * sz, right, sz, cmp, arg, depth); n = left; }
    }
}

static int depth_limit(size_t n) { int d = 0; while (n > 1) { n >>= 1; d++; } return 2 * d + 2; }

void qsort_r(void *base, size_t n, size_t sz, cmp_r_fn cmp, void *arg)
{ if (n > 1 && sz) intro((char *)base, n, sz, cmp, arg, depth_limit(n)); }

static int cmp_adapt(const void *a, const void *b, void *arg)
{ return ((int (*)(const void *, const void *))arg)(a, b); }

void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{ if (n > 1 && sz) intro((char *)base, n, sz, cmp_adapt, (void *)cmp, depth_limit(n)); }

/* ---------------------------------------------------------------------- */
/* allocation shims layered on malloc.c                                   */
/* ---------------------------------------------------------------------- */
/* aligned_alloc / posix_memalign, on an allocator with no alignment API.
 *
 * malloc.c guarantees 16 bytes (the arena is __attribute__((aligned(16))), the
 * block header is 16 bytes and every payload size is align16'd), so requests up
 * to 16 are satisfied by plain malloc -- and the pointer returned is malloc's
 * OWN pointer, which is the part that matters.
 *
 * Stronger alignments are REFUSED, deliberately. The usual trick -- over-
 * allocate and hand back an aligned pointer inside the block -- cannot work
 * here: free() identifies a block by the header immediately below the pointer
 * it is given, validated by tag and checksum, and a pointer it does not
 * recognise is a deliberate NO-OP. An interior pointer would therefore not
 * crash; it would leak, silently, forever. An honest EINVAL that the caller can
 * branch on beats a leak nobody can see. */
#define MALLOC_ALIGN 16u

void *aligned_alloc(size_t align, size_t size)
{
    if (align == 0 || (align & (align - 1)) || (size % align) != 0) { errno = EINVAL; return NULL; }
    if (align > MALLOC_ALIGN) { errno = EINVAL; return NULL; }
    return malloc(size);
}
int posix_memalign(void **out, size_t align, size_t size)
{
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    if (align > MALLOC_ALIGN) return EINVAL;
    void *p = malloc(size ? size : 1);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* system() -- a real one: LogitOS has fork/execve and /bin/sh             */
/* ---------------------------------------------------------------------- */
int system(const char *cmd)
{
    if (!cmd) { int fd = open("/bin/sh", O_RDONLY); if (fd < 0) return 0; close(fd); return 1; }
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *argv[4];
        argv[0] = (char *)"sh"; argv[1] = (char *)"-c"; argv[2] = (char *)cmd; argv[3] = NULL;
        execv("/bin/sh", argv);
        _Exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return status;
}

/* ---------------------------------------------------------------------- */
/* temporary names                                                        */
/* ---------------------------------------------------------------------- */
/* LogitOS has no O_EXCL and no getpid-unique namespace guarantees beyond the
 * pid itself, so this is "unique enough for one machine", not a security
 * primitive: never use mkstemp here to defend against a hostile local user,
 * because there is no hostile local user and no protection if there were. */
static unsigned tmp_seq;
char *mktemp(char *tmpl)
{
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) { errno = EINVAL; if (n) tmpl[0] = 0; return tmpl; }
    unsigned v = (unsigned)getpid() * 2654435761u + (++tmp_seq) * 40503u;
    for (int i = 0; i < 6; i++) { tmpl[n - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[v % 36]; v /= 36; v += 7; }
    return tmpl;
}
int mkstemp(char *tmpl)
{
    for (int att = 0; att < 64; att++) {
        char save[7];
        size_t n = strlen(tmpl);
        if (n < 6) { errno = EINVAL; return -1; }
        memcpy(save, tmpl + n - 6, 7);
        if (att) memcpy(tmpl + n - 6, "XXXXXX", 6);
        mktemp(tmpl);
        if (!tmpl[0]) { memcpy(tmpl + n - 6, save, 7); return -1; }
        int fd = open(tmpl, O_RDWR | O_CREAT | O_TRUNC);
        if (fd >= 0) return fd;
    }
    errno = EEXIST;
    return -1;
}
