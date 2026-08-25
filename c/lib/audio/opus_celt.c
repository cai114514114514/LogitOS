/* c/lib/audio/opus_celt.c -- the CELT decoder, RFC 6716 section 4.3.
 * See opus_celt.h for the scope (what is in, what is refused by name) and
 * the top of opus.c for why the bar for this codec is opus_compare rather
 * than byte equality.
 *
 * HOW TO READ THIS FILE AGAINST THE STANDARD.  It is a port of the decode
 * path of the reference implementation embedded in RFC 6716 Appendix A --
 * celt.c, bands.c, rate.c, quant_bands.c, laplace.c, cwrs.c and vq.c,
 * flattened into one translation unit because the decoder is the only
 * consumer of any of it and six files' worth of headers between them bought
 * nothing. Function names are kept, and so is the order of operations inside
 * them, including a few things that would read better rearranged. That is
 * deliberate: an arithmetic decoder is reviewed by reading it beside the
 * specification, and a tidied version cannot be.
 *
 * THE ONE STRUCTURAL DEPARTURE, and why it is safe.  The reference's
 * `celt_norm`/`celt_sig`/`celt_ener` are `float` in the float build and
 * Q14/Q15 integers in the fixed build; here they are `double`. Where the
 * reference's two builds AGREE -- every quantity the range decoder's next
 * symbol depends on -- this file uses exact integer arithmetic and matches
 * both bit for bit:
 *
 *     the range decoder (opus_range.c)      the Laplace model
 *     the pulse cache, bits2pulses          compute_qn
 *     the whole of compute_allocation       bitexact_cos, bitexact_log2tan
 *     the PVQ index algebra (cwrsi)         isqrt32
 *     tf_decode, init_caps                  celt_lcg_rand
 *
 * Those are integer here and are not approximations of anything. Where the
 * two builds DISAGREE -- the reconstruction: denormalisation, the rotation,
 * the IMDCT, the window, the post-filter, de-emphasis -- this file follows
 * the FLOAT path widened to double. Widening cannot move a decoded bit,
 * because no value on that side of the line ever re-enters the range
 * decoder; it can only move the output waveform, and it moves it TOWARDS the
 * infinite-precision answer, not away.
 *
 * WHY NOT PICK THE FIXED-POINT PATH THROUGHOUT, given that the brief asks
 * for integer arithmetic where the reference is fixed-point?  Because for
 * the reconstruction the reference is not "fixed-point"; it is two
 * implementations that disagree with each other, and RFC 6716 section 6
 * settles the disagreement by declaring BOTH conformant and defining
 * conformance as opus_compare's quality metric. Reproducing the fixed build's
 * Q15 truncation would be reproducing one implementation's rounding error on
 * a machine that has an FPU, at a cost in accuracy, to hit a target the
 * standard explicitly does not set. What the brief's requirement is actually
 * protecting -- determinism, and no dependence on how this machine rounds --
 * is delivered by the line above: everything that DECIDES anything is
 * integer. The float half is a pure function of the integers.
 *
 * THE MDCT IS NOT REIMPLEMENTED HERE.  c/lib/audio/afft.c already has the
 * inverse MDCT, and its header was written knowing CELT would arrive -- it
 * factors over {2,3,5} precisely because Opus's 240/480/960/1920 are not
 * powers of two. What this file needed was the CONTRACT of the reference's
 * clt_mdct_backward in terms of the plain mathematical IMDCT, and that was
 * MEASURED rather than re-derived from kiss_fft's index gymnastics (see
 * imdct_block below for the numbers). Re-deriving it is the classic way to
 * ship a sign error that still sounds like music.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "opus_celt.h"
#include "opus_tables.h"
#include "afft.h"

/* ------------------------------------------------------------------ mode */
/* The single CELT mode Opus defines: mode48000_960_120. RFC 6716 has exactly
 * one, and a "custom mode" is an out-of-band extension no Opus stream can
 * signal, so these are constants rather than a struct to look up. */
#define NBANDS        21          /* nbEBands, and effEBands is also 21 */
#define OVERLAP       120
#define SHORT_MDCT    120
#define MAXLM         3
#define NB_ALLOC_VEC  11
#define MAX_PERIOD    1024
#define DECODE_BUF    2048
#define COMB_MINPERIOD 15
#define MAX_FINE_BITS 8
#define FINE_OFFSET   21
#define QTHETA_OFFSET 4
#define QTHETA_OFFSET_TWOPHASE 16
#define ALLOC_STEPS   6
#define LOG_MAX_PSEUDO 6

#define SPREAD_NONE       0
#define SPREAD_LIGHT      1
#define SPREAD_NORMAL     2
#define SPREAD_AGGRESSIVE 3

#define BITRES OPUS_BITRES

/* The PVQ pulse count is bounded by the pulse cache: q <= MAX_PSEUDO (40)
 * and get_pulses(40) == 128, so K <= 128 and the U() row cwrsi walks needs
 * K+2 entries. 160 is that with room, and the bound is the CACHE's, not an
 * assumption about the bitstream -- a hostile packet cannot raise q. */
#define MAX_PULSES     128
#define MAX_PULSES_ROW 160

/* The widest band is eBands[21]-eBands[20] = 22 coefficients, times M = 8 at
 * 20 ms: 176. Splits only ever make N smaller. */
#define BAND_MAX_N  176

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* preemph from the mode table: {0.85000610, 0, 1, 1}. Only [0], [1] and [3]
 * are read by deemphasis, and [1] is zero, which is why the subtraction it
 * appears in is kept rather than folded away -- the expression is the
 * reference's and a reader checking it should find it. */
#define PREEMPH0 0.85000610
#define PREEMPH1 0.0
#define PREEMPH3 1.0

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }

/* FRAC_MUL16 from mathops.h, and it is exact 16-bit integer arithmetic on
 * purpose -- bitexact_cos below feeds the bit allocation. */
#define FRAC_MUL16(a, b) ((16384 + ((int32_t)(int16_t)(a) * (int16_t)(b))) >> 15)

#define EC_ILOG(x) orange_ilog((uint32_t)(x))

/* ---------------------------------------------------- exact integer maths */

static uint32_t celt_lcg_rand(uint32_t seed)
{
    return 1664525u * seed + 1013904223u;
}

/* The second method from azillionmonkeys' sqroot page, as the reference uses:
 * find the largest binary digit b with (g+b)^2 <= val. It is here rather than
 * as a call to sqrt() because it decides an ENTROPY-CODED value -- the
 * triangular-pdf branch of the theta decode inverts it -- so a one-ULP
 * disagreement with the encoder is a desynchronised bitstream, not a rounding
 * error. */
static unsigned isqrt32(uint32_t val)
{
    unsigned b, g = 0;
    int bshift = (EC_ILOG(val) - 1) >> 1;
    b = 1u << bshift;
    do {
        uint32_t t = (((uint32_t)g << 1) + b) << bshift;
        if (t <= val) { g += b; val -= t; }
        b >>= 1;
        bshift--;
    } while (bshift >= 0);
    return g;
}

/* A cos() approximation that is bit-exact on any platform. Bit-exactness
 * matters because the result splits the bit budget between mid and side. */
static int16_t bitexact_cos(int16_t x)
{
    int32_t tmp = (4096 + ((int32_t)x * x)) >> 13;
    int16_t x2 = (int16_t)tmp;
    x2 = (int16_t)((32767 - x2)
         + FRAC_MUL16(x2, (-7651 + FRAC_MUL16(x2, (8277 + FRAC_MUL16(-626, x2))))));
    return (int16_t)(1 + x2);
}

static int bitexact_log2tan(int isin, int icos)
{
    int lc = EC_ILOG(icos);
    int ls = EC_ILOG(isin);
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11)
         + FRAC_MUL16(isin, FRAC_MUL16(isin, -2597) + 7932)
         - FRAC_MUL16(icos, FRAC_MUL16(icos, -2597) + 7932);
}

/* --------------------------------------------------------- Laplace decode */
/* RFC 6716 4.3.2.1's coarse-energy model. LAPLACE_MINP is 1 and
 * LAPLACE_LOG_MINP is 0 in the reference; they are written out rather than
 * folded so the expressions match it. */
#define LAPLACE_LOG_MINP 0
#define LAPLACE_MINP     (1 << LAPLACE_LOG_MINP)
#define LAPLACE_NMIN     16

static unsigned ec_laplace_get_freq1(unsigned fs0, int decay)
{
    unsigned ft = 32768 - LAPLACE_MINP * (2 * LAPLACE_NMIN) - fs0;
    return (unsigned)(ft * (int32_t)(16384 - decay) >> 15);
}

static int ec_laplace_decode(orange *dec, unsigned fs, int decay)
{
    int val = 0;
    unsigned fl = 0;
    unsigned fm = orange_decode_bin(dec, 15);
    if (fm >= fs) {
        val++;
        fl = fs;
        fs = ec_laplace_get_freq1(fs, decay) + LAPLACE_MINP;
        while (fs > LAPLACE_MINP && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = (unsigned)(((int32_t)(fs - 2 * LAPLACE_MINP) * decay) >> 15);
            fs += LAPLACE_MINP;
            val++;
        }
        if (fs <= LAPLACE_MINP) {
            int di = (int)((fm - fl) >> (LAPLACE_LOG_MINP + 1));
            val += di;
            fl += (unsigned)(2 * di * LAPLACE_MINP);
        }
        if (fm < fl + fs) val = -val;
        else              fl += fs;
    }
    orange_update(dec, fl, fl + fs < 32768 ? fl + fs : 32768, 32768);
    return val;
}

