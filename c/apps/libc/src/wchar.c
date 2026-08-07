/* Wide characters and the multibyte conversions.
 *
 * wchar_t is UTF-32; the "C" locale's multibyte encoding is UTF-8. That second
 * choice is the one deviation from glibc's C locale and it is deliberate --
 * LogitOS is UTF-8 from the font subsystem to the filesystem (M14), so an
 * ASCII-only mbstowcs would be the one component unable to read the text every
 * other component produces. The decoder is a strict one: overlong forms, UTF-16
 * surrogates and anything above U+10FFFF are rejected with EILSEQ, because a
 * lenient UTF-8 decoder in a browser is a security bug, not a convenience. */
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include "libc_internal.h"

/* ---------------------------------------------------------------------- */
/* UTF-8                                                                   */
/* ---------------------------------------------------------------------- */
#define MB_INVALID ((size_t)-1)
#define MB_INCOMPLETE ((size_t)-2)

int mbsinit(const mbstate_t *st) { return !st || st->__have == 0; }

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *st)
{
    static mbstate_t internal;
    if (!st) st = &internal;
    if (!s) { s = ""; n = 1; pwc = NULL; st->__have = st->__want = 0; }

    unsigned int wc = st->__have ? st->__wch : 0;
    unsigned char have = st->__have, want = st->__want;
    size_t used = 0;

    if (!have) {
        if (n == 0) return MB_INCOMPLETE;
        unsigned char c = (unsigned char)s[0];
        used = 1;
        if (c < 0x80) { if (pwc) *pwc = (wchar_t)c; st->__have = st->__want = 0; return c ? 1 : 0; }
        if (c < 0xc2) { errno = EILSEQ; return MB_INVALID; }   /* continuation or overlong lead */
        if (c < 0xe0) { wc = c & 0x1f; want = 2; }
        else if (c < 0xf0) { wc = c & 0x0f; want = 3; }
        else if (c < 0xf5) { wc = c & 0x07; want = 4; }
        else { errno = EILSEQ; return MB_INVALID; }
        have = 1;
    }

    while (have < want) {
        if (used >= n) {
            st->__wch = wc; st->__have = have; st->__want = want;
            return MB_INCOMPLETE;
        }
        unsigned char c = (unsigned char)s[used];
        if ((c & 0xc0) != 0x80) { errno = EILSEQ; return MB_INVALID; }
        /* Reject overlong forms and out-of-range code points at the second
         * byte, where the range is still decidable. */
        if (have == 1) {
            if (want == 3 && wc == 0 && c < 0xa0) { errno = EILSEQ; return MB_INVALID; }
            if (want == 4 && wc == 0 && c < 0x90) { errno = EILSEQ; return MB_INVALID; }
            if (want == 4 && wc == 4 && c >= 0x90) { errno = EILSEQ; return MB_INVALID; }
        }
        wc = (wc << 6) | (c & 0x3f);
        have++; used++;
    }
    if (wc >= 0xd800 && wc <= 0xdfff) { errno = EILSEQ; return MB_INVALID; }  /* surrogate */
    if (wc > 0x10ffff) { errno = EILSEQ; return MB_INVALID; }
    st->__have = st->__want = 0; st->__wch = 0;
    if (pwc) *pwc = (wchar_t)wc;
    return wc ? used : 0;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *st)
{ static mbstate_t internal; return mbrtowc(NULL, s, n, st ? st : &internal); }

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *st)
{
    (void)st;
    char tmp[4];
    if (!s) { s = tmp; wc = 0; }
    unsigned int c = (unsigned int)wc;
    if (c < 0x80) { s[0] = (char)c; return 1; }
    if (c < 0x800) { s[0] = (char)(0xc0 | (c >> 6)); s[1] = (char)(0x80 | (c & 0x3f)); return 2; }
    if (c >= 0xd800 && c <= 0xdfff) { errno = EILSEQ; return MB_INVALID; }
    if (c < 0x10000) {
        s[0] = (char)(0xe0 | (c >> 12));
        s[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        s[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    if (c <= 0x10ffff) {
        s[0] = (char)(0xf0 | (c >> 18));
        s[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        s[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        s[3] = (char)(0x80 | (c & 0x3f));
        return 4;
    }
    errno = EILSEQ;
    return MB_INVALID;
}

wint_t btowc(int c)
{
    if (c == EOF) return WEOF;
    unsigned char b = (unsigned char)c;
    return b < 0x80 ? (wint_t)b : WEOF;      /* a lone high byte is not a character */
}
int wctob(wint_t c) { return (c != WEOF && (unsigned)c < 0x80) ? (int)c : EOF; }

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *st)
{
    static mbstate_t internal;
    if (!st) st = &internal;
    const char *s = *src;
    size_t n = 0;
    for (;;) {
        if (dst && n >= len) break;
        wchar_t wc;
        size_t k = mbrtowc(&wc, s, 4, st);
        if (k == MB_INVALID || k == MB_INCOMPLETE) { if (k == MB_INCOMPLETE) errno = EILSEQ; if (dst) *src = s; return MB_INVALID; }
        if (k == 0) { if (dst) { dst[n] = 0; *src = NULL; } return n; }
        if (dst) dst[n] = wc;
        s += k; n++;
    }
    *src = s;
    return n;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *st)
{
    (void)st;
    const wchar_t *w = *src;
    size_t n = 0;
    char buf[4];
    for (;;) {
        if (*w == 0) { if (dst) { if (n < len) dst[n] = 0; *src = NULL; } return n; }
        size_t k = wcrtomb(buf, *w, NULL);
        if (k == MB_INVALID) { if (dst) *src = w; return MB_INVALID; }
        if (dst) { if (n + k > len) break; memcpy(dst + n, buf, k); }
        n += k; w++;
    }
    *src = w;
    return n;
}

/* The non-restartable C89 forms. They carry their own hidden state, which for
 * UTF-8 means: mbtowc(NULL,...) resets it and reports that the encoding IS
 * state-dependent-free (returns 0), which is the correct answer for UTF-8. */
int mblen(const char *s, size_t n)
{
    static mbstate_t st;
    if (!s) { memset(&st, 0, sizeof st); return 0; }
    size_t k = mbrtowc(NULL, s, n, &st);
    if (k == MB_INVALID || k == MB_INCOMPLETE) { errno = EILSEQ; return -1; }
    return (int)k;
}
int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    static mbstate_t st;
    if (!s) { memset(&st, 0, sizeof st); return 0; }
    size_t k = mbrtowc(pwc, s, n, &st);
    if (k == MB_INVALID || k == MB_INCOMPLETE) { errno = EILSEQ; return -1; }
    return (int)k;
}
int wctomb(char *s, wchar_t wc)
{
    if (!s) return 0;
    size_t k = wcrtomb(s, wc, NULL);
    return k == MB_INVALID ? -1 : (int)k;
}
size_t mbstowcs(wchar_t *dst, const char *src, size_t len)
{ const char *p = src; mbstate_t st; memset(&st, 0, sizeof st); return mbsrtowcs(dst, &p, len, &st); }
size_t wcstombs(char *dst, const wchar_t *src, size_t len)
{ const wchar_t *p = src; mbstate_t st; memset(&st, 0, sizeof st); return wcsrtombs(dst, &p, len, &st); }

/* ---------------------------------------------------------------------- */
/* wide strings                                                            */
/* ---------------------------------------------------------------------- */
size_t wcslen(const wchar_t *s) { const wchar_t *p = s; while (*p) p++; return (size_t)(p - s); }
size_t wcsnlen(const wchar_t *s, size_t m) { size_t i = 0; while (i < m && s[i]) i++; return i; }
wchar_t *wcscpy(wchar_t *d, const wchar_t *s) { wchar_t *r = d; while ((*d++ = *s++)) ; return r; }
wchar_t *wcpcpy(wchar_t *d, const wchar_t *s) { while ((*d = *s)) { d++; s++; } return d; }
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{ wchar_t *r = d; while (n && (*d = *s)) { d++; s++; n--; } while (n--) *d++ = 0; return r; }
wchar_t *wcscat(wchar_t *d, const wchar_t *s) { wchar_t *r = d; while (*d) d++; while ((*d++ = *s++)) ; return r; }
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{ wchar_t *r = d; while (*d) d++; while (n-- && *s) *d++ = *s++; *d = 0; return r; }

int wcscmp(const wchar_t *a, const wchar_t *b)
{ while (*a && *a == *b) { a++; b++; } return (*a > *b) - (*a < *b); }
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{ for (; n; n--, a++, b++) { if (*a != *b) return (*a > *b) - (*a < *b); if (!*a) break; } return 0; }
static wchar_t wlc(wchar_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int wcscasecmp(const wchar_t *a, const wchar_t *b)
{ while (*a && wlc(*a) == wlc(*b)) { a++; b++; } return (wlc(*a) > wlc(*b)) - (wlc(*a) < wlc(*b)); }
int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{ for (; n; n--, a++, b++) { wchar_t x = wlc(*a), y = wlc(*b); if (x != y) return (x > y) - (x < y); if (!*a) break; } return 0; }
/* C locale: collation order is code-point order (see locale.c). */
int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n)
{ size_t l = wcslen(s); if (n) { size_t k = l < n - 1 ? l : n - 1; wmemcpy(d, s, k); d[k] = 0; } return l; }

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{ for (;; s++) { if (*s == c) return (wchar_t *)s; if (!*s) return NULL; } }
wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{ const wchar_t *last = NULL; do { if (*s == c) last = s; } while (*s++); return (wchar_t *)last; }
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n)
{
    if (!*n) return (wchar_t *)h;
    for (; *h; h++) {
        const wchar_t *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (wchar_t *)h;
    }
    return NULL;
}
size_t wcsspn(const wchar_t *s, const wchar_t *set)
{ size_t i = 0; for (; s[i]; i++) { const wchar_t *p = set; while (*p && *p != s[i]) p++; if (!*p) break; } return i; }
size_t wcscspn(const wchar_t *s, const wchar_t *set)
{ size_t i = 0; for (; s[i]; i++) { const wchar_t *p = set; while (*p && *p != s[i]) p++; if (*p) break; } return i; }
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set)
{ s += wcscspn(s, set); return *s ? (wchar_t *)s : NULL; }
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save)
{
    if (!s) s = *save;
    if (!s) return NULL;
    s += wcsspn(s, delim);
    if (!*s) { *save = s; return NULL; }
    wchar_t *tok = s;
    s += wcscspn(s, delim);
    if (*s) { *s = 0; *save = s + 1; } else *save = s;
    return tok;
}
wchar_t *wcsdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *p = malloc(n * sizeof *p);
    if (p) wmemcpy(p, s, n); else errno = ENOMEM;
    return p;
}

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n) { return memcpy(d, s, n * sizeof *d); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return memmove(d, s, n * sizeof *d); }
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{ for (; n; n--, a++, b++) if (*a != *b) return (*a > *b) - (*a < *b); return 0; }
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{ for (; n; n--, s++) if (*s == c) return (wchar_t *)s; return NULL; }

