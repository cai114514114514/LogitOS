/* tests/unit/audio_units_test.c -- per-module tests for the audio decoders.
 *
 * A whole-file differential says something is wrong. These say what. Each
 * piece that can be checked on its own is checked against something written
 * independently of it -- a naive O(N^2) transform, libm, or a mathematical
 * property of the object itself -- rather than against a second copy of the
 * same idea.
 *
 * It includes mp3.c rather than linking it so that the internals (the IMDCTs,
 * the filter bank, the cube root, the Huffman trees) can be reached. They are
 * static because nothing outside the decoder should call them, not because
 * they should go untested.
 */

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abits.h"
#include "mp3.c"          /* deliberate: see the header comment */

static int failures, checks;

#define CHECK(cond, ...) do {                                  \
        checks++;                                              \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

/* --- the bit reader ------------------------------------------------------ */

static void test_bits(void)
{
    /* 0xB5 = 1011 0101, 0x3C = 0011 1100 */
    static const uint8_t buf[4] = { 0xB5, 0x3C, 0xFF, 0x00 };
    abits b;

    ab_init(&b, buf, 4);
    CHECK(ab_u(&b, 1) == 1, "u(1)");
    CHECK(ab_u(&b, 3) == 3, "u(3) across no boundary");   /* 011 */
    CHECK(ab_u(&b, 8) == 0x53, "u(8) across a byte boundary");
    CHECK(ab_pos(&b) == 12, "position after 12 bits");
    CHECK(ab_u(&b, 4) == 0xC, "u(4) tail of byte 1");
    CHECK(!ab_error(&b), "no error yet");

    /* Reading exactly to the end is fine; one bit more is an error, and the
     * error is sticky so later reads stay zero. */
    ab_init(&b, buf, 1);
    CHECK(ab_u(&b, 8) == 0xB5, "full byte");
    CHECK(ab_left(&b) == 0, "nothing left");
    CHECK(ab_u(&b, 1) == 0 && ab_error(&b), "read past the end errors");
    CHECK(ab_u(&b, 4) == 0, "sticky error returns zero");

    /* 32- and 64-bit reads. */
    static const uint8_t big[8] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
    ab_init(&b, big, 8);
    CHECK(ab_u(&b, 32) == 0x12345678u, "u(32)");
    ab_init(&b, big, 8);
    CHECK(ab_u64(&b, 64) == 0x123456789ABCDEF0ull, "u64(64)");
    ab_init(&b, big, 8);
    CHECK(ab_u64(&b, 36) == 0x123456789ull, "u64(36)");

    /* Signed fields, including the extremes where a naive shift is UB. */
    static const uint8_t s1[1] = { 0x80 };
    ab_init(&b, s1, 1);
    CHECK(ab_s(&b, 1) == -1, "s(1) of a set bit is -1");
    ab_init(&b, s1, 1);
    CHECK(ab_s(&b, 8) == -128, "s(8) of 0x80");
    static const uint8_t s2[1] = { 0x7F };
    ab_init(&b, s2, 1);
    CHECK(ab_s(&b, 8) == 127, "s(8) of 0x7F");
    static const uint8_t s3[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    ab_init(&b, s3, 4);
    CHECK(ab_s(&b, 32) == -1, "s(32) of all ones");

    /* Peeking past the end reads as zeros and must NOT trip the error flag:
     * Huffman decoding legitimately peeks a full codeword at the tail. */
    ab_init(&b, buf, 1);
    ab_skip(&b, 6);
    CHECK(ab_peek(&b, 16) == 0x4000, "peek past the end pads with zeros");
    CHECK(!ab_error(&b), "peek does not set the error flag");

    /* Unary runs, and the bound that stops a corrupt file from spinning. */
    static const uint8_t un[2] = { 0x08, 0x00 };    /* 00001 000 0000 0000 */
    ab_init(&b, un, 2);
    CHECK(ab_unary(&b, 100) == 4, "unary counts 4 zeros");
    ab_init(&b, un, 2);
    CHECK(ab_unary(&b, 2) == 0 && ab_error(&b), "unary respects its limit");

    /* Alignment. */
    ab_init(&b, buf, 4);
    ab_skip(&b, 3);
    CHECK(ab_align(&b) == 5 && ab_pos(&b) == 8, "align skips to the byte");
    CHECK(ab_align(&b) == 0, "align on a boundary is a no-op");

    /* Seeking out of range is refused rather than clamped silently. */
    ab_init(&b, buf, 4);
    ab_seek(&b, 33);
    CHECK(ab_error(&b), "seek past the end errors");
}

/* --- arithmetic ---------------------------------------------------------- */

static void test_arith(void)
{
    /* The from-scratch cube root against libm, over every value the format can
     * actually present to it. It does not need to be correctly rounded; it
     * needs to be accurate enough that x^(4/3) is exact to well under the
     * conformance bound, and identical on both builds. */
    double worst = 0.0;
    int worst_at = 0;
    for (int i = 1; i <= 8206; i++) {
        double got = a_cbrt((double)i);
        double want = cbrt((double)i);
        double rel = fabs(got - want) / want;
        if (rel > worst) { worst = rel; worst_at = i; }
    }
    CHECK(worst < 1e-15, "a_cbrt worst relative error %.3e at %d", worst, worst_at);
    CHECK(a_cbrt(0.0) == 0.0, "cbrt(0)");
    CHECK(fabs(a_cbrt(8.0) - 2.0) < 1e-15, "cbrt(8)");
    CHECK(fabs(a_cbrt(1e-9) - 1e-3) < 1e-17, "cbrt of a small value");
    printf("  a_cbrt: worst relative error %.3e over 8206 values\n", worst);

    /* 2^(e/4) over the whole range of exponents requantisation can produce.
     * The floor-division for negative e is the part that goes wrong. */
    worst = 0.0;
    for (int e = -400; e <= 100; e++) {
        double got = pow2q(e);
        double want = pow(2.0, e / 4.0);
        double rel = fabs(got - want) / want;
        if (rel > worst) worst = rel;
    }
    CHECK(worst < 1e-15, "pow2q worst relative error %.3e", worst);
    CHECK(pow2q(0) == 1.0, "2^0 exactly 1");
    CHECK(pow2q(4) == 2.0, "2^1 exactly 2");
    CHECK(pow2q(-4) == 0.5, "2^-1 exactly 0.5");
    CHECK(pow2q(-8) == 0.25, "2^-2 exactly 0.25");
    printf("  pow2q:  worst relative error %.3e over 501 exponents\n", worst);

    /* exp2i must be exact -- it is constructing an exponent field, so any
     * error at all means the field is wrong. */
    int exact = 1;
    for (int n = -200; n <= 200; n++)
        if (exp2i(n) != pow(2.0, n)) exact = 0;
    CHECK(exact, "exp2i is not exact");
}

/* --- Huffman codebooks --------------------------------------------------- */

static void test_huffman(void)
{
    /* Every codebook must be a complete prefix code -- the Kraft sum is
     * exactly 1 -- and the tree built from it must decode each codeword back
     * to the symbol it came from. The generator checks the first property in
     * Python; this checks it again on the emitted C, which is what actually
     * ships, and adds the round trip. */
    int books = 0;
    for (int t = 0; t < 34; t++) {
        const mp3_hufftab *tab = &mp3_huff[t];
        if (!tab->n) continue;
        books++;

        double kraft = 0.0;
        for (int s = 0; s < tab->n; s++) kraft += ldexp(1.0, -tab->len[s]);
        CHECK(fabs(kraft - 1.0) < 1e-12, "table %d kraft sum %.12f", t, kraft);

        int32_t *tree = NULL;
        CHECK(build_tree(&tree, tab) == AUDIO_OK && tree != NULL,
              "table %d tree build failed", t);
        if (!tree) continue;

        int roundtrip = 1;
        for (int s = 0; s < tab->n; s++) {
            /* Serialise this symbol's codeword MSB first, then decode it. */
            uint8_t bytes[8];
            memset(bytes, 0, sizeof(bytes));
            int len = tab->len[s];
            uint32_t code = tab->code[s];
            for (int i = 0; i < len; i++) {
                int bit = (int)((code >> (len - 1 - i)) & 1u);
                if (bit) bytes[i / 8] |= (uint8_t)(0x80 >> (i % 8));
            }
            abits b;
            ab_init(&b, bytes, 8);
            int got = huff_sym(&b, tree, 64);
            if (got != s || ab_pos(&b) != len) roundtrip = 0;
        }
        CHECK(roundtrip, "table %d does not round-trip every codeword", t);
        free(tree);
    }
    CHECK(books == 17, "expected 17 codebooks, found %d", books);
    printf("  huffman: %d codebooks, Kraft-complete and round-tripping\n", books);

    /* The table_select map must never point at an empty codebook for a value
     * an encoder can legally write, and linbits must match the standard's
     * pairing of tables 16-23 and 24-31. */
    static const int LIN16[8] = { 1, 2, 3, 4, 6, 8, 10, 13 };
    static const int LIN24[8] = { 4, 5, 6, 7, 8, 9, 11, 13 };
    for (int t = 16; t < 24; t++) {
        CHECK(mp3_ht_book[t] == 16, "table %d should use codebook 16", t);
        CHECK(mp3_ht_linbits[t] == LIN16[t - 16], "table %d linbits", t);
    }
    for (int t = 24; t < 32; t++) {
        CHECK(mp3_ht_book[t] == 24, "table %d should use codebook 24", t);
        CHECK(mp3_ht_linbits[t] == LIN24[t - 24], "table %d linbits", t);
    }
    CHECK(mp3_ht_book[0] == 0 && mp3_ht_book[4] == 0 && mp3_ht_book[14] == 0,
          "tables 0, 4 and 14 carry no codebook");
}

/* --- the transforms ------------------------------------------------------ */

/* Independently written references, straight from the equations, using libm.
 * The decoder's versions index precomputed periodic cosine tables instead; if
 * the modular index arithmetic is wrong these disagree immediately. */
static void ref_imdct36(const double *in, double *out)
{
    for (int i = 0; i < 36; i++) {
        double s = 0.0;
        for (int k = 0; k < 18; k++)
            s += in[k] * cos(M_PI / 72.0 * (2.0 * i + 1.0 + 18.0) * (2.0 * k + 1.0));
        out[i] = s;
    }
}

static void ref_imdct12(const double *in, double *out)
{
    for (int i = 0; i < 12; i++) {
        double s = 0.0;
        for (int k = 0; k < 6; k++)
            s += in[k] * cos(M_PI / 24.0 * (2.0 * i + 1.0 + 6.0) * (2.0 * k + 1.0));
        out[i] = s;
    }
}

static unsigned rngstate = 12345;
static double rnd(void)
{
    rngstate = rngstate * 1103515245u + 12345u;
    return ((double)((rngstate >> 8) & 0xFFFF) / 32768.0) - 1.0;
}

static void test_transforms(void)
{
    double worst36 = 0.0, worst12 = 0.0;
    for (int trial = 0; trial < 200; trial++) {
        double in[18], a[36], b[36];
        for (int i = 0; i < 18; i++) in[i] = rnd();
        imdct36(in, a);
        ref_imdct36(in, b);
        for (int i = 0; i < 36; i++) {
            double e = fabs(a[i] - b[i]);
            if (e > worst36) worst36 = e;
        }
        imdct12(in, a);
        ref_imdct12(in, b);
        for (int i = 0; i < 12; i++) {
            double e = fabs(a[i] - b[i]);
            if (e > worst12) worst12 = e;
        }
    }
    CHECK(worst36 < 1e-12, "imdct36 differs from the direct formula by %.3e", worst36);
    CHECK(worst12 < 1e-13, "imdct12 differs from the direct formula by %.3e", worst12);
    printf("  imdct:   36-point max error %.3e, 12-point %.3e (200 random blocks)\n",
           worst36, worst12);

    /* The block windows are the standard's, so check the properties the
     * standard states rather than the values: types 1 and 3 must be flat over
     * their long half and zero over the part the next block covers, and the
     * long window must satisfy the Princen-Bradley condition w[i]^2 +
     * w[i+18]^2 = 1 that makes the overlap-add reconstruct. */
    int flat = 1, zero = 1;
    for (int i = 18; i < 24; i++) if (mp3_win[1][i] != 1.0) flat = 0;
    for (int i = 30; i < 36; i++) if (mp3_win[1][i] != 0.0) zero = 0;
    for (int i = 12; i < 18; i++) if (mp3_win[3][i] != 1.0) flat = 0;
    for (int i = 0; i < 6; i++)   if (mp3_win[3][i] != 0.0) zero = 0;
    CHECK(flat, "start/stop windows are not flat where the standard says");
    CHECK(zero, "start/stop windows are not zero where the standard says");
    double worstpb = 0.0;
    for (int i = 0; i < 18; i++) {
        double s = mp3_win[0][i] * mp3_win[0][i] + mp3_win[0][i + 18] * mp3_win[0][i + 18];
        double e = fabs(s - 1.0);
        if (e > worstpb) worstpb = e;
    }
    CHECK(worstpb < 1e-15, "long window fails the Princen-Bradley identity by %.3e",
          worstpb);

    /* The alias-reduction butterflies must be a rotation: cs^2 + ca^2 = 1. */
    double worstaa = 0.0;
    for (int i = 0; i < 8; i++) {
        double s = mp3_aa_cs[i] * mp3_aa_cs[i] + mp3_aa_ca[i] * mp3_aa_ca[i];
        if (fabs(s - 1.0) > worstaa) worstaa = fabs(s - 1.0);
    }
    CHECK(worstaa < 1e-15, "alias butterflies are not orthonormal (%.3e)", worstaa);
}

/* --- the synthesis filter bank ------------------------------------------- */

/* The ISO decoder flow written out literally, with a real 1024-entry shift
 * register and libm cosines: no circular buffer, no cosine table. If the
 * decoder's modular indexing of either is wrong, this disagrees. */
struct refsynth { double V[1024]; };

static void ref_synth(struct refsynth *r, const double *S, double *out)
{
    for (int i = 1023; i >= 64; i--) r->V[i] = r->V[i - 64];
    for (int i = 0; i < 64; i++) {
        double s = 0.0;
        for (int k = 0; k < 32; k++)
            s += S[k] * cos((16.0 + i) * (2.0 * k + 1.0) * M_PI / 64.0);
        r->V[i] = s;
    }
    double U[512];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 32; j++) {
            U[i * 64 + j]      = r->V[i * 128 + j];
            U[i * 64 + 32 + j] = r->V[i * 128 + 96 + j];
        }
    for (int j = 0; j < 32; j++) {
        double s = 0.0;
        for (int i = 0; i < 16; i++) s += U[j + 32 * i] * mp3_dwin[j + 32 * i];
        out[j] = s;
    }
}