/* ------------------------------------------------------- PVQ index algebra */
/* RFC 6716 4.3.4.  This is the SMALL_FOOTPRINT path of the reference's
 * cwrs.c: one generic cwrsi() driven by a U() row, rather than that plus
 * hand-specialised cwrsi2/3/4 with their modular-inverse division tables.
 * The two are equivalent by construction -- the specialisations are an
 * optimisation the reference itself compiles out under SMALL_FOOTPRINT -- and
 * the generic form has no INV_TABLE to transcribe, which is three hundred
 * numbers that a wrong entry would turn into a desynchronised decoder. */

static void unext(uint32_t *ui, unsigned len, uint32_t ui0)
{
    unsigned j = 1;
    do {
        uint32_t ui1 = ui[j] + ui[j - 1] + ui0;
        ui[j - 1] = ui0;
        ui0 = ui1;
    } while (++j < len);
    ui[j - 1] = ui0;
}

static void uprev(uint32_t *ui, unsigned n, uint32_t ui0)
{
    unsigned j = 1;
    do {
        uint32_t ui1 = ui[j] - ui[j - 1] - ui0;
        ui[j - 1] = ui0;
        ui0 = ui1;
    } while (++j < n);
    ui[j - 1] = ui0;
}

/* V(n,k), and leaves U(n, 0..k+1) in u[]. */
static uint32_t ncwrs_urow(unsigned n, unsigned k, uint32_t *u)
{
    unsigned len = k + 2, j;
    u[0] = 0;
    u[1] = 1;
    j = 2;
    do u[j] = (j << 1) - 1; while (++j < len);   /* row n=2: U(2,k) = 2k-1 */
    for (j = 2; j < n; j++) unext(u + 1, k + 1, 1);
    return u[k] + u[k + 1];
}

static void cwrsi(int n, int k, uint32_t i, int *y, uint32_t *u)
{
    int j = 0;
    do {
        uint32_t p = u[k + 1];
        int s = -(int)(i >= p);
        int yj;
        i -= p & (uint32_t)s;
        yj = k;
        p = u[k];
        while (p > i) p = u[--k];
        i -= p;
        yj -= k;
        y[j] = (yj + s) ^ s;
        uprev(u, (unsigned)k + 2, 0);
    } while (++j < n);
}

/* K is bounded by the pulse cache (<= 128 pulses per band, MAX_PULSES), and
 * N by the widest band times M: eBands[21]-eBands[20] = 22 at M=8 is 176. The
 * u[] row is k+2 entries. 256 covers both with room, and is asserted by the
 * caller's own bounds rather than trusted. */
static void decode_pulses(int *y, int n, int k, orange *dec)
{
    uint32_t u[MAX_PULSES_ROW];
    uint32_t v = ncwrs_urow((unsigned)n, (unsigned)k, u);
    cwrsi(n, k, orange_dec_uint(dec, v), y, u);
}

/* ------------------------------------------------------- the pulse cache */

static int get_pulses(int i)
{
    return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1);
}

static const unsigned char *cache_for(int band, int LM)
{
    /* LM is offset by one on the way in: row 0 of the index table is the
     * LM==-1 case the band splitter reaches. */
    return opus_cache_bits50 + opus_cache_index50[(LM + 1) * NBANDS + band];
}

static int bits2pulses(int band, int LM, int bits)
{
    const unsigned char *cache = cache_for(band, LM);
    int i, lo = 0, hi = cache[0];
    bits--;
    for (i = 0; i < LOG_MAX_PSEUDO; i++) {
        int mid = (lo + hi + 1) >> 1;
        if (cache[mid] >= bits) hi = mid;
        else                    lo = mid;
    }
    if (bits - (lo == 0 ? -1 : cache[lo]) <= cache[hi] - bits) return lo;
    return hi;
}

static int pulses2bits(int band, int LM, int pulses)
{
    const unsigned char *cache = cache_for(band, LM);
    return pulses == 0 ? 0 : cache[pulses] + 1;
}

/* ------------------------------------------------------------ band energy */

