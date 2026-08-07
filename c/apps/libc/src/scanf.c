/* mini-libc formatted input.
 *
 * Was: sscanf only, no %[scanset], no %p, no fscanf/scanf, and a return value
 * that never distinguished "input ended" (EOF) from "the input did not match"
 * (0) -- a difference every read loop in real C is written around.
 *
 * Now: one engine over an abstract character source, so sscanf, fscanf, scanf,
 * vfscanf and vscanf are the same code (stdio.c supplies the sources), and the
 * conversions follow C's actual rule for how much input a directive consumes:
 * the longest sequence that IS, OR IS A PREFIX OF, a matching sequence. That
 * rule is why "1e" fails a %f rather than quietly yielding 1.0, and it is
 * checked against glibc in tests/unit/libc_diff_test.c. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "libc_internal.h"

#define EOFC (-1)
static int sp(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* A pushback stack over the source. C only obliges an implementation to be able
 * to put back one character, and a FILE can only take one back; the extra slots
 * here are consumed by later directives in the SAME call, which is where the
 * multi-character backtrack actually matters. Only the most recent character is
 * returned to the underlying stream at the end. */
struct eng {
    struct __scan_src *src;
    int pb[8], npb;
    long nread;
};

static int eg(struct eng *e)
{
    int c = e->npb ? e->pb[--e->npb] : e->src->get(e->src);
    if (c != EOFC) e->nread++;
    return c;
}
static void eu(struct eng *e, int c)
{
    if (c == EOFC) return;
    if (e->npb < (int)(sizeof e->pb / sizeof e->pb[0])) e->pb[e->npb++] = c;
    e->nread--;
}

/* ---- "is this string still a prefix of a valid number?" ---------------- */
static int hexd(int c)
{ return (c >= '0' && c <= '9') || (lowc(c) >= 'a' && lowc(c) <= 'f'); }

static int word_prefix(const char *s, int n, const char *word)
{
    for (int i = 0; i < n; i++) { if (!word[i]) return 0; if (lowc((unsigned char)s[i]) != word[i]) return 0; }
    return 1;
}

static int flt_prefix(const char *s, int n)
{
    int i = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= n) return 1;
    if (lowc((unsigned char)s[i]) == 'i') return word_prefix(s + i, n - i, "infinity");
    if (lowc((unsigned char)s[i]) == 'n') {
        /* nan, optionally followed by (n-char-sequence) */
        int k = n - i;
        if (k <= 3) return word_prefix(s + i, k, "nan");
        if (!word_prefix(s + i, 3, "nan")) return 0;
        if (s[i + 3] != '(') return 0;
        for (int j = i + 4; j < n; j++) {
            char c = s[j];
            if (c == ')') return j == n - 1;
            if (!((c >= '0' && c <= '9') || (lowc((unsigned char)c) >= 'a' && lowc((unsigned char)c) <= 'z') || c == '_')) return 0;
        }
        return 1;
    }
    if (s[i] == '0' && i + 1 < n && lowc((unsigned char)s[i + 1]) == 'x') {
        i += 2;
        int dot = 0, p = 0, psign = 0, pdig = 0, dig = 0;
        for (; i < n; i++) {
            char c = s[i];
            if (!p && hexd((unsigned char)c)) { dig++; continue; }
            if (!p && c == '.' && !dot) { dot = 1; continue; }
            if (!p && lowc((unsigned char)c) == 'p' && dig) { p = 1; continue; }
            if (p && (c == '+' || c == '-') && !psign && !pdig) { psign = 1; continue; }
            if (p && c >= '0' && c <= '9') { pdig++; continue; }
            return 0;
        }
        return 1;
    }
    { int dig = 0, dot = 0, e = 0, esign = 0, edig = 0;
      for (; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { if (e) edig++; else dig++; continue; }
        if (c == '.' && !dot && !e) { dot = 1; continue; }
        if (lowc((unsigned char)c) == 'e' && !e && dig) { e = 1; continue; }
        if ((c == '+' || c == '-') && e && !esign && !edig) { esign = 1; continue; }
        return 0;
      }
      return 1; }
}

static int int_prefix(const char *s, int n, int base)
{
    int i = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= n) return 1;
    if ((base == 0 || base == 16) && s[i] == '0') {
        if (i + 1 >= n) return 1;
        if (lowc((unsigned char)s[i + 1]) == 'x') { i += 2; base = 16; }
    }
    if (base == 0) base = (s[i] == '0') ? 8 : 10;
    for (; i < n; i++) {
        int c = lowc((unsigned char)s[i]);
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : 99;
        if (v >= base) return 0;
    }
    return 1;
}

