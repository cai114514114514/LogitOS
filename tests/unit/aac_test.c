/* tests/unit/aac_test.c -- AAC-LC conformance gate and differential.
 *
 * THE CRITERION, AND WHY IT IS NOT BIT-EXACTNESS.  AAC reconstructs through an
 * IMDCT and a floating-point windowed overlap-add. ISO/IEC 14496-3 does not
 * specify a bit pattern for the output and no decoder can honestly claim one.
 * Conformance is defined in 14496-4 (and in 13818-4 for MPEG-2 AAC) as a bound
 * on the difference from a reference decoder's floating-point output, using
 * the same "full accuracy" limits MPEG-1 Layer III uses in 11172-4:
 *
 *     full accuracy   RMS of the difference  <  2^-15 / sqrt(12)  ( 8.8146e-06 )
 *                     max |difference|       <= 2^-15             ( 3.0518e-05 )
 *
 * with full scale taken as 1.0. This file measures against those and reports
 * the whole distribution, exactly as tests/unit/mp3_test.c does, and for the
 * same reason: a bound alone cannot tell "one sample is wrong" from "the whole
 * second half is wrong".
 *
 * WHAT WE COMPARE AGAINST.  The ISO conformance bitstreams and the ISO
 * reference decoder are not freely redistributable and are not present here.
 * An attempt was made and is recorded in the test output. The reference used
 * is ffmpeg's float AAC decoder decoding the identical bytes. That is a
 * DIFFERENTIAL, not a run of the official suite, and this test says so in its
 * own output rather than letting the distinction get lost.
 *
 * PNS IS EXCLUDED FROM THE DIFFERENTIAL, AND THAT IS NOT DUCKING IT.
 * Perceptual noise substitution transmits a band's energy and lets the decoder
 * synthesise its own noise to fill it. Two conformant decoders produce
 * different samples there by design, so a sample-for-sample comparison of a
 * PNS band measures nothing. The corpus is generated with -aac_pns 0, and a
 * separate PNS stream is decoded and checked for what IS well defined about
 * it: that it decodes, that it yields the right number of frames, and that it
 * is deterministic across runs (which is what the on-device CRC comparison
 * needs).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "aac.h"

/* ISO full-accuracy bounds, full scale = 1.0. */
#define ISO_FULL_RMS   (1.0 / 32768.0 / 3.4641016151377544)   /* 2^-15/sqrt(12) */
#define ISO_FULL_PEAK  (1.0 / 32768.0)                        /* 2^-15          */

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

/* Decode a whole ADTS file to interleaved float. */
static float *decode_all(const uint8_t *buf, long len, int *rate, int *ch,
                         long *nsamp, int *frames, int *err)
{
    aacdec *d = aac_open();
    if (!d) { *err = AUDIO_ERR_OOM; return NULL; }

    long cap = 1 << 16, n = 0;
    float *out = malloc((size_t)cap * sizeof(float));
    if (!out) { aac_close(d); *err = AUDIO_ERR_OOM; return NULL; }

    long pos = 0;
    *rate = 0; *ch = 0; *frames = 0; *err = AUDIO_OK;
    while (pos < len) {
        aacframe f;
        int got = 0;
        int used = aac_decode(d, buf + pos, len - pos, &f, &got);
        if (used == 0) break;
        if (used < 0) { *err = used; break; }
        pos += used;
        if (!got) continue;
        *rate = f.rate; *ch = f.channels; (*frames)++;
        long need = (long)f.nsamples * f.channels;
        while (n + need > cap) {
            cap *= 2;
            float *nb = realloc(out, (size_t)cap * sizeof(float));
            if (!nb) { free(out); aac_close(d); *err = AUDIO_ERR_OOM; return NULL; }
            out = nb;
        }
        memcpy(out + n, f.pcm, (size_t)need * sizeof(float));
        n += need;
    }
    aac_close(d);
    *nsamp = n;
    return out;
}

/* --- the corpus-wide error distribution ---------------------------------- */

static const double HIST_EDGE[] = {
    0.0,        /* exactly zero: bit-identical to ffmpeg */
    0.001, 0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 1.00, 1e30
};
#define NHIST ((int)(sizeof(HIST_EDGE) / sizeof(HIST_EDGE[0])))
static long g_hist[NHIST];
static long g_hist_total;