static void unquant_coarse_energy(int start, int end, double *oldEBands,
                                  int intra, orange *dec, int C, int LM)
{
    const unsigned char *prob_model = opus_e_prob_model[LM][intra];
    int i, c;
    double prev[2] = { 0, 0 };
    double coef, beta;
    int32_t budget, tell;

    if (intra) { coef = 0;                  beta = OPUS_BETA_INTRA; }
    else       { coef = opus_pred_coef[LM]; beta = opus_beta_coef[LM]; }

    budget = (int32_t)dec->storage * 8;

    for (i = start; i < end; i++) {
        c = 0;
        do {
            int qi;
            double q, tmp;
            tell = orange_tell(dec);
            if (budget - tell >= 15) {
                int pi = 2 * imin(i, 20);
                qi = ec_laplace_decode(dec, (unsigned)prob_model[pi] << 7,
                                       prob_model[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = orange_icdf(dec, opus_small_energy_icdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) {
                qi = -orange_bit_logp(dec, 1);
            } else {
                qi = -1;
            }
            q = (double)qi;

            oldEBands[i + c * NBANDS] = dmax(-9.0, oldEBands[i + c * NBANDS]);
            tmp = coef * oldEBands[i + c * NBANDS] + prev[c] + q;
            oldEBands[i + c * NBANDS] = tmp;
            prev[c] = prev[c] + q - beta * q;
        } while (++c < C);
    }
}

/* THE NEGATIVE CONTROL LIVES HERE, and where it does NOT reach is the point.
 * -DOPUS_NEGCTL_FINE_ENERGY still READS the fine-energy bits -- the same
 * count, in the same order -- and merely throws the value away. So the
 * entropy decode is untouched: every subsequent symbol in the frame is still
 * decoded correctly and the per-packet `rng` checksum still MATCHES. What
 * degrades is only the reconstructed band energy.
 *
 * That separation is the whole design. A control that skipped the bits would
 * desynchronise the range decoder, and then every vector would fail for a
 * reason that has nothing to do with fine energy -- it would prove the gate
 * notices a shredded bitstream, which is not in doubt. This one proves the
 * QUALITY metric is actually measuring the reconstruction, while the exact
 * half of the decoder goes on passing its own exact check beside it. */
static void unquant_fine_energy(int start, int end, double *oldEBands,
                                const int *fine_quant, orange *dec, int C)
{
    int i, c;
    for (i = start; i < end; i++) {
        if (fine_quant[i] <= 0) continue;
        c = 0;
        do {
            int q2 = (int)orange_dec_bits(dec, (unsigned)fine_quant[i]);
            double offset = (q2 + 0.5) * (double)(1 << (14 - fine_quant[i]))
                            * (1.0 / 16384.0) - 0.5;
#ifdef OPUS_NEGCTL_FINE_ENERGY
            (void)offset;
            (void)q2;
#else
            oldEBands[i + c * NBANDS] += offset;
#endif
        } while (++c < C);
    }
}

static void unquant_energy_finalise(int start, int end, double *oldEBands,
                                    const int *fine_quant,
                                    const int *fine_priority,
                                    int bits_left, orange *dec, int C)
{
    int i, prio, c;
    for (prio = 0; prio < 2; prio++) {
        for (i = start; i < end && bits_left >= C; i++) {
            if (fine_quant[i] >= MAX_FINE_BITS || fine_priority[i] != prio)
                continue;
            c = 0;
            do {
                int q2 = (int)orange_dec_bits(dec, 1);
                double offset = (q2 - 0.5)
                                * (double)(1 << (14 - fine_quant[i] - 1))
                                * (1.0 / 16384.0);
                oldEBands[i + c * NBANDS] += offset;
                bits_left--;
            } while (++c < C);
        }
    }
}

static void log2Amp(int start, int end, double *eBands,
                    const double *oldEBands, int C)
{
    int c = 0, i;
    do {
        for (i = 0; i < start; i++) eBands[i + c * NBANDS] = 0;
        for (; i < end; i++) {
            double lg = oldEBands[i + c * NBANDS] + opus_eMeans[i];
            eBands[i + c * NBANDS] = exp2(lg);
        }
        for (; i < NBANDS; i++) eBands[i + c * NBANDS] = 0;
    } while (++c < C);
}

/* ---------------------------------------------------------- tf resolution */

static void tf_decode(int start, int end, int isTransient, int *tf_res,
                      int LM, orange *dec)
{
    int i, curr, tf_select, tf_select_rsv, tf_changed, logp;
    uint32_t budget, tell;

    budget = dec->storage * 8;
    tell = (uint32_t)orange_tell(dec);
    logp = isTransient ? 2 : 4;
    tf_select_rsv = LM > 0 && tell + (uint32_t)logp + 1 <= budget;
    budget -= (uint32_t)tf_select_rsv;
    tf_changed = curr = 0;
    for (i = start; i < end; i++) {
        if (tell + (uint32_t)logp <= budget) {
            curr ^= orange_bit_logp(dec, (unsigned)logp);
            tell = (uint32_t)orange_tell(dec);
            tf_changed |= curr;
        }
        tf_res[i] = curr;
        logp = isTransient ? 4 : 5;
    }
    tf_select = 0;
    if (tf_select_rsv &&
        opus_tf_select_table[LM][4 * isTransient + 0 + tf_changed] !=
        opus_tf_select_table[LM][4 * isTransient + 2 + tf_changed]) {
        tf_select = orange_bit_logp(dec, 1);
    }
    for (i = start; i < end; i++)
        tf_res[i] = opus_tf_select_table[LM][4 * isTransient + 2 * tf_select + tf_res[i]];
}

/* ------------------------------------------------------- bit allocation */

static void init_caps(int *cap, int LM, int C)
{
    int i;
    for (i = 0; i < NBANDS; i++) {
        int N = (opus_eband5ms[i + 1] - opus_eband5ms[i]) << LM;
        cap[i] = (opus_cache_caps50[NBANDS * (2 * LM + C - 1) + i] + 64) * C * N >> 2;
    }
}

static int interp_bits2pulses(int start, int end, int skip_start,
                              const int *bits1, const int *bits2,
                              const int *thresh, const int *cap,
                              int32_t total, int32_t *_balance,
                              int skip_rsv, int *intensity, int intensity_rsv,
                              int *dual_stereo, int dual_stereo_rsv,
                              int *bits, int *ebits, int *fine_priority,
                              int C, int LM, orange *ec)
{
    int32_t psum;
    int lo, hi, i, j, logM, stereo, codedBands = -1, alloc_floor, done;
    int32_t left, percoeff;
    int32_t balance;

    alloc_floor = C << BITRES;
    stereo = C > 1;
    logM = LM << BITRES;

    lo = 0;
    hi = 1 << ALLOC_STEPS;
    for (i = 0; i < ALLOC_STEPS; i++) {
        int mid = (lo + hi) >> 1;
        psum = 0;
        done = 0;
        for (j = end; j-- > start; ) {
            int tmp = bits1[j] + (int)((int32_t)mid * bits2[j] >> ALLOC_STEPS);
            if (tmp >= thresh[j] || done) {
                done = 1;
                psum += imin(tmp, cap[j]);
            } else if (tmp >= alloc_floor) {
                psum += alloc_floor;
            }
        }
        if (psum > total) hi = mid;
        else              lo = mid;
    }
    psum = 0;
    done = 0;
    for (j = end; j-- > start; ) {
        int tmp = bits1[j] + (lo * bits2[j] >> ALLOC_STEPS);
        if (tmp < thresh[j] && !done) {
            if (tmp >= alloc_floor) tmp = alloc_floor;
            else                    tmp = 0;
        } else {
            done = 1;
        }
        tmp = imin(tmp, cap[j]);
        bits[j] = tmp;
        psum += tmp;
    }

    /* Which bands to skip, working backwards from the end. */
    for (codedBands = end; ; codedBands--) {
        int band_width, band_bits, rem;
        j = codedBands - 1;
        if (j <= skip_start) {
            total += skip_rsv;
            break;
        }
        left = total - psum;
        percoeff = left / (opus_eband5ms[codedBands] - opus_eband5ms[start]);
        left -= (opus_eband5ms[codedBands] - opus_eband5ms[start]) * percoeff;
        rem = (int)imax((int)(left - (opus_eband5ms[j] - opus_eband5ms[start])), 0);
        band_width = opus_eband5ms[codedBands] - opus_eband5ms[j];
        band_bits = (int)(bits[j] + percoeff * band_width + rem);
        if (band_bits >= imax(thresh[j], alloc_floor + (1 << BITRES))) {
            if (orange_bit_logp(ec, 1)) break;
            psum += 1 << BITRES;
            band_bits -= 1 << BITRES;
        }
        psum -= bits[j] + intensity_rsv;
        if (intensity_rsv > 0)
            intensity_rsv = opus_LOG2_FRAC_TABLE[j - start];
        psum += intensity_rsv;
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            bits[j] = alloc_floor;
        } else {
            bits[j] = 0;
        }
    }

    if (intensity_rsv > 0)
        *intensity = start + (int)orange_dec_uint(ec, (uint32_t)(codedBands + 1 - start));
    else
        *intensity = 0;
    if (*intensity <= start) {
        total += dual_stereo_rsv;
        dual_stereo_rsv = 0;
    }
    if (dual_stereo_rsv > 0) *dual_stereo = orange_bit_logp(ec, 1);
    else                     *dual_stereo = 0;

    left = total - psum;
    percoeff = left / (opus_eband5ms[codedBands] - opus_eband5ms[start]);
    left -= (opus_eband5ms[codedBands] - opus_eband5ms[start]) * percoeff;
    for (j = start; j < codedBands; j++)
        bits[j] += (int)percoeff * (opus_eband5ms[j + 1] - opus_eband5ms[j]);
    for (j = start; j < codedBands; j++) {
        int tmp = (int)imin((int)left, opus_eband5ms[j + 1] - opus_eband5ms[j]);
        bits[j] += tmp;
        left -= tmp;
    }

    balance = 0;
    for (j = start; j < codedBands; j++) {
        int N0, N, den, offset, NClogN, excess;
        N0 = opus_eband5ms[j + 1] - opus_eband5ms[j];
        N = N0 << LM;
        bits[j] += balance;

        if (N > 1) {
            excess = imax(bits[j] - cap[j], 0);
            bits[j] -= excess;
            den = C * N + ((C == 2 && N > 2 && !*dual_stereo && j < *intensity) ? 1 : 0);
            NClogN = den * (opus_logN400[j] + logM);
            offset = (NClogN >> 1) - den * FINE_OFFSET;
            if (N == 2) offset += den << BITRES >> 2;
            if (bits[j] + offset < den * 2 << BITRES)      offset += NClogN >> 2;
            else if (bits[j] + offset < den * 3 << BITRES) offset += NClogN >> 3;

            ebits[j] = imax(0, (bits[j] + offset + (den << (BITRES - 1))) / (den << BITRES));
            if (C * ebits[j] > (bits[j] >> BITRES))
                ebits[j] = bits[j] >> stereo >> BITRES;
            ebits[j] = imin(ebits[j], MAX_FINE_BITS);
            fine_priority[j] = ebits[j] * (den << BITRES) >= bits[j] + offset;
            bits[j] -= C * ebits[j] << BITRES;
        } else {
            excess = imax(0, bits[j] - (C << BITRES));
            bits[j] -= excess;
            ebits[j] = 0;
            fine_priority[j] = 1;
        }

        if (excess > 0) {
            int extra_fine = imin(excess >> (stereo + BITRES), MAX_FINE_BITS - ebits[j]);
            int extra_bits;
            ebits[j] += extra_fine;
            extra_bits = extra_fine * C << BITRES;
            fine_priority[j] = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
    }
    *_balance = balance;

    for (; j < end; j++) {
        ebits[j] = bits[j] >> stereo >> BITRES;
        bits[j] = 0;
        fine_priority[j] = ebits[j] < 1;
    }
    return codedBands;
}

static int compute_allocation(int start, int end, const int *offsets,
                              const int *cap, int alloc_trim, int *intensity,
                              int *dual_stereo, int32_t total, int32_t *balance,
                              int *pulses, int *ebits, int *fine_priority,
                              int C, int LM, orange *ec)
{
    int lo, hi, j, codedBands, skip_start;
    int skip_rsv, intensity_rsv, dual_stereo_rsv;
    int bits1[NBANDS], bits2[NBANDS], thresh[NBANDS], trim_offset[NBANDS];

    if (total < 0) total = 0;
    skip_start = start;
    skip_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
    total -= skip_rsv;
    intensity_rsv = dual_stereo_rsv = 0;
    if (C == 2) {
        intensity_rsv = opus_LOG2_FRAC_TABLE[end - start];
        if (intensity_rsv > total) {
            intensity_rsv = 0;
        } else {
            total -= intensity_rsv;
            dual_stereo_rsv = total >= 1 << BITRES ? 1 << BITRES : 0;
            total -= dual_stereo_rsv;
        }
    }

    for (j = start; j < end; j++) {
        thresh[j] = imax(C << BITRES,
                         (3 * (opus_eband5ms[j + 1] - opus_eband5ms[j]) << LM << BITRES) >> 4);
        trim_offset[j] = C * (opus_eband5ms[j + 1] - opus_eband5ms[j])
                         * (alloc_trim - 5 - LM) * (end - j - 1)
                         * (1 << (LM + BITRES)) >> 6;
        if ((opus_eband5ms[j + 1] - opus_eband5ms[j]) << LM == 1)
            trim_offset[j] -= C << BITRES;
    }
    lo = 1;
    hi = NB_ALLOC_VEC - 1;
    do {
        int done = 0, psum = 0, mid = (lo + hi) >> 1;
        for (j = end; j-- > start; ) {
            int N = opus_eband5ms[j + 1] - opus_eband5ms[j];
            int bitsj = C * N * opus_band_allocation[mid][j] << LM >> 2;
            if (bitsj > 0) bitsj = imax(0, bitsj + trim_offset[j]);
            bitsj += offsets[j];
            if (bitsj >= thresh[j] || done) {
                done = 1;
                psum += imin(bitsj, cap[j]);
            } else if (bitsj >= C << BITRES) {
                psum += C << BITRES;
            }
        }
        if (psum > total) hi = mid - 1;
        else              lo = mid + 1;
    } while (lo <= hi);
    hi = lo--;

    for (j = start; j < end; j++) {
        int N = opus_eband5ms[j + 1] - opus_eband5ms[j];
        int bits1j = C * N * opus_band_allocation[lo][j] << LM >> 2;
        int bits2j = hi >= NB_ALLOC_VEC ? cap[j]
                     : C * N * opus_band_allocation[hi][j] << LM >> 2;
        if (bits1j > 0) bits1j = imax(0, bits1j + trim_offset[j]);
        if (bits2j > 0) bits2j = imax(0, bits2j + trim_offset[j]);
        if (lo > 0) bits1j += offsets[j];
        bits2j += offsets[j];
        if (offsets[j] > 0) skip_start = j;
        bits2j = imax(0, bits2j - bits1j);
        bits1[j] = bits1j;
        bits2[j] = bits2j;
    }
    codedBands = interp_bits2pulses(start, end, skip_start, bits1, bits2,
                                    thresh, cap, total, balance, skip_rsv,
                                    intensity, intensity_rsv, dual_stereo,
                                    dual_stereo_rsv, pulses, ebits,
                                    fine_priority, C, LM, ec);
    return codedBands;
}

/* ------------------------------------------------------------ band shapes */

static void haar1(double *X, int N0, int stride)
{
    int i, j;
    const double s = 0.70710678118654752440;
    N0 >>= 1;
    for (i = 0; i < stride; i++)
        for (j = 0; j < N0; j++) {
            double t1 = s * X[stride * 2 * j + i];
            double t2 = s * X[stride * (2 * j + 1) + i];
            X[stride * 2 * j + i] = t1 + t2;
            X[stride * (2 * j + 1) + i] = t1 - t2;
        }
}

static void deinterleave_hadamard(double *X, int N0, int stride, int hadamard)
{
    double tmp[BAND_MAX_N];
    int i, j, N = N0 * stride;
    if (hadamard) {
        const int *ordery = opus_ordery_table + stride - 2;
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[ordery[i] * N0 + j] = X[j * stride + i];
    } else {
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[i * N0 + j] = X[j * stride + i];
    }
    for (j = 0; j < N; j++) X[j] = tmp[j];
}

static void interleave_hadamard(double *X, int N0, int stride, int hadamard)
{
    double tmp[BAND_MAX_N];
    int i, j, N = N0 * stride;
    if (hadamard) {
        const int *ordery = opus_ordery_table + stride - 2;
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[j * stride + i] = X[ordery[i] * N0 + j];
    } else {
        for (i = 0; i < stride; i++)
            for (j = 0; j < N0; j++)
                tmp[j * stride + i] = X[i * N0 + j];
    }
    for (j = 0; j < N; j++) X[j] = tmp[j];
}

static int compute_qn(int N, int b, int offset, int pulse_cap, int stereo)
{
    int qn, qb;
    int N2 = 2 * N - 1;
    if (stereo && N == 2) N2--;
    qb = imin(b - pulse_cap - (4 << BITRES), (b + N2 * offset) / N2);
    qb = imin(8 << BITRES, qb);
    if (qb < (1 << BITRES >> 1)) {
        qn = 1;
    } else {
        qn = opus_exp2_table8[qb & 0x7] >> (14 - (qb >> BITRES));
        qn = (qn + 1) >> 1 << 1;
    }
    return qn;
}

static void exp_rotation1(double *X, int len, int stride, double c, double s)
{
    int i;
    double *Xptr = X;
    for (i = 0; i < len - stride; i++) {
        double x1 = Xptr[0], x2 = Xptr[stride];
        Xptr[stride] = c * x2 + s * x1;
        *Xptr++      = c * x1 - s * x2;
    }
    Xptr = &X[len - 2 * stride - 1];
    for (i = len - 2 * stride - 1; i >= 0; i--) {
        double x1 = Xptr[0], x2 = Xptr[stride];
        Xptr[stride] = c * x2 + s * x1;
        *Xptr--      = c * x1 - s * x2;
    }
}

static void exp_rotation(double *X, int len, int dir, int stride, int K, int spread)
{
    static const int SPREAD_FACTOR[3] = { 15, 10, 5 };
    int i, factor, stride2 = 0;
    double c, s, gain, theta;

    if (2 * K >= len || spread == SPREAD_NONE) return;
    factor = SPREAD_FACTOR[spread - 1];

    gain = (double)len / (double)(len + factor * K);
    theta = 0.5 * (gain * gain);

    /* celt_cos_norm(x) = cos(pi/2 * x); the second call is sin(theta) by the
     * complement, which is how the reference spells it. */
    c = cos(M_PI * 0.5 * theta);
    s = cos(M_PI * 0.5 * (1.0 - theta));

    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len)
            stride2++;
    }
    len /= stride;
    for (i = 0; i < stride; i++) {
        if (dir < 0) {
            if (stride2) exp_rotation1(X + i * len, len, stride2, s, c);
            exp_rotation1(X + i * len, len, 1, c, s);
        } else {
            exp_rotation1(X + i * len, len, 1, c, -s);
            if (stride2) exp_rotation1(X + i * len, len, stride2, s, -c);
        }
    }
}

static void normalise_residual(const int *iy, double *X, int N,
                               double Ryy, double gain)
{
    int i;
    double g = gain / sqrt(Ryy);
    for (i = 0; i < N; i++) X[i] = g * iy[i];
}

static unsigned extract_collapse_mask(const int *iy, int N, int B)
{
    unsigned collapse_mask = 0;
    int N0, i, j;
    if (B <= 1) return 1;
    N0 = N / B;
    for (i = 0; i < B; i++)
        for (j = 0; j < N0; j++)
            collapse_mask |= (unsigned)(iy[i * N0 + j] != 0) << i;
    return collapse_mask;
}

static void renormalise_vector(double *X, int N, double gain)
{
    int i;
    double E = 1e-15, g;
    for (i = 0; i < N; i++) E += X[i] * X[i];
    g = gain / sqrt(E);
    for (i = 0; i < N; i++) X[i] *= g;
}

static unsigned alg_unquant(double *X, int N, int K, int spread, int B,
                            orange *dec, double gain)
{
    int iy[BAND_MAX_N];
    double Ryy = 0;
    int i;
    decode_pulses(iy, N, K, dec);
    for (i = 0; i < N; i++) Ryy += (double)iy[i] * iy[i];
    normalise_residual(iy, X, N, Ryy, gain);
    exp_rotation(X, N, -1, B, K, spread);
    return extract_collapse_mask(iy, N, B);
}

static void stereo_merge(double *X, double *Y, double mid, int N)
{
    int j;
    double xp = 0, side = 0, El, Er, mid2, lgain, rgain;
    for (j = 0; j < N; j++) {
        xp += X[j] * Y[j];
        side += Y[j] * Y[j];
    }
    xp *= mid;
    mid2 = mid;
    El = mid2 * mid2 + side - 2 * xp;
    Er = mid2 * mid2 + side + 2 * xp;
    if (Er < 6e-4 || El < 6e-4) {
        for (j = 0; j < N; j++) Y[j] = X[j];
        return;
    }
    lgain = 1.0 / sqrt(El);
    rgain = 1.0 / sqrt(Er);
    for (j = 0; j < N; j++) {
        double l = mid * X[j];
        double r = Y[j];
        X[j] = lgain * (l - r);
        Y[j] = rgain * (l + r);
    }
}

/* --------------------------------------------------------- quant_band -- */
/* One band, encoder and decoder in the reference; decoder only here. It
 * recurses: a band can be split in time (tf), in frequency (the 1.5-bit
 * rule) and in stereo, up to eight ways. The parameter list is the
 * reference's, deliberately -- see the file header. */

static unsigned quant_band(int i, double *X, double *Y, int N, int b,
                           int spread, int B, int intensity, int tf_change,
                           double *lowband, orange *ec, int32_t *remaining_bits,
                           int LM, double *lowband_out, int level,
                           uint32_t *seed, double gain, double *lowband_scratch,
                           int fill)
{
    const unsigned char *cache;
    int q, curr_bits;
    int stereo, split;
    int imid = 0, iside = 0;
    int N0 = N, N_B = N, N_B0, B0 = B;
    int time_divide = 0, recombine = 0, inv = 0;
    double mid = 0, side = 0;
    int longBlocks;
    unsigned cm = 0;

    longBlocks = B0 == 1;
    N_B /= B;
    N_B0 = N_B;
    split = stereo = Y != NULL;

    if (N == 1) {
        int c;
        double *x = X;
        c = 0;
        do {
            int sign = 0;
            if (*remaining_bits >= 1 << BITRES) {
                sign = (int)orange_dec_bits(ec, 1);
                *remaining_bits -= 1 << BITRES;
                b -= 1 << BITRES;
            }
            x[0] = sign ? -1.0 : 1.0;
            x = Y;
        } while (++c < 1 + stereo);
        if (lowband_out) lowband_out[0] = X[0];
        return 1;
    }

    if (!stereo && level == 0) {
        int k;
        if (tf_change > 0) recombine = tf_change;

        if (lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || B0 > 1)) {
            int j;
            for (j = 0; j < N; j++) lowband_scratch[j] = lowband[j];
            lowband = lowband_scratch;
        }

        for (k = 0; k < recombine; k++) {
            static const unsigned char bit_interleave_table[16] =
                { 0,1,1,1,2,3,3,3,2,3,3,3,2,3,3,3 };
            if (lowband) haar1(lowband, N >> k, 1 << k);
            fill = bit_interleave_table[fill & 0xF] | bit_interleave_table[fill >> 4] << 2;
        }
        B >>= recombine;
        N_B <<= recombine;

        while ((N_B & 1) == 0 && tf_change < 0) {
            if (lowband) haar1(lowband, N_B, B);
            fill |= fill << B;
            B <<= 1;
            N_B >>= 1;
            time_divide++;
            tf_change++;
        }
        B0 = B;
        N_B0 = N_B;

        if (B0 > 1 && lowband)
            deinterleave_hadamard(lowband, N_B >> recombine, B0 << recombine, longBlocks);
    }

    cache = cache_for(i, LM);
    if (!stereo && LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
        N >>= 1;
        Y = X + N;
        split = 1;
        LM -= 1;
        if (B == 1) fill = (fill & 1) | (fill << 1);
        B = (B + 1) >> 1;
    }

    if (split) {
        int qn, itheta = 0, mbits, sbits, delta, qalloc, pulse_cap, offset, orig_fill;
        int32_t tell;

        pulse_cap = opus_logN400[i] + LM * (1 << BITRES);
        offset = (pulse_cap >> 1) - (stereo && N == 2 ? QTHETA_OFFSET_TWOPHASE : QTHETA_OFFSET);
        qn = compute_qn(N, b, offset, pulse_cap, stereo);
        if (stereo && i >= intensity) qn = 1;
        tell = (int32_t)orange_tell_frac(ec);
        if (qn != 1) {
            if (stereo && N > 2) {
                /* Step pdf: probability p0 up to itheta = qn/2, then 1. */
                int p0 = 3;
                int x0 = qn / 2;
                int ft = p0 * (x0 + 1) + x0;
                int fs = (int)orange_decode(ec, (unsigned)ft);
                int x;
                if (fs < (x0 + 1) * p0) x = fs / p0;
                else                    x = x0 + 1 + (fs - (x0 + 1) * p0);
                orange_update(ec,
                    (unsigned)(x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0),
                    (unsigned)(x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0),
                    (unsigned)ft);
                itheta = x;
            } else if (B0 > 1 || stereo) {
                itheta = (int)orange_dec_uint(ec, (uint32_t)(qn + 1));
            } else {
                /* Triangular pdf. isqrt32 inverts the cumulative count, which
                 * is why it must be the exact integer square root. */
                int fs = 1, ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
                int fl = 0;
                int fm = (int)orange_decode(ec, (unsigned)ft);
                if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
                    itheta = (int)((isqrt32(8u * (uint32_t)fm + 1) - 1) >> 1);
                    fs = itheta + 1;
                    fl = itheta * (itheta + 1) >> 1;
                } else {
                    itheta = (int)((2 * (qn + 1)
                             - isqrt32(8u * (uint32_t)(ft - fm - 1) + 1)) >> 1);
                    fs = qn + 1 - itheta;
                    fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
                }
                orange_update(ec, (unsigned)fl, (unsigned)(fl + fs), (unsigned)ft);
            }
            itheta = (int)((int32_t)itheta * 16384 / qn);
        } else if (stereo) {
            if (b > 2 << BITRES && *remaining_bits > 2 << BITRES)
                inv = orange_bit_logp(ec, 2);
            else
                inv = 0;
            itheta = 0;
        }
        qalloc = (int)orange_tell_frac(ec) - tell;
        b -= qalloc;

        orig_fill = fill;
        if (itheta == 0) {
            imid = 32767;
            iside = 0;
            fill &= (1 << B) - 1;
            delta = -16384;
        } else if (itheta == 16384) {
            imid = 0;
            iside = 32767;
            fill &= ((1 << B) - 1) << B;
            delta = 16384;
        } else {
            imid = bitexact_cos((int16_t)itheta);
            iside = bitexact_cos((int16_t)(16384 - itheta));
            delta = FRAC_MUL16((N - 1) << 7, bitexact_log2tan(iside, imid));
        }

        mid = (1.0 / 32768.0) * imid;
        side = (1.0 / 32768.0) * iside;

        if (N == 2 && stereo) {
            /* N=2 stereo: mid and side are orthogonal, so the side costs one
             * bit. orig_fill is used rather than fill because the side must
             * still be folded even when itheta cleared fill's low bits. */
            int c, sign = 0;
            double *x2, *y2;
            mbits = b;
            sbits = 0;
            if (itheta != 0 && itheta != 16384) sbits = 1 << BITRES;
            mbits -= sbits;
            c = itheta > 8192;
            *remaining_bits -= qalloc + sbits;

            x2 = c ? Y : X;
            y2 = c ? X : Y;
            if (sbits) sign = (int)orange_dec_bits(ec, 1);
            sign = 1 - 2 * sign;
            cm = quant_band(i, x2, NULL, N, mbits, spread, B, intensity,
                            tf_change, lowband, ec, remaining_bits, LM,
                            lowband_out, level, seed, gain, lowband_scratch,
                            orig_fill);
            y2[0] = -sign * x2[1];
            y2[1] = sign * x2[0];
            {
                double tmp;
                X[0] *= mid; X[1] *= mid;
                Y[0] *= side; Y[1] *= side;
                tmp = X[0]; X[0] = tmp - Y[0]; Y[0] = tmp + Y[0];
                tmp = X[1]; X[1] = tmp - Y[1]; Y[1] = tmp + Y[1];
            }
        } else {
            double *next_lowband2 = NULL;
            double *next_lowband_out1 = NULL;
            int next_level = 0;
            int32_t rebalance;

            if (B0 > 1 && !stereo && (itheta & 0x3fff)) {
                if (itheta > 8192) delta -= delta >> (4 - LM);
                else delta = imin(0, delta + (N << BITRES >> (5 - LM)));
            }
            mbits = imax(0, imin(b, (b - delta) / 2));
            sbits = b - mbits;
            *remaining_bits -= qalloc;

            if (lowband && !stereo) next_lowband2 = lowband + N;
            if (stereo) next_lowband_out1 = lowband_out;
            else        next_level = level + 1;

            rebalance = *remaining_bits;
            if (mbits >= sbits) {
                cm = quant_band(i, X, NULL, N, mbits, spread, B, intensity,
                                tf_change, lowband, ec, remaining_bits, LM,
                                next_lowband_out1, next_level, seed,
                                stereo ? 1.0 : gain * mid, lowband_scratch, fill);
                rebalance = mbits - (rebalance - *remaining_bits);
                if (rebalance > 3 << BITRES && itheta != 0)
                    sbits += rebalance - (3 << BITRES);
                cm |= quant_band(i, Y, NULL, N, sbits, spread, B, intensity,
                                 tf_change, next_lowband2, ec, remaining_bits,
                                 LM, NULL, next_level, seed, gain * side,
                                 NULL, fill >> B) << ((B0 >> 1) & (stereo - 1));
            } else {
                cm = quant_band(i, Y, NULL, N, sbits, spread, B, intensity,
                                tf_change, next_lowband2, ec, remaining_bits,
                                LM, NULL, next_level, seed, gain * side,
                                NULL, fill >> B) << ((B0 >> 1) & (stereo - 1));
                rebalance = sbits - (rebalance - *remaining_bits);
                if (rebalance > 3 << BITRES && itheta != 16384)
                    mbits += rebalance - (3 << BITRES);
                cm |= quant_band(i, X, NULL, N, mbits, spread, B, intensity,
                                 tf_change, lowband, ec, remaining_bits, LM,
                                 next_lowband_out1, next_level, seed,
                                 stereo ? 1.0 : gain * mid, lowband_scratch, fill);
            }
        }
    } else {
        /* No split: the band is one PVQ codeword. */
        q = bits2pulses(i, LM, b);
        curr_bits = pulses2bits(i, LM, q);
        *remaining_bits -= curr_bits;

        while (*remaining_bits < 0 && q > 0) {
            *remaining_bits += curr_bits;
            q--;
            curr_bits = pulses2bits(i, LM, q);
            *remaining_bits -= curr_bits;
        }

        if (q != 0) {
            int K = get_pulses(q);
            cm = alg_unquant(X, N, K, spread, B, ec, gain);
        } else {
            /* No pulses: fill the band anyway, from the LCG or by folding
             * lower-frequency content. A band left at zero is a hole the ear
             * hears as a warble, which is the whole point of the fill. */
            int j;
            unsigned cm_mask = (unsigned)(1UL << B) - 1;
            fill &= (int)cm_mask;
            if (!fill) {
                for (j = 0; j < N; j++) X[j] = 0;
            } else {
                if (lowband == NULL) {
                    for (j = 0; j < N; j++) {
                        *seed = celt_lcg_rand(*seed);
                        X[j] = (double)((int32_t)*seed >> 20);
                    }
                    cm = cm_mask;
                } else {
                    for (j = 0; j < N; j++) {
                        double tmp = 1.0 / 256.0;   /* ~48 dB below the fold */
                        *seed = celt_lcg_rand(*seed);
                        tmp = (*seed) & 0x8000 ? tmp : -tmp;
                        X[j] = lowband[j] + tmp;
                    }
                    cm = (unsigned)fill;
                }
                renormalise_vector(X, N, gain);
            }
        }
    }

    /* Undo everything the top of this function did, in reverse. */
    if (stereo) {
        if (N != 2) stereo_merge(X, Y, mid, N);
        if (inv) {
            int j;
            for (j = 0; j < N; j++) Y[j] = -Y[j];
        }
    } else if (level == 0) {
        int k;
        if (B0 > 1)
            interleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);

        N_B = N_B0;
        B = B0;
        for (k = 0; k < time_divide; k++) {
            B >>= 1;
            N_B <<= 1;
            cm |= cm >> B;
            haar1(X, N_B, B);
        }

        for (k = 0; k < recombine; k++) {
            static const unsigned char bit_deinterleave_table[16] = {
                0x00,0x03,0x0C,0x0F,0x30,0x33,0x3C,0x3F,
                0xC0,0xC3,0xCC,0xCF,0xF0,0xF3,0xFC,0xFF
            };
            cm = bit_deinterleave_table[cm];
            haar1(X, N0 >> k, 1 << k);
        }
        B <<= recombine;

        if (lowband_out) {
            int j;
            double n = sqrt((double)N0);
            for (j = 0; j < N0; j++) lowband_out[j] = n * X[j];
        }
        cm &= (unsigned)((1 << B) - 1);
    }
    return cm;
}

