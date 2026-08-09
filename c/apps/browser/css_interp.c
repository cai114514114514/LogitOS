/* css_interp.c -- CSS value interpolation, and `transform` as a value.
 *
 * See css_interp.h for what this is for and why the decomposition is the part
 * that matters. Everything here is convention-critical, so the convention is
 * stated once, at the top, and nothing below deviates from it:
 *
 *   m[i * 4 + j] is the CSS spec's matrix[i][j].
 *
 * That single sentence resolves the trap this file was going to fall into.
 * matrix3d(a1..a16) fills m[0..15] in order, so the array IS the spec's
 * indexing with no permutation -- and the spec's decompose/recompose read
 * matrix[i][j] directly, so they can be transcribed rather than re-derived.
 * The same array read as m[col * 4 + row] is the ordinary column-major
 * column-vector matrix, which is what ci_mul() composes and what a renderer
 * would upload; the two readings are the same bytes because the spec's
 * row-vector matrix is the transpose of the column-vector one and matrix3d's
 * argument order fills one row-wise exactly as it fills the other
 * column-wise.
 *
 * The consequence worth writing down, because it looks like a bug: the
 * quaternion the spec's decomposition produces is the CONJUGATE of the one a
 * graphics text would produce for the same rotation. It is self-consistent
 * with the spec's recompose (verified on rotateZ(90deg): decompose gives
 * z = -sin45, recompose reads it back as m[1] = +1), and slerp does not care
 * about the sign convention as long as both endpoints share it. Mixing the
 * two conventions -- building a quaternion from rotate3d() the textbook way
 * and feeding it to the spec's recompose -- rotates the wrong way, and every
 * matched-axis test still passes because those never reach recompose.
 */
#include "css_interp.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

/* NO atof, NO strtod, NO pow, NO log10 -- and that is a link constraint, not
 * taste. This TU is in BROWSER_PIPE, so it is linked into the ring-3
 * browser.aex against mini-libc, where <stdlib.h> DECLARES atof and nothing
 * defines it; the failure would be an undefined symbol at the browser link,
 * hours after the host suite went green. The four are replaced below by a
 * decimal scanner and a power-of-ten helper, both of which this file wants
 * anyway: the scanner has to agree with the CSS number grammar rather than
 * with C's, and pow10() is exact for the exponents that occur here.
 *
 * sin/cos/tan/acos/sqrt/fabs/floor DO come from third_party/libm, which the
 * browser already links for QuickJS. */

#ifndef CI_PI
#define CI_PI 3.14159265358979323846
#endif

static int ci_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int ci_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* 10^k. Exact in a double for |k| <= 22, which covers every exponent a CSS
 * number can carry after the fractional digits are folded in; beyond that the
 * repeated multiply is as good as anything short of a full strtod. */
static double ci_pow10(int k)
{
    static const double P[23] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    int neg = k < 0;
    if (neg) k = -k;
    double r;
    if (k <= 22) r = P[k];
    else { r = P[22]; for (int i = 22; i < k; i++) r *= 10.0; }
    return neg ? 1.0 / r : r;
}

/* The magnitude of a nonzero value: floor(log10(|v|)), without log10. */
static int ci_mag(double a)
{
    int mag = 0;
    if (a >= 1.0) { while (a >= 10.0) { a /= 10.0; mag++; } }
    else { while (a < 1.0 && mag > -320) { a *= 10.0; mag--; } }
    return mag;
}

/* ======================================================================
 * Number serialisation
 *
 * CSSOM serialises a number with as few characters as round-trip allows, and
 * WPT compares STRINGS -- so "2" and "2.00000" are a pass and a fail of the
 * same computation. Six significant digits is what the transform tests round
 * to (transform-2d-getComputedStyle-001.html applies toFixed(6) to both sides
 * before comparing), and it is what every engine emits.
 *
 * Fixed notation across the whole ordinary range rather than %g, because %g
 * switches to an exponent at 1e6 and `matrix(1000000, ...)` is an ordinary
 * value for a translate in a long page. Exponents are kept only where fixed
 * notation would be absurd, and the WPT matrix regexp allows [-0-9.e]+ there.
 *
 * "-0" is normalised to "0": it arises constantly (cos(90deg) is -0 in the
 * rotate matrices) and no engine prints it.
 * ====================================================================== */
int ci_num_text(double v, char *out, int outmax)
{
    if (outmax <= 0) return 0;
    if (!(v == v)) { snprintf(out, (size_t)outmax, "0"); return (int)strlen(out); }
    if (v == 0.0) { snprintf(out, (size_t)outmax, "0"); return 1; }

    double a = fabs(v);
    if (a >= 1e21 || a < 1e-7) {
        snprintf(out, (size_t)outmax, "%g", v);
        return (int)strlen(out);
    }
    int mag = ci_mag(a);
    int dec = 5 - mag;                       /* six significant digits */
    if (dec < 0) {
        /* Above a million there are no decimals left to drop, and printing
         * "%.0f" would keep every digit -- 123456789 instead of 123457000.
         * Six SIGNIFICANT digits means rounding the integer part too. */
        double f = ci_pow10(-dec);
        v = (v < 0 ? -1.0 : 1.0) * floor(fabs(v) / f + 0.5) * f;
        dec = 0;
    }
    if (dec > 15) dec = 15;
    snprintf(out, (size_t)outmax, "%.*f", dec, v);

    /* Strip the trailing zeros the fixed format always produces. */
    char *p = out;
    int has_dot = 0;
    for (; *p; p++) if (*p == '.') has_dot = 1;
    if (has_dot) {
        int n = (int)strlen(out);
        while (n > 0 && out[n - 1] == '0') n--;
        if (n > 0 && out[n - 1] == '.') n--;
        out[n] = 0;
    }
    /* Rounding to six digits can turn -0.0000001 into "-0". */
    if (!strcmp(out, "-0")) { out[0] = '0'; out[1] = 0; }
    return (int)strlen(out);
}

/* ======================================================================
 * 4x4 matrices
 * ====================================================================== */
static void ci_identity(double m[16])
{
    for (int i = 0; i < 16; i++) m[i] = 0.0;
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

/* C = A * B, column-vector composition: a transform list M1 M2 ... Mn is
 * M1 * M2 * ... * Mn, so the FIRST function in the list is the outermost. */
static void ci_mul(const double a[16], const double b[16], double out[16])
{
    double c[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[col * 4 + k];
            c[col * 4 + row] = s;
        }
    memcpy(out, c, sizeof c);
}

static int ci_is_2d(const double m[16])
{
    const double e = 1e-12;
    return fabs(m[2]) < e && fabs(m[3]) < e && fabs(m[6]) < e && fabs(m[7]) < e &&
           fabs(m[8]) < e && fabs(m[9]) < e && fabs(m[11]) < e && fabs(m[14]) < e &&
           fabs(m[10] - 1.0) < e && fabs(m[15] - 1.0) < e;
}

/* ---- one function -> its matrix ---------------------------------------- */
static void fn_matrix(const struct ci_fn *f, double refw, double refh, double m[16])
{
    ci_identity(m);
    double x, y, z, ang, c, s, t;
    switch (f->kind) {
    case CI_TRANSLATE:
    case CI_TRANSLATEX:
    case CI_TRANSLATEY:
    case CI_TRANSLATEZ:
    case CI_TRANSLATE3D: {
        double tx = 0, ty = 0, tz = 0;
        if (f->kind == CI_TRANSLATEY) {
            /* translateY's one argument is the Y one, so its percentage
             * resolves against the box HEIGHT -- reading it out of slot 0 and
             * then swapping would resolve it against the width. */
            ty = f->a[0] + f->pc[0] * refh / 100.0;
        } else if (f->kind == CI_TRANSLATEZ) {
            tz = f->a[0];
        } else {
            tx = f->a[0] + f->pc[0] * refw / 100.0;
            if (f->kind != CI_TRANSLATEX) ty = f->a[1] + f->pc[1] * refh / 100.0;
            if (f->kind == CI_TRANSLATE3D) tz = f->a[2];
        }
        m[12] = tx; m[13] = ty; m[14] = tz;
        break; }
    case CI_SCALE:
    case CI_SCALEX:
    case CI_SCALEY:
    case CI_SCALEZ:
    case CI_SCALE3D: {
        double sx = 1, sy = 1, sz = 1;
        if (f->kind == CI_SCALEX) sx = f->a[0];
        else if (f->kind == CI_SCALEY) sy = f->a[0];
        else if (f->kind == CI_SCALEZ) sz = f->a[0];
        else { sx = f->a[0]; sy = f->a[1]; sz = (f->kind == CI_SCALE3D) ? f->a[2] : 1; }
        m[0] = sx; m[5] = sy; m[10] = sz;
        break; }
    case CI_ROTATE:
    case CI_ROTATEZ:
        ang = f->a[0]; c = cos(ang); s = sin(ang);
        m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
        break;
    case CI_ROTATEX:
        ang = f->a[0]; c = cos(ang); s = sin(ang);
        m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
        break;
    case CI_ROTATEY:
        ang = f->a[0]; c = cos(ang); s = sin(ang);
        m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
        break;
    case CI_ROTATE3D: {
        x = f->a[0]; y = f->a[1]; z = f->a[2]; ang = f->a[3];
        double len = sqrt(x * x + y * y + z * z);
        if (len == 0.0) break;                       /* a zero axis is the identity */
        x /= len; y /= len; z /= len;
        c = cos(ang); s = sin(ang); t = 1.0 - c;
        m[0] = t * x * x + c;      m[1] = t * x * y + s * z;  m[2] = t * x * z - s * y;
        m[4] = t * x * y - s * z;  m[5] = t * y * y + c;      m[6] = t * y * z + s * x;
        m[8] = t * x * z + s * y;  m[9] = t * y * z - s * x;  m[10] = t * z * z + c;
        break; }
    case CI_SKEW:
        m[4] = tan(f->a[0]); m[1] = tan(f->a[1]);
        break;
    case CI_SKEWX:
        m[4] = tan(f->a[0]);
        break;
    case CI_SKEWY:
        m[1] = tan(f->a[0]);
        break;
    case CI_MATRIX:
        m[0] = f->a[0]; m[1] = f->a[1]; m[4] = f->a[2];
        m[5] = f->a[3]; m[12] = f->a[4]; m[13] = f->a[5];
        break;
    case CI_MATRIX3D:
        for (int i = 0; i < 16; i++) m[i] = f->a[i];
        break;
    case CI_PERSPECTIVE:
        /* perspective(none) -- and any non-positive depth -- is the identity.
         * The spec spells the identity of perspective() as perspective(none),
         * i.e. an infinite depth, which is why the interpolation below works
         * on the RECIPROCAL and not on the depth. */
        if (f->a[0] > 0.0) m[11] = -1.0 / f->a[0];
        break;
    default: break;
    }
}

void ci_transform_matrix(const struct ci_xform *t, double refw, double refh, double m[16])
{
    ci_identity(m);
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        double f[16];
        fn_matrix(&t->f[i], refw, refh, f);
        ci_mul(m, f, m);
    }
}