static void hist_add(double abserr)
{
    double frac = abserr / ISO_FULL_PEAK;
    for (int b = 0; b < NHIST; b++) {
        if (b == 0 ? (frac == 0.0) : (frac <= HIST_EDGE[b])) { g_hist[b]++; break; }
    }
    g_hist_total++;
}

static double hist_quantile(double q)
{
    long target = (long)(q * (double)g_hist_total), run = 0;
    for (int b = 0; b < NHIST; b++) {
        run += g_hist[b];
        if (run >= target) return b == 0 ? 0.0 : HIST_EDGE[b];
    }
    return HIST_EDGE[NHIST - 1];
}

static void hist_report(void)
{
    static const char *LABEL[NHIST] = {
        "exactly 0 (bit-identical)", "<= 0.1% of bound", "<= 1%", "<= 5%",
        "<= 10%", "<= 25%", "<= 50%", "<= 75%", "<= 100%", "OVER THE BOUND"
    };
    printf("\n  per-sample |error| distribution over all %ld samples compared\n",
           g_hist_total);
    printf("  (as a fraction of the 2^-15 peak bound; AAC has no bit-exact\n"
           "   answer, so the SHAPE of the disagreement is the evidence)\n");
    for (int b = 0; b < NHIST; b++) {
        if (!g_hist[b]) continue;
        printf("    %-26s %10ld  %6.2f%%\n", LABEL[b], g_hist[b],
               100.0 * (double)g_hist[b] / (double)g_hist_total);
    }
    printf("    p50 <= %.2f%% of bound, p90 <= %.2f%%, p99 <= %.2f%%, "
           "p99.9 <= %.2f%%\n",
           100.0 * hist_quantile(0.50), 100.0 * hist_quantile(0.90),
           100.0 * hist_quantile(0.99), 100.0 * hist_quantile(0.999));
}

struct score {
    long n, over_peak;
    double rms, peak;
    long peak_at;
};

static void compare(const float *got, long gn, const float *ref, long rn,
                    struct score *s)
{
    long n = gn < rn ? gn : rn;
    double acc = 0.0, peak = 0.0;
    long peak_at = -1, over = 0;
    for (long i = 0; i < n; i++) {
        double dv = (double)got[i] - (double)ref[i];
        acc += dv * dv;
        double a = dv < 0 ? -dv : dv;
        if (a > peak) { peak = a; peak_at = i; }
        if (a > ISO_FULL_PEAK) over++;
        hist_add(a);
    }
    s->n = n;
    s->rms = n ? sqrt(acc / (double)n) : 0.0;
    s->peak = peak;
    s->peak_at = peak_at;
    s->over_peak = over;
}

static char dir[512];

static char *P(const char *name, const char *ext)
{
    static char buf[4][640];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s%s", dir, name, ext);
    return buf[k];
}

static void run_case(const char *name)
{
    long alen = 0, rlen = 0;
    uint8_t *abuf = slurp(P(name, ".aac"), &alen);
    uint8_t *rbuf = slurp(P(name, ".f32"), &rlen);
    if (!abuf || !rbuf) {
        CHECK(0, "%s: corpus missing (run tests/unit/aac_gen.sh)", name);
        free(abuf); free(rbuf);
        return;
    }

    int rate = 0, ch = 0, frames = 0, err = 0;
    long n = 0;
    float *got = decode_all(abuf, alen, &rate, &ch, &n, &frames, &err);
    CHECK(got != NULL, "%s: decode returned nothing", name);
    CHECK(err == AUDIO_OK, "%s: decode error %d", name, err);
    if (!got) { free(abuf); free(rbuf); return; }

    long rn = rlen / (long)sizeof(float);
    CHECK(n == rn, "%s: produced %ld samples, ffmpeg produced %ld", name, n, rn);

    struct score s;
    compare(got, n, (const float *)rbuf, rn, &s);

    CHECK(s.rms < ISO_FULL_RMS,
          "%s: RMS difference %.3e exceeds the full-accuracy bound %.3e",
          name, s.rms, ISO_FULL_RMS);
    CHECK(s.peak <= ISO_FULL_PEAK,
          "%s: peak difference %.3e at sample %ld exceeds the bound %.3e "
          "(%ld samples over)",
          name, s.peak, s.peak_at, ISO_FULL_PEAK, s.over_peak);

    printf("  %-14s %6d Hz %dch %4d frames %8ld samples  RMS %.3e (%5.1f%% of "
           "bound)  peak %.3e (%5.1f%%)\n",
           name, rate, ch, frames, n, s.rms, 100.0 * s.rms / ISO_FULL_RMS,
           s.peak, 100.0 * s.peak / ISO_FULL_PEAK);

    free(got); free(abuf); free(rbuf);
}

