/* Exact decimal <-> binary floating-point conversion.
 *
 * WHY THIS FILE EXISTS. The old strtod accumulated digits into a double and
 * then multiplied by a power of ten built by repeated squaring; the old printf
 * peeled decimal digits off a double by repeated multiplication by 10. Both are
 * wrong, and wrong silently: each rounds at least twice, so "1e23" parsed back
 * to the wrong double and "%.17g" did not round-trip. A libc may be missing a
 * function -- the linker says so -- but it may not misround, because nothing
 * says so.
 *
 * A double is m x 2^e with m a 53-bit integer, so its value is a rational whose
 * denominator is a power of two: the decimal expansion TERMINATES, in at most
 * 767 significant digits. Everything below is integer arithmetic on that exact
 * expansion, held as a big decimal (one digit per byte). Parsing runs the same
 * machinery backwards. Both directions are therefore correctly rounded for
 * every input, which is a property a differential test against glibc can prove
 * rather than a claim (see tests/unit/libc_diff_test.c).
 *
 * The big-decimal shift/round algorithm follows the one in Go's strconv
 * (decimal.go / atof.go), which is the clearest published treatment of the
 * exact slow path; the fast paths, the hex-float parser and the printf-facing
 * renderer are ours.
 *
 * The working buffers are `static`: mini-libc is single-threaded (see
 * <pthread.h>) and these functions never nest, so the alternative is ~1.7 KiB
 * of stack in every printf frame for no benefit. */

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include "libc_internal.h"

#define DBUF 832                 /* working digits (DMAX + headroom for a shift) */
#define DMAX __LIBC_DTOA_MAX     /* digits actually kept */

struct dec {
    unsigned char d[DBUF];       /* raw digit values 0..9, most significant first */
    int nd;                      /* digits in d */
    int dp;                      /* value = 0.d[0..nd) * 10^dp */
    int neg;
    int trunc;                   /* digits were dropped: the true value is larger */
};

static void dtrim(struct dec *a)
{
    while (a->nd > 0 && a->d[a->nd - 1] == 0) a->nd--;
    if (a->nd == 0) a->dp = 0;
}

/* a /= 2^k, 1 <= k <= 60. In place: the write cursor never passes the read
 * cursor, so no scratch buffer is needed. */
static void dshr(struct dec *a, unsigned k)
{
    int r = 0, w = 0;
    uint64_t n = 0;

    /* Pull in leading digits until the accumulator holds at least one output
     * digit's worth (n >> k != 0). */
    for (;;) {
        if ((n >> k) != 0) break;
        if (r >= a->nd) {                 /* ran out of input: result < 1 */
            if (n == 0) { a->nd = 0; a->dp = 0; return; }
            while ((n >> k) == 0) { n *= 10; r++; }
            break;
        }
        n = n * 10 + a->d[r++];
    }
    a->dp -= r - 1;

    for (; r < a->nd; r++) {
        unsigned char c = a->d[r];
        uint64_t dig = n >> k;
        n -= dig << k;
        a->d[w++] = (unsigned char)dig;
        n = n * 10 + c;
    }
    while (n > 0) {
        uint64_t dig = n >> k;
        n -= dig << k;
        if (w < DMAX) a->d[w++] = (unsigned char)dig;
        else if (dig > 0) a->trunc = 1;
        n *= 10;
    }
    a->nd = w;
    dtrim(a);
}

/* a *= 2^k, 1 <= k <= 60. The product gains at most ceil(k*log10 2) digits, so
 * the result is built right-aligned at nd+delta and then slid down. */