/* ======================================================================
 * Parsing
 * ====================================================================== */
struct ci_scan { const char *s; int n, i; };

static void sk_ws(struct ci_scan *k) { while (k->i < k->n && ci_ws(k->s[k->i])) k->i++; }

/* A CSS <number>, scanned and converted in one pass. Deliberately NOT strtod:
 * the CSS grammar is a subset (no hex, no inf/nan, no leading whitespace once
 * the caller has skipped it) and accepting more than the grammar here would
 * make ci_transform_parse say yes to a declaration the cascade must drop. */
static int sk_num(struct ci_scan *k, double *out)
{
    sk_ws(k);
    int st = k->i, seen = 0, neg = 0;
    double mant = 0.0;
    int dexp = 0;

    if (k->i < k->n && (k->s[k->i] == '+' || k->s[k->i] == '-')) {
        neg = (k->s[k->i] == '-');
        k->i++;
    }
    while (k->i < k->n && k->s[k->i] >= '0' && k->s[k->i] <= '9') {
        mant = mant * 10.0 + (k->s[k->i] - '0');
        k->i++; seen = 1;
    }
    if (k->i < k->n && k->s[k->i] == '.') {
        k->i++;
        while (k->i < k->n && k->s[k->i] >= '0' && k->s[k->i] <= '9') {
            mant = mant * 10.0 + (k->s[k->i] - '0');
            dexp--; k->i++; seen = 1;
        }
    }
    if (!seen) { k->i = st; return 0; }

    if (k->i < k->n && (k->s[k->i] == 'e' || k->s[k->i] == 'E')) {
        int save = k->i;
        k->i++;
        int eneg = 0, e = 0, d = 0;
        if (k->i < k->n && (k->s[k->i] == '+' || k->s[k->i] == '-')) {
            eneg = (k->s[k->i] == '-'); k->i++;
        }
        while (k->i < k->n && k->s[k->i] >= '0' && k->s[k->i] <= '9') {
            if (e < 10000) e = e * 10 + (k->s[k->i] - '0');
            k->i++; d = 1;
        }
        /* `1e` with no digits is not an exponent -- the `e` belongs to the
         * unit that follows (`1em`), so the scan backs up rather than
         * swallowing it. */
        if (!d) k->i = save;
        else dexp += eneg ? -e : e;
    }

    double v = (dexp == 0) ? mant : mant * ci_pow10(dexp);
    *out = neg ? -v : v;
    return 1;
}

static int sk_unit(struct ci_scan *k, char *buf, int max)
{
    int o = 0;
    if (k->i < k->n && k->s[k->i] == '%') { k->i++; if (max > 1) { buf[0] = '%'; buf[1] = 0; } return 1; }
    while (k->i < k->n && ((ci_lc(k->s[k->i]) >= 'a' && ci_lc(k->s[k->i]) <= 'z'))) {
        if (o < max - 1) buf[o++] = (char)ci_lc(k->s[k->i]);
        k->i++;
    }
    buf[o] = 0;
    return o;
}

/* A <length> made absolute. Returns 0 on an unknown unit. Viewport units are
 * NOT here on purpose: this file has no viewport, and answering them with a
 * guess would be worse than declining the value. */
static int len_px(double v, const char *u, double fs, double root, double *out)
{
    if (!u[0])                  { *out = v;                 return v == 0.0; }  /* unitless: only 0 */
    if (!strcmp(u, "px"))       { *out = v;                 return 1; }
    if (!strcmp(u, "em"))       { *out = v * fs;            return 1; }
    if (!strcmp(u, "rem"))      { *out = v * root;          return 1; }
    if (!strcmp(u, "ex"))       { *out = v * fs * 0.5;      return 1; }
    if (!strcmp(u, "ch"))       { *out = v * fs * 0.5;      return 1; }
    if (!strcmp(u, "in"))       { *out = v * 96.0;          return 1; }
    if (!strcmp(u, "cm"))       { *out = v * 96.0 / 2.54;   return 1; }
    if (!strcmp(u, "mm"))       { *out = v * 96.0 / 25.4;   return 1; }
    if (!strcmp(u, "q"))        { *out = v * 96.0 / 101.6;  return 1; }
    if (!strcmp(u, "pt"))       { *out = v * 96.0 / 72.0;   return 1; }
    if (!strcmp(u, "pc"))       { *out = v * 16.0;          return 1; }
    return 0;
}

/* An <angle> ALWAYS carries a unit. `rotate(10)` and even `rotate(0)` are
 * parse errors -- unitless zero is a valid <length>, never a valid <angle> --
 * and css/css-transforms/parsing asks about exactly that. Accepting a bare
 * number here would make CSS.supports say yes to a declaration the cascade
 * must drop. */
static int ang_rad(double v, const char *u, double *out)
{
    if (!strcmp(u, "deg")) { *out = v * CI_PI / 180.0; return 1; }
    if (!strcmp(u, "rad"))          { *out = v;                 return 1; }
    if (!strcmp(u, "grad"))         { *out = v * CI_PI / 200.0; return 1; }
    if (!strcmp(u, "turn"))         { *out = v * CI_PI * 2.0;   return 1; }
    return 0;
}

struct fninfo { const char *name; unsigned char kind; unsigned char nargs; };

/* Longest first is not needed -- the name is matched whole, up to '('. */
static const struct fninfo g_fns[] = {
    { "translate",   CI_TRANSLATE,   2 }, { "translatex",  CI_TRANSLATEX,  1 },
    { "translatey",  CI_TRANSLATEY,  1 }, { "translatez",  CI_TRANSLATEZ,  1 },
    { "translate3d", CI_TRANSLATE3D, 3 },
    { "scale",       CI_SCALE,       2 }, { "scalex",      CI_SCALEX,      1 },
    { "scaley",      CI_SCALEY,      1 }, { "scalez",      CI_SCALEZ,      1 },
    { "scale3d",     CI_SCALE3D,     3 },
    { "rotate",      CI_ROTATE,      1 }, { "rotatex",     CI_ROTATEX,     1 },
    { "rotatey",     CI_ROTATEY,     1 }, { "rotatez",     CI_ROTATEZ,     1 },
    { "rotate3d",    CI_ROTATE3D,    4 },
    { "skew",        CI_SKEW,        2 }, { "skewx",       CI_SKEWX,       1 },
    { "skewy",       CI_SKEWY,       1 },
    { "matrix",      CI_MATRIX,      6 }, { "matrix3d",    CI_MATRIX3D,   16 },
    { "perspective", CI_PERSPECTIVE, 1 },
    { 0, 0, 0 }
};