/* Collect the longest prefix-of-a-match into buf, honouring the field width. */
static int collect(struct eng *e, char *buf, int cap, int width,
                   int (*ok)(const char *, int), int base, int use_base)
{
    int n = 0;
    int limit = (width > 0 && width < cap - 1) ? width : cap - 1;
    while (n < limit) {
        int c = eg(e);
        if (c == EOFC) break;
        buf[n] = (char)c;
        int good = use_base ? int_prefix(buf, n + 1, base) : ok(buf, n + 1);
        if (!good) { eu(e, c); break; }
        n++;
    }
    buf[n] = 0;
    return n;
}

/* ---------------------------------------------------------------------- */
int __libc_vscan(struct __scan_src *src, const char *fmt, va_list ap)
{
    struct eng E; E.src = src; E.npb = 0; E.nread = 0;
    int assigned = 0, eof_hit = 0, progress = 0;
    const char *p = fmt;
    char buf[512];

    for (; *p; p++) {
        if (sp((unsigned char)*p)) {
            /* Whitespace in the format matches any run, including none. Hitting
             * EOF here is not by itself a failure. */
            int c, skipped = 0;
            while ((c = eg(&E)) != EOFC && sp(c)) skipped++;
            if (c == EOFC) eof_hit = 1; else eu(&E, c);
            if (skipped) progress = 1;
            continue;
        }
        if (*p != '%') {
            int c = eg(&E);
            if (c == EOFC) { eof_hit = 1; goto out; }
            if (c != (unsigned char)*p) { eu(&E, c); goto out; }
            progress = 1;
            continue;
        }
        p++;
        if (*p == '%') {
            int c;
            while ((c = eg(&E)) != EOFC && sp(c)) ;
            if (c == EOFC) { eof_hit = 1; goto out; }
            if (c != '%') { eu(&E, c); goto out; }
            progress = 1;
            continue;
        }

        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p++ - '0'); if (width > 0x100000) width = 0x100000; }
        int lmod = 0;                 /* -2 hh, -1 h, 0 int, 1 l, 2 ll/j/z/t, 3 L */
        for (;;) {
            if (*p == 'l') { lmod = (lmod >= 1) ? 2 : 1; p++; }
            else if (*p == 'h') { lmod = (lmod <= -1) ? -2 : -1; p++; }
            else if (*p == 'j' || *p == 'z' || *p == 't') { lmod = 2; p++; }
            else if (*p == 'L') { lmod = 3; p++; }
            else if (*p == 'q') { lmod = 2; p++; }
            else break;
        }
        char conv = *p;
        if (!conv) break;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            int c;
            while ((c = eg(&E)) != EOFC && sp(c)) ;
            if (c == EOFC) { eof_hit = 1; goto out; }
            eu(&E, c);
            int base = (conv == 'd' || conv == 'u') ? 10
                     : (conv == 'i') ? 0 : (conv == 'o') ? 8 : 16;
            int n = collect(&E, buf, (int)sizeof buf, width, NULL, base, 1);
            if (n == 0) goto out;                              /* matching failure */
            char *endp;
            int uns = (conv != 'd' && conv != 'i');
            unsigned long long uv = 0; long long sv = 0;
            if (uns) uv = strtoull(buf, &endp, base); else sv = strtoll(buf, &endp, base);
            /* C: the directive consumes the longest prefix-OF-a-match, and then
             * fails unless that item IS a match. So "0x" under %i is consumed
             * and reported as a matching failure -- it is not silently the
             * number 0 with the "x" pushed back. */
            if (endp != buf + n) goto out;
            if (suppress) break;
            if (conv == 'p') { *va_arg(ap, void **) = (void *)(uintptr_t)uv; assigned++; break; }
            if (uns) {
                if (lmod <= -2) *va_arg(ap, unsigned char *) = (unsigned char)uv;
                else if (lmod == -1) *va_arg(ap, unsigned short *) = (unsigned short)uv;
                else if (lmod == 0) *va_arg(ap, unsigned int *) = (unsigned int)uv;
                else if (lmod == 1) *va_arg(ap, unsigned long *) = (unsigned long)uv;
                else *va_arg(ap, unsigned long long *) = uv;
            } else {
                if (lmod <= -2) *va_arg(ap, signed char *) = (signed char)sv;
                else if (lmod == -1) *va_arg(ap, short *) = (short)sv;
                else if (lmod == 0) *va_arg(ap, int *) = (int)sv;
                else if (lmod == 1) *va_arg(ap, long *) = (long)sv;
                else *va_arg(ap, long long *) = sv;
            }
            assigned++;
            break; }

        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            int c;
            while ((c = eg(&E)) != EOFC && sp(c)) ;
            if (c == EOFC) { eof_hit = 1; goto out; }
            eu(&E, c);
            int n = collect(&E, buf, (int)sizeof buf, width, flt_prefix, 0, 0);
            if (n == 0) goto out;
            char *endp;
            /* Same rule as the integer case: "1e" is a prefix of "1e5", so it
             * is consumed and then fails, rather than converting to 1.0. */
            double dv = strtod(buf, &endp);
            if (endp != buf + n) goto out;
            if (suppress) break;
            if (lmod == 3) *va_arg(ap, long double *) = (long double)dv;
            else if (lmod >= 1) *va_arg(ap, double *) = dv;
            else *va_arg(ap, float *) = (float)dv;
            assigned++;
            break; }

        case 's': {
            int c;
            while ((c = eg(&E)) != EOFC && sp(c)) ;
            if (c == EOFC) { eof_hit = 1; goto out; }
            int w = width > 0 ? width : 0x7fffffff, k = 0;
            char *out = suppress ? NULL : va_arg(ap, char *);
            wchar_t *wout = NULL;
            if (!suppress && lmod >= 1) { wout = (wchar_t *)out; out = NULL; }
            while (c != EOFC && !sp(c) && k < w) {
                if (out) out[k] = (char)c;
                if (wout) wout[k] = (wchar_t)(unsigned char)c;
                k++;
                c = eg(&E);
            }
            eu(&E, c);
            if (out) out[k] = 0;
            if (wout) wout[k] = 0;
            if (!suppress) assigned++;
            break; }

        case 'c': {
            int w = width > 0 ? width : 1, k = 0;
            char *out = suppress ? NULL : va_arg(ap, char *);
            wchar_t *wout = NULL;
            if (!suppress && lmod >= 1) { wout = (wchar_t *)out; out = NULL; }
            while (k < w) {
                int c = eg(&E);
                if (c == EOFC) { eof_hit = 1; if (k == 0) goto out; goto out; }
                if (out) out[k] = (char)c;
                if (wout) wout[k] = (wchar_t)(unsigned char)c;
                k++;
            }
            if (!suppress) assigned++;
            break; }

        case '[': {
            /* %[...]: the scanset. ']' first (or after '^') is a literal ']';
             * 'a-z' is a range; a trailing '-' is a literal '-'. */
            unsigned char in[256]; memset(in, 0, sizeof in);
            p++;
            int neg = 0;
            if (*p == '^') { neg = 1; p++; }
            if (*p == ']') { in[(unsigned char)']'] = 1; p++; }
            while (*p && *p != ']') {
                if (p[0] == '-' && p[1] && p[1] != ']' && p[-1] != '[' ) {
                    unsigned char lo = (unsigned char)p[-1], hi = (unsigned char)p[1];
                    if (lo <= hi) for (int v = lo; v <= hi; v++) in[v] = 1;
                    p += 2;
                    continue;
                }
                in[(unsigned char)*p] = 1;
                p++;
            }
            if (*p != ']') goto out;                 /* malformed directive */
            if (neg) for (int v = 0; v < 256; v++) in[v] = (unsigned char)!in[v];
            int w = width > 0 ? width : 0x7fffffff, k = 0;
            char *out = suppress ? NULL : va_arg(ap, char *);
            wchar_t *wout = NULL;
            if (!suppress && lmod >= 1) { wout = (wchar_t *)out; out = NULL; }
            for (;;) {
                if (k >= w) break;
                int c = eg(&E);
                if (c == EOFC) { if (k == 0) { eof_hit = 1; goto out; } break; }
                if (!in[(unsigned char)c]) { eu(&E, c); break; }
                if (out) out[k] = (char)c;
                if (wout) wout[k] = (wchar_t)(unsigned char)c;
                k++;
            }
            if (k == 0) goto out;                    /* matching failure */
            if (out) out[k] = 0;
            if (wout) wout[k] = 0;
            if (!suppress) assigned++;
            break; }

        case 'n': {
            if (suppress) break;
            long n = E.nread;
            if (lmod >= 2) *va_arg(ap, long long *) = n;
            else if (lmod == 1) *va_arg(ap, long *) = n;
            else if (lmod == -1) *va_arg(ap, short *) = (short)n;
            else if (lmod <= -2) *va_arg(ap, signed char *) = (signed char)n;
            else *va_arg(ap, int *) = (int)n;
            break; }                                  /* %n is not a conversion */

        default:
            goto out;                                 /* unknown directive */
        }
    }

out:
    /* Return at most one character to the real stream; see the note on `pb`. */
    if (E.npb) src->unget(src, E.pb[E.npb - 1]);
    /* EOF is reserved for "the input ran out before ANYTHING matched". Once a
     * literal or an explicit whitespace directive has consumed input, running
     * out is an ordinary zero-conversion return -- the difference every
     * `while (scanf(...) != EOF)` loop is built on. */
    if (assigned == 0 && eof_hit && !progress) return EOFC;
    return assigned;
}