/* A plain DFT, for the band-selectivity property below. */
static double band_energy(const double *x, long n, double lo, double hi)
{
    double e = 0.0;
    long bins = n / 2;
    for (long b = 0; b < bins; b++) {
        double f = (double)b / (double)n;         /* cycles per sample */
        if (f < lo || f >= hi) continue;
        double re = 0.0, im = 0.0;
        for (long t = 0; t < n; t++) {
            double a = -2.0 * M_PI * (double)b * (double)t / (double)n;
            re += x[t] * cos(a);
            im += x[t] * sin(a);
        }
        e += re * re + im * im;
    }
    return e;
}

static void test_synth(void)
{
    mp3dec *d = mp3_open();
    CHECK(d != NULL, "mp3_open");
    if (!d) return;

    /* 1. against the literal ISO flow, over enough blocks that the shift
     *    register wraps its 1024 entries several times. */
    struct refsynth r;
    memset(&r, 0, sizeof(r));
    double worst = 0.0;
    float got[32];
    double want[32];
    for (int blk = 0; blk < 80; blk++) {
        double S[32];
        for (int k = 0; k < 32; k++) S[k] = rnd();
        synth(d, 0, S, got, 1);
        ref_synth(&r, S, want);
        for (int j = 0; j < 32; j++) {
            double e = fabs((double)got[j] - want[j]);
            if (e > worst) worst = e;
        }
    }
    CHECK(worst < 1e-6, "synth differs from the literal ISO flow by %.3e", worst);
    printf("  synth:   max deviation from the ISO flow %.3e over 80 blocks\n", worst);

    /* 2. the property the filter bank exists for: subband k must pass the
     *    frequency band [k/64, (k+1)/64) of the sample rate and reject the
     *    rest. This validates the window and the matrixing together, without
     *    reference to how either is stored. A wrong sign pattern in the window
     *    -- the bug that first showed up as a click every 32 samples -- puts
     *    the energy in the mirror band and fails here loudly. */
    double worst_leak = 0.0;
    int worst_band = -1;
    for (int k = 0; k < 32; k++) {
        mp3dec *e = mp3_open();
        if (!e) break;
        double x[512];
        for (int blk = 0; blk < 16; blk++) {
            double S[32];
            for (int i = 0; i < 32; i++) S[i] = 0.0;
            if (blk == 0) S[k] = 1.0;   /* an impulse, so x is the impulse response */
            float o[32];
            synth(e, 0, S, o, 1);
            for (int j = 0; j < 32; j++) x[blk * 32 + j] = o[j];
        }
        mp3_close(e);
        double lo = (double)k / 64.0, hi = (double)(k + 1) / 64.0;
        /* Allow the immediately adjacent bands: the prototype's transition
         * band is one subband wide by design, so "in band" means [k-1, k+2). */
        double inb = band_energy(x, 512, lo - 1.0 / 64.0, hi + 1.0 / 64.0);
        double all = band_energy(x, 512, 0.0, 0.5);
        double leak = all > 0 ? (all - inb) / all : 1.0;
        if (leak > worst_leak) { worst_leak = leak; worst_band = k; }
    }
    CHECK(worst_leak < 0.01,
          "subband impulse leaks %.4f of its energy outside its own band "
          "(worst at subband %d)", worst_leak, worst_band);
    printf("  synth:   worst out-of-band leakage %.5f (subband %d)\n",
           worst_leak, worst_band);

    mp3_close(d);
}