int ci_transform_parse(const char *s, int len, double fs_px, double root_px,
                       struct ci_xform *out)
{
    if (!s || !out) return -1;
    if (len < 0) len = (int)strlen(s);
    out->n = 0;
    struct ci_scan k = { s, len, 0 };
    sk_ws(&k);
    if (k.i + 4 <= k.n && ci_lc(s[k.i]) == 'n' && ci_lc(s[k.i+1]) == 'o' &&
        ci_lc(s[k.i+2]) == 'n' && ci_lc(s[k.i+3]) == 'e') {
        k.i += 4; sk_ws(&k);
        return (k.i == k.n) ? 0 : -1;
    }
    while (1) {
        sk_ws(&k);
        if (k.i >= k.n) break;
        int ns = k.i;
        while (k.i < k.n && k.s[k.i] != '(' && !ci_ws(k.s[k.i])) k.i++;
        int nl = k.i - ns;
        if (nl <= 0 || nl > 15) return -1;
        char nm[16];
        for (int q = 0; q < nl; q++) nm[q] = (char)ci_lc(s[ns + q]);
        nm[nl] = 0;
        sk_ws(&k);
        if (k.i >= k.n || k.s[k.i] != '(') return -1;
        k.i++;
        const struct fninfo *fi = 0;
        for (int q = 0; g_fns[q].name; q++)
            if (!strcmp(g_fns[q].name, nm)) { fi = &g_fns[q]; break; }
        if (!fi) return -1;
        if (out->n >= CI_MAXFN) return -1;

        struct ci_fn *f = &out->f[out->n];
        memset(f, 0, sizeof *f);
        f->kind = fi->kind;

        /* perspective(none) -- the identity, spelled. */
        sk_ws(&k);
        if (fi->kind == CI_PERSPECTIVE && k.i + 4 <= k.n && ci_lc(k.s[k.i]) == 'n' &&
            ci_lc(k.s[k.i+1]) == 'o' && ci_lc(k.s[k.i+2]) == 'n' && ci_lc(k.s[k.i+3]) == 'e') {
            k.i += 4; sk_ws(&k);
            if (k.i >= k.n || k.s[k.i] != ')') return -1;
            k.i++;
            f->a[0] = 0.0;                      /* 0 means "no perspective" here */
            f->nargs = 1;
            out->n++;
            continue;
        }

        int na = 0;
        while (1) {
            double v;
            if (!sk_num(&k, &v)) return -1;
            char u[8];
            sk_unit(&k, u, sizeof u);
            if (na >= 16) return -1;
            switch (fi->kind) {
            case CI_TRANSLATE: case CI_TRANSLATEX: case CI_TRANSLATEY:
            case CI_TRANSLATEZ: case CI_TRANSLATE3D:
                if (u[0] == '%') {
                    /* translateZ takes no percentage -- the spec is explicit,
                     * because there is no z-axis reference length. */
                    if (fi->kind == CI_TRANSLATEZ || (fi->kind == CI_TRANSLATE3D && na == 2))
                        return -1;
                    f->pc[na] = v; f->haspc = 1;
                } else if (!len_px(v, u, fs_px, root_px, &f->a[na])) return -1;
                break;
            case CI_PERSPECTIVE:
                if (!len_px(v, u, fs_px, root_px, &f->a[na])) return -1;
                break;
            case CI_ROTATE: case CI_ROTATEX: case CI_ROTATEY: case CI_ROTATEZ:
                if (!ang_rad(v, u, &f->a[na])) return -1;
                break;
            case CI_ROTATE3D:
                if (na < 3) { if (u[0]) return -1; f->a[na] = v; }
                else if (!ang_rad(v, u, &f->a[na])) return -1;
                break;
            case CI_SKEW: case CI_SKEWX: case CI_SKEWY:
                if (!ang_rad(v, u, &f->a[na])) return -1;
                break;
            default:                                     /* scale, matrix: <number> */
                if (u[0]) return -1;
                f->a[na] = v;
                break;
            }
            na++;
            sk_ws(&k);
            if (k.i < k.n && k.s[k.i] == ',') { k.i++; continue; }
            if (k.i < k.n && k.s[k.i] == ')') { k.i++; break; }
            /* Whitespace-separated arguments are not valid CSS in any of
             * these functions; a missing comma is a parse error, not a
             * separator. */
            return -1;
        }

        /* Argument-count rules, and the omitted-second-argument defaults the
         * WPT transform_translate_second_omited / scale tests ask about. */
        switch (fi->kind) {
        case CI_TRANSLATE:
            if (na != 1 && na != 2) return -1;
            break;                                    /* ty defaults to 0, already zeroed */
        case CI_SCALE:
            if (na == 1) { f->a[1] = f->a[0]; }        /* sy defaults to sx */
            else if (na != 2) return -1;
            break;
        case CI_SKEW:
            if (na != 1 && na != 2) return -1;
            break;                                    /* ay defaults to 0 */
        default:
            if (na != fi->nargs) return -1;
            break;
        }
        f->nargs = fi->nargs;
        out->n++;
    }
    return 0;
}

/* ======================================================================
 * Serialisation
 * ====================================================================== */
static int ob(char *out, int outmax, int o, const char *s)
{
    while (*s && o < outmax - 1) out[o++] = *s++;
    out[o] = 0;
    return o;
}

static int ob_num(char *out, int outmax, int o, double v)
{
    char b[48];
    ci_num_text(v, b, sizeof b);
    return ob(out, outmax, o, b);
}

int ci_matrix_text(const double m[16], char *out, int outmax)
{
    if (!out || outmax <= 0) return 0;
    out[0] = 0;
    int o = 0;
    if (ci_is_2d(m)) {
        static const int idx[6] = { 0, 1, 4, 5, 12, 13 };
        o = ob(out, outmax, o, "matrix(");
        for (int i = 0; i < 6; i++) {
            if (i) o = ob(out, outmax, o, ", ");
            o = ob_num(out, outmax, o, m[idx[i]]);
        }
        return ob(out, outmax, o, ")");
    }
    o = ob(out, outmax, o, "matrix3d(");
    for (int i = 0; i < 16; i++) {
        if (i) o = ob(out, outmax, o, ", ");
        o = ob_num(out, outmax, o, m[i]);
    }
    return ob(out, outmax, o, ")");
}

/* A length argument, which may be a length, a percentage, or -- part-way
 * through an interpolation between the two -- a calc() of both. */
static int ob_lp(char *out, int outmax, int o, double px, double pc, int haspc)
{
    if (!haspc || pc == 0.0) { o = ob_num(out, outmax, o, px); return ob(out, outmax, o, "px"); }
    if (px == 0.0) { o = ob_num(out, outmax, o, pc); return ob(out, outmax, o, "%"); }
    o = ob(out, outmax, o, "calc(");
    o = ob_num(out, outmax, o, px);
    o = ob(out, outmax, o, "px ");
    o = ob(out, outmax, o, pc < 0 ? "- " : "+ ");
    o = ob_num(out, outmax, o, pc < 0 ? -pc : pc);
    o = ob(out, outmax, o, "%)");
    return o;
}

static int ob_deg(char *out, int outmax, int o, double rad)
{
    o = ob_num(out, outmax, o, rad * 180.0 / CI_PI);
    return ob(out, outmax, o, "deg");
}

int ci_transform_text(const struct ci_xform *t, char *out, int outmax)
{
    if (!out || outmax <= 0) return 0;
    out[0] = 0;
    if (!t || t->n == 0) return ob(out, outmax, 0, "none");
    int o = 0;
    for (int i = 0; i < t->n; i++) {
        const struct ci_fn *f = &t->f[i];
        if (i) o = ob(out, outmax, o, " ");
        const char *nm = "";
        for (int q = 0; g_fns[q].name; q++)
            if (g_fns[q].kind == f->kind) { nm = g_fns[q].name; break; }
        /* The canonical spellings differ from the lookup keys in case only. */
        static const char *const disp[CI__NFN] = {
            "translate", "translateX", "translateY", "translateZ", "translate3d",
            "scale", "scaleX", "scaleY", "scaleZ", "scale3d",
            "rotate", "rotateX", "rotateY", "rotateZ", "rotate3d",
            "skew", "skewX", "skewY", "matrix", "matrix3d", "perspective"
        };
        nm = (f->kind < CI__NFN) ? disp[f->kind] : nm;
        o = ob(out, outmax, o, nm);
        o = ob(out, outmax, o, "(");
        switch (f->kind) {
        case CI_TRANSLATE:
            o = ob_lp(out, outmax, o, f->a[0], f->pc[0], f->haspc);
            if (f->a[1] != 0.0 || f->pc[1] != 0.0) {
                o = ob(out, outmax, o, ", ");
                o = ob_lp(out, outmax, o, f->a[1], f->pc[1], f->haspc);
            }
            break;
        case CI_TRANSLATEX: case CI_TRANSLATEY:
            o = ob_lp(out, outmax, o, f->a[0], f->pc[0], f->haspc);
            break;
        case CI_TRANSLATEZ:
            o = ob_num(out, outmax, o, f->a[0]); o = ob(out, outmax, o, "px");
            break;
        case CI_TRANSLATE3D:
            o = ob_lp(out, outmax, o, f->a[0], f->pc[0], f->haspc);
            o = ob(out, outmax, o, ", ");
            o = ob_lp(out, outmax, o, f->a[1], f->pc[1], f->haspc);
            o = ob(out, outmax, o, ", ");
            o = ob_num(out, outmax, o, f->a[2]); o = ob(out, outmax, o, "px");
            break;
        case CI_SCALE:
            o = ob_num(out, outmax, o, f->a[0]);
            if (f->a[1] != f->a[0]) { o = ob(out, outmax, o, ", "); o = ob_num(out, outmax, o, f->a[1]); }
            break;
        case CI_SCALEX: case CI_SCALEY: case CI_SCALEZ:
            o = ob_num(out, outmax, o, f->a[0]);
            break;
        case CI_SCALE3D:
            for (int q = 0; q < 3; q++) { if (q) o = ob(out, outmax, o, ", "); o = ob_num(out, outmax, o, f->a[q]); }
            break;
        case CI_ROTATE: case CI_ROTATEX: case CI_ROTATEY: case CI_ROTATEZ:
            o = ob_deg(out, outmax, o, f->a[0]);
            break;
        case CI_ROTATE3D:
            for (int q = 0; q < 3; q++) { o = ob_num(out, outmax, o, f->a[q]); o = ob(out, outmax, o, ", "); }
            o = ob_deg(out, outmax, o, f->a[3]);
            break;
        case CI_SKEW:
            o = ob_deg(out, outmax, o, f->a[0]);
            if (f->a[1] != 0.0) { o = ob(out, outmax, o, ", "); o = ob_deg(out, outmax, o, f->a[1]); }
            break;
        case CI_SKEWX: case CI_SKEWY:
            o = ob_deg(out, outmax, o, f->a[0]);
            break;
        case CI_MATRIX:
            for (int q = 0; q < 6; q++) { if (q) o = ob(out, outmax, o, ", "); o = ob_num(out, outmax, o, f->a[q]); }
            break;
        case CI_MATRIX3D:
            for (int q = 0; q < 16; q++) { if (q) o = ob(out, outmax, o, ", "); o = ob_num(out, outmax, o, f->a[q]); }
            break;
        case CI_PERSPECTIVE:
            if (f->a[0] > 0.0) { o = ob_num(out, outmax, o, f->a[0]); o = ob(out, outmax, o, "px"); }
            else o = ob(out, outmax, o, "none");
            break;
        default: break;
        }
        o = ob(out, outmax, o, ")");
    }
    return o;
}

