#include <stddef.h>

/* errno is defined in io.c (the single syscall TU); declared via <errno.h>. */

#define SYS_EXIT 2
static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

int printf(const char *, ...);

void exit(int code)  { sys(SYS_EXIT, code, 0, 0); for (;;) {} }
void _Exit(int code) { sys(SYS_EXIT, code, 0, 0); for (;;) {} }
void abort(void)     { printf("\n[libc] abort()\n"); sys(SYS_EXIT, 134, 0, 0); for (;;) {} }

int  abs(int x)        { return x < 0 ? -x : x; }
long labs(long x)      { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

char *getenv(const char *n) { (void)n; return NULL; }

static unsigned long rng = 0x2545F4914F6CDD1DUL;
int  rand(void)        { rng = rng * 6364136223846793005UL + 1442695040888963407UL; return (int)((rng >> 33) & 0x7fffffff); }
void srand(unsigned s) { rng = s; }

static int isspace_(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int digit(int c, int base)
{ int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99; return v < base ? v : -1; }

long long strtoll(const char *s, char **end, int base)
{
    while (isspace_(*s)) s++;
    int neg = 0; if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = (s[0] == '0') ? 8 : 10;
    long long v = 0; int any = 0, d;
    while ((d = digit(*s, base)) >= 0) { v = v * base + d; s++; any = 1; }
    if (end) *end = (char *)(any ? s : s);
    return neg ? -v : v;
}
unsigned long long strtoull(const char *s, char **end, int base)
{ return (unsigned long long)strtoll(s, end, base); }
long strtol(const char *s, char **e, int b) { return (long)strtoll(s, e, b); }
unsigned long strtoul(const char *s, char **e, int b) { return (unsigned long)strtoll(s, e, b); }
int  atoi(const char *s)  { return (int)strtoll(s, NULL, 10); }
long atol(const char *s)  { return (long)strtoll(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* 10^e by exponentiation-by-squaring -- keeps mini-libc free of a libm dependency. */
static double pow10i(int e)
{
    int neg = e < 0; if (neg) e = -e;
    double r = 1.0, b = 10.0;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return neg ? 1.0 / r : r;
}
double strtod(const char *s, char **end)
{
    while (isspace_(*s)) s++;
    int neg = 0; if (*s == '+' || *s == '-') neg = (*s++ == '-');
    double v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    if (*s == '.') { s++; double f = 0.1; while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f *= 0.1; any = 1; } }
    if (any && (*s == 'e' || *s == 'E')) {
        s++; int eneg = 0; if (*s == '+' || *s == '-') eneg = (*s++ == '-');
        int e = 0; while (*s >= '0' && *s <= '9') e = e * 10 + (*s++ - '0');
        v *= pow10i(eneg ? -e : e);
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
float  strtof(const char *s, char **e) { return (float)strtod(s, e); }
double atof(const char *s) { return strtod(s, NULL); }

void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *))
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const void *p = (const char *)base + mid * sz;
        int c = cmp(key, p);
        if (c < 0) hi = mid; else if (c > 0) lo = mid + 1; else return (void *)p;
    }
    return NULL;
}

void *memcpy(void *, const void *, size_t);
static void swap_bytes(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
static void qsort_r(char *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    while (n > 1) {
        if (n < 12) {  /* insertion sort for small ranges */
            for (size_t i = 1; i < n; i++)
                for (size_t j = i; j > 0 && cmp(base + j * sz, base + (j - 1) * sz) < 0; j--)
                    swap_bytes(base + j * sz, base + (j - 1) * sz, sz);
            return;
        }
        char *pivot = base + (n / 2) * sz;
        swap_bytes(pivot, base + (n - 1) * sz, sz);
        pivot = base + (n - 1) * sz;
        size_t i = 0;
        for (size_t j = 0; j < n - 1; j++)
            if (cmp(base + j * sz, pivot) < 0) { swap_bytes(base + j * sz, base + i * sz, sz); i++; }
        swap_bytes(base + i * sz, pivot, sz);
        size_t left = i, right = n - i - 1;
        if (left < right) { qsort_r(base, left, sz, cmp); base += (i + 1) * sz; n = right; }
        else { qsort_r(base + (i + 1) * sz, right, sz, cmp); n = left; }
    }
}
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{ if (n > 1 && sz) qsort_r((char *)base, n, sz, cmp); }