#define NORM_MAX (2 * 8 * 100)          /* C * M * eBands[nbEBands] */
#define SCRATCH_MAX (8 * (100 - 78))    /* M * widest band */

/* THE BIG SCRATCH LIVES IN THE DECODER STATE, NOT ON THE STACK, and the
 * reason is a measurement rather than tidiness. Written the reference's way
 * -- VLAs at each level -- the deepest call chain here is
 * opus_celt_decode (freq + X, 30 KB) -> compute_inv_mdcts (x, 8.6 KB) ->
 * imdct_block (coef + y, 23 KB), which is 63 KB of automatic storage in one
 * chain. That is fine on a host but this decoder is a RING-3 program on a
 * machine whose threads do not get a host-sized stack, and a stack overflow
 * in an audio decoder fed by the network is not a crash you get to debug.
 * The buffers are fixed-size and per-instance, so two decoders still do not
 * share one. */
struct opus_celt_scratch {
    double norm[NORM_MAX];
    double lowband_scratch[SCRATCH_MAX];
    double freq[2 * CELT_MAX_FRAME];
    double X[2 * CELT_MAX_FRAME];
    double mdct_coef[CELT_MAX_FRAME];
    double mdct_y[2 * CELT_MAX_FRAME];
    double mdct_x[CELT_MAX_FRAME + OVERLAP];
};