/* ======================================================================
 * Decomposition -- css-transforms-2 "decomposing a 3D matrix", transcribed.
 * ====================================================================== */
static double v3_len(const double v[3]) { return sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }

static void v3_scale(double v[3], double s) { v[0] *= s; v[1] *= s; v[2] *= s; }

static double v3_dot(const double a[3], const double b[3])
{ return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static void v3_cross(const double a[3], const double b[3], double o[3])
{
    double r[3] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0] };
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}

/* o = a * s1 + b * s2 */
static void v3_comb(const double a[3], const double b[3], double s1, double s2, double o[3])
{
    double r[3] = { a[0]*s1 + b[0]*s2, a[1]*s1 + b[1]*s2, a[2]*s1 + b[2]*s2 };
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}

static int m4_inverse(const double m[16], double inv[16]);

int ci_decompose(const double m0[16], struct ci_decomp *d)
{
    double m[16];
    if (m0[15] == 0.0) return 0;
    for (int i = 0; i < 16; i++) m[i] = m0[i] / m0[15];

    /* Perspective. The spec solves for it against the matrix with its
     * perspective column cleared. */
    double pm[16];
    memcpy(pm, m, sizeof pm);
    pm[3] = pm[7] = pm[11] = 0.0;
    pm[15] = 1.0;
    double pinv[16];
    if (!m4_inverse(pm, pinv)) return 0;

    if (m[3] != 0.0 || m[7] != 0.0 || m[11] != 0.0) {
        /* m = P * pm, and P differs from the identity only in its last ROW,
         * which is exactly (m[3], m[7], m[11], m[15]). So that row is
         * p * pm, and p = rhs * inverse(pm) -- read out of pinv along its
         * ROWS, which is the transpose the spec spells as a separate step. */
        double rhs[4] = { m[3], m[7], m[11], m[15] };
        for (int i = 0; i < 4; i++) {
            double s = 0.0;
            for (int j = 0; j < 4; j++) s += rhs[j] * pinv[i * 4 + j];
            d->perspective[i] = s;
        }
    } else {
        d->perspective[0] = d->perspective[1] = d->perspective[2] = 0.0;
        d->perspective[3] = 1.0;
    }

    d->translate[0] = m[12];
    d->translate[1] = m[13];
    d->translate[2] = m[14];

    double row[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) row[i][j] = m[i * 4 + j];

    d->scale[0] = v3_len(row[0]);
    if (d->scale[0] != 0.0) v3_scale(row[0], 1.0 / d->scale[0]);

    d->skew[0] = v3_dot(row[0], row[1]);
    v3_comb(row[1], row[0], 1.0, -d->skew[0], row[1]);
    d->scale[1] = v3_len(row[1]);
    if (d->scale[1] != 0.0) v3_scale(row[1], 1.0 / d->scale[1]);
    if (d->scale[1] != 0.0) d->skew[0] /= d->scale[1];

    d->skew[1] = v3_dot(row[0], row[2]);
    v3_comb(row[2], row[0], 1.0, -d->skew[1], row[2]);
    d->skew[2] = v3_dot(row[1], row[2]);
    v3_comb(row[2], row[1], 1.0, -d->skew[2], row[2]);
    d->scale[2] = v3_len(row[2]);
    if (d->scale[2] != 0.0) {
        v3_scale(row[2], 1.0 / d->scale[2]);
        d->skew[1] /= d->scale[2];
        d->skew[2] /= d->scale[2];
    }

    /* A negative determinant means one axis is mirrored; the spec flips all
     * three scales and all three rows so the rotation stays a rotation. */
    double pdum[3];
    v3_cross(row[1], row[2], pdum);
    if (v3_dot(row[0], pdum) < 0.0) {
        for (int i = 0; i < 3; i++) {
            d->scale[i] = -d->scale[i];
            row[i][0] = -row[i][0]; row[i][1] = -row[i][1]; row[i][2] = -row[i][2];
        }
    }

    double t = row[0][0] + row[1][1] + row[2][2] + 1.0;
    double s, x, y, z, w;
    if (t > 1e-4) {
        s = 0.5 / sqrt(t);
        w = 0.25 / s;
        x = (row[2][1] - row[1][2]) * s;
        y = (row[0][2] - row[2][0]) * s;
        z = (row[1][0] - row[0][1]) * s;
    } else if (row[0][0] > row[1][1] && row[0][0] > row[2][2]) {
        s = sqrt(1.0 + row[0][0] - row[1][1] - row[2][2]) * 2.0;
        x = 0.25 * s;
        y = (row[0][1] + row[1][0]) / s;
        z = (row[0][2] + row[2][0]) / s;
        w = (row[2][1] - row[1][2]) / s;
    } else if (row[1][1] > row[2][2]) {
        s = sqrt(1.0 + row[1][1] - row[0][0] - row[2][2]) * 2.0;
        x = (row[0][1] + row[1][0]) / s;
        y = 0.25 * s;
        z = (row[1][2] + row[2][1]) / s;
        w = (row[0][2] - row[2][0]) / s;
    } else {
        s = sqrt(1.0 + row[2][2] - row[0][0] - row[1][1]) * 2.0;
        x = (row[0][2] + row[2][0]) / s;
        y = (row[1][2] + row[2][1]) / s;
        z = 0.25 * s;
        w = (row[1][0] - row[0][1]) / s;
    }
    d->quaternion[0] = x; d->quaternion[1] = y;
    d->quaternion[2] = z; d->quaternion[3] = w;
    return 1;
}

void ci_recompose(const struct ci_decomp *d, double m[16])
{
    ci_identity(m);
    m[3]  = d->perspective[0];
    m[7]  = d->perspective[1];
    m[11] = d->perspective[2];
    m[15] = d->perspective[3];

    double tm[16];
    ci_identity(tm);
    tm[12] = d->translate[0]; tm[13] = d->translate[1]; tm[14] = d->translate[2];
    ci_mul(m, tm, m);

    double x = d->quaternion[0], y = d->quaternion[1];
    double z = d->quaternion[2], w = d->quaternion[3];
    double rm[16];
    ci_identity(rm);
    rm[0] = 1.0 - 2.0 * (y * y + z * z);
    rm[1] = 2.0 * (x * y - z * w);
    rm[2] = 2.0 * (x * z + y * w);
    rm[4] = 2.0 * (x * y + z * w);
    rm[5] = 1.0 - 2.0 * (x * x + z * z);
    rm[6] = 2.0 * (y * z - x * w);
    rm[8] = 2.0 * (x * z - y * w);
    rm[9] = 2.0 * (y * z + x * w);
    rm[10] = 1.0 - 2.0 * (x * x + y * y);
    ci_mul(m, rm, m);

    /* ONE skew matrix carrying all three shears, not three multiplied in
     * sequence. The decomposition's Gram-Schmidt produces
     *     col1 = s1 * (R_col1 + skew_xy * R_col0)
     *     col2 = s2 * (R_col2 + skew_xz * R_col0 + skew_yz * R_col1)
     * which is M = R * Sk * Sc for a SINGLE Sk holding xy, xz and yz at
     * once. Three sequential shear matrices multiply the cross terms
     * together and do not reproduce that -- the round-trip in
     * tests/unit/interp_test.c fails on exactly this if they are split. */
    double sm[16];
    ci_identity(sm);
    sm[4] = d->skew[0];                       /* xy */
    sm[8] = d->skew[1];                       /* xz */
    sm[9] = d->skew[2];                       /* yz */
    ci_mul(m, sm, m);

    ci_identity(sm);
    sm[0] = d->scale[0]; sm[5] = d->scale[1]; sm[10] = d->scale[2];
    ci_mul(m, sm, m);
}

static void quat_slerp(const double a[4], const double b[4], double p, double o[4])
{
    double q1[4] = { a[0], a[1], a[2], a[3] };
    double q2[4] = { b[0], b[1], b[2], b[3] };
    double dot = q1[0]*q2[0] + q1[1]*q2[1] + q1[2]*q2[2] + q1[3]*q2[3];
    if (dot < -1.0) dot = -1.0;
    if (dot > 1.0) dot = 1.0;
    if (dot > 0.9999995) {                       /* parallel: lerp and renormalise */
        for (int i = 0; i < 4; i++) o[i] = q1[i] + p * (q2[i] - q1[i]);
        double L = sqrt(o[0]*o[0] + o[1]*o[1] + o[2]*o[2] + o[3]*o[3]);
        if (L > 0.0) for (int i = 0; i < 4; i++) o[i] /= L;
        return;
    }
    double th = acos(dot), st = sin(th);
    double w1 = sin((1.0 - p) * th) / st;
    double w2 = sin(p * th) / st;
    for (int i = 0; i < 4; i++) o[i] = q1[i] * w1 + q2[i] * w2;
}