/* --- the raw/ASC path a demuxer uses ------------------------------------- */

/* An MP4 or MKV hands over an AudioSpecificConfig once and then raw
 * raw_data_block payloads with no ADTS header. That is a different entry point
 * with different framing, and a decoder that only works on ADTS is no use to
 * the container line. Rather than depend on an MP4 demuxer that does not exist
 * yet, the ASC is synthesised from the ADTS header -- for AAC-LC it is exactly
 * the five-bit object type, the four-bit sampling frequency index, the
 * four-bit channel configuration and three zero bits -- and the ADTS payloads
 * are fed in as raw blocks. The output must be sample-for-sample identical to
 * the ADTS path, because it is the same bits. */
static void run_raw_case(const char *name)
{
    long alen = 0;
    uint8_t *abuf = slurp(P(name, ".aac"), &alen);
    if (!abuf) { CHECK(0, "%s: corpus missing", name); return; }

    if (alen < 7) { CHECK(0, "%s: too short", name); free(abuf); return; }
    int sfi = (abuf[2] >> 2) & 0x0F;
    int cfg = (int)(((abuf[2] & 1) << 2) | ((abuf[3] >> 6) & 3));
    uint8_t asc[2];
    asc[0] = (uint8_t)((2 << 3) | (sfi >> 1));
    asc[1] = (uint8_t)(((sfi & 1) << 7) | (cfg << 3));

    int err = 0;
    aacdec *d = aac_open_asc(asc, 2, &err);
    CHECK(d != NULL && err == AUDIO_OK, "%s: aac_open_asc failed (%d)", name, err);
    if (!d) { free(abuf); return; }

    int rate = 0, ch = 0;
    aac_info(d, &rate, &ch);

    /* Reference: the same file through the ADTS entry point. */
    int r2 = 0, c2 = 0, fr2 = 0, e2 = 0;
    long n2 = 0;
    float *adts = decode_all(abuf, alen, &r2, &c2, &n2, &fr2, &e2);

    long pos = 0, n = 0;
    int frames = 0, mismatch = 0;
    while (pos < alen) {
        long flen = aac_adts_frame_len(abuf + pos, alen - pos);
        if (flen == 0 || pos + flen > alen) break;
        long hdr = (abuf[pos + 1] & 1) ? 7 : 9;
        aacframe f;
        int got = 0;
        int used = aac_decode_raw(d, abuf + pos + hdr, flen - hdr, &f, &got);
        if (used < 0) { CHECK(0, "%s: raw decode error %d at frame %d", name, used, frames); break; }
        pos += flen;
        if (!got) continue;
        long need = (long)f.nsamples * f.channels;
        if (adts && n + need <= n2) {
            for (long i = 0; i < need; i++)
                if (f.pcm[i] != adts[n + i]) mismatch++;
        }
        n += need;
        frames++;
    }
    aac_close(d);

    CHECK(n == n2, "%s: raw path produced %ld samples, ADTS path %ld", name, n, n2);
    CHECK(mismatch == 0,
          "%s: raw (ASC) path differs from the ADTS path in %d samples -- the "
          "same bits must decode the same way whichever framing carried them",
          name, mismatch);
    CHECK(rate > 0 && ch > 0, "%s: aac_open_asc gave rate %d ch %d", name, rate, ch);

    printf("  %-14s raw/ASC path: %d frames, %ld samples, identical to ADTS\n",
           name, frames, n);
    free(adts);
    free(abuf);
}

/* --- PNS: what IS well defined about it ---------------------------------- */