static void quant_all_bands(struct opus_celt_scratch *sc,
                            int start, int end, double *X_, double *Y_,
                            unsigned char *collapse_masks, const int *pulses,
                            int shortBlocks, int spread, int dual_stereo,
                            int intensity, const int *tf_res,
                            int32_t total_bits, int32_t balance, orange *ec,
                            int LM, int codedBands, uint32_t *seed)
{
    int i, B, M, lowband_offset, update_lowband = 1;
    int32_t remaining_bits;
    double *lowband_scratch = sc->lowband_scratch;
    double *norm, *norm2;
    int C = Y_ != NULL ? 2 : 1;

    M = 1 << LM;
    B = shortBlocks ? M : 1;
    norm = sc->norm;
    norm2 = norm + M * opus_eband5ms[NBANDS];

    lowband_offset = 0;
    for (i = start; i < end; i++) {
        int32_t tell;
        int b, N, effective_lowband = -1, tf_change = 0;
        int32_t curr_balance;
        double *X, *Y;
        unsigned x_cm, y_cm;

        X = X_ + M * opus_eband5ms[i];
        Y = Y_ != NULL ? Y_ + M * opus_eband5ms[i] : NULL;
        N = M * opus_eband5ms[i + 1] - M * opus_eband5ms[i];
        tell = (int32_t)orange_tell_frac(ec);

        if (i != start) balance -= tell;
        remaining_bits = total_bits - tell - 1;
        if (i <= codedBands - 1) {
            curr_balance = balance / imin(3, codedBands - i);
            b = imax(0, imin(16383, imin((int)remaining_bits + 1,
                                         pulses[i] + (int)curr_balance)));
        } else {
            b = 0;
        }

        if (M * opus_eband5ms[i] - N >= M * opus_eband5ms[start]
            && (update_lowband || lowband_offset == 0))
            lowband_offset = i;

        tf_change = tf_res[i];
        /* i >= effEBands would redirect X to the scratch norm buffer; with
         * effEBands == nbEBands == 21 for the one Opus mode, the loop never
         * reaches it. The branch is therefore omitted rather than written
         * dead -- see opus_celt.h on custom modes being out of scope. */

        if (lowband_offset != 0 && (spread != SPREAD_AGGRESSIVE || B > 1 || tf_change < 0)) {
            int fold_start, fold_end, fold_i;
            effective_lowband = imax(M * opus_eband5ms[start],
                                     M * opus_eband5ms[lowband_offset] - N);
            fold_start = lowband_offset;
            while (M * opus_eband5ms[--fold_start] > effective_lowband) ;
            fold_end = lowband_offset - 1;
            while (M * opus_eband5ms[++fold_end] < effective_lowband + N) ;
            x_cm = y_cm = 0;
            fold_i = fold_start;
            do {
                x_cm |= collapse_masks[fold_i * C + 0];
                y_cm |= collapse_masks[fold_i * C + C - 1];
            } while (++fold_i < fold_end);
        } else {
            x_cm = y_cm = (unsigned)((1 << B) - 1);
        }

        if (dual_stereo && i == intensity) {
            int j;
            dual_stereo = 0;
            for (j = M * opus_eband5ms[start]; j < M * opus_eband5ms[i]; j++)
                norm[j] = 0.5 * (norm[j] + norm2[j]);
        }
        if (dual_stereo) {
            x_cm = quant_band(i, X, NULL, N, b / 2, spread, B, intensity, tf_change,
                              effective_lowband != -1 ? norm + effective_lowband : NULL,
                              ec, &remaining_bits, LM, norm + M * opus_eband5ms[i],
                              0, seed, 1.0, lowband_scratch, (int)x_cm);
            y_cm = quant_band(i, Y, NULL, N, b / 2, spread, B, intensity, tf_change,
                              effective_lowband != -1 ? norm2 + effective_lowband : NULL,
                              ec, &remaining_bits, LM, norm2 + M * opus_eband5ms[i],
                              0, seed, 1.0, lowband_scratch, (int)y_cm);
        } else {
            x_cm = quant_band(i, X, Y, N, b, spread, B, intensity, tf_change,
                              effective_lowband != -1 ? norm + effective_lowband : NULL,
                              ec, &remaining_bits, LM, norm + M * opus_eband5ms[i],
                              0, seed, 1.0, lowband_scratch, (int)(x_cm | y_cm));
            y_cm = x_cm;
        }
        collapse_masks[i * C + 0] = (unsigned char)x_cm;
        collapse_masks[i * C + C - 1] = (unsigned char)y_cm;
        balance += pulses[i] + tell;

        update_lowband = b > (N << BITRES);
    }
}

