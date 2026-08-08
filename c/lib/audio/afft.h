/* c/lib/audio/afft.h -- mixed-radix complex FFT and the inverse MDCT built on
 * it, shared by the transform codecs (AAC, Vorbis, Opus/CELT).
 *
 * WHY MIXED RADIX AND NOT JUST RADIX-2.  AAC and Vorbis have power-of-two
 * transform sizes and a radix-2 FFT would serve them. Opus does not: CELT's
 * frame sizes at 48 kHz are 120, 240, 480 and 960 samples, so its inverse MDCT
 * lengths are 240, 480, 960 and 1920 -- every one of them 2^a * 3 * 5. A
 * decoder that only did powers of two would have to fall back to the O(N^2)
 * transform for Opus, which is the format most likely to be decoded in real
 * time. So the plan factors N over {4, 2, 3, 5} and has a butterfly for each,
 * plus a generic one that would handle a prime factor if a format ever brought
 * one (none of these three do).
 *
 * WHY THE IMDCT IS THE SIMPLE FORMULATION.  The textbook fast IMDCT folds the
 * transform into an N/4-point complex FFT with a pre- and post-rotation. This
 * one uses a full N-point FFT instead:
 *
 *     A[k] = X[k] * e^{i*2*pi*n0*k/N}   for k < N/2, zero above
 *     B[n] = sum_k A[k] e^{i*2*pi*n*k/N}
 *     y[n] = Re{ e^{i*pi*(n+n0)/N} * B[n] }        n0 = N/4 + 1/2
 *
 * which is four times the arithmetic of the folded version and about forty
 * times less than the direct O(N^2) sum. That is a deliberate trade. The
 * folded version's index gymnastics are where MDCT implementations
 * traditionally go wrong, and a sign error there is a bug that still sounds
 * like music -- exactly the class this project's negative controls exist to
 * catch. This form is three lines of algebra from the definition, and
 * tests/unit/afft_test.c checks it against the definition at every size the
 * three codecs use. If a profile ever says the IMDCT is the bottleneck, the
 * folded version can replace it behind this same interface with the direct
 * formula still standing as the test oracle.
 *
 * Two symmetries that follow from the definition ARE used, because they are
 * free: y[N/2-1-j] = -y[j] folds the first half onto itself and
 * y[3N/2-1-j] = y[j] folds the second half onto itself, so a quarter of the
 * post-rotations produce the whole output.
 */
#ifndef LOGIT_AFFT_H
#define LOGIT_AFFT_H

typedef struct afft afft;

/* n must factor over {2,3,5} (any product; all sizes these codecs use do).
 * `inverse` selects the sign of the exponent: 0 gives e^{-i2pi nk/N},
 * 1 gives e^{+i2pi nk/N}. Neither scales by 1/N. Returns NULL on OOM. */
afft *afft_new(int n, int inverse);
void  afft_free(afft *f);

/* Out-of-place: in and out are n interleaved (re,im) pairs and must not
 * alias. */
void  afft_run(const afft *f, const double *in, double *out);

/* --- inverse MDCT -------------------------------------------------------- */

typedef struct amdct amdct;

/* n is the WINDOW length (the number of output samples); the transform takes
 * n/2 spectral coefficients. n must be a multiple of 4 and factor over
 * {2,3,5}. */
amdct *amdct_new(int n);
void   amdct_free(amdct *m);

/* y[0..n-1] = sum_{k<n/2} X[k] cos(2*pi/n * (k+1/2) * (i + 1/2 + n/4)).
 * No scaling is applied: AAC wants 2/N, Vorbis wants 1/N with its own sign
 * convention, CELT folds its scale into the window. Each caller applies its
 * own, which keeps this function the one thing the test oracle checks. */
void   amdct_imdct(const amdct *m, const double *X, double *y);

int    amdct_size(const amdct *m);

#endif /* LOGIT_AFFT_H */