static void dshl(struct dec *a, unsigned k)
{
    int delta = (int)((k * 30103u) / 100000u) + 1;   /* >= ceil(k * log10 2) */
    int r = a->nd - 1, w = a->nd + delta;
    uint64_t n = 0;

    for (; r >= 0; r--) {
        n += (uint64_t)a->d[r] << k;       /* n stays < 10*2^60, no overflow */
        uint64_t quo = n / 10;
        a->d[--w] = (unsigned char)(n - quo * 10);
        n = quo;
    }
    while (n > 0) {
        uint64_t quo = n / 10;
        a->d[--w] = (unsigned char)(n - quo * 10);
        n = quo;
    }

    int nd = a->nd + delta - w;
    if (w > 0) memmove(a->d, a->d + w, (size_t)nd);
    a->dp += delta - w;
    if (nd > DMAX) {
        for (int i = DMAX; i < nd; i++) if (a->d[i]) { a->trunc = 1; break; }
        nd = DMAX;
    }
    a->nd = nd;
    dtrim(a);
}

/* a *= 2^k for any k (negative shifts right). */
static void dshift(struct dec *a, int k)
{
    if (a->nd == 0) return;
    if (k > 0) {
        while (k > 60) { dshl(a, 60); k -= 60; }
        if (k > 0) dshl(a, (unsigned)k);
    } else if (k < 0) {
        k = -k;
        while (k > 60) { dshr(a, 60); k -= 60; if (a->nd == 0) return; }
        if (k > 0) dshr(a, (unsigned)k);
    }
}

/* Round-to-nearest, ties-to-even, when cutting after `nd` digits. A tie is only
 * a tie if nothing was truncated earlier -- `trunc` means there are unseen
 * nonzero digits below, which breaks it upward. */
static int round_up(const struct dec *a, int nd)
{
    if (nd < 0 || nd >= a->nd) return 0;
    if (a->d[nd] == 5 && nd + 1 == a->nd) {
        if (a->trunc) return 1;
        return nd > 0 && (a->d[nd - 1] & 1);
    }
    return a->d[nd] >= 5;
}

static void dround(struct dec *a, int nd)
{
    /* Cutting before the first digit means the value is strictly below half a
     * unit in the last kept place: the result is zero, not "leave it alone". */
    if (nd < 0) { a->nd = 0; a->dp = 0; a->trunc = 0; return; }
    if (nd >= a->nd) return;
    if (round_up(a, nd)) {
        int i = nd - 1;
        for (; i >= 0; i--) if (a->d[i] < 9) { a->d[i]++; break; }
        if (i < 0) { a->d[0] = 1; a->nd = 1; a->dp++; a->trunc = 0; return; }
        a->nd = i + 1;                       /* the 9s that wrapped became 0s */
    } else {
        a->nd = nd;
    }
    a->trunc = 0;
    dtrim(a);
}

/* Round to an integer (at the decimal point) and return it. */
static uint64_t dround_int(struct dec *a)
{
    if (a->dp > 20) return ~(uint64_t)0;
    uint64_t n = 0; int i;
    for (i = 0; i < a->dp && i < a->nd; i++) n = n * 10 + a->d[i];
    for (; i < a->dp; i++) n *= 10;
    if (round_up(a, a->dp)) n++;
    return n;
}

/* ---------------------------------------------------------------------- */
/* decimal -> binary                                                       */
/* ---------------------------------------------------------------------- */

struct fltinfo { int mantbits, expbits, bias; };
static const struct fltinfo f64 = { 52, 11, -1023 };
static const struct fltinfo f32 = { 23,  8,  -127 };

/* Number of bits to shift to move the decimal point by one decimal place, for
 * small counts: powtab[i] ~= i * log2(10), chosen so a shift never overshoots. */
static const int powtab[] = { 1, 3, 6, 9, 13, 16, 19, 22, 26 };