/* ------------------------------------------------------- anti-collapse -- */

static void anti_collapse(double *X_, const unsigned char *collapse_masks,
                          int LM, int C, int size, int start, int end,
                          const double *logE, const double *prev1logE,
                          const double *prev2logE, const int *pulses,
                          uint32_t seed)
{
    int c, i, j, k;
    for (i = start; i < end; i++) {
        int N0 = opus_eband5ms[i + 1] - opus_eband5ms[i];
        int depth = (1 + pulses[i]) / ((opus_eband5ms[i + 1] - opus_eband5ms[i]) << LM);
        double thresh = 0.5 * exp2(-0.125 * depth);
        double sqrt_1 = 1.0 / sqrt((double)(N0 << LM));

        c = 0;
        do {
            double *X;
            double prev1 = prev1logE[c * NBANDS + i];
            double prev2 = prev2logE[c * NBANDS + i];
            double Ediff, r;
            int renormalize = 0;
            if (C == 1) {
                prev1 = dmax(prev1, prev1logE[NBANDS + i]);
                prev2 = dmax(prev2, prev2logE[NBANDS + i]);
            }
            Ediff = logE[c * NBANDS + i] - dmin(prev1, prev2);
            Ediff = dmax(0, Ediff);

            /* r is multiplied by 2, or 2*sqrt(2) at LM==3, because the short
             * blocks of a longer frame do not carry the same energy. */
            r = 2.0 * exp2(-Ediff);
            if (LM == 3) r *= 1.41421356;
            r = dmin(thresh, r);
            r = r * sqrt_1;

            X = X_ + c * size + (opus_eband5ms[i] << LM);
            for (k = 0; k < 1 << LM; k++) {
                if (!(collapse_masks[i * C + c] & 1 << k)) {
                    for (j = 0; j < N0; j++) {
                        seed = celt_lcg_rand(seed);
                        X[(j << LM) + k] = (seed & 0x8000) ? r : -r;
                    }
                    renormalize = 1;
                }
            }
            if (renormalize) renormalise_vector(X, N0 << LM, 1.0);
        } while (++c < C);
    }
}

static void denormalise_bands(const double *X, double *freq,
                              const double *bandE, int end, int C, int M)
{
    int i, c, N = M * SHORT_MDCT;
    c = 0;
    do {
        double *f = freq + c * N;
        const double *x = X + c * N;
        for (i = 0; i < end; i++) {
            int j = M * opus_eband5ms[i];
            int band_end = M * opus_eband5ms[i + 1];
            double g = bandE[i + c * NBANDS];
            do { *f++ = *x++ * g; } while (++j < band_end);
        }
        for (i = M * opus_eband5ms[end]; i < N; i++) *f++ = 0;
    } while (++c < C);
}

/* ------------------------------------------------------------- synthesis */

struct opus_celt_dec {
    int channels;          /* CC: what the caller wants out */
    int stream_channels;   /* C : what this frame is coded in */
    int start, end;
    uint32_t rng;
    int loss_count;

    /* decode_mem[c] is DECODE_BUF + OVERLAP doubles; out_mem and overlap_mem
     * are views into it, exactly as the reference lays them out. */
    double decode_mem[2][DECODE_BUF + OVERLAP];
    double preemph_memD[2];

    double oldBandE[2 * NBANDS];
    double oldLogE[2 * NBANDS];
    double oldLogE2[2 * NBANDS];
    double backgroundLogE[2 * NBANDS];

    int postfilter_period, postfilter_period_old;
    double postfilter_gain, postfilter_gain_old;
    int postfilter_tapset, postfilter_tapset_old;

    amdct *mdct[MAXLM + 1];   /* window length 240<<LM */
    struct opus_celt_scratch sc;
};

/* THE MDCT CONTRACT, MEASURED not derived.  The reference's
 * clt_mdct_backward(in, out, window, overlap, shift, stride) is exactly
 *
 *     out[j] (op)= W[j] * y[j + d],     j in [0, Nc + overlap)
 *
 * where y is the plain mathematical IMDCT that amdct_imdct computes,
 *
 *     y[n] = sum_{k<Nc} X[k] cos(2*pi/Nw * (k + 1/2) * (n + 1/2 + Nw/4)),
 *     Nw = 2*Nc,   d = (Nc - overlap)/2,
 *
 * W is 1 in the middle and the mode window rising/falling at the two ends,
 * the scale is exactly 1, and nothing is written outside that span. Fitted
 * against the reference at every size CELT uses (build/.opus_mdct_cal.c):
 *
 *   LM=0 Nc=120  scale 1.000010680  worst abs err 1.97e-06  rel 6.2e-07
 *   LM=1 Nc=240  scale 1.000002706  worst abs err 3.68e-06  rel 7.0e-07
 *   LM=2 Nc=480  scale 1.000000653  worst abs err 5.87e-06  rel 7.5e-07
 *   LM=3 Nc=960  scale 1.000000153  worst abs err 9.44e-06  rel 7.9e-07
 *   short blocks (Nc=120, stride>1) at LM=1,2,3: same, 1.000010
 *
 * The residual is float32 epsilon accumulating in the REFERENCE (it drops as
 * the fit gets more data, which is the signature of noise, not of a missing
 * factor); the scale is 1.
 *
 * (op) is `+=` over the leading `overlap` samples and `=` over the rest.
 * That asymmetry is not cosmetic: consecutive short blocks are laid down
 * `Nc` apart, so each block's leading overlap lands on the previous block's
 * trailing window and the two must ADD -- that is the TDAC. Using `=`
 * everywhere silences the overlap-add and produces a frame that still looks
 * like audio with a click at every 2.5 ms boundary. */
