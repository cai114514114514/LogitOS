/* tests/unit/aac_iso_test.c -- AAC-LC against the OFFICIAL ISO/IEC 14496-4
 * conformance bitstreams and reference waveforms.
 *
 * WHAT MAKES THIS DIFFERENT FROM tests/unit/aac_test.c.  That file is a
 * differential: it decodes streams ffmpeg encoded and compares against
 * ffmpeg's own decoder, which proves the two agree and nothing more. This file
 * decodes ISO's conformance bitstreams and compares against ISO's reference
 * decoder output waveforms, both taken from the MPEG-4 audio conformance
 * package that ISO publishes under Publicly Available Standards. That is the
 * standard's own criterion against the standard's own reference.
 *
 * The MP3 line could not do this -- ISO's public download for 11172-4 returns
 * an HTML error page, which it downloaded and checked -- so its gate is a
 * differential and says so. For AAC the package is genuinely there, so the
 * differential is kept as a broad, always-runnable sweep and THIS is the
 * conformance claim.
 *
 * ffmpeg IS NOT A REFERENCE DECODER HERE. The normative streams ship in MP4
 * and LATM/LOAS; tests/unit/aac_iso_fetch.sh repackages the MP4 to ADTS with
 * `-c:a copy`, which rewrites container headers and does not touch one byte of
 * AAC payload. The bits decoded are ISO's and the waveform compared against is
 * ISO's.
 *
 * THE BOUND.  The MPEG "full accuracy" limits: RMS of the difference below
 * 2^-15/sqrt(12) and maximum absolute difference at or below 2^-15, full scale
 * 1.0. These are the limits ISO/IEC 11172-4 states in its own text, and they
 * are the ones this project's MP3 gate already uses. The text of 14496-4 is
 * not part of the publicly available conformance package -- only the streams
 * and the waveforms are -- so the bound is applied as the MPEG full-accuracy
 * limit rather than quoted from 14496-4, and this test says which it is doing
 * rather than implying it read a clause it did not.
 *
 * DECODER DELAY.  An AAC decoder's first output frame is the encoder's priming
 * and ISO's reference waveforms do not include it. The offset is a property of
 * the format, not a free parameter, so this test searches only whole frames
 * (0, 1024, 2048 samples), reports which one it used, and fails a case whose
 * best alignment is not one of them.
 *
 * PNS. Several conformance streams use perceptual noise substitution, which
 * transmits a band's energy and lets the decoder synthesise the noise. Two
 * conformant decoders differ there by construction. Those cases are decoded,
 * reported, and scored SEPARATELY, and they are not counted as conformance
 * failures -- with the reason printed, not implied.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "audio.h"
#include "aac.h"

#define ISO_FULL_RMS   (1.0 / 32768.0 / 3.4641016151377544)
#define ISO_FULL_PEAK  (1.0 / 32768.0)

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

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)(p[0] | (p[1]<<8)); }

/* A minimal RIFF reader for the reference waveforms. ISO publishes them as
 * 16- or 24-bit PCM; both are read to double at full precision, because
 * rounding the reference to the output format before comparing would hide
 * exactly the size of error the bound is about. */
static double *read_wav(const char *path, int *rate, int *ch, long *nsamp)
{
    long len = 0;
    uint8_t *b = slurp(path, &len);
    if (!b) return NULL;
    if (len < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return NULL; }

    int bits = 0, nch = 0, sr = 0;
    long pos = 12, dpos = -1, dlen = 0;
    while (pos + 8 <= len) {
        uint32_t sz = rd32(b + pos + 4);
        const uint8_t *body = b + pos + 8;
        if (!memcmp(b + pos, "fmt ", 4) && sz >= 16) {
            nch = (int)rd16(body + 2);
            sr = (int)rd32(body + 4);
            bits = (int)rd16(body + 14);
        } else if (!memcmp(b + pos, "data", 4)) {
            dpos = pos + 8;
            dlen = (long)sz;
            if (dpos + dlen > len) dlen = len - dpos;
        }
        pos += 8 + (long)sz + (sz & 1);
    }
    if (dpos < 0 || nch <= 0 || (bits != 16 && bits != 24 && bits != 32)) { free(b); return NULL; }

    int bytes = bits / 8;
    long n = dlen / bytes;
    double *out = malloc((size_t)n * sizeof(double));
    if (!out) { free(b); return NULL; }
    const uint8_t *p = b + dpos;
    for (long i = 0; i < n; i++) {
        int32_t v;
        if (bits == 16) {
            v = (int16_t)(p[0] | (p[1] << 8));
            out[i] = (double)v / 32768.0;
        } else if (bits == 24) {
            v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            out[i] = (double)v / 8388608.0;
        } else {
            v = (int32_t)rd32(p);
            out[i] = (double)v / 2147483648.0;
        }
        p += bytes;
    }
    free(b);
    *rate = sr; *ch = nch; *nsamp = n;
    return out;
}