/* ---------------------------------------------------------------------- */
/* wide -> number                                                          */
/* ---------------------------------------------------------------------- */
/* A number's characters are all ASCII, so the wide input is narrowed into a
 * bounded buffer, parsed by the one (correctly rounded) narrow parser, and the
 * endptr offset mapped back one-for-one. The buffer bounds the significant
 * digits at 1024, which is past the 767 that can affect a double -- and past
 * the point where any integer conversion is anything but ERANGE. */
#define WNUM 1088
static size_t narrow_prefix(const wchar_t *s, char *buf)
{
    size_t i = 0;
    while (i < WNUM - 1 && s[i] && (unsigned)s[i] < 128) { buf[i] = (char)s[i]; i++; }
    buf[i] = 0;
    return i;
}
#define WSTRTO(name, type, call)                                              \
    type name(const wchar_t *s, wchar_t **end)                                \
    {                                                                         \
        char buf[WNUM]; char *e;                                              \
        narrow_prefix(s, buf);                                                \
        type v = call;                                                        \
        if (end) *end = (wchar_t *)s + (e - buf);                             \
        return v;                                                             \
    }
WSTRTO(wcstod,  double,      __libc_strtox(buf, &e, 64))
WSTRTO(wcstof,  float,       (float)__libc_strtox(buf, &e, 32))
WSTRTO(wcstold, long double, (long double)__libc_strtox(buf, &e, 64))