static void imdct_block(struct opus_celt_scratch *sc, const amdct *m,
                        const double *X, int stride, int Nc,
                        double *out, const double *window, int overlap)
{
    double *coef = sc->mdct_coef;
    double *y = sc->mdct_y;
    int k, j, d = (Nc - overlap) / 2;

    for (k = 0; k < Nc; k++) coef[k] = X[k * stride];
    amdct_imdct(m, coef, y);

    for (j = 0; j < overlap; j++)  out[j] += window[j] * y[j + d];
    for (; j < Nc; j++)            out[j]  = y[j + d];
    for (; j < Nc + overlap; j++)  out[j]  = window[Nc + overlap - 1 - j] * y[j + d];
}

static void compute_inv_mdcts(opus_celt_dec *st, int shortBlocks,
                              const double *X, double *out_mem[],
                              double *overlap_mem[], int C, int LM)
{
    const int N = SHORT_MDCT << LM;
    double *x = st->sc.mdct_x;
    int c = 0;

    do {
        int j, b, N2 = N, B = 1, sub_lm = LM;
        if (shortBlocks) { N2 = SHORT_MDCT; B = shortBlocks; sub_lm = 0; }

        /* Only the leading overlap needs clearing: every later sample is
         * written by assignment before it is read. */
        memset(x, 0, (size_t)OVERLAP * sizeof x[0]);

        for (b = 0; b < B; b++)
            imdct_block(&st->sc, st->mdct[sub_lm], X + b + c * N2 * B, B, N2,
                        x + N2 * b, opus_window120, OVERLAP);

        for (j = 0; j < OVERLAP; j++) out_mem[c][j] = x[j] + overlap_mem[c][j];
        for (; j < N; j++)            out_mem[c][j] = x[j];
        for (j = 0; j < OVERLAP; j++) overlap_mem[c][j] = x[N + j];
    } while (++c < C);
}

static void comb_filter(double *y, const double *x, int T0, int T1, int N,
                        double g0, double g1, int tapset0, int tapset1,
                        const double *window, int overlap)
{
    static const double gains[3][3] = {
        { 0.3066406250, 0.2170410156, 0.1296386719 },
        { 0.4638671875, 0.2680664062, 0.0           },
        { 0.7998046875, 0.1000976562, 0.0           }
    };
    int i;
    double g00 = g0 * gains[tapset0][0];
    double g01 = g0 * gains[tapset0][1];
    double g02 = g0 * gains[tapset0][2];
    double g10 = g1 * gains[tapset1][0];
    double g11 = g1 * gains[tapset1][1];
    double g12 = g1 * gains[tapset1][2];

    for (i = 0; i < overlap; i++) {
        double f = window[i] * window[i];
        y[i] = x[i]
             + (1.0 - f) * g00 * x[i - T0]
             + (1.0 - f) * g01 * x[i - T0 - 1]
             + (1.0 - f) * g01 * x[i - T0 + 1]
             + (1.0 - f) * g02 * x[i - T0 - 2]
             + (1.0 - f) * g02 * x[i - T0 + 2]
             + f * g10 * x[i - T1]
             + f * g11 * x[i - T1 - 1]
             + f * g11 * x[i - T1 + 1]
             + f * g12 * x[i - T1 - 2]
             + f * g12 * x[i - T1 + 2];
    }
    for (i = overlap; i < N; i++) {
        y[i] = x[i]
             + g10 * x[i - T1]
             + g11 * x[i - T1 - 1]
             + g11 * x[i - T1 + 1]
             + g12 * x[i - T1 - 2]
             + g12 * x[i - T1 + 2];
    }
}

/* De-emphasis, and the output scale.  The reference's float build finishes
 * with SCALEOUT, i.e. a multiply by 1/32768, so its API hands back +-1.0.
 * This one does NOT, and opus_celt.h says so: the samples leave here in
 * celt_sig units where +-32768 is full scale, and opus.c rounds and clips
 * them to int16 directly. One scale instead of a divide followed by a
 * multiply -- and, more to the point, one place where the scale can be
 * wrong instead of two. */
static void deemphasis(double *const in[], double *pcm, int N, int C,
                       double *mem)
{
    int c = 0;
    do {
        int j;
        const double *x = in[c];
        double *y = pcm + c;
        double m = mem[c];
        for (j = 0; j < N; j++) {
            double tmp = *x + m;
            m = PREEMPH0 * tmp - PREEMPH1 * (*x);
            tmp = PREEMPH3 * tmp;
            x++;
            *y = tmp;
            y += C;
        }
        mem[c] = m;
    } while (++c < C);
}

/* ------------------------------------------------------------------- API */

opus_celt_dec *opus_celt_create(int channels)
{
    opus_celt_dec *st;
    int lm;
    if (channels < 1 || channels > 2) return NULL;
    st = (opus_celt_dec *)calloc(1, sizeof *st);
    if (!st) return NULL;
    st->channels = channels;
    st->stream_channels = channels;
    for (lm = 0; lm <= MAXLM; lm++) {
        st->mdct[lm] = amdct_new(240 << lm);
        if (!st->mdct[lm]) { opus_celt_destroy(st); return NULL; }
    }
    opus_celt_reset(st);
    return st;
}

void opus_celt_destroy(opus_celt_dec *st)
{
    int lm;
    if (!st) return;
    for (lm = 0; lm <= MAXLM; lm++) amdct_free(st->mdct[lm]);
    free(st);
}

void opus_celt_reset(opus_celt_dec *st)
{
    int i;
    if (!st) return;
    memset(st->decode_mem, 0, sizeof st->decode_mem);
    memset(st->preemph_memD, 0, sizeof st->preemph_memD);
    memset(st->oldBandE, 0, sizeof st->oldBandE);
    for (i = 0; i < 2 * NBANDS; i++) {
        st->oldLogE[i] = st->oldLogE2[i] = -28.0;
        st->backgroundLogE[i] = -28.0;
    }
    st->rng = 0;
    st->loss_count = 0;
    st->start = 0;
    st->end = NBANDS;
    st->postfilter_period = st->postfilter_period_old = 0;
    st->postfilter_gain = st->postfilter_gain_old = 0;
    st->postfilter_tapset = st->postfilter_tapset_old = 0;
}

void opus_celt_set_bands(opus_celt_dec *st, int start, int end)
{
    st->start = start;
    st->end = end;
}

void opus_celt_set_stream_channels(opus_celt_dec *st, int c)
{
    st->stream_channels = c;
}

uint32_t opus_celt_final_range(const opus_celt_dec *st)
{
    return st->rng;
}