static uint64_t dec_to_bits(struct dec *a, const struct fltinfo *f, int *ovf, int *inex)
{
    int exp; uint64_t mant;
    *ovf = 0; if (inex) *inex = 0;

    if (a->nd == 0) { mant = 0; exp = f->bias; goto out; }
    if (a->dp > 310) goto overflow;
    if (a->dp < -330) { mant = 0; exp = f->bias; goto out; }

    exp = 0;
    while (a->dp > 0) {
        int n = (a->dp >= (int)(sizeof powtab / sizeof powtab[0])) ? 27 : powtab[a->dp];
        dshift(a, -n); exp += n;
        if (a->nd == 0) { mant = 0; exp = f->bias; goto out; }
    }
    while (a->dp < 0 || (a->dp == 0 && a->d[0] < 5)) {
        int n = (-a->dp >= (int)(sizeof powtab / sizeof powtab[0])) ? 27 : powtab[-a->dp];
        dshift(a, n); exp -= n;
        if (a->nd == 0) { mant = 0; exp = f->bias; goto out; }
    }
    exp--;                                   /* [0.5,1) -> [1,2) */

    if (exp < f->bias + 1) { int n = f->bias + 1 - exp; dshift(a, -n); exp += n; }
    if (exp - f->bias >= (1 << f->expbits) - 1) goto overflow;

    dshift(a, 1 + f->mantbits);
    /* Whether the final rounding to an integer DISCARDED anything is exactly
     * the question ERANGE-on-underflow asks: a subnormal result is only an
     * underflow if precision was lost reaching it. 0x1p-1074 is the smallest
     * subnormal and is exact, and glibc reports no error for it. */
    if (inex) *inex = (a->trunc || a->nd > a->dp);
    mant = dround_int(a);
    if (mant == (uint64_t)2 << f->mantbits) {
        mant >>= 1; exp++;
        if (exp - f->bias >= (1 << f->expbits) - 1) goto overflow;
    }
    if ((mant & ((uint64_t)1 << f->mantbits)) == 0) exp = f->bias;   /* subnormal */
    goto out;

overflow:
    mant = 0; exp = (1 << f->expbits) - 1 + f->bias; *ovf = 1;
out:
    {
        uint64_t bits = mant & (((uint64_t)1 << f->mantbits) - 1);
        bits |= (uint64_t)((exp - f->bias) & ((1 << f->expbits) - 1)) << f->mantbits;
        if (a->neg) bits |= (uint64_t)1 << (f->mantbits + f->expbits);
        return bits;
    }
}