void ci_decomp_interp(const struct ci_decomp *a, const struct ci_decomp *b,
                      double p, struct ci_decomp *o)
{
#define L(x) ((x##a) + ((x##b) - (x##a)) * p)
    for (int i = 0; i < 3; i++) {
        double xa = a->translate[i], xb = b->translate[i]; o->translate[i] = L(x);
    }
    for (int i = 0; i < 3; i++) {
        double xa = a->scale[i], xb = b->scale[i]; o->scale[i] = L(x);
    }
    /* The skews interpolate as their TANGENTS -- which is what the decomposed
     * form already holds, since the shear factors come out of the matrix as
     * tan(angle). Interpolating angles here instead would be a different
     * curve that happens to agree at 0 and 1. */
    for (int i = 0; i < 3; i++) {
        double xa = a->skew[i], xb = b->skew[i]; o->skew[i] = L(x);
    }
    for (int i = 0; i < 4; i++) {
        double xa = a->perspective[i], xb = b->perspective[i]; o->perspective[i] = L(x);
    }
#undef L
    quat_slerp(a->quaternion, b->quaternion, p, o->quaternion);
}

/* 4x4 inverse by cofactors. Returns 0 for a singular matrix -- which is not an
 * error case to paper over: a singular transform cannot be decomposed, and the
 * spec says the interpolation falls back to the discrete rule. */
