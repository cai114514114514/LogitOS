/* c/lib/audio/amath.h -- the transcendentals the transform codecs need,
 * from scratch.
 *
 * WHY THIS FILE EXISTS AT ALL.  c/lib/audio links LIBC_OBJS and nothing else
 * (see the AUD_OBJ rules in the Makefile): mini-libc has no libm, and the
 * musl libm subset under third_party/ is linked only into the browser and the
 * JS engine. MP3 got away with it because every transcendental it needs is a
 * constant in the generated mp3_tables.h and its two run-time nonlinearities
 * (x^(4/3) and 2^(e/4)) are computed in mp3.c by hand. AAC, Vorbis and Opus
 * cannot: their MDCT sizes are chosen at run time from the bitstream (Opus
 * alone uses four different ones, and Vorbis's two block sizes are whatever
 * the identification header says), so the twiddle factors cannot be a table
 * baked at build time. That means a real sin/cos, and once there is a real
 * sin/cos there may as well be a correct exp2/log2/pow rather than three more
 * ad-hoc approximations.
 *
 * ACCURACY, AND WHY IT IS NOT DECORATION.  These feed conformance tests whose
 * whole content is a bound on the output error. A sloppy sin() shows up as a
 * noise floor in every MDCT and would be indistinguishable from a decoder bug
 * -- worse, it would move the score without moving any of the logic under
 * test. Every routine here is written to be accurate to a few ulp of double
 * and is checked against the host libm over a dense sweep in
 * tests/unit/amath_test.c, which is part of test-audio-codec-units. The bar
 * there is 4e-16 relative, i.e. a couple of ulp, not "close enough for audio".
 *
 * Everything is double. The codecs decode in double and quantise once at the
 * end, for the same reason mp3.c does: the conformance criterion is defined on
 * the decoder's floating-point output, so the arithmetic that produces it
 * should not be the thing spending the error budget.
 */
#ifndef LOGIT_AMATH_H
#define LOGIT_AMATH_H

#define A_PI  3.14159265358979323846
#define A_2PI 6.28318530717958647692

/* sin/cos for any finite argument. Reduced with a three-part pi/2 so the
 * twiddle arguments used by the transforms (which reach a few thousand
 * radians for the larger FFTs) do not lose low bits to cancellation. */
double a_sin(double x);
double a_cos(double x);

/* sqrt. On x86-64 this is the SSE2 instruction, which is correctly rounded by
 * definition; elsewhere a Newton iteration that converges to the same value.
 * The kernel builds -msse2 and so does userland (M15), so the fast path is the
 * one that runs. */
double a_sqrt(double x);

double a_exp2(double x);      /* 2^x */
double a_log2(double x);      /* log2(x); x <= 0 -> 0, which no caller passes */
double a_exp(double x);
double a_log(double x);
double a_pow(double x, double y);   /* x >= 0 */
double a_cbrt(double x);            /* signed cube root */

/* ldexp: x * 2^n, exactly, by exponent arithmetic. Used all over the codecs to
 * apply a power-of-two gain without going through pow(). */
double a_ldexp(double x, int n);

/* floor/fabs, so a decoder never reaches for the ones in math.h by habit. */
double a_floor(double x);
static inline double a_fabs(double x) { return x < 0 ? -x : x; }

#endif /* LOGIT_AMATH_H */
