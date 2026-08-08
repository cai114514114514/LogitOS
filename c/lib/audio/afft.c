/* c/lib/audio/afft.c -- see afft.h.
 *
 * The FFT is the recursive Cooley-Tukey decimation-in-time arrangement: the
 * plan is a list of (radix, m) pairs, each level scatters its p sub-transforms
 * of length m and then applies the butterfly for that radix. Radices 4, 2, 3
 * and 5 have their own butterflies; anything else falls through to a generic
 * O(p^2) inner DFT, which none of these three codecs reaches.
 */

#include <stdlib.h>
#include <string.h>
#include "afft.h"
#include "amath.h"

#define AFFT_MAXFACTORS 32

typedef struct { double r, i; } acpx;

struct afft {
    int n;
    int inverse;
    acpx *tw;                    /* n twiddles: e^{sign*2pi*i*k/n} */
    int factors[2 * AFFT_MAXFACTORS];   /* (radix, m) pairs, terminated by 0 */
};

static inline acpx cmul(acpx a, acpx b)
{
    acpx r;
    r.r = a.r * b.r - a.i * b.i;
    r.i = a.r * b.i + a.i * b.r;
    return r;
}
static inline acpx cadd(acpx a, acpx b) { acpx r; r.r = a.r + b.r; r.i = a.i + b.i; return r; }
static inline acpx csub(acpx a, acpx b) { acpx r; r.r = a.r - b.r; r.i = a.i - b.i; return r; }

/* Fill the factor list. Prefers radix 4 over two radix 2 passes; the rest in
 * increasing order so a single 3 or 5 lands at the outermost level. */
static int plan_factors(int n, int *out)
{
    int nf = 0, p = 4;
    int rem = n;
    do {
        if (nf >= AFFT_MAXFACTORS - 1) return 0;
        while (rem % p) {
            switch (p) {
            case 4:  p = 2; break;
            case 2:  p = 3; break;
            default: p += 2; break;
            }
            if (p * p > rem) p = rem;      /* rem is prime */
        }
        rem /= p;
        out[2 * nf] = p;
        out[2 * nf + 1] = rem;
        nf++;
    } while (rem > 1);
    out[2 * nf] = 0;
    out[2 * nf + 1] = 0;
    return 1;
}

afft *afft_new(int n, int inverse)
{
    if (n <= 0 || n > (1 << 22)) return NULL;
    afft *f = (afft *)malloc(sizeof(*f));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->n = n;
    f->inverse = inverse ? 1 : 0;
    f->tw = (acpx *)malloc((size_t)n * sizeof(acpx));
    if (!f->tw) { free(f); return NULL; }
    double sign = inverse ? 1.0 : -1.0;
    for (int k = 0; k < n; k++) {
        double a = sign * A_2PI * (double)k / (double)n;
        f->tw[k].r = a_cos(a);
        f->tw[k].i = a_sin(a);
    }
    if (!plan_factors(n, f->factors)) { free(f->tw); free(f); return NULL; }
    return f;
}

void afft_free(afft *f)
{
    if (!f) return;
    free(f->tw);
    free(f);
}

static void bfly2(acpx *out, int stride, int m, const acpx *tw)
{
    for (int i = 0; i < m; i++) {
        acpx t = cmul(out[i + m], tw[i * stride]);
        out[i + m] = csub(out[i], t);
        out[i]     = cadd(out[i], t);
    }
}

static void bfly4(acpx *out, int stride, int m, const acpx *tw, int inverse)
{
    for (int i = 0; i < m; i++) {
        acpx s0 = out[i];
        acpx s1 = cmul(out[i + m],     tw[i * stride]);
        acpx s2 = cmul(out[i + 2 * m], tw[2 * i * stride]);
        acpx s3 = cmul(out[i + 3 * m], tw[3 * i * stride]);

        acpx t0 = cadd(s0, s2), t1 = csub(s0, s2);
        acpx t2 = cadd(s1, s3), t3 = csub(s1, s3);

        out[i]         = cadd(t0, t2);
        out[i + 2 * m] = csub(t0, t2);
        if (inverse) {
            out[i + m].r     = t1.r - t3.i;
            out[i + m].i     = t1.i + t3.r;
            out[i + 3 * m].r = t1.r + t3.i;
            out[i + 3 * m].i = t1.i - t3.r;
        } else {
            out[i + m].r     = t1.r + t3.i;
            out[i + m].i     = t1.i - t3.r;
            out[i + 3 * m].r = t1.r - t3.i;
            out[i + 3 * m].i = t1.i + t3.r;
        }
    }
}