static int m4_inverse(const double m[16], double inv[16])
{
    double a[16];
    a[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    a[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    a[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    a[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    a[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    a[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    a[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    a[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    a[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    a[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    a[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    a[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    a[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    a[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    a[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    a[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    double det = m[0]*a[0] + m[1]*a[4] + m[2]*a[8] + m[3]*a[12];
    if (det == 0.0) return 0;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) inv[i] = a[i] * det;
    return 1;
}

/* ======================================================================
 * Transform list interpolation
 * ====================================================================== */
static int g_decomps;
int ci_decompose_count(void) { return g_decomps; }
void ci_decompose_count_reset(void) { g_decomps = 0; }

static int fam(int kind)
{
    switch (kind) {
    case CI_TRANSLATE: case CI_TRANSLATEX: case CI_TRANSLATEY:
    case CI_TRANSLATEZ: case CI_TRANSLATE3D: return 1;
    case CI_SCALE: case CI_SCALEX: case CI_SCALEY:
    case CI_SCALEZ: case CI_SCALE3D: return 2;
    case CI_ROTATE: case CI_ROTATEX: case CI_ROTATEY:
    case CI_ROTATEZ: case CI_ROTATE3D: return 3;
    case CI_SKEW: case CI_SKEWX: case CI_SKEWY: return 4;
    case CI_PERSPECTIVE: return 5;
    default: return 6;                                    /* matrix / matrix3d */
    }
}

static int is3d(int kind)
{
    return kind == CI_TRANSLATEZ || kind == CI_TRANSLATE3D ||
           kind == CI_SCALEZ || kind == CI_SCALE3D ||
           kind == CI_ROTATEX || kind == CI_ROTATEY || kind == CI_ROTATE3D ||
           kind == CI_MATRIX3D;
}

/* The identity of a function KIND -- what the shorter of two lists is padded
 * with. Note scale's identity is 1 and translate's is 0, and getting that
 * backwards produces an animation that collapses the element at one end. */
static void fn_identity(int kind, struct ci_fn *f)
{
    memset(f, 0, sizeof *f);
    f->kind = (unsigned char)kind;
    switch (kind) {
    case CI_SCALE: case CI_SCALE3D: f->a[0] = f->a[1] = f->a[2] = 1.0; break;
    case CI_SCALEX: case CI_SCALEY: case CI_SCALEZ: f->a[0] = 1.0; break;
    case CI_ROTATE3D: f->a[0] = 0.0; f->a[1] = 0.0; f->a[2] = 1.0; f->a[3] = 0.0; break;
    case CI_MATRIX: f->a[0] = 1; f->a[1] = 0; f->a[2] = 0; f->a[3] = 1; f->a[4] = 0; f->a[5] = 0; break;
    case CI_MATRIX3D: { double m[16]; ci_identity(m); for (int i = 0; i < 16; i++) f->a[i] = m[i]; break; }
    case CI_PERSPECTIVE: f->a[0] = 0.0; break;            /* perspective(none) */
    default: break;                                        /* translate/skew: zeros */
    }
}

/* Normalise a function to its family's common primitive so two different
 * spellings of the same thing (translateX and translate3d) interpolate
 * componentwise instead of falling to the matrix path. */
static void to_primitive(const struct ci_fn *in, int want, struct ci_fn *o)
{
    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)want;
    switch (want) {
    case CI_TRANSLATE:
    case CI_TRANSLATE3D:
        switch (in->kind) {
        case CI_TRANSLATEY: o->a[1] = in->a[0]; o->pc[1] = in->pc[0]; break;
        case CI_TRANSLATEZ: o->a[2] = in->a[0]; break;
        default:
            o->a[0] = in->a[0]; o->pc[0] = in->pc[0];
            if (in->kind != CI_TRANSLATEX) { o->a[1] = in->a[1]; o->pc[1] = in->pc[1]; }
            if (in->kind == CI_TRANSLATE3D) o->a[2] = in->a[2];
            break;
        }
        o->haspc = (o->pc[0] != 0.0 || o->pc[1] != 0.0) ? 1 : in->haspc;
        break;
    case CI_SCALE:
    case CI_SCALE3D:
        o->a[0] = o->a[1] = o->a[2] = 1.0;
        switch (in->kind) {
        case CI_SCALEX: o->a[0] = in->a[0]; break;
        case CI_SCALEY: o->a[1] = in->a[0]; break;
        case CI_SCALEZ: o->a[2] = in->a[0]; break;
        default:
            o->a[0] = in->a[0]; o->a[1] = in->a[1];
            if (in->kind == CI_SCALE3D) o->a[2] = in->a[2];
            break;
        }
        break;
    case CI_ROTATE3D:
        switch (in->kind) {
        case CI_ROTATEX: o->a[0] = 1; o->a[3] = in->a[0]; break;
        case CI_ROTATEY: o->a[1] = 1; o->a[3] = in->a[0]; break;
        case CI_ROTATE3D: o->a[0] = in->a[0]; o->a[1] = in->a[1]; o->a[2] = in->a[2]; o->a[3] = in->a[3]; break;
        default: o->a[2] = 1; o->a[3] = in->a[0]; break;   /* rotate / rotateZ */
        }
        break;
    case CI_SKEW:
        if (in->kind == CI_SKEWY) o->a[1] = in->a[0];
        else { o->a[0] = in->a[0]; if (in->kind == CI_SKEW) o->a[1] = in->a[1]; }
        break;
    default:
        *o = *in;
        o->kind = (unsigned char)want;
        break;
    }
}

/* The common primitive for a pair, or -1 when there is none. */
static int primitive_for(int ka, int kb)
{
    if (ka == kb) return ka;
    int fa = fam(ka), fb = fam(kb);
    if (fa != fb) return -1;
    switch (fa) {
    case 1: return (is3d(ka) || is3d(kb)) ? CI_TRANSLATE3D : CI_TRANSLATE;
    case 2: return (is3d(ka) || is3d(kb)) ? CI_SCALE3D : CI_SCALE;
    case 3: return CI_ROTATE3D;
    case 4: return CI_SKEW;
    case 5: return CI_PERSPECTIVE;
    default: return -1;                                   /* matrix vs matrix3d */
    }
}

static double lerp(double a, double b, double p) { return a + (b - a) * p; }

/* rotate3d interpolation, which has two regimes and the boundary between them
 * is where implementations get it wrong. Colinear axes (including the case
 * where one angle is zero, so its axis carries no information) interpolate the
 * ANGLE about the shared axis; anything else goes through quaternion slerp. */
static void interp_rotate3d(const struct ci_fn *a, const struct ci_fn *b,
                            double p, struct ci_fn *o)
{
    double ax[3] = { a->a[0], a->a[1], a->a[2] }, aa = a->a[3];
    double bx[3] = { b->a[0], b->a[1], b->a[2] }, ba = b->a[3];
    double la = v3_len(ax), lb = v3_len(bx);
    if (la > 0.0) v3_scale(ax, 1.0 / la);
    if (lb > 0.0) v3_scale(bx, 1.0 / lb);

    int za = (aa == 0.0) || la == 0.0, zb = (ba == 0.0) || lb == 0.0;
    if (za && !zb) { ax[0] = bx[0]; ax[1] = bx[1]; ax[2] = bx[2]; za = 0; }
    else if (zb && !za) { bx[0] = ax[0]; bx[1] = ax[1]; bx[2] = ax[2]; }

    double dot = v3_dot(ax, bx);
    if (fabs(dot - 1.0) < 1e-9 || (za && zb)) {
        memset(o, 0, sizeof *o);
        o->kind = CI_ROTATE3D;
        o->a[0] = (za && zb) ? 0.0 : ax[0];
        o->a[1] = (za && zb) ? 0.0 : ax[1];
        o->a[2] = (za && zb) ? 1.0 : ax[2];
        o->a[3] = lerp(aa, ba, p);
        return;
    }

    double qa[4], qb[4], q[4];
    double ha = aa * 0.5, hb = ba * 0.5;
    qa[0] = ax[0] * sin(ha); qa[1] = ax[1] * sin(ha); qa[2] = ax[2] * sin(ha); qa[3] = cos(ha);
    qb[0] = bx[0] * sin(hb); qb[1] = bx[1] * sin(hb); qb[2] = bx[2] * sin(hb); qb[3] = cos(hb);
    quat_slerp(qa, qb, p, q);

    memset(o, 0, sizeof *o);
    o->kind = CI_ROTATE3D;
    double w = q[3];
    if (w > 1.0) w = 1.0;
    if (w < -1.0) w = -1.0;
    double half = acos(w), sh = sin(half);
    if (fabs(sh) < 1e-12) { o->a[2] = 1.0; o->a[3] = 0.0; return; }
    o->a[0] = q[0] / sh; o->a[1] = q[1] / sh; o->a[2] = q[2] / sh;
    o->a[3] = 2.0 * half;
}

/* One matched pair. */
static void interp_fn(const struct ci_fn *a, const struct ci_fn *b, int kind,
                      double p, struct ci_fn *o)
{
    struct ci_fn pa, pb;
    to_primitive(a, kind, &pa);
    to_primitive(b, kind, &pb);

    if (kind == CI_ROTATE3D) { interp_rotate3d(&pa, &pb, p, o); return; }

    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)kind;

    if (kind == CI_PERSPECTIVE) {
        /* The RECIPROCAL interpolates, not the depth -- the decomposed form of
         * perspective(d) holds -1/d, and perspective(none) is 1/d == 0. WPT's
         * transform-interpolation-001.html states this in its own helper:
         *   1 / ((1 - p) / from + p / to)
         * Interpolating the depths directly passes at 0 and 1 and is wrong
         * everywhere between. */
        double ra = (pa.a[0] > 0.0) ? 1.0 / pa.a[0] : 0.0;
        double rb = (pb.a[0] > 0.0) ? 1.0 / pb.a[0] : 0.0;
        double r = lerp(ra, rb, p);
        o->a[0] = (r > 0.0) ? 1.0 / r : 0.0;
        return;
    }

    int n = 16;
    switch (kind) {
    case CI_TRANSLATE: case CI_SCALE: case CI_SKEW: n = 2; break;
    case CI_TRANSLATEX: case CI_TRANSLATEY: case CI_TRANSLATEZ:
    case CI_SCALEX: case CI_SCALEY: case CI_SCALEZ:
    case CI_SKEWX: case CI_SKEWY:
    case CI_ROTATE: case CI_ROTATEX: case CI_ROTATEY: case CI_ROTATEZ: n = 1; break;
    case CI_TRANSLATE3D: case CI_SCALE3D: n = 3; break;
    case CI_MATRIX: n = 6; break;
    case CI_MATRIX3D: n = 16; break;
    default: n = 4; break;
    }
    for (int i = 0; i < n; i++) o->a[i] = lerp(pa.a[i], pb.a[i], p);
    if (pa.haspc || pb.haspc) {
        o->haspc = 1;
        for (int i = 0; i < 3 && i < n; i++) o->pc[i] = lerp(pa.pc[i], pb.pc[i], p);
    }
    o->nargs = (unsigned char)n;
}

void ci_transform_interp(const struct ci_xform *a, const struct ci_xform *b,
                         double p, struct ci_xform *out)
{
    int na = a ? a->n : 0, nb = b ? b->n : 0;
    int n = na > nb ? na : nb;

    /* Do the lists match position by position, once the shorter is padded with
     * identities of the longer's types? matrix/matrix3d pairs deliberately do
     * NOT count as matched: the spec interpolates those by decomposition even
     * when the two functions are the same, because componentwise interpolation
     * of matrix entries is not a rotation. */
    int matched = 1;
    for (int i = 0; i < n && matched; i++) {
        int ka = (i < na) ? a->f[i].kind : -1;
        int kb = (i < nb) ? b->f[i].kind : -1;
        if (ka < 0) ka = kb;
        if (kb < 0) kb = ka;
        if (fam(ka) == 6 || fam(kb) == 6) { matched = 0; break; }
        if (primitive_for(ka, kb) < 0) matched = 0;
    }

#ifdef CI_NEGCTL_NO_DECOMPOSE
    /* THE NEGATIVE CONTROL, and it is not "remove interpolation".
     *
     * With this defined, mismatched lists are interpolated COMPONENTWISE
     * anyway -- pairing rotate against scale and lerping their arguments as if
     * they were the same function. Every animation between two translate()s
     * stays perfect, every page still looks right, and every interpolation
     * between two different function types gets a plausible, smooth, finite,
     * wrong answer. That is the bug this file exists to not have, and
     * tests/interp.mk requires the suite to catch it. */
    matched = 1;
#endif

    if (matched && n <= CI_MAXFN) {
        out->n = n;
        for (int i = 0; i < n; i++) {
            struct ci_fn fa, fb;
            int ka = (i < na) ? a->f[i].kind : -1;
            int kb = (i < nb) ? b->f[i].kind : -1;
            if (ka < 0) { fn_identity(kb, &fa); ka = kb; } else fa = a->f[i];
            if (kb < 0) { fn_identity(ka, &fb); kb = ka; } else fb = b->f[i];
            int prim = primitive_for(ka, kb);
            if (prim < 0) prim = ka;
            interp_fn(&fa, &fb, prim, p, &out->f[i]);
        }
        return;
    }

    /* The decomposition path. */
    g_decomps++;
    double ma[16], mb[16];
    ci_transform_matrix(a, 0, 0, ma);
    ci_transform_matrix(b, 0, 0, mb);
    struct ci_decomp da, db, dm;
    double m[16];
    if (!ci_decompose(ma, &da) || !ci_decompose(mb, &db)) {
        /* A singular endpoint cannot be decomposed; the spec falls back to the
         * discrete rule, flipping at the halfway point. */
        const struct ci_xform *pick = (p < 0.5) ? a : b;
        if (pick) *out = *pick; else out->n = 0;
        return;
    }
    ci_decomp_interp(&da, &db, p, &dm);
    ci_recompose(&dm, m);

    out->n = 1;
    memset(&out->f[0], 0, sizeof out->f[0]);
    if (ci_is_2d(m)) {
        out->f[0].kind = CI_MATRIX;
        out->f[0].nargs = 6;
        static const int idx[6] = { 0, 1, 4, 5, 12, 13 };
        for (int i = 0; i < 6; i++) out->f[0].a[i] = m[idx[i]];
    } else {
        out->f[0].kind = CI_MATRIX3D;
        out->f[0].nargs = 16;
        for (int i = 0; i < 16; i++) out->f[0].a[i] = m[i];
    }
}

/* ======================================================================
 * Generic values -- structural componentwise interpolation
 *
 * NOT a per-property table, and that is the design decision worth defending.
 * A table of ~200 properties and their value types is 200 chances to be wrong
 * and it goes stale the day LibCSS gains a property. What every interpolable
 * CSS value has in common is its SHAPE: the two endpoints agree token for
 * token except in their numbers. `10px 20px` against `30px 40px`,
 * `rgb(0, 0, 0)` against `rgb(255, 0, 0)`, `2px solid red` against
 * `10px solid red` -- same shape, interpolate the numbers, copy everything
 * else through.
 *
 * When the shapes disagree, this declines (-1) rather than guessing, and the
 * caller applies the discrete rule. Declining is the safe direction: a
 * discrete flip is a legal CSS behaviour for some pair of values, while a
 * blended answer for a pair that does not blend is never legal for any.
 * ====================================================================== */
struct tok { int isnum; double v; const char *s; int n; };

static int tokenize(const char *s, int len, struct tok *t, int max)
{
    int i = 0, k = 0;
    while (i < len) {
        while (i < len && ci_ws(s[i])) i++;
        if (i >= len) break;
        if (k >= max) return -1;
        char c = s[i];
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            struct ci_scan sc = { s, len, i };
            double v;
            if (sk_num(&sc, &v)) {
                t[k].isnum = 1; t[k].v = v; t[k].s = s + i; t[k].n = sc.i - i;
                i = sc.i;
                k++;
                continue;
            }
        }
        int st = i;
        while (i < len && !ci_ws(s[i]) &&
               !(s[i] == '-' || s[i] == '+' || s[i] == '.' || (s[i] >= '0' && s[i] <= '9')))
            i++;
        if (i == st) i++;                          /* never stall */
        t[k].isnum = 0; t[k].s = s + st; t[k].n = i - st;
        k++;
    }
    return k;
}

static const char *const g_discrete[] = {
    "display", "visibility", "position", "float", "clear", "overflow",
    "overflow-x", "overflow-y", "box-sizing", "white-space", "font-family",
    "text-align", "text-transform", "list-style-type", "border-top-style",
    "border-right-style", "border-bottom-style", "border-left-style",
    "flex-direction", "flex-wrap", "justify-content", "align-items",
    "align-self", "align-content", "transform-style", "backface-visibility",
    "transform-box", "animation-name", "transition-property", "content",
    "cursor", "direction", "unicode-bidi", "table-layout", "caption-side",
    "empty-cells", "pointer-events", "mix-blend-mode", "isolation",
    "object-fit", "resize", "text-overflow", "word-break", "overflow-wrap",
    0
};

int ci_prop_is_discrete(const char *prop)
{
    if (!prop) return 0;
    for (int i = 0; g_discrete[i]; i++) if (!strcmp(g_discrete[i], prop)) return 1;
    return 0;
}

int ci_value_interp(const char *prop, const char *from, const char *to,
                    double p, char *out, int outmax)
{
    if (!from || !to || !out || outmax <= 0) return -1;
    out[0] = 0;

    /* `transform` has its own API: it needs the element's box for percentages
     * and a choice between the computed and the resolved serialisation, and
     * neither is expressible through a string-in/string-out call. Declining
     * here rather than half-answering is what stops a caller from getting the
     * componentwise-always behaviour by accident. */
    if (prop && !strcmp(prop, "transform")) return -1;
    if (ci_prop_is_discrete(prop)) return -1;

    int lf = (int)strlen(from), lt = (int)strlen(to);
    struct tok ta[64], tb[64];
    int na = tokenize(from, lf, ta, 64);
    int nb = tokenize(to, lt, tb, 64);
    if (na < 0 || nb < 0 || na != nb || na == 0) return -1;
    for (int i = 0; i < na; i++) {
        if (ta[i].isnum != tb[i].isnum) return -1;
        if (!ta[i].isnum) {
            if (ta[i].n != tb[i].n || memcmp(ta[i].s, tb[i].s, (size_t)ta[i].n)) return -1;
        }
    }

    int o = 0;
    int wrote = 0;
    for (int i = 0; i < na; i++) {
        /* Space only between tokens that were separated in the source; the
         * unit suffix of a number is a separate non-numeric token and must
         * stay glued to it. */
        if (wrote && (ta[i].s > from) && ci_ws(ta[i].s[-1])) o = ob(out, outmax, o, " ");
        if (ta[i].isnum) o = ob_num(out, outmax, o, lerp(ta[i].v, tb[i].v, p));
        else {
            for (int q = 0; q < ta[i].n && o < outmax - 1; q++) out[o++] = ta[i].s[q];
            out[o] = 0;
        }
        wrote = 1;
    }
    return o;
}

/* ======================================================================
 * Composite operations: add and accumulate
 *
 * WHAT A COMPOSITE OPERATION IS, stated once so the rest is short. A keyframe
 * carries a value AND a rule for how that value meets the value the element
 * already has:
 *
 *     replace      the keyframe value IS the value                (default)
 *     add          combine it with the underlying value by ADDITION
 *     accumulate   combine it with the underlying value by ACCUMULATION
 *
 * Composition happens FIRST and interpolation happens to the results, which is
 * why this is a value-level operation and not a timing one: `from add [100px]`
 * over an underlying `50px` is an endpoint of 150px, and 150px is what then
 * interpolates towards the other composed endpoint.
 *
 * THE MEASUREMENT THAT SIZED THIS, and it is not the one the work order had.
 * 2,122 `Compositing ...` subtests fail in css/. 1,928 of them never reach a
 * value at all -- they fail on `assert_true(CSS.supports(property, value))`
 * three lines earlier, on values LibCSS still rejects (mask-border-*,
 * border-image-*, `min-content` for width, four-value background-position,
 * box-shadow, offset-rotate). Those belong to the value-parser line and no
 * amount of correct composition moves one of them. The 194 that DO reach a
 * value are all this, and every one of them was wrong by exactly the
 * underlying value.
 * ====================================================================== */

int ci_composite_op(const char *name)
{
    if (!name) return CI_COMPOSITE_REPLACE;
    if (!strcmp(name, "add")) return CI_COMPOSITE_ADD;
    if (!strcmp(name, "accumulate")) return CI_COMPOSITE_ACCUMULATE;
    return CI_COMPOSITE_REPLACE;
}

/* ---- transform lists ----------------------------------------------------
 *
 * Addition is CONCATENATION, and that is the spec's whole definition: the
 * result is the underlying list followed by the value's list. It is not a
 * componentwise sum and it is not a matrix product taken early -- the two
 * lists stay separate functions so a later interpolation can still match them
 * up function by function.
 *
 * The overflow rule is the one place this can lose information: CI_MAXFN caps
 * a list at 24 functions and two long lists concatenate past it. Rather than
 * truncate -- which silently drops transforms off the end of an animation --
 * the whole thing collapses to the matrix of the concatenation, which is the
 * same transform written as one function instead of a different transform
 * written as a prefix. */
void ci_transform_add(const struct ci_xform *u, const struct ci_xform *v,
                      struct ci_xform *out)
{
    int nu = u ? u->n : 0, nv = v ? v->n : 0;

    if (nu + nv <= CI_MAXFN) {
        struct ci_xform r;
        r.n = 0;
        for (int i = 0; i < nu; i++) r.f[r.n++] = u->f[i];
        for (int i = 0; i < nv; i++) r.f[r.n++] = v->f[i];
        *out = r;
        return;
    }

    /* Too long to keep as a list: multiply the two matrices, in order. */
    double mu[16], mv[16], m[16];
    ci_transform_matrix(u, 0, 0, mu);
    ci_transform_matrix(v, 0, 0, mv);
    ci_mul(mu, mv, m);
    out->n = 1;
    memset(&out->f[0], 0, sizeof out->f[0]);
    if (ci_is_2d(m)) {
        out->f[0].kind = CI_MATRIX; out->f[0].nargs = 6;
        static const int idx[6] = { 0, 1, 4, 5, 12, 13 };
        for (int i = 0; i < 6; i++) out->f[0].a[i] = m[idx[i]];
    } else {
        out->f[0].kind = CI_MATRIX3D; out->f[0].nargs = 16;
        for (int i = 0; i < 16; i++) out->f[0].a[i] = m[i];
    }
}

/* Everything from here to ci_transform_accumulate is the componentwise
 * machinery, and it is compiled out under the negative control -- which is not
 * tidiness: the control's whole claim is that `accumulate` reaches none of it,
 * and leaving it linked but unreachable would let a stray caller keep it alive
 * and quietly weaken the control. Two unused-function warnings in the control
 * build would say the same thing less clearly. */
#ifndef CI_NEGCTL_ACCUM_IS_ADD

/* How many arguments the primitive `kind` carries. The same table interp_fn
 * uses; a function rather than a second literal copy, because two copies
 * drifting apart shows up as a serialisation short by one argument, which
 * reads as a value bug and is not one. */
static int prim_nargs(int kind)
{
    switch (kind) {
    case CI_TRANSLATE: case CI_SCALE: case CI_SKEW: return 2;
    case CI_TRANSLATEX: case CI_TRANSLATEY: case CI_TRANSLATEZ:
    case CI_SCALEX: case CI_SCALEY: case CI_SCALEZ:
    case CI_SKEWX: case CI_SKEWY:
    case CI_ROTATE: case CI_ROTATEX: case CI_ROTATEY: case CI_ROTATEZ:
    case CI_PERSPECTIVE: return 1;
    case CI_TRANSLATE3D: case CI_SCALE3D: return 3;
    case CI_ROTATE3D: return 4;
    case CI_MATRIX: return 6;
    case CI_MATRIX3D: return 16;
    default: return 4;
    }
}

/* Accumulate one matched pair into `o`. Returns 0 when the pair cannot
 * accumulate, which forces the WHOLE list back to concatenation -- a list that
 * accumulated in some positions and concatenated in others would be a
 * transform nobody specified.
 *
 * THE RULE THAT IS NOT ADDITION: a scale factor accumulates as `a + b - 1`.
 * Scale is multiplicative about 1, so accumulating a doubling onto a doubling
 * is a trebling and not a quadrupling. WPT states it outright --
 * transform-scale-composition.html: underlying `scaleX(2)`, accumulateFrom
 * `scaleX(3)`, at progress 0 the answer is `scaleX(4)`. Everything else here
 * adds: translations, angles, skews, matrix-decomposition components. */
static int accum_fn(const struct ci_fn *a, const struct ci_fn *b, int kind,
                    struct ci_fn *o)
{
    struct ci_fn pa, pb;
    to_primitive(a, kind, &pa);
    to_primitive(b, kind, &pb);

    memset(o, 0, sizeof *o);
    o->kind = (unsigned char)kind;

    if (kind == CI_ROTATE3D) {
        /* Two rotations accumulate only about a COMMON axis; otherwise the sum
         * of the angles means nothing and the answer is the concatenation. A
         * zero-angle rotation has no axis to disagree about and takes the
         * other's, which is what makes rotate(0) a true identity here. */
        double la = sqrt(pa.a[0]*pa.a[0] + pa.a[1]*pa.a[1] + pa.a[2]*pa.a[2]);
        double lb = sqrt(pb.a[0]*pb.a[0] + pb.a[1]*pb.a[1] + pb.a[2]*pb.a[2]);
        double ax[3] = { 0, 0, 1 };
        if (la > 1e-9 && lb > 1e-9) {
            double na[3] = { pa.a[0]/la, pa.a[1]/la, pa.a[2]/la };
            double nb[3] = { pb.a[0]/lb, pb.a[1]/lb, pb.a[2]/lb };
            int same = fabs(na[0]-nb[0]) < 1e-6 && fabs(na[1]-nb[1]) < 1e-6 &&
                       fabs(na[2]-nb[2]) < 1e-6;
            if (same) { ax[0]=na[0]; ax[1]=na[1]; ax[2]=na[2]; }
            else if (fabs(pa.a[3]) < 1e-12) { ax[0]=nb[0]; ax[1]=nb[1]; ax[2]=nb[2]; }
            else if (fabs(pb.a[3]) < 1e-12) { ax[0]=na[0]; ax[1]=na[1]; ax[2]=na[2]; }
            else return 0;
        } else if (la > 1e-9) { ax[0]=pa.a[0]/la; ax[1]=pa.a[1]/la; ax[2]=pa.a[2]/la; }
        else if (lb > 1e-9)   { ax[0]=pb.a[0]/lb; ax[1]=pb.a[1]/lb; ax[2]=pb.a[2]/lb; }
        o->a[0] = ax[0]; o->a[1] = ax[1]; o->a[2] = ax[2];
        o->a[3] = pa.a[3] + pb.a[3];
        o->nargs = 4;
        return 1;
    }

    if (kind == CI_PERSPECTIVE) {
        /* perspective accumulates on the reciprocal the interpolation uses,
         * for the same reason: the decomposed quantity is 1/d, and
         * perspective(none) is 1/d == 0. */
        double ra = (pa.a[0] > 0.0) ? 1.0 / pa.a[0] : 0.0;
        double rb = (pb.a[0] > 0.0) ? 1.0 / pb.a[0] : 0.0;
        double r = ra + rb;
        o->a[0] = (r > 0.0) ? 1.0 / r : 0.0;
        o->nargs = 1;
        return 1;
    }

    int n = prim_nargs(kind);
    int isscale = (fam(kind) == 2);
    for (int i = 0; i < n; i++)
        o->a[i] = isscale ? (pa.a[i] + pb.a[i] - 1.0) : (pa.a[i] + pb.a[i]);
    if (pa.haspc || pb.haspc) {
        o->haspc = 1;
        for (int i = 0; i < 3 && i < n; i++) o->pc[i] = pa.pc[i] + pb.pc[i];
    }
    o->nargs = (unsigned char)n;
    return 1;
}

/* Accumulate two matrices through their decompositions.
 *
 * A matrix has no components to accumulate until it is taken apart, and taking
 * it apart is what makes `matrix(0, 1, -1, 0, 100, 0)` -- a rotate 90 with a
 * translate 100 -- accumulate a pure translateX(100px) into rotate-90 with
 * translate-200, rather than into an entrywise sum that is not a rotation at
 * all. transform-matrix-composition.html is the transcription source.
 *
 * The rotation composes as a QUATERNION PRODUCT and not a component sum: for
 * two rotations about one axis the product adds the angles, which is the
 * accumulation rule, and for different axes it gives the composed rotation
 * instead of nonsense. */
static int accum_matrix(const struct ci_fn *a, const struct ci_fn *b,
                        struct ci_fn *o)
{
    struct ci_xform xa, xb;
    double ma[16], mb[16], mr[16];
    xa.n = 1; xa.f[0] = *a;
    xb.n = 1; xb.f[0] = *b;
    ci_transform_matrix(&xa, 0, 0, ma);
    ci_transform_matrix(&xb, 0, 0, mb);

    struct ci_decomp da, db, dr;
    if (!ci_decompose(ma, &da) || !ci_decompose(mb, &db)) return 0;

    for (int i = 0; i < 3; i++) {
        dr.translate[i] = da.translate[i] + db.translate[i];
        dr.scale[i]     = da.scale[i] + db.scale[i] - 1.0;
        dr.skew[i]      = da.skew[i] + db.skew[i];
    }
    for (int i = 0; i < 3; i++) dr.perspective[i] = da.perspective[i] + db.perspective[i];
    /* perspective[3] is the homogeneous 1; summing it would double it. */
    dr.perspective[3] = 1.0;

    /* q = qa * qb, in (x, y, z, w) order. */
    const double *qa = da.quaternion, *qb = db.quaternion;
    dr.quaternion[0] = qa[3]*qb[0] + qa[0]*qb[3] + qa[1]*qb[2] - qa[2]*qb[1];
    dr.quaternion[1] = qa[3]*qb[1] - qa[0]*qb[2] + qa[1]*qb[3] + qa[2]*qb[0];
    dr.quaternion[2] = qa[3]*qb[2] + qa[0]*qb[1] - qa[1]*qb[0] + qa[2]*qb[3];
    dr.quaternion[3] = qa[3]*qb[3] - qa[0]*qb[0] - qa[1]*qb[1] - qa[2]*qb[2];

    ci_recompose(&dr, mr);
    memset(o, 0, sizeof *o);
    if (ci_is_2d(mr)) {
        o->kind = CI_MATRIX; o->nargs = 6;
        static const int idx[6] = { 0, 1, 4, 5, 12, 13 };
        for (int i = 0; i < 6; i++) o->a[i] = mr[idx[i]];
    } else {
        o->kind = CI_MATRIX3D; o->nargs = 16;
        for (int i = 0; i < 16; i++) o->a[i] = mr[i];
    }
    return 1;
}

#endif /* !CI_NEGCTL_ACCUM_IS_ADD */

int ci_transform_accumulate(const struct ci_xform *u, const struct ci_xform *v,
                            struct ci_xform *out)
{
#ifdef CI_NEGCTL_ACCUM_IS_ADD
    /* THE NEGATIVE CONTROL, and it is deliberately not "delete accumulate".
     *
     * `accumulate` behaving as `add` is exactly what a careful implementation
     * written FROM MEMORY produces, because for every scalar type the two ARE
     * the same operation -- a length, a percentage, a number and a colour
     * channel all add either way, and scalars are most of the corpus. The
     * distinction lives entirely in list-valued types, where add concatenates
     * and accumulate combines componentwise, and the two answers RENDER
     * almost alike: `scaleX(2) scaleX(3)` against `scaleX(4)` is a third in
     * one axis and nothing about the page looks broken.
     *
     * tests/interp.mk requires the suite to go red here. If it does not, the
     * suite is testing scalars and is not testing accumulation at all. */
    ci_transform_add(u, v, out);
    return 0;
#else
    int nu = u ? u->n : 0, nv = v ? v->n : 0;

    /* `none` has nothing to disagree with, so it accumulates as the identity
     * on the other side rather than forcing a concatenation. */
    if (nu == 0) { if (v) *out = *v; else out->n = 0; return 1; }
    if (nv == 0) { *out = *u; return 1; }

    if (nu != nv || nu > CI_MAXFN) { ci_transform_add(u, v, out); return 0; }

    struct ci_xform r;
    r.n = nu;
    for (int i = 0; i < nu; i++) {
        int ka = u->f[i].kind, kb = v->f[i].kind;
        if (fam(ka) == 6 || fam(kb) == 6) {
            if (!accum_matrix(&u->f[i], &v->f[i], &r.f[i])) {
                ci_transform_add(u, v, out); return 0;
            }
            continue;
        }
        int prim = primitive_for(ka, kb);
        if (prim < 0 || !accum_fn(&u->f[i], &v->f[i], prim, &r.f[i])) {
            ci_transform_add(u, v, out); return 0;
        }
    }
    *out = r;
    return 1;
#endif
}

int ci_transform_composite(const struct ci_xform *u, const struct ci_xform *v,
                           int op, struct ci_xform *out)
{
    if (!v || !out) return -1;
    if (op == CI_COMPOSITE_ADD)        { ci_transform_add(u, v, out); return 0; }
    if (op == CI_COMPOSITE_ACCUMULATE) return ci_transform_accumulate(u, v, out);
    return -1;                                       /* replace: nothing to do */
}

/* ---- generic values -----------------------------------------------------
 *
 * The same structural argument ci_value_interp makes, one operation over: two
 * computed values of the same property have the same SHAPE, so combining them
 * is combining their numbers and copying the rest through. `50px` with
 * `100px` is `150px`; `rgb(50, 50, 50)` with `rgb(10, 10, 10)` is
 * `rgb(60, 60, 60)`; `2px solid red` with `3px solid red` is `5px solid red`.
 * A shape mismatch declines and the caller uses the value unchanged, which is
 * what the spec says a type with no addition defined does.
 *
 * Scalars add under BOTH operations, and the whole care in this file is to
 * stop that fact leaking into the list case above.
 *
 * NOT HANDLED, and named rather than left to be discovered: a length against a
 * percentage. `10%` add `100px` is `calc(100px + 10%)` in a real browser and
 * this declines it, because a calc() the rest of the pipeline cannot read back
 * is worse than the value unchanged -- css_computed_text has no calc()
 * serialisation, so the comparison would be against a string nothing here can
 * produce. It costs 5 of the 194 reachable subtests, and closing it is a
 * css_engine change before it is a css_interp one. */
int ci_value_composite(const char *prop, const char *underlying,
                       const char *value, int op, char *out, int outmax)
{
    if (!out || outmax <= 0) return -1;
    out[0] = 0;
    if (op != CI_COMPOSITE_ADD && op != CI_COMPOSITE_ACCUMULATE) return -1;
    if (!value) return -1;
    /* Nothing to combine with. Saying so with -1 rather than copying `value`
     * keeps "composited" and "not composited" distinguishable at the call
     * site, which is what lets js_anim.c stay silent instead of inventing an
     * answer for a property the engine does not report. */
    if (!underlying || !*underlying) return -1;

    /* The transform family goes through ci_transform_composite, which needs
     * the parsed list. Declining here is the guard ci_value_interp already
     * uses and for the same reason: a structural token sum over two transform
     * lists quietly produces a componentwise answer where the spec says
     * concatenate. */
    if (prop && (!strcmp(prop, "transform") || !strcmp(prop, "rotate") ||
                 !strcmp(prop, "scale") || !strcmp(prop, "translate")))
        return -1;
    /* A discrete type has no addition defined: the value replaces. */
    if (ci_prop_is_discrete(prop)) return -1;

    int lu = (int)strlen(underlying), lv = (int)strlen(value);
    struct tok tu[64], tv[64];
    int nu = tokenize(underlying, lu, tu, 64);
    int nv = tokenize(value, lv, tv, 64);
    if (nu < 0 || nv < 0 || nu != nv || nu == 0) return -1;
    for (int i = 0; i < nu; i++) {
        if (tu[i].isnum != tv[i].isnum) return -1;
        if (!tu[i].isnum) {
            if (tu[i].n != tv[i].n || memcmp(tu[i].s, tv[i].s, (size_t)tu[i].n)) return -1;
        }
    }

    int o = 0, wrote = 0;
    for (int i = 0; i < nu; i++) {
        if (wrote && (tv[i].s > value) && ci_ws(tv[i].s[-1])) o = ob(out, outmax, o, " ");
        if (tv[i].isnum) o = ob_num(out, outmax, o, tu[i].v + tv[i].v);
        else {
            for (int q = 0; q < tv[i].n && o < outmax - 1; q++) out[o++] = tv[i].s[q];
            out[o] = 0;
        }
        wrote = 1;
    }
    return o;
}