/* --- frame parsing ------------------------------------------------------- */

static void test_header(void)
{
    mp3hdr h;
    /* MPEG-1 Layer III, 128 kbps, 44100, stereo, no CRC. */
    uint8_t f[4] = { 0xFF, 0xFB, 0x90, 0x00 };
    CHECK(parse_header(f, 4, &h) == AUDIO_OK, "a valid header must parse");
    CHECK(h.rate == 44100 && h.bitrate == 128 && h.channels == 2, "header fields");
    CHECK(h.frame_bytes == 417, "frame size %d, expected 417", h.frame_bytes);
    CHECK(h.side_bytes == 32, "side info size");

    /* MPEG-2, 64 kbps, 22050, mono. */
    uint8_t g[4] = { 0xFF, 0xF3, 0x80, 0xC0 };
    CHECK(parse_header(g, 4, &h) == AUDIO_OK, "MPEG-2 header");
    CHECK(h.lsf == 1 && h.rate == 22050 && h.channels == 1, "MPEG-2 fields");
    CHECK(h.frame_bytes == 208, "MPEG-2 frame size %d", h.frame_bytes);
    CHECK(h.side_bytes == 9, "MPEG-2 mono side info");

    /* Each reserved encoding must be refused, not guessed at. */
    uint8_t bad[4];
    memcpy(bad, f, 4); bad[1] = 0xF3 | 0x18;      /* version 01 = reserved */
    bad[1] = (uint8_t)((f[1] & ~0x18) | 0x08);
    CHECK(parse_header(bad, 4, &h) != AUDIO_OK, "reserved version accepted");
    memcpy(bad, f, 4); bad[1] = (uint8_t)((f[1] & ~0x06) | 0x00);   /* layer reserved */
    CHECK(parse_header(bad, 4, &h) != AUDIO_OK, "reserved layer accepted");
    memcpy(bad, f, 4); bad[2] = (uint8_t)(f[2] | 0xF0);             /* bitrate 1111 */
    CHECK(parse_header(bad, 4, &h) != AUDIO_OK, "bad bitrate index accepted");
    memcpy(bad, f, 4); bad[2] = (uint8_t)(f[2] & 0x0F);             /* bitrate free */
    CHECK(parse_header(bad, 4, &h) == AUDIO_ERR_UNSUPPORTED, "free format not refused");
    memcpy(bad, f, 4); bad[2] = (uint8_t)(f[2] | 0x0C);             /* sample rate 11 */
    CHECK(parse_header(bad, 4, &h) != AUDIO_OK, "reserved sample rate accepted");
    memcpy(bad, f, 4); bad[3] = (uint8_t)((f[3] & ~3) | 2);         /* emphasis 10 */
    CHECK(parse_header(bad, 4, &h) != AUDIO_OK, "reserved emphasis accepted");
    memcpy(bad, f, 4); bad[0] = 0xFE;
    CHECK(parse_header(bad, 4, &h) != AUDIO_OK, "broken sync accepted");
    CHECK(parse_header(f, 3, &h) == AUDIO_ERR_RANGE, "short buffer must ask for more");

    /* CRC-16, cross-checked against an independent computation of the same
     * spec (polynomial 0x8005, init 0xFFFF, no reflection). The check value
     * for "123456789" under those parameters is 0xAEE7. */
    CHECK(mp3_crc16((const uint8_t *)"123456789", 9) == 0xAEE7,
          "crc16 check value is %04X, expected AEE7",
          mp3_crc16((const uint8_t *)"123456789", 9));

    /* ID3v2 skipping: the size field is syncsafe, so a byte with the high bit
     * set means it is not a tag at all. */
    uint8_t id3[64];
    memset(id3, 0, sizeof(id3));
    memcpy(id3, "ID3", 3);
    id3[9] = 20;
    CHECK(mp3_id3_len(id3, 64) == 30, "ID3v2 length %ld", mp3_id3_len(id3, 64));
    id3[5] = 0x10;                       /* footer present */
    CHECK(mp3_id3_len(id3, 64) == 40, "ID3v2 with footer");
    id3[5] = 0; id3[7] = 0x80;           /* not syncsafe */
    CHECK(mp3_id3_len(id3, 64) == 0, "non-syncsafe size must not be trusted");
    id3[7] = 0; id3[9] = 200;            /* claims more than the buffer holds */
    CHECK(mp3_id3_len(id3, 64) == 0, "oversized ID3 must not be trusted");
}

int main(void)
{
    printf("audio module tests\n");
    test_bits();
    test_arith();
    test_huffman();
    test_transforms();
    test_synth();
    test_header();

    if (failures) {
        printf("AUDIO-UNITS-FAIL %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("AUDIO-UNITS-OK %d checks passed\n", checks);
    return 0;
}
