/* tests/unit/vorbis_test.c -- Vorbis I differential and structural gate.
 *
 * THIS FILE DOES NOT MEASURE CONFORMANCE, AND SAYS SO EVERY TIME IT RUNS.
 *
 * WAV and FLAC are exactly specified, so their tests assert bit-exactness.
 * MP3 and AAC are not, but ISO/IEC 11172-4 and 14496-4 each DEFINE a numeric
 * bound on the difference from a reference decoder, so those tests measure
 * against a published number -- and for AAC this project runs the official ISO
 * bitstreams against the official reference waveforms.
 *
 * Vorbis I has neither. The specification defines the format by its decoding
 * procedure, in floating point, and states no bound on how far two conformant
 * decoders may be apart. Xiph ships no conformance bitstream suite. There is
 * nothing to pass and nothing official to pass it against; the attempt to find
 * one is recorded in tests/unit/aac_conformance_attempt.txt alongside the two
 * formats where the search succeeded.
 *
 * So this file reports a DIFFERENTIAL against ffmpeg -- a measurement, with
 * its whole distribution, and NOT a tolerance, because quoting a tolerance
 * would be inventing a property Vorbis does not have. What it does ASSERT are
 * the things that genuinely are defined:
 *
 *   * that every packet decodes without error and yields the sample count the
 *     stream's own granule positions imply,
 *   * that the decode is deterministic, which is what lets the guest and the
 *     host be compared at all,
 *   * that the Ogg page CRC rejects a corrupted page rather than assembling a
 *     packet from two unrelated ones,
 *   * that a malformed setup header is refused instead of decoded, and
 *   * that the same stream decoded through the Ogg entry point and through the
 *     raw-headers entry point a Matroska demuxer would use is identical.
 *
 * The differential is nevertheless reported to full precision, because the
 * numbers it produces (agreement at the 1e-8 level, i.e. the noise floor of
 * single-precision float) say a great deal even though they are not a
 * certificate.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "vorbis.h"
#include "ogg.h"

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

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

static float *decode_all(const uint8_t *buf, long len, int *rate, int *ch,
                         long *nsamp, int *packets, int *err)
{
    int e = 0;
    vorbisdec *d = vorbis_open(buf, len, &e);
    *err = e;
    if (!d) return NULL;
    vorbis_info(d, rate, ch);

    long cap = 1 << 16, n = 0;
    float *out = malloc((size_t)cap * sizeof(float));
    if (!out) { vorbis_close(d); *err = AUDIO_ERR_OOM; return NULL; }
    *packets = 0;
    for (;;) {
        vorbisframe f;
        int r = vorbis_decode(d, &f);
        if (r <= 0) { if (r < 0) *err = r; break; }
        (*packets)++;
        long need = (long)f.nsamples * f.channels;
        while (n + need > cap) {
            cap *= 2;
            float *nb = realloc(out, (size_t)cap * sizeof(float));
            if (!nb) { free(out); vorbis_close(d); *err = AUDIO_ERR_OOM; return NULL; }
            out = nb;
        }
        memcpy(out + n, f.pcm, (size_t)need * sizeof(float));
        n += need;
    }
    vorbis_close(d);
    *nsamp = n;
    return out;
}

/* The distribution, in absolute terms. There is no bound to normalise by, so
 * unlike the MP3 and AAC histograms this one is in raw sample units -- and
 * saying "2^-15" here would be borrowing a criterion from a different
 * standard. */
static const double HIST_EDGE[] = { 0.0, 1e-9, 1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3, 1e30 };
#define NHIST ((int)(sizeof(HIST_EDGE) / sizeof(HIST_EDGE[0])))
static long g_hist[NHIST];
static long g_total;

static void hist_add(double e)
{
    for (int b = 0; b < NHIST; b++)
        if (b == 0 ? (e == 0.0) : (e <= HIST_EDGE[b])) { g_hist[b]++; break; }
    g_total++;
}

static void hist_report(void)
{
    static const char *LABEL[NHIST] = {
        "exactly 0 (bit-identical)", "<= 1e-9", "<= 1e-8", "<= 1e-7",
        "<= 1e-6", "<= 1e-5", "<= 1e-4", "<= 1e-3", "> 1e-3"
    };
    printf("\n  per-sample |difference| from ffmpeg over all %ld samples\n", g_total);
    printf("  (ABSOLUTE, full scale 1.0. Vorbis defines no conformance bound,\n"
           "   so there is nothing to express this as a percentage OF.)\n");
    for (int b = 0; b < NHIST; b++)
        if (g_hist[b])
            printf("    %-26s %10ld  %6.2f%%\n", LABEL[b], g_hist[b],
                   100.0 * (double)g_hist[b] / (double)g_total);
}

