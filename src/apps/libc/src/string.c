#include <stddef.h>
#include <stdint.h>

static int lc_(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int strcasecmp(const char *a, const char *b)
{ while (*a && lc_((unsigned char)*a) == lc_((unsigned char)*b)) { a++; b++; }
  return lc_((unsigned char)*a) - lc_((unsigned char)*b); }
int strncasecmp(const char *a, const char *b, size_t n)
{ for (; n; n--, a++, b++) { int d = lc_((unsigned char)*a) - lc_((unsigned char)*b); if (d || !*a) return d; } return 0; }

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    /* Word-at-a-time bulk copy only when both ends share alignment. */
    if (n >= sizeof(uint64_t) &&
        (((uintptr_t)dd ^ (uintptr_t)ss) & (sizeof(uint64_t) - 1)) == 0) {
        while (((uintptr_t)dd & (sizeof(uint64_t) - 1)) && n) { *dd++ = *ss++; n--; }
        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)dd = *(const uint64_t *)ss;
            dd += sizeof(uint64_t); ss += sizeof(uint64_t); n -= sizeof(uint64_t);
        }
    }
    while (n--) *dd++ = *ss++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) {
        /* Forward copy: word-at-a-time when alignment matches. */
        if (n >= sizeof(uint64_t) &&
            (((uintptr_t)dd ^ (uintptr_t)ss) & (sizeof(uint64_t) - 1)) == 0) {
            while (((uintptr_t)dd & (sizeof(uint64_t) - 1)) && n) { *dd++ = *ss++; n--; }
            while (n >= sizeof(uint64_t)) {
                *(uint64_t *)dd = *(const uint64_t *)ss;
                dd += sizeof(uint64_t); ss += sizeof(uint64_t); n -= sizeof(uint64_t);
            }
        }
        while (n--) *dd++ = *ss++;
    }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    unsigned char *dd = d;
    if (n >= sizeof(uint64_t)) {
        uint64_t w = (uint64_t)(unsigned char)c * 0x0101010101010101ULL;
        while (((uintptr_t)dd & (sizeof(uint64_t) - 1)) && n) { *dd++ = (unsigned char)c; n--; }
        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)dd = w;
            dd += sizeof(uint64_t); n -= sizeof(uint64_t);
        }
    }
    while (n--) *dd++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t m) { size_t i = 0; while (i < m && s[i]) i++; return i; }
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{ for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) break; } return 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n)
{ char *r = d; while (n && (*d = *s)) { d++; s++; n--; } while (n--) *d++ = 0; return r; }
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) ; return r; }
char *strncat(char *d, const char *s, size_t n)
{ char *r = d; while (*d) d++; while (n-- && *s) *d++ = *s++; *d = 0; return r; }
char *strchr(const char *s, int c)
{ for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strrchr(const char *s, int c)
{ const char *last = NULL; do { if (*s == (char)c) last = s; } while (*s++); return (char *)last; }
char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}
size_t strspn(const char *s, const char *set)
{ size_t i = 0; for (; s[i]; i++) { const char *p = set; while (*p && *p != s[i]) p++; if (!*p) break; } return i; }
size_t strcspn(const char *s, const char *set)
{ size_t i = 0; for (; s[i]; i++) { const char *p = set; while (*p && *p != s[i]) p++; if (*p) break; } return i; }
char *strpbrk(const char *s, const char *set)
{ s += strcspn(s, set); return *s ? (char *)s : NULL; }

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s) s = *save;
    s += strspn(s, delim);            /* skip leading delimiters */
    if (!*s) { *save = s; return NULL; }
    char *tok = s;
    s += strcspn(s, delim);           /* run to next delimiter */
    if (*s) { *s = 0; *save = s + 1; }
    else      *save = s;
    return tok;
}
char *strtok(char *s, const char *delim)
{ static char *save; return strtok_r(s, delim, &save); }

void *memmem(const void *hay, size_t hn, const void *ndl, size_t nn)
{
    const unsigned char *h = hay, *n = ndl;
    if (nn == 0) return (void *)hay;
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nn) == 0) return (void *)(h + i);
    return NULL;
}

void *malloc(size_t);
char *strdup(const char *s)
{ size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strndup(const char *s, size_t m)
{ size_t n = strnlen(s, m); char *p = malloc(n + 1); if (p) { memcpy(p, s, n); p[n] = 0; } return p; }
char *strerror(int e) { (void)e; return "error"; }