static void bfly3(acpx *out, int stride, int m, const acpx *tw, int n)
{
    /* The imaginary part of e^{sign*2pi i/3}; sign is carried by tw. */
    double epi3 = tw[stride * m].i;
    for (int i = 0; i < m; i++) {
        acpx s0 = out[i];
        acpx s1 = cmul(out[i + m],     tw[i * stride]);
        acpx s2 = cmul(out[i + 2 * m], tw[2 * i * stride]);

        acpx t  = cadd(s1, s2);
        acpx d  = csub(s1, s2);

        out[i + m].r = s0.r - 0.5 * t.r;
        out[i + m].i = s0.i - 0.5 * t.i;

        out[i] = cadd(s0, t);

        out[i + 2 * m].r = out[i + m].r + epi3 * d.i;
        out[i + 2 * m].i = out[i + m].i - epi3 * d.r;
        out[i + m].r     = out[i + m].r - epi3 * d.i;
        out[i + m].i     = out[i + m].i + epi3 * d.r;
    }
    (void)n;
}

static void bfly5(acpx *out, int stride, int m, const acpx *tw, int n)
{
    acpx ya = tw[stride * m];
    acpx yb = tw[stride * 2 * m];
    for (int i = 0; i < m; i++) {
        acpx s0 = out[i];
        acpx s1 = cmul(out[i + m],     tw[i * stride]);
        acpx s2 = cmul(out[i + 2 * m], tw[2 * i * stride]);
        acpx s3 = cmul(out[i + 3 * m], tw[3 * i * stride]);
        acpx s4 = cmul(out[i + 4 * m], tw[4 * i * stride]);

        acpx a14 = cadd(s1, s4), d14 = csub(s1, s4);
        acpx a23 = cadd(s2, s3), d23 = csub(s2, s3);

        out[i].r = s0.r + a14.r + a23.r;
        out[i].i = s0.i + a14.i + a23.i;

        /* w = e^{sign*2pi*i/5}, ya = w, yb = w^2, and w^4 = conj(w),
         * w^3 = conj(w^2). Pairing q with 5-q turns each output into
         *
         *   X[1] = s0 + ya.r*a14 + yb.r*a23 + i*(ya.i*d14 + yb.i*d23)
         *   X[2] = s0 + yb.r*a14 + ya.r*a23 + i*(yb.i*d14 - ya.i*d23)
         *
         * and X[4], X[3] are the same with the i-term negated. Multiplying by
         * i is what decides the sign of each half: i*(u+iv) = -v + iu, so the
         * REAL part of the i-term is minus the imaginary part of the sum. */
        double b1r = s0.r + a14.r * ya.r + a23.r * yb.r;
        double b1i = s0.i + a14.i * ya.r + a23.i * yb.r;
        double c1r = -(d14.i * ya.i + d23.i * yb.i);
        double c1i =  (d14.r * ya.i + d23.r * yb.i);

        double b2r = s0.r + a14.r * yb.r + a23.r * ya.r;
        double b2i = s0.i + a14.i * yb.r + a23.i * ya.r;
        double c2r = -(d14.i * yb.i - d23.i * ya.i);
        double c2i =  (d14.r * yb.i - d23.r * ya.i);

        out[i + m].r     = b1r + c1r;  out[i + m].i     = b1i + c1i;
        out[i + 4 * m].r = b1r - c1r;  out[i + 4 * m].i = b1i - c1i;
        out[i + 2 * m].r = b2r + c2r;  out[i + 2 * m].i = b2i + c2i;
        out[i + 3 * m].r = b2r - c2r;  out[i + 3 * m].i = b2i - c2i;
    }
    (void)n;
}

/* Generic radix: a direct DFT of size p over the p sub-results. Correct for
 * any p and never fast; present so an unusual size degrades in speed rather
 * than in correctness. */
static void bfly_generic(acpx *out, int stride, int m, const acpx *tw, int n, int p)
{
    acpx scratch[32];
    if (p > 32) return;                     /* plan_factors never emits one */
    for (int u = 0; u < m; u++) {
        for (int q = 0; q < p; q++) scratch[q] = out[u + q * m];
        for (int k = 0; k < p; k++) {
            acpx acc = scratch[0];
            int twidx = 0;
            for (int q = 1; q < p; q++) {
                twidx += stride * (u + m * k);
                if (twidx >= n) twidx -= n;
                acc = cadd(acc, cmul(scratch[q], tw[twidx]));
            }
            out[u + k * m] = acc;
        }
    }
}

