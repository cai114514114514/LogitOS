/* c/lib/audio/amath.c -- see amath.h. From-scratch double-precision
 * transcendentals for the transform codecs.
 *
 * The shapes here are the textbook ones and are chosen for being checkable
 * rather than for being clever:
 *
 *   sin/cos   Cody-Waite argument reduction against a three-word pi/2 (so the
 *             reduced argument keeps its low bits even when x is a few
 *             thousand radians, which the larger FFT twiddle loops reach),
 *             then the Taylor series on |r| <= pi/4 where it converges fast
 *             enough that degree 15 is already below one ulp.
 *   exp2      split into an integer part done by exponent arithmetic and a
 *             fractional part in [-1/2, 1/2] done by the series for
 *             exp(f*ln2). Exact for integer x, which matters: the codecs use
 *             it for power-of-two gains.
 *   log2      frexp to a mantissa, shifted into [1/sqrt2, sqrt2) so the atanh
 *             series argument stays small, then the odd series for
 *             log((1+t)/(1-t)).
 *   pow       exp2(y*log2(x)), with the exact cases (integer y, x==2^k) not
 *             special-cased because no caller needs them exact.
 *   cbrt      bit-hack seed then Newton, which is what mp3.c already does; it
 *             is repeated here rather than shared because mp3.c is a shipped,
 *             conformance-measured decoder and this file is new code.
 */

#include <stdint.h>
#include "amath.h"

/* --- bit access ---------------------------------------------------------- */
/* Through a union rather than a cast: the fuzz harness builds this with
 * -fsanitize=undefined and a punned pointer dereference is exactly what that
 * flags. */
typedef union { double d; uint64_t u; } dbits;

double a_ldexp(double x, int n)
{
    if (x == 0.0 || !(x == x)) return x;
    /* Step in bounded chunks so a large n cannot overflow the exponent field
     * mid-way and produce a NaN out of a finite result. */
    while (n > 1000) { x *= 8.98846567431158e307 /* 2^1023 */; n -= 1023; }
    while (n < -1000) { x *= 1.1125369292536007e-308 /* 2^-1022 */; n += 1022; }
    dbits b;
    b.d = 1.0;
    b.u = (uint64_t)(1023 + n) << 52;
    return x * b.d;
}

double a_floor(double x)
{
    if (!(x == x)) return x;
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return x;  /* |x| >= 2^53 */
    double t = (double)(long long)x;
    return (t > x) ? t - 1.0 : t;
}

double a_sqrt(double x)
{
    if (!(x > 0.0)) return (x == 0.0) ? x : (x != x ? x : 0.0);
#if defined(__x86_64__) || defined(__i386__)
    /* SSE2 sqrtsd is correctly rounded, and both the host build and the
     * LogitOS build are x86-64 with SSE enabled (M15). */
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    /* Newton on the reciprocal square root, seeded from the exponent, then one
     * final Newton on the root itself in double. Converges to within an ulp. */
    dbits b; b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    dbits s; s.d = x;
    s.u = (s.u & 0x000FFFFFFFFFFFFFull) | ((uint64_t)1023 << 52);   /* m in [1,2) */
    double m = s.d;
    double r = 1.0 / (0.5 + 0.5 * m);           /* crude 1/sqrt(m) seed */
    for (int i = 0; i < 5; i++) r = r * (1.5 - 0.5 * m * r * r);
    double root = m * r;
    root = 0.5 * (root + m / root);
    root = 0.5 * (root + m / root);
    /* sqrt(x) = sqrt(m) * 2^(e/2) */
    if (e & 1) { root *= 1.4142135623730951; e -= 1; }
    return a_ldexp(root, e >> 1);
#endif
}

/* --- sin / cos ----------------------------------------------------------- */

/* pi/2 in three doubles: the sum is pi/2 to about 160 bits, so
 * x - k*PIO2_HI - k*PIO2_MI - k*PIO2_LO keeps full precision for the |x| the
 * transforms produce. */
#define PIO2_HI 1.57079632673412561417e+00   /* 0x3FF921FB54400000 */
#define PIO2_MI 6.07710050650619224932e-11   /* 0x3DD0B4611A626331 */
#define PIO2_LO 2.02226624879595063154e-21   /* 0x3BA3198A2E037073 */

/* sin(r) on |r| <= pi/4, Taylor to r^15. The next term is r^17/17! < 1e-22 at
 * r = pi/4, i.e. below the ulp of the result. */
static double sin_poly(double r)
{
    double z = r * r;
    return r + r * z * (-1.66666666666666657415e-01
        + z * ( 8.33333333333329961475e-03
        + z * (-1.98412698412696162806e-04
        + z * ( 2.75573192239198852272e-06
        + z * (-2.50521083761581193182e-08
        + z * ( 1.60590438125280493886e-10
        + z * (-7.64716373181981647590e-13)))))));
}

/* cos(r) on |r| <= pi/4, Taylor to r^16. */
static double cos_poly(double r)
{
    double z = r * r;
    return 1.0 + z * (-5.00000000000000000000e-01
        + z * ( 4.16666666666666643537e-02
        + z * (-1.38888888888888104986e-03
        + z * ( 2.48015873015272437954e-05
        + z * (-2.75573192096030452154e-07
        + z * ( 2.08767534970579600759e-09
        + z * (-1.14707451580261341571e-11
        + z * ( 4.77947733238738500856e-14))))))));
}