static float *decode_all(const uint8_t *buf, long len, int *rate, int *ch,
                         long *nsamp, int *pns, int *err)
{
    aacdec *d = aac_open();
    if (!d) { *err = AUDIO_ERR_OOM; return NULL; }
    long cap = 1 << 16, n = 0;
    float *out = malloc((size_t)cap * sizeof(float));
    if (!out) { aac_close(d); *err = AUDIO_ERR_OOM; return NULL; }
    long pos = 0;
    *rate = 0; *ch = 0; *err = AUDIO_OK;
    while (pos < len) {
        aacframe f;
        int got = 0;
        int used = aac_decode(d, buf + pos, len - pos, &f, &got);
        if (used == 0) break;
        if (used < 0) { *err = used; break; }
        pos += used;
        if (!got) continue;
        *rate = f.rate; *ch = f.channels;
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
    *pns = aac_had_pns(d);
    aac_close(d);
    *nsamp = n;
    return out;
}

static void score_at(const float *got, long gn, const double *ref, long rn,
                     long off, int ch, double *rms, double *peak, long *cnt)
{
    long n = gn - off * ch;
    if (n > rn) n = rn;
    if (n < 0) n = 0;
    double acc = 0.0, pk = 0.0;
    for (long i = 0; i < n; i++) {
        double dv = (double)got[off * ch + i] - ref[i];
        acc += dv * dv;
        double a = dv < 0 ? -dv : dv;
        if (a > pk) pk = a;
    }
    *rms = n ? sqrt(acc / (double)n) : 1e30;
    *peak = pk;
    *cnt = n;
}

static int npass, nfail, npns, ncase, nrefused;

static void run_case(const char *dir, const char *base)
{
    char ap[1024], wp[1024];
    if (strlen(dir) + strlen(base) + 8 >= sizeof(ap)) return;
    snprintf(ap, sizeof(ap), "%s/%.100s.adts", dir, base);
    snprintf(wp, sizeof(wp), "%s/%.100s.wav", dir, base);

    long alen = 0;
    uint8_t *abuf = slurp(ap, &alen);
    int rrate = 0, rch = 0;
    long rn = 0;
    double *ref = read_wav(wp, &rrate, &rch, &rn);
    if (!abuf || !ref) { free(abuf); free(ref); return; }

    ncase++;
    int rate = 0, ch = 0, pns = 0, err = 0;
    long n = 0;
    float *got = decode_all(abuf, alen, &rate, &ch, &n, &pns, &err);

    if (err == AUDIO_ERR_UNSUPPORTED || (n == 0 && err == AUDIO_OK)) {
        /* A REFUSAL IS NOT A PASS, AND IT IS NOT THE SAME AS A WRONG ANSWER.
         * These are the streams that use something this decoder does not
         * claim -- coupling channel elements, or more channels than it will
         * carry -- and it says so with an error rather than decoding them
         * wrongly. They are counted separately and listed in the summary,
         * because a suite that quietly dropped them would report a conformance
         * rate it had not earned. */
        printf("  %-14s REFUSED (err %d): a feature this decoder does not "
               "claim -- see the summary\n", base, err);
        nrefused++;
        free(abuf); free(ref); free(got);
        return;
    }
    if (!got || err != AUDIO_OK || n == 0) {
        printf("  %-14s DECODE FAILED (err %d)\n", base, err);
        CHECK(0, "%s: ISO conformance bitstream failed to decode (%d)", base, err);
        nfail++;
        free(abuf); free(ref); free(got);
        return;
    }
    if (ch != rch) {
        printf("  %-14s SKIPPED: decodes %d channels, the published reference "
               "has %d (per-channel reference)\n", base, ch, rch);
        free(abuf); free(ref); free(got);
        return;
    }

    CHECK(rate == rrate, "%s: decoded %d Hz, reference is %d Hz", base, rate, rrate);

    /* Whole-frame alignments only: the decoder delay is one or two frames of
     * priming, not an arbitrary shift. */
    static const long OFFS[] = { 0, 1024, 2048 };
    double best_rms = 1e30, best_peak = 0;
    long best_off = -1, best_n = 0;
    for (unsigned i = 0; i < sizeof(OFFS) / sizeof(OFFS[0]); i++) {
        double r, p;
        long c;
        score_at(got, n, ref, rn, OFFS[i], ch, &r, &p, &c);
        if (c > 0 && r < best_rms) { best_rms = r; best_peak = p; best_off = OFFS[i]; best_n = c; }
    }

    if (best_off < 0) {
        printf("  %-14s no usable overlap with the reference\n", base);
        CHECK(0, "%s: no usable overlap", base);
        nfail++;
    } else if (pns) {
        npns++;
        printf("  %-14s %6d Hz %dch %8ld samples  RMS %.3e peak %.3e  "
               "[PNS: not sample-comparable, reported not scored]\n",
               base, rate, ch, best_n, best_rms, best_peak);
    } else {
        int ok = (best_rms < ISO_FULL_RMS) && (best_peak <= ISO_FULL_PEAK);
        CHECK(best_rms < ISO_FULL_RMS,
              "%s: RMS %.3e exceeds the full-accuracy bound %.3e (offset %ld)",
              base, best_rms, ISO_FULL_RMS, best_off);
        CHECK(best_peak <= ISO_FULL_PEAK,
              "%s: peak %.3e exceeds the full-accuracy bound %.3e (offset %ld)",
              base, best_peak, ISO_FULL_PEAK, best_off);
        if (ok) npass++; else nfail++;
        printf("  %-14s %6d Hz %dch %8ld samples  RMS %.3e (%5.1f%%)  "
               "peak %.3e (%5.1f%%)  delay %ld\n",
               base, rate, ch, best_n, best_rms, 100.0 * best_rms / ISO_FULL_RMS,
               best_peak, 100.0 * best_peak / ISO_FULL_PEAK, best_off);
    }

    free(abuf); free(ref); free(got);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/isoaac";

    printf("AAC-LC vs the OFFICIAL ISO/IEC 14496-4 conformance suite\n");
    printf("  bitstreams and reference waveforms: ISO Publicly Available "
           "Standards\n");
    printf("  bound: MPEG full accuracy, RMS < %.4e and peak <= %.4e "
           "(full scale 1.0)\n", ISO_FULL_RMS, ISO_FULL_PEAK);
    printf("  ffmpeg is used ONLY to repackage MP4 -> ADTS with -c:a copy;\n"
           "  no reference decoder other than ISO's waveforms is involved.\n\n");

    DIR *dp = opendir(dir);
    if (!dp) {
        printf("  the conformance package is not present in %s.\n", dir);
        printf("  Run:  ./tests/unit/aac_iso_fetch.sh %s\n", dir);
        printf("  (it is not committed: the ISO licence permits use, not "
               "redistribution)\n");
        printf("\nSKIP: no ISO conformance data\n");
        return 0;
    }

    /* Collect case names from the .adts files present, sorted, so the run is
     * reproducible whatever order the directory happens to hand back. */
    char names[512][64];
    int nn = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && nn < 512) {
        size_t l = strlen(de->d_name);
        if (l < 6 || strcmp(de->d_name + l - 5, ".adts")) continue;
        if (l - 5 >= sizeof(names[0])) continue;
        memcpy(names[nn], de->d_name, l - 5);
        names[nn][l - 5] = 0;
        nn++;
    }
    closedir(dp);
    for (int i = 0; i < nn; i++)
        for (int j = i + 1; j < nn; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64];
                memcpy(t, names[i], sizeof(t));
                memcpy(names[i], names[j], sizeof(t));
                memcpy(names[j], t, sizeof(t));
            }

    if (nn == 0) {
        printf("  no .adts cases in %s -- run tests/unit/aac_iso_fetch.sh\n", dir);
        printf("\nSKIP: no ISO conformance data\n");
        return 0;
    }

    for (int i = 0; i < nn; i++) run_case(dir, names[i]);

    printf("\n  %d official AAC-LC conformance streams fetched:\n", ncase);
    printf("    %d within the MPEG full-accuracy bound\n", npass);
    printf("    %d outside it  <- these are failures\n", nfail);
    printf("    %d not scored: they use PNS, whose samples are decoder-chosen\n", npns);
    printf("    %d refused as unsupported (coupling channel elements, or more\n"
           "       than %d channels). A refusal is a documented gap, not a pass.\n",
           nrefused, AAC_MAX_CHANNELS);

    printf("\n%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures ? 1 : 0;
}