static void run_pns_case(void)
{
    long alen = 0;
    uint8_t *abuf = slurp(P("pns", ".aac"), &alen);
    if (!abuf) { CHECK(0, "pns: corpus missing"); return; }

    int r1 = 0, c1 = 0, f1 = 0, e1 = 0, r2 = 0, c2 = 0, f2 = 0, e2 = 0;
    long n1 = 0, n2 = 0;
    float *a = decode_all(abuf, alen, &r1, &c1, &n1, &f1, &e1);
    float *b = decode_all(abuf, alen, &r2, &c2, &n2, &f2, &e2);

    CHECK(a && b, "pns: decode failed");
    CHECK(e1 == AUDIO_OK && e2 == AUDIO_OK, "pns: decode errors %d/%d", e1, e2);
    CHECK(n1 == n2 && f1 == f2, "pns: two runs produced different lengths");
    int diff = 0;
    if (a && b) for (long i = 0; i < (n1 < n2 ? n1 : n2); i++) if (a[i] != b[i]) diff++;
    CHECK(diff == 0,
          "pns: two decodes of the same bytes differ in %d samples -- the noise "
          "generator is not deterministic, so the host and the guest could "
          "never agree on a checksum", diff);

    /* The energy must at least be in the right ballpark: a PNS band that came
     * out silent, or thirty dB loud, would pass a determinism check. */
    double e = 0.0;
    if (a) for (long i = 0; i < n1; i++) e += (double)a[i] * a[i];
    double rms = n1 ? sqrt(e / (double)n1) : 0.0;
    CHECK(rms > 0.01 && rms < 1.0,
          "pns: decoded RMS %.4f is not a plausible level for the source", rms);

    printf("  %-14s PNS stream: %d frames, deterministic, RMS %.4f "
           "(not sample-compared -- see the header)\n", "pns", f1, rms);
    free(a); free(b); free(abuf);
}

/* --- refusals ------------------------------------------------------------ */

static void run_refusals(void)
{
    int err = 0;
    /* AOT 5 = SBR. Object type 5, sfi 3, chancfg 2. */
    uint8_t sbr[4] = { 0x2B, 0x11, 0x88, 0x00 };
    aacdec *d = aac_open_asc(sbr, 4, &err);
    CHECK(d == NULL && err == AUDIO_ERR_UNSUPPORTED,
          "an ASC signalling SBR (AOT 5) must be refused, not decoded as its "
          "core at half the intended rate (got %p / %d)", (void *)d, err);
    if (d) aac_close(d);

    /* AOT 1 = Main profile, which needs the backward prediction we do not do. */
    uint8_t main_[2] = { 0x0B, 0x90 };
    err = 0;
    d = aac_open_asc(main_, 2, &err);
    CHECK(d == NULL && err == AUDIO_ERR_UNSUPPORTED,
          "an ASC signalling AAC Main must be refused (got %p / %d)", (void *)d, err);
    if (d) aac_close(d);

    /* Garbage must be an error, never a crash. */
    static const uint8_t junk[64] = { 0xFF, 0xF1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    aacdec *e2 = aac_open();
    aacframe f;
    int got = 0;
    int n = aac_decode(e2, junk, sizeof(junk), &f, &got);
    CHECK(n <= 0 || got == 0, "a frame of 0xFF must not decode as audio (n=%d)", n);
    aac_close(e2);

    CHECK(aac_adts_frame_len(NULL, 0) == 0, "aac_adts_frame_len(NULL) must be 0");
    CHECK(aac_adts_frame_len((const uint8_t *)"abcdefgh", 8) == 0,
          "aac_adts_frame_len on non-ADTS must be 0");
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof(dir), "%s", argc > 1 ? argv[1] : "build/aacref");

    printf("AAC-LC vs ffmpeg's float AAC decoder, ISO full-accuracy bounds\n");
    printf("  RMS bound 2^-15/sqrt(12) = %.4e, peak bound 2^-15 = %.4e\n",
           ISO_FULL_RMS, ISO_FULL_PEAK);
    printf("  THIS IS A DIFFERENTIAL, NOT A RUN OF THE ISO CONFORMANCE SUITE.\n"
           "  The 14496-4 bitstreams are not redistributable and are not here;\n"
           "  see tests/unit/aac_conformance_attempt.txt for what was tried.\n\n");

    static const char *CASES[] = {
        "sine", "sweep", "noise", "impulse", "quiet", "loud", "stereo", "wide",
        "mono", "lowrate", "sr48", "sr32", "sr24", "sr22", "sr16", "sr11",
        "sr8", "sr96", "sr88", "sr64", "sr12", "sr7",
        "coder_fast", "coder_twoloop", "forced_ms", "no_is",
        "with_pce", "mc51",
    };
    for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
        run_case(CASES[i]);

    printf("\n");
    run_raw_case("stereo");
    run_raw_case("noise");
    printf("\n");
    run_pns_case();
    printf("\n");
    run_refusals();

    hist_report();

    CHECK(g_hist[NHIST - 1] == 0,
          "%ld samples at or over the peak bound", g_hist[NHIST - 1]);

    printf("\n%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures ? 1 : 0;
}