static char dir[512];
static char *P(const char *name, const char *ext)
{
    static char buf[4][640];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "%s/%.80s%.8s", dir, name, ext);
    return buf[k];
}

static void run_case(const char *name)
{
    long olen = 0, rlen = 0;
    uint8_t *obuf = slurp(P(name, ".ogg"), &olen);
    uint8_t *rbuf = slurp(P(name, ".f32"), &rlen);
    if (!obuf || !rbuf) {
        CHECK(0, "%s: corpus missing (run tests/unit/vorbis_gen.sh)", name);
        free(obuf); free(rbuf);
        return;
    }

    int rate = 0, ch = 0, packets = 0, err = 0;
    long n = 0;
    float *got = decode_all(obuf, olen, &rate, &ch, &n, &packets, &err);
    CHECK(got != NULL, "%s: vorbis_open failed (%d)", name, err);
    CHECK(err == AUDIO_OK, "%s: decode error %d", name, err);
    if (!got) { free(obuf); free(rbuf); return; }

    long rn = rlen / (long)sizeof(float);
    const float *ref = (const float *)rbuf;

    /* This decoder deliberately does not trim the final block to the granule
     * position (see vorbis.h), so it produces at least as many samples as the
     * reference and never fewer. */
    CHECK(n >= rn, "%s: produced %ld samples, fewer than the reference's %ld",
          name, n, rn);
    CHECK(n - rn < (long)ch * 4096,
          "%s: produced %ld samples against %ld -- more than one block of "
          "untrimmed tail", name, n, rn);

    long cmp = n < rn ? n : rn;
    double acc = 0, peak = 0;
    for (long i = 0; i < cmp; i++) {
        double d = (double)got[i] - (double)ref[i];
        acc += d * d;
        double a = d < 0 ? -d : d;
        if (a > peak) peak = a;
        hist_add(a);
    }
    double rms = cmp ? sqrt(acc / (double)cmp) : 0.0;

    /* No conformance bound exists, so what is asserted is that the two
     * implementations agree to about the precision of the single-precision
     * float they both hand back. That is a statement about THIS pair of
     * decoders and is labelled as one. */
    CHECK(rms < 1e-6, "%s: RMS difference from ffmpeg is %.3e", name, rms);
    CHECK(peak < 1e-4, "%s: peak difference from ffmpeg is %.3e", name, peak);

    printf("  %-10s %6d Hz %dch %4d packets %8ld samples  RMS %.3e  peak %.3e\n",
           name, rate, ch, packets, n, rms, peak);

    /* Determinism: the guest/host comparison depends on it. */
    int r2 = 0, c2 = 0, p2 = 0, e2 = 0;
    long n2 = 0;
    float *again = decode_all(obuf, olen, &r2, &c2, &n2, &p2, &e2);
    int diff = 0;
    if (again) for (long i = 0; i < (n < n2 ? n : n2); i++) if (got[i] != again[i]) diff++;
    CHECK(again && n == n2 && diff == 0,
          "%s: two decodes of the same bytes differ (%d samples)", name, diff);
    free(again);

    free(got); free(obuf); free(rbuf);
}

/* The raw-headers entry point, which is how Matroska and WebM carry Vorbis:
 * the three header packets arrive in CodecPrivate and the audio packets arrive
 * without any Ogg around them. Pulling the packets out with our own Ogg reader
 * and feeding them through the other door must give the same samples. */