/* Exact powers of ten that fit a double without rounding. */
static const double p10[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* Clinger's fast path: when the mantissa and the power of ten are both exactly
 * representable, one IEEE multiply or divide is correctly rounded by
 * definition. Applies to the overwhelming majority of literals in real source. */
static int fast_path(const struct dec *a, const struct fltinfo *f, double *out)
{
    if (a->nd > 15 || a->trunc) return 0;
    uint64_t m = 0;
    for (int i = 0; i < a->nd; i++) m = m * 10 + a->d[i];
    int e = a->dp - a->nd;
    double d = (double)m;
    if (e > 0) {
        if (e > 22 + (15 - a->nd)) return 0;
        if (e > 22) { d *= p10[e - 22]; e = 22; }   /* still exact: m has spare bits */
        d *= p10[e];
    } else if (e < 0) {
        if (-e > 22) return 0;
        d /= p10[-e];
    }
    if (a->neg) d = -d;
    if (f == &f32) { float ff = (float)d; if ((double)ff != d) return 0; *out = ff; return 1; }
    *out = d;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* strtod / strtof                                                         */
/* ---------------------------------------------------------------------- */

static struct dec g_dec;                     /* see the header comment on `static` */

static int is_space(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int prefix_ci(const char *s, const char *word)
{
    int i = 0;
    while (word[i] && lower((unsigned char)s[i]) == word[i]) i++;
    return word[i] ? 0 : i;
}

static double make_inf(int neg)
{ double h = 1e308; h *= 10; return neg ? -h : h; }

static double make_nan(void)
{ return __builtin_nan(""); }

/* C99 hexadecimal floating literal: 0x h.hhh p[+-]ddd.
 *
 * A binary fraction is a terminating DECIMAL fraction, so the exact same big
 * decimal that parses "1e-300" can hold "0x1.8p-40" without losing a bit: build
 * the significand's decimal digits, scale by 2^e2, and hand it to the one
 * correctly-rounded decimal->binary routine. Hand-rolled bit twiddling here
 * would be a second rounding implementation to get wrong.
 *
 * The significand is accumulated into 64 bits; a longer literal contributes to
 * `trunc`, which is exactly the sticky bit the tie-breaker needs. */
static double parse_hex(const char *s, const char **endp, int neg,
                        const struct fltinfo *f, int *range)
{
    const char *p = s;                       /* p points just after "0x" */
    uint64_t m = 0; int any = 0, e2 = 0, sticky = 0, seen_dot = 0;

    for (;; p++) {
        int v = hexval((unsigned char)*p);
        if (v < 0) {
            if (*p == '.' && !seen_dot) { seen_dot = 1; continue; }
            break;
        }
        any = 1;
        if (seen_dot) e2 -= 4;
        /* Full significand: the digit is dropped (sticky) and the remaining
         * value scales by 16 -- which for a fraction digit exactly undoes the
         * `e2 -= 4` above, leaving the point where it was. */
        if (m >> 60) { sticky |= (v != 0); e2 += 4; }
        else m = (m << 4) | (unsigned)v;
    }
    if (!any) { *endp = NULL; return 0.0; }

    if (lower((unsigned char)*p) == 'p') {
        const char *q = p + 1; int eneg = 0;
        if (*q == '+' || *q == '-') eneg = (*q++ == '-');
        if (*q >= '0' && *q <= '9') {
            long ev = 0;
            while (*q >= '0' && *q <= '9') { if (ev < 100000) ev = ev * 10 + (*q - '0'); q++; }
            e2 += (int)(eneg ? -ev : ev);
            p = q;
        }
    }
    *endp = p;
    if (m == 0) return neg ? -0.0 : 0.0;

    struct dec *a = &g_dec;
    char t[24]; int n = 0;
    uint64_t mm = m;
    while (mm) { t[n++] = (char)(mm % 10); mm /= 10; }
    for (int i = 0; i < n; i++) a->d[i] = (unsigned char)t[n - 1 - i];
    a->nd = n; a->dp = n; a->neg = neg; a->trunc = sticky;
    dshift(a, e2);

    int ovf = 0, inex = 0;
    uint64_t b = dec_to_bits(a, f, &ovf, &inex);
    if (ovf) { *range = 1; return make_inf(neg); }
    inex = inex || sticky;
    if (f == &f32) {
        union { uint32_t u; float f; } u; u.u = (uint32_t)b;
        if (u.f == 0 || (((u.u >> 23) & 0xff) == 0 && inex)) *range = 1;
        return u.f;
    }
    union { uint64_t u; double d; } u; u.u = b;
    if (u.d == 0 || (((u.u >> 52) & 0x7ff) == 0 && inex)) *range = 1;
    return u.d;
}

double __libc_strtox(const char *s, char **end, int bits)
{
    const struct fltinfo *f = (bits == 32) ? &f32 : &f64;
    const char *start = s, *p = s;
    int neg = 0, range = 0;
    double result;

    while (is_space((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');

    /* inf / infinity / nan[(chars)] -- C99 requires all three spellings. */
    if (lower((unsigned char)*p) == 'i') {
        int n = prefix_ci(p, "infinity");
        if (n) { if (end) *end = (char *)(p + n); return make_inf(neg); }
        n = prefix_ci(p, "inf");
        if (n) { if (end) *end = (char *)(p + n); return make_inf(neg); }
    } else if (lower((unsigned char)*p) == 'n') {
        int n = prefix_ci(p, "nan");
        if (n) {
            const char *q = p + n;
            if (*q == '(') { const char *r = q + 1;
                while ((*r >= '0' && *r <= '9') || (*r >= 'a' && *r <= 'z') ||
                       (*r >= 'A' && *r <= 'Z') || *r == '_') r++;
                if (*r == ')') q = r + 1; }
            if (end) *end = (char *)q;
            return neg ? -make_nan() : make_nan();
        }
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char *he = NULL;
        double v = parse_hex(p + 2, &he, neg, f, &range);
        if (he) {
            if (end) *end = (char *)he;
            if (range) errno = ERANGE;
            return v;
        }
        /* "0x" with no hex digits: C says the subject sequence is "0". */
        if (end) *end = (char *)(p + 1);
        return neg ? -0.0 : 0.0;
    }

    /* Decimal: digits [. digits] [eE [+-] digits] */
    struct dec *a = &g_dec;
    a->nd = 0; a->dp = 0; a->neg = neg; a->trunc = 0;
    int any = 0, seen_dot = 0, nz = 0;       /* nz: significant digits started */
    for (;; p++) {
        if (*p == '.') { if (seen_dot) break; seen_dot = 1; continue; }
        if (*p < '0' || *p > '9') break;
        any = 1;
        if (*p == '0' && !nz) { if (seen_dot) a->dp--; continue; }   /* leading zeros */
        nz = 1;
        if (a->nd < DMAX) { a->d[a->nd++] = (unsigned char)(*p - '0'); if (!seen_dot) a->dp++; }
        else { if (*p != '0') a->trunc = 1; if (!seen_dot) a->dp++; }
    }
    if (!any) { if (end) *end = (char *)start; return 0.0; }
    if (!nz) a->dp = 0;

    if (lower((unsigned char)*p) == 'e') {
        const char *q = p + 1; int eneg = 0;
        if (*q == '+' || *q == '-') eneg = (*q++ == '-');
        if (*q >= '0' && *q <= '9') {
            long ev = 0;
            while (*q >= '0' && *q <= '9') { if (ev < 100000) ev = ev * 10 + (*q - '0'); q++; }
            a->dp += (int)(eneg ? -ev : ev);
            p = q;
        }
    }
    if (end) *end = (char *)p;
    dtrim(a);

    if (a->nd == 0) return neg ? -0.0 : 0.0;

    if (fast_path(a, f, &result)) return result;

    int ovf = 0, inex = 0;
    uint64_t b = dec_to_bits(a, f, &ovf, &inex);
    if (ovf) { errno = ERANGE; return make_inf(neg); }
    /* Underflow is "subnormal AND lost precision", not "subnormal". */
    if (f == &f32) {
        union { uint32_t u; float f; } u; u.u = (uint32_t)b;
        if (u.f == 0 || (((u.u >> 23) & 0xff) == 0 && inex)) errno = ERANGE;
        return u.f;
    }
    union { uint64_t u; double d; } u; u.u = b;
    if (u.d == 0 || (((u.u >> 52) & 0x7ff) == 0 && inex)) errno = ERANGE;
    return u.d;
}

/* ---------------------------------------------------------------------- */
/* binary -> decimal (printf)                                              */
/* ---------------------------------------------------------------------- */

int __libc_dtoa(double v, int mode, int ndig, char *buf, int *decpt)
{
    union { double d; uint64_t u; } u; u.d = v;
    uint64_t frac = u.u & (((uint64_t)1 << 52) - 1);
    int bexp = (int)((u.u >> 52) & 0x7ff);
    uint64_t mant;
    int e2;

    if (bexp == 0) { mant = frac; e2 = -1074; }             /* zero / subnormal */
    else { mant = frac | ((uint64_t)1 << 52); e2 = bexp - 1075; }

    struct dec *a = &g_dec;
    a->nd = 0; a->dp = 0; a->neg = 0; a->trunc = 0;
    if (mant == 0) { *decpt = 1; buf[0] = 0; return 0; }

    /* mant as decimal digits, then scale by 2^e2 -- exactly. */
    char t[24]; int n = 0;
    while (mant) { t[n++] = (char)(mant % 10); mant /= 10; }
    for (int i = 0; i < n; i++) a->d[i] = (unsigned char)t[n - 1 - i];
    a->nd = n; a->dp = n;
    dshift(a, e2);

    if (mode == 0) { if (ndig < 1) ndig = 1; dround(a, ndig); }
    else           { dround(a, a->dp + ndig); }

    for (int i = 0; i < a->nd; i++) buf[i] = (char)('0' + a->d[i]);
    buf[a->nd] = 0;
    *decpt = a->dp;
    return a->nd;
}