static void fft_work(const afft *f, acpx *out, const acpx *in,
                     int fstride, int in_stride, const int *factors)
{
    int p = factors[0];
    int m = factors[1];
    acpx *end = out + p * m;

    if (m == 1) {
        acpx *o = out;
        do {
            *o = *in;
            in += fstride * in_stride;
            o++;
        } while (o != end);
    } else {
        acpx *o = out;
        do {
            fft_work(f, o, in, fstride * p, in_stride, factors + 2);
            in += fstride * in_stride;
            o += m;
        } while (o != end);
    }

    switch (p) {
    case 2:  bfly2(out, fstride, m, f->tw); break;
    case 3:  bfly3(out, fstride, m, f->tw, f->n); break;
    case 4:  bfly4(out, fstride, m, f->tw, f->inverse); break;
    case 5:  bfly5(out, fstride, m, f->tw, f->n); break;
    default: bfly_generic(out, fstride, m, f->tw, f->n, p); break;
    }
}

void afft_run(const afft *f, const double *in, double *out)
{
    if (!f || !in || !out) return;
    fft_work(f, (acpx *)out, (const acpx *)in, 1, 1, f->factors);
}

/* --- inverse MDCT -------------------------------------------------------- */

struct amdct {
    int n;                 /* window length */
    afft *fft;             /* length n, inverse sign */
    acpx *pre;             /* n/2 pre-rotations  e^{i*2pi*n0*k/n} */
    acpx *post;            /* n/4 post-rotations e^{i*pi*(j+n0)/n}, j < n/4 */
    acpx *post2;           /* n/4 more, for j in [n/2, 3n/4) */
    acpx *buf;             /* n input scratch */
    acpx *spec;            /* n output scratch */
};

amdct *amdct_new(int n)
{
    if (n < 4 || (n & 3)) return NULL;
    amdct *m = (amdct *)malloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->n = n;
    m->fft   = afft_new(n, 1);
    m->pre   = (acpx *)malloc((size_t)(n / 2) * sizeof(acpx));
    m->post  = (acpx *)malloc((size_t)(n / 4) * sizeof(acpx));
    m->post2 = (acpx *)malloc((size_t)(n / 4) * sizeof(acpx));
    m->buf   = (acpx *)malloc((size_t)n * sizeof(acpx));
    m->spec  = (acpx *)malloc((size_t)n * sizeof(acpx));
    if (!m->fft || !m->pre || !m->post || !m->post2 || !m->buf || !m->spec) {
        amdct_free(m);
        return NULL;
    }

    double n0 = (double)(n / 4) + 0.5;
    for (int k = 0; k < n / 2; k++) {
        double a = A_2PI * n0 * (double)k / (double)n;
        m->pre[k].r = a_cos(a);
        m->pre[k].i = a_sin(a);
    }
    for (int j = 0; j < n / 4; j++) {
        double a = A_PI * ((double)j + n0) / (double)n;
        m->post[j].r = a_cos(a);
        m->post[j].i = a_sin(a);
        double b = A_PI * ((double)(j + n / 2) + n0) / (double)n;
        m->post2[j].r = a_cos(b);
        m->post2[j].i = a_sin(b);
    }
    return m;
}

void amdct_free(amdct *m)
{
    if (!m) return;
    afft_free(m->fft);
    free(m->pre); free(m->post); free(m->post2); free(m->buf); free(m->spec);
    free(m);
}

int amdct_size(const amdct *m) { return m ? m->n : 0; }

void amdct_imdct(const amdct *m, const double *X, double *y)
{
    if (!m || !X || !y) return;
    int n = m->n, h = n / 2, q = n / 4;

    for (int k = 0; k < h; k++) {
        m->buf[k].r = X[k] * m->pre[k].r;
        m->buf[k].i = X[k] * m->pre[k].i;
    }
    memset(m->buf + h, 0, (size_t)h * sizeof(acpx));

    afft_run(m->fft, (const double *)m->buf, (double *)m->spec);

    /* The definition gives two symmetries, each folding one half of the output
     * onto itself. Writing n0 = n/4 + 1/2, the arguments at index j and at
     * index j' sum to a multiple of 2*pi plus a constant:
     *
     *   j' = n/2 - 1 - j     (j + n0) + (j' + n0) = n       -> y[j'] = -y[j]
     *   j' = 3n/2 - 1 - j    (j + n0) + (j' + n0) = 2n      -> y[j'] = +y[j]
     *
     * The first maps [0, n/4) onto [n/4, n/2); the second maps [n/2, 3n/4)
     * onto [3n/4, n). So a quarter of the post-rotations produce a half of the
     * output, and two quarters produce all of it. */
    for (int j = 0; j < q; j++) {
        double v = m->spec[j].r * m->post[j].r - m->spec[j].i * m->post[j].i;
        y[j] = v;
        y[h - 1 - j] = -v;
    }
    for (int j = 0; j < q; j++) {
        int idx = j + h;
        double v = m->spec[idx].r * m->post2[j].r - m->spec[idx].i * m->post2[j].i;
        y[idx] = v;
        y[n - 1 - j] = v;
    }
}