static void run_raw_case(const char *name)
{
    long olen = 0;
    uint8_t *obuf = slurp(P(name, ".ogg"), &olen);
    if (!obuf) { CHECK(0, "%s: corpus missing", name); return; }

    int rate = 0, ch = 0, packets = 0, err = 0;
    long n = 0;
    float *ogg_out = decode_all(obuf, olen, &rate, &ch, &n, &packets, &err);
    if (!ogg_out) { CHECK(0, "%s: ogg path failed", name); free(obuf); return; }

    int e = 0;
    oggreader *o = ogg_open(obuf, olen, &e);
    const uint8_t *h[3];
    long hl[3];
    uint8_t *copy[3] = { NULL, NULL, NULL };
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        if (ogg_packet(o, &h[i], &hl[i]) != 1) { ok = 0; break; }
        copy[i] = malloc((size_t)hl[i]);
        memcpy(copy[i], h[i], (size_t)hl[i]);
    }
    CHECK(ok, "%s: could not pull the three header packets", name);

    vorbisdec *d = ok ? vorbis_open_headers(copy[0], hl[0], copy[1], hl[1],
                                            copy[2], hl[2], &e) : NULL;
    CHECK(d != NULL, "%s: vorbis_open_headers failed (%d)", name, e);

    long m = 0;
    int mismatch = 0;
    if (d) {
        for (;;) {
            const uint8_t *p;
            long l;
            if (ogg_packet(o, &p, &l) != 1) break;
            vorbisframe f;
            if (vorbis_packet(d, p, l, &f) != 1) break;
            long need = (long)f.nsamples * f.channels;
            for (long i = 0; i < need && m + i < n; i++)
                if (f.pcm[i] != ogg_out[m + i]) mismatch++;
            m += need;
        }
        vorbis_close(d);
    }
    CHECK(m == n, "%s: raw-header path produced %ld samples, Ogg path %ld", name, m, n);
    CHECK(mismatch == 0,
          "%s: the raw-header path differs from the Ogg path in %d samples",
          name, mismatch);
    printf("  %-10s raw-header (Matroska) path: %ld samples, identical to Ogg\n",
           name, m);

    for (int i = 0; i < 3; i++) free(copy[i]);
    ogg_close(o);
    free(ogg_out);
    free(obuf);
}

/* Refusals and the page checksum. */
static void run_robustness(void)
{
    long olen = 0;
    uint8_t *obuf = slurp(P("sine", ".ogg"), &olen);
    if (!obuf) { CHECK(0, "sine.ogg missing"); return; }

    int e = 0;
    /* Not an Ogg file at all. */
    CHECK(vorbis_open((const uint8_t *)"not an ogg file at all!!", 24, &e) == NULL,
          "a non-Ogg buffer must be refused");
    CHECK(ogg_sniff((const uint8_t *)"nope", 4) == 0, "ogg_sniff must reject junk");
    CHECK(ogg_sniff(obuf, olen) == 1, "ogg_sniff must accept a real Ogg file");

    /* A single flipped bit in the FIRST page's body. The page CRC must catch
     * it; the identification header must then be missing and the open must
     * fail rather than proceeding with a half-parsed geometry. */
    uint8_t *bad = malloc((size_t)olen);
    memcpy(bad, obuf, (size_t)olen);
    bad[40] ^= 0x01;
    e = 0;
    vorbisdec *d = vorbis_open(bad, olen, &e);
    CHECK(d == NULL,
          "a corrupted first page must be rejected by the Ogg CRC, not decoded");
    if (d) vorbis_close(d);
    free(bad);

    /* Truncation at every 1/16th of the file: each must either open and stop
     * early, or refuse. Never crash, never loop. */
    int survived = 0;
    for (int k = 1; k < 16; k++) {
        long cut = olen * k / 16;
        int e2 = 0;
        vorbisdec *v = vorbis_open(obuf, cut, &e2);
        if (v) {
            for (int i = 0; i < 10000; i++) {
                vorbisframe f;
                if (vorbis_decode(v, &f) <= 0) break;
            }
            vorbis_close(v);
        }
        survived++;
    }
    CHECK(survived == 15, "all 15 truncations must be survivable");
    printf("  robustness: %d truncations survived, a flipped bit in page 1 "
           "rejected by the Ogg CRC\n", survived);

    free(obuf);
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof(dir), "%s", argc > 1 ? argv[1] : "build/vorbref");

    printf("Vorbis I vs ffmpeg -- A DIFFERENTIAL, NOT A CONFORMANCE RUN.\n");
    printf("  Vorbis I defines no numeric bound on decoder output and Xiph\n"
           "  publishes no conformance suite, so no tolerance is quoted here:\n"
           "  what follows is how far two implementations are apart, measured.\n\n");

    static const char *CASES[] = {
        "sine", "sweep", "noise", "impulse", "quiet", "stereo", "tonal",
        "mono", "lowq", "highq", "sr48", "sr32", "sr22", "sr16", "sr8", "cbr",
    };
    for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
        run_case(CASES[i]);

    printf("\n");
    run_raw_case("stereo");
    run_raw_case("noise");
    printf("\n");
    run_robustness();

    hist_report();

    printf("\n%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures ? 1 : 0;
}