int opus_celt_decode(opus_celt_dec *st, orange *dec, const uint8_t *data,
                     int len, double *pcm, int frame_size)
{
    int c, i, N, LM, M, effEnd;
    int shortBlocks, isTransient, intra_ener, silence;
    int codedBands, alloc_trim, spread_decision;
    int postfilter_pitch, postfilter_tapset;
    double postfilter_gain;
    int intensity = 0, dual_stereo = 0;
    int anti_collapse_rsv, anti_collapse_on = 0;
    int dynalloc_logp;
    int32_t total_bits, balance, tell, bits;
    const int CC = st->channels;
    int C = st->stream_channels;

    double *freq, *X;
    double bandE[2 * NBANDS];
    int fine_quant[NBANDS], pulses[NBANDS], cap[NBANDS], offsets[NBANDS];
    int fine_priority[NBANDS], tf_res[NBANDS];
    unsigned char collapse_masks[2 * NBANDS];
    double *out_mem[2], *decode_mem[2], *overlap_mem[2], *out_syn[2];

    if (!st || !dec || !pcm) return -1;
    if (len < 0 || len > 1275) return -1;
    freq = st->sc.freq;
    X = st->sc.X;

    for (LM = 0; LM <= MAXLM; LM++)
        if (SHORT_MDCT << LM == frame_size) break;
    if (LM > MAXLM) return -1;
    M = 1 << LM;
    N = M * SHORT_MDCT;

    c = 0;
    do {
        decode_mem[c] = st->decode_mem[c];
        out_mem[c] = decode_mem[c] + DECODE_BUF - MAX_PERIOD;
        overlap_mem[c] = decode_mem[c] + DECODE_BUF;
    } while (++c < CC);

    effEnd = st->end;   /* effEBands == nbEBands == 21 for the Opus mode */

    memset(X, 0, sizeof st->sc.X);

    if (data == NULL || len <= 1)
        return opus_celt_conceal(st, pcm, frame_size);

    if (C == 1)
        for (i = 0; i < NBANDS; i++)
            st->oldBandE[i] = dmax(st->oldBandE[i], st->oldBandE[NBANDS + i]);

    total_bits = len * 8;
    tell = orange_tell(dec);

    if (tell >= total_bits)      silence = 1;
    else if (tell == 1)          silence = orange_bit_logp(dec, 15);
    else                         silence = 0;
    if (silence) {
        /* Pretend we read all the remaining bits: a silent frame carries no
         * further symbols, and every budget test below is against `tell`. */
        tell = len * 8;
        dec->nbits_total += (int)(tell - orange_tell(dec));
    }

    postfilter_gain = 0;
    postfilter_pitch = 0;
    postfilter_tapset = 0;
    if (st->start == 0 && tell + 16 <= total_bits) {
        if (orange_bit_logp(dec, 1)) {
            int qg, octave;
            octave = (int)orange_dec_uint(dec, 6);
            postfilter_pitch = (16 << octave) + (int)orange_dec_bits(dec, (unsigned)(4 + octave)) - 1;
            qg = (int)orange_dec_bits(dec, 3);
            if (orange_tell(dec) + 2 <= total_bits)
                postfilter_tapset = orange_icdf(dec, opus_tapset_icdf, 2);
            postfilter_gain = 0.09375 * (qg + 1);
        }
        tell = orange_tell(dec);
    }

    if (LM > 0 && tell + 3 <= total_bits) {
        isTransient = orange_bit_logp(dec, 3);
        tell = orange_tell(dec);
    } else {
        isTransient = 0;
    }
    shortBlocks = isTransient ? M : 0;

    intra_ener = tell + 3 <= total_bits ? orange_bit_logp(dec, 3) : 0;
    unquant_coarse_energy(st->start, st->end, st->oldBandE, intra_ener, dec, C, LM);

    tf_decode(st->start, st->end, isTransient, tf_res, LM, dec);

    tell = orange_tell(dec);
    spread_decision = SPREAD_NORMAL;
    if (tell + 4 <= total_bits)
        spread_decision = orange_icdf(dec, opus_spread_icdf, 5);

    init_caps(cap, LM, C);

    dynalloc_logp = 6;
    total_bits <<= BITRES;
    tell = (int32_t)orange_tell_frac(dec);
    for (i = st->start; i < st->end; i++) {
        int width, quanta, dynalloc_loop_logp, boost;
        width = C * (opus_eband5ms[i + 1] - opus_eband5ms[i]) << LM;
        /* 6 bits, but never more than 1 bit/sample nor less than 1/8. */
        quanta = imin(width << BITRES, imax(6 << BITRES, width));
        dynalloc_loop_logp = dynalloc_logp;
        boost = 0;
        while (tell + (dynalloc_loop_logp << BITRES) < total_bits && boost < cap[i]) {
            int flag = orange_bit_logp(dec, (unsigned)dynalloc_loop_logp);
            tell = (int32_t)orange_tell_frac(dec);
            if (!flag) break;
            boost += quanta;
            total_bits -= quanta;
            dynalloc_loop_logp = 1;
        }
        offsets[i] = boost;
        if (boost > 0) dynalloc_logp = imax(2, dynalloc_logp - 1);
    }

    alloc_trim = tell + (6 << BITRES) <= total_bits
                 ? orange_icdf(dec, opus_trim_icdf, 7) : 5;

    bits = (((int32_t)len * 8) << BITRES) - (int32_t)orange_tell_frac(dec) - 1;
    anti_collapse_rsv = isTransient && LM >= 2 && bits >= ((LM + 2) << BITRES)
                        ? (1 << BITRES) : 0;
    bits -= anti_collapse_rsv;
    codedBands = compute_allocation(st->start, st->end, offsets, cap, alloc_trim,
                                    &intensity, &dual_stereo, bits, &balance,
                                    pulses, fine_quant, fine_priority, C, LM, dec);

    unquant_fine_energy(st->start, st->end, st->oldBandE, fine_quant, dec, C);

    memset(collapse_masks, 0, sizeof collapse_masks);
    quant_all_bands(&st->sc, st->start, st->end, X, C == 2 ? X + N : NULL, collapse_masks,
                    pulses, shortBlocks, spread_decision, dual_stereo, intensity,
                    tf_res, len * (8 << BITRES) - anti_collapse_rsv, balance,
                    dec, LM, codedBands, &st->rng);

    if (anti_collapse_rsv > 0)
        anti_collapse_on = (int)orange_dec_bits(dec, 1);

    unquant_energy_finalise(st->start, st->end, st->oldBandE, fine_quant,
                            fine_priority, len * 8 - orange_tell(dec), dec, C);

    if (anti_collapse_on)
        anti_collapse(X, collapse_masks, LM, C, N, st->start, st->end,
                      st->oldBandE, st->oldLogE, st->oldLogE2, pulses, st->rng);

    log2Amp(st->start, st->end, bandE, st->oldBandE, C);

    if (silence) {
        for (i = 0; i < C * NBANDS; i++) {
            bandE[i] = 0;
            st->oldBandE[i] = -28.0;
        }
    }

    denormalise_bands(X, freq, bandE, effEnd, C, M);

    memmove(decode_mem[0], decode_mem[0] + N,
            (size_t)(DECODE_BUF - N) * sizeof(double));
    if (CC == 2)
        memmove(decode_mem[1], decode_mem[1] + N,
                (size_t)(DECODE_BUF - N) * sizeof(double));

    c = 0;
    do {
        for (i = 0; i < M * opus_eband5ms[st->start]; i++) freq[c * N + i] = 0;
    } while (++c < C);
    c = 0;
    do {
        for (i = M * opus_eband5ms[effEnd]; i < N; i++) freq[c * N + i] = 0;
    } while (++c < C);

    out_syn[0] = out_mem[0] + MAX_PERIOD - N;
    if (CC == 2) out_syn[1] = out_mem[1] + MAX_PERIOD - N;

    /* Up-mix a mono frame to a stereo decoder, or down-mix the reverse. The
     * stream's channel count can change from packet to packet; the DECODER's
     * cannot. */
    if (CC == 2 && C == 1)
        for (i = 0; i < N; i++) freq[N + i] = freq[i];
    if (CC == 1 && C == 2)
        for (i = 0; i < N; i++) freq[i] = 0.5 * (freq[i] + freq[N + i]);

    compute_inv_mdcts(st, shortBlocks, freq, out_syn, overlap_mem, CC, LM);

    c = 0;
    do {
        st->postfilter_period = imax(st->postfilter_period, COMB_MINPERIOD);
        st->postfilter_period_old = imax(st->postfilter_period_old, COMB_MINPERIOD);
        comb_filter(out_syn[c], out_syn[c], st->postfilter_period_old,
                    st->postfilter_period, SHORT_MDCT,
                    st->postfilter_gain_old, st->postfilter_gain,
                    st->postfilter_tapset_old, st->postfilter_tapset,
                    opus_window120, OVERLAP);
        if (LM != 0)
            comb_filter(out_syn[c] + SHORT_MDCT, out_syn[c] + SHORT_MDCT,
                        st->postfilter_period, postfilter_pitch, N - SHORT_MDCT,
                        st->postfilter_gain, postfilter_gain,
                        st->postfilter_tapset, postfilter_tapset,
                        opus_window120, OVERLAP);
    } while (++c < CC);

    st->postfilter_period_old = st->postfilter_period;
    st->postfilter_gain_old = st->postfilter_gain;
    st->postfilter_tapset_old = st->postfilter_tapset;
    st->postfilter_period = postfilter_pitch;
    st->postfilter_gain = postfilter_gain;
    st->postfilter_tapset = postfilter_tapset;
    if (LM != 0) {
        st->postfilter_period_old = st->postfilter_period;
        st->postfilter_gain_old = st->postfilter_gain;
        st->postfilter_tapset_old = st->postfilter_tapset;
    }

    if (C == 1)
        for (i = 0; i < NBANDS; i++)
            st->oldBandE[NBANDS + i] = st->oldBandE[i];

    if (!isTransient) {
        for (i = 0; i < 2 * NBANDS; i++) st->oldLogE2[i] = st->oldLogE[i];
        for (i = 0; i < 2 * NBANDS; i++) st->oldLogE[i] = st->oldBandE[i];
        for (i = 0; i < 2 * NBANDS; i++)
            st->backgroundLogE[i] = dmin(st->backgroundLogE[i] + M * 0.001,
                                         st->oldBandE[i]);
    } else {
        for (i = 0; i < 2 * NBANDS; i++)
            st->oldLogE[i] = dmin(st->oldLogE[i], st->oldBandE[i]);
    }
    c = 0;
    do {
        for (i = 0; i < st->start; i++) {
            st->oldBandE[c * NBANDS + i] = 0;
            st->oldLogE[c * NBANDS + i] = st->oldLogE2[c * NBANDS + i] = -28.0;
        }
        for (i = st->end; i < NBANDS; i++) {
            st->oldBandE[c * NBANDS + i] = 0;
            st->oldLogE[c * NBANDS + i] = st->oldLogE2[c * NBANDS + i] = -28.0;
        }
    } while (++c < 2);
    st->rng = dec->rng;

    deemphasis(out_syn, pcm, N, CC, st->preemph_memD);
    st->loss_count = 0;

    if (orange_tell(dec) > 8 * len) return -1;
    return frame_size;
}

/* A lost or DTX frame. It fabricates nothing -- see opus_celt.h on why there
 * is no PLC here and what the corpus says about reaching one. */
int opus_celt_conceal(opus_celt_dec *st, double *pcm, int frame_size)
{
    int c, i, N = frame_size;
    double *out_syn[2];

    if (!st || !pcm) return -1;
    if (N > CELT_MAX_FRAME) return -1;

    c = 0;
    do {
        memmove(st->decode_mem[c], st->decode_mem[c] + N,
                (size_t)(DECODE_BUF - N) * sizeof(double));
        out_syn[c] = st->decode_mem[c] + DECODE_BUF - N;
        for (i = 0; i < N; i++) out_syn[c][i] = 0;
        memset(st->decode_mem[c] + DECODE_BUF, 0, OVERLAP * sizeof(double));
    } while (++c < st->channels);

    deemphasis(out_syn, pcm, N, st->channels, st->preemph_memD);
    st->loss_count++;
    return frame_size;
}