/* Reduce x to r in [-pi/4, pi/4] and a quadrant 0..3. */
static int reduce_pio2(double x, double *r)
{
    double q = x * 0.63661977236758134308;      /* 2/pi */
    q = (q >= 0) ? a_floor(q + 0.5) : -a_floor(-q + 0.5);
    /* q can be enormous for a wild argument; the transforms never produce one,
     * but a fuzzed stream must not make this loop or misbehave. */
    if (!(q > -1e15 && q < 1e15)) { *r = 0.0; return 0; }
    double t = x - q * PIO2_HI;
    t = t - q * PIO2_MI;
    t = t - q * PIO2_LO;
    *r = t;
    long long k = (long long)q;
    return (int)(k & 3) < 0 ? (int)((k & 3) + 4) : (int)(k & 3);
}

double a_sin(double x)
{
    if (!(x == x)) return x;
    double r;
    int q = reduce_pio2(x, &r);
    switch (q) {
    case 0:  return  sin_poly(r);
    case 1:  return  cos_poly(r);
    case 2:  return -sin_poly(r);
    default: return -cos_poly(r);
    }
}

double a_cos(double x)
{
    if (!(x == x)) return x;
    double r;
    int q = reduce_pio2(x, &r);
    switch (q) {
    case 0:  return  cos_poly(r);
    case 1:  return -sin_poly(r);
    case 2:  return -cos_poly(r);
    default: return  sin_poly(r);
    }
}

/* --- exp2 / log2 --------------------------------------------------------- */

double a_exp2(double x)
{
    if (!(x == x)) return x;
    if (x > 1023.0)  return 8.98846567431158e307 * 8.98846567431158e307;   /* +inf */
    if (x < -1074.0) return 0.0;

    double n = a_floor(x + 0.5);
    double f = x - n;                    /* |f| <= 1/2 */
    double t = f * 0.69314718055994530942;   /* f*ln2, |t| <= 0.347 */

    /* exp(t) by Taylor; at |t| = 0.347 the t^14 term is ~2e-18 relative. */
    double s = 1.0 + t * (1.0
        + t * (1.0 / 2.0
        + t * (1.0 / 6.0
        + t * (1.0 / 24.0
        + t * (1.0 / 120.0
        + t * (1.0 / 720.0
        + t * (1.0 / 5040.0
        + t * (1.0 / 40320.0
        + t * (1.0 / 362880.0
        + t * (1.0 / 3628800.0
        + t * (1.0 / 39916800.0
        + t * (1.0 / 479001600.0))))))))))));
    return a_ldexp(s, (int)n);
}

double a_log2(double x)
{
    if (!(x > 0.0)) return 0.0;
    dbits b; b.d = x;
    int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    if (e == -1023) {                        /* subnormal: scale into range */
        b.d = x * 18014398509481984.0;       /* 2^54 */
        e = (int)((b.u >> 52) & 0x7FF) - 1023 - 54;
    }
    b.u = (b.u & 0x000FFFFFFFFFFFFFull) | ((uint64_t)1023 << 52);
    double m = b.d;                          /* [1, 2) */
    if (m > 1.4142135623730951) { m *= 0.5; e += 1; }   /* [1/sqrt2, sqrt2) */

    /* log(m) = 2*atanh(t), t = (m-1)/(m+1), |t| <= 0.1716. Truncating after
     * t^15 leaves 2/ln2 * t^17/17 = 1.6e-14 of relative error at the ends of
     * that range -- which is 75 ulp, and showed up immediately as the worst
     * case in the sweep against the host libm. Carried to t^21 the next term
     * is 4e-18, i.e. below the rounding of the terms already summed. */
    double t = (m - 1.0) / (m + 1.0);
    double z = t * t;
    double s = 1.0 / 21.0;
    s = 1.0 / 19.0 + z * s;
    s = 1.0 / 17.0 + z * s;
    s = 1.0 / 15.0 + z * s;
    s = 1.0 / 13.0 + z * s;
    s = 1.0 / 11.0 + z * s;
    s = 1.0 /  9.0 + z * s;
    s = 1.0 /  7.0 + z * s;
    s = 1.0 /  5.0 + z * s;
    s = 1.0 /  3.0 + z * s;
    s = 1.0        + z * s;
    s = t * s;
    return (double)e + 2.0 * s * 1.44269504088896340736;   /* * 1/ln2 */
}

double a_exp(double x) { return a_exp2(x * 1.44269504088896340736); }
double a_log(double x) { return a_log2(x) * 0.69314718055994530942; }

double a_pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (x == 0.0) return (y > 0.0) ? 0.0 : 0.0;
    if (x < 0.0) {
        /* Only integer exponents are meaningful; the codecs never ask. */
        double n = a_floor(y);
        if (n != y) return 0.0;
        double r = a_exp2(y * a_log2(-x));
        long long k = (long long)n;
        return (k & 1) ? -r : r;
    }
    return a_exp2(y * a_log2(x));
}

double a_cbrt(double x)
{
    if (x == 0.0 || !(x == x)) return x;
    int neg = x < 0.0;
    if (neg) x = -x;

    /* Seed from the exponent: dividing the biased exponent by three lands
     * within a few percent, and three Newton steps take it to full double. */
    dbits b; b.d = x;
    b.u = b.u / 3 + 0x2A9F76253119D328ull;
    double r = b.d;
    for (int i = 0; i < 4; i++) r = r - (r - x / (r * r)) * (1.0 / 3.0);
    return neg ? -r : r;
}