#define WSTRTOI(name, type, call)                                             \
    type name(const wchar_t *s, wchar_t **end, int base)                      \
    {                                                                         \
        char buf[WNUM]; char *e;                                              \
        narrow_prefix(s, buf);                                                \
        type v = call;                                                        \
        if (end) *end = (wchar_t *)s + (e - buf);                             \
        return v;                                                             \
    }
WSTRTOI(wcstol,   long,               strtol(buf, &e, base))
WSTRTOI(wcstoul,  unsigned long,      strtoul(buf, &e, base))
WSTRTOI(wcstoll,  long long,          strtoll(buf, &e, base))
WSTRTOI(wcstoull, unsigned long long, strtoull(buf, &e, base))
WSTRTOI(wcstoimax, long long,          strtoll(buf, &e, base))
WSTRTOI(wcstoumax, unsigned long long, strtoull(buf, &e, base))

/* ---------------------------------------------------------------------- */
/* wctype                                                                  */
/* ---------------------------------------------------------------------- */
/* ASCII-only, on purpose and conformantly -- see the header comment in
 * <wctype.h>. Each is written against the wint_t domain so that WEOF (-1) and
 * any code point above 127 answer false rather than indexing a table. */
static int a_(wint_t c) { return (unsigned)c < 128; }
int iswalnum(wint_t c) { return a_(c) && ((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')); }
int iswalpha(wint_t c) { return a_(c) && ((c>='a'&&c<='z')||(c>='A'&&c<='Z')); }
int iswblank(wint_t c) { return a_(c) && (c==' '||c=='\t'); }
int iswcntrl(wint_t c) { return a_(c) && ((unsigned)c < 32 || c == 127); }
int iswdigit(wint_t c) { return a_(c) && c>='0' && c<='9'; }
int iswgraph(wint_t c) { return a_(c) && (unsigned)(c-33) < 94; }
int iswlower(wint_t c) { return a_(c) && c>='a' && c<='z'; }
int iswprint(wint_t c) { return a_(c) && (unsigned)(c-32) < 95; }
int iswpunct(wint_t c) { return iswgraph(c) && !iswalnum(c); }
int iswspace(wint_t c) { return a_(c) && (c==' '||(c>=9&&c<=13)); }
int iswupper(wint_t c) { return a_(c) && c>='A' && c<='Z'; }
int iswxdigit(wint_t c){ return a_(c) && ((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')); }
wint_t towlower(wint_t c) { return iswupper(c) ? c + 32 : c; }
wint_t towupper(wint_t c) { return iswlower(c) ? c - 32 : c; }

/* wctype()/iswctype() and wctrans()/towctrans() are the "look the class up by
 * name" interface. The named classes are exactly the twelve C requires. */
static const char *const classnames[] = {
    "alnum","alpha","blank","cntrl","digit","graph","lower","print","punct","space","upper","xdigit"
};
wctype_t wctype(const char *name)
{
    for (unsigned i = 0; i < sizeof classnames / sizeof *classnames; i++)
        if (!strcmp(name, classnames[i])) return i + 1;
    return 0;
}
int iswctype(wint_t c, wctype_t t)
{
    switch (t) {
    case 1: return iswalnum(c); case 2: return iswalpha(c); case 3: return iswblank(c);
    case 4: return iswcntrl(c); case 5: return iswdigit(c); case 6: return iswgraph(c);
    case 7: return iswlower(c); case 8: return iswprint(c); case 9: return iswpunct(c);
    case 10: return iswspace(c); case 11: return iswupper(c); case 12: return iswxdigit(c);
    default: return 0;
    }
}
wctrans_t wctrans(const char *name)
{ if (!strcmp(name, "tolower")) return 1; if (!strcmp(name, "toupper")) return 2; return 0; }
wint_t towctrans(wint_t c, wctrans_t t)
{ return t == 1 ? towlower(c) : t == 2 ? towupper(c) : c; }

/* ---------------------------------------------------------------------- */
/* wide formatted I/O                                                      */
/* ---------------------------------------------------------------------- */
/* Implemented by round-tripping through the narrow formatter: the wide format
 * is encoded to UTF-8, formatted, and the UTF-8 result decoded back to wide.
 * %lc and %ls already emit UTF-8 through wctomb, so they survive the round trip
 * exactly; a %c carrying a non-ASCII BYTE does not, which is inherent to mixing
 * byte and wide conversions and is why C says not to. */
#define WFMT_MAX 4096

int vswprintf(wchar_t *out, size_t cap, const wchar_t *fmt, va_list ap)
{
    char nfmt[WFMT_MAX], nbuf[WFMT_MAX];
    size_t n = 0;
    for (size_t i = 0; fmt[i]; i++) {
        size_t k = wcrtomb(nfmt + n, fmt[i], NULL);
        if (k == (size_t)-1 || n + k + 1 >= sizeof nfmt) { errno = EILSEQ; return -1; }
        n += k;
    }
    nfmt[n] = 0;
    int r = vsnprintf(nbuf, sizeof nbuf, nfmt, ap);
    if (r < 0) return -1;
    if ((size_t)r >= sizeof nbuf) { errno = EOVERFLOW; return -1; }
    /* swprintf returns -1 (not the would-be length) when the result does not
     * fit -- it differs from snprintf here, and callers rely on it. */
    const char *p = nbuf;
    size_t w = 0;
    while (*p) {
        wchar_t wc;
        size_t k = mbrtowc(&wc, p, 4, NULL);
        if (k == (size_t)-1 || k == (size_t)-2) { errno = EILSEQ; return -1; }
        if (k == 0) break;
        if (w + 1 >= cap) return -1;
        out[w++] = wc; p += k;
    }
    if (cap) out[w] = 0;
    return (int)w;
}
int swprintf(wchar_t *out, size_t cap, const wchar_t *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vswprintf(out, cap, fmt, ap); va_end(ap); return r; }

int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap)
{
    wchar_t wbuf[WFMT_MAX];
    int r = vswprintf(wbuf, WFMT_MAX, fmt, ap);
    if (r < 0) return -1;
    char nbuf[WFMT_MAX * 2];
    size_t n = 0;
    for (int i = 0; i < r; i++) {
        size_t k = wcrtomb(nbuf + n, wbuf[i], NULL);
        if (k == (size_t)-1 || n + k >= sizeof nbuf) return -1;
        n += k;
    }
    return (int)fwrite(nbuf, 1, n, f) == (int)n ? r : -1;
}
int fwprintf(FILE *f, const wchar_t *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfwprintf(f, fmt, ap); va_end(ap); return r; }
int vwprintf(const wchar_t *fmt, va_list ap) { return vfwprintf(stdout, fmt, ap); }
int wprintf(const wchar_t *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfwprintf(stdout, fmt, ap); va_end(ap); return r; }

int vswscanf(const wchar_t *s, const wchar_t *fmt, va_list ap)
{
    char nin[WFMT_MAX], nfmt[WFMT_MAX];
    size_t n = 0;
    for (size_t i = 0; s[i] && n + 4 < sizeof nin; i++) n += wcrtomb(nin + n, s[i], NULL);
    nin[n] = 0;
    n = 0;
    for (size_t i = 0; fmt[i] && n + 4 < sizeof nfmt; i++) n += wcrtomb(nfmt + n, fmt[i], NULL);
    nfmt[n] = 0;
    return vsscanf(nin, nfmt, ap);
}
int swscanf(const wchar_t *s, const wchar_t *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vswscanf(s, fmt, ap); va_end(ap); return r; }
int vfwscanf(FILE *f, const wchar_t *fmt, va_list ap)
{
    char nfmt[WFMT_MAX];
    size_t n = 0;
    for (size_t i = 0; fmt[i] && n + 4 < sizeof nfmt; i++) n += wcrtomb(nfmt + n, fmt[i], NULL);
    nfmt[n] = 0;
    return vfscanf(f, nfmt, ap);
}
int fwscanf(FILE *f, const wchar_t *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfwscanf(f, fmt, ap); va_end(ap); return r; }
int vwscanf(const wchar_t *fmt, va_list ap) { return vfwscanf(stdin, fmt, ap); }
int wscanf(const wchar_t *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfwscanf(stdin, fmt, ap); va_end(ap); return r; }

wint_t fgetwc(FILE *f)
{
    char buf[4]; mbstate_t st; memset(&st, 0, sizeof st);
    for (int i = 0; i < 4; i++) {
        int c = fgetc(f);
        if (c == EOF) return WEOF;
        buf[i] = (char)c;
        wchar_t wc;
        size_t k = mbrtowc(&wc, buf, (size_t)i + 1, &st);
        if (k == (size_t)-1) { errno = EILSEQ; return WEOF; }
        if (k != (size_t)-2) return (wint_t)wc;
    }
    errno = EILSEQ;
    return WEOF;
}
wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(stdin); }
wchar_t *fgetws(wchar_t *s, int n, FILE *f)
{
    int i = 0;
    if (n <= 0) return NULL;
    while (i < n - 1) { wint_t c = fgetwc(f); if (c == WEOF) break; s[i++] = (wchar_t)c; if (c == L'\n') break; }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}
wint_t fputwc(wchar_t wc, FILE *f)
{
    char buf[4];
    size_t k = wcrtomb(buf, wc, NULL);
    if (k == (size_t)-1) return WEOF;
    return fwrite(buf, 1, k, f) == k ? (wint_t)wc : WEOF;
}
wint_t putwc(wchar_t wc, FILE *f) { return fputwc(wc, f); }
wint_t putwchar(wchar_t wc) { return fputwc(wc, stdout); }
int fputws(const wchar_t *s, FILE *f)
{ while (*s) if (fputwc(*s++, f) == WEOF) return -1; return 0; }
wint_t ungetwc(wint_t c, FILE *f)
{
    if (c == WEOF || (unsigned)c > 127) return WEOF;   /* one byte of pushback */
    return ungetc((int)c, f) == EOF ? WEOF : c;
}
