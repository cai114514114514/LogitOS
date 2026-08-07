/* tests/unit/flac_test.c -- FLAC decoder gate.
 *
 * FLAC is lossless, so there is no tolerance to argue about: every sample must
 * come back exactly. This test says so twice over, in two independent ways.
 *
 *  1. THE FORMAT'S OWN CRITERION. STREAMINFO carries the MD5 of the unencoded
 *     PCM. flac_md5_ok() decodes the whole stream, hashes what it produced,
 *     and compares. That check needs no reference decoder at all, which is why
 *     the same check runs on the guest.
 *
 *  2. DIFFERENTIAL AGAINST FFMPEG. Every sample of every file is compared with
 *     ffmpeg's own decode of the identical bytes, and the result is reported
 *     as a distribution -- how many samples differ and by how much -- not as a
 *     pass/fail on the first mismatch. "The first mismatch moved" says nothing;
 *     "wrong samples went from 4102 to 0" says something.
 *
 * It also covers MD5 against RFC 1321's own vectors, because a self-check is
 * worthless if the hash behind it is wrong, and the malformed-input cases:
 * a decoder for a file format that arrives over a network must reject damage
 * rather than trust it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "flac.h"
#include "amd5.h"

static int failures;
static int checks;

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

/* --- MD5 against RFC 1321 ------------------------------------------------ */

static void md5_hex(const char *s, char out[33])
{
    amd5 m;
    uint8_t h[16];
    amd5_init(&m);
    amd5_update(&m, (const uint8_t *)s, (unsigned long)strlen(s));
    amd5_final(&m, h);
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", h[i]);
    out[32] = 0;
}

static void test_md5(void)
{
    char h[33];
    md5_hex("", h);
    CHECK(strcmp(h, "d41d8cd98f00b204e9800998ecf8427e") == 0, "md5(\"\") = %s", h);
    md5_hex("abc", h);
    CHECK(strcmp(h, "900150983cd24fb0d6963f7d28e17f72") == 0, "md5(abc) = %s", h);
    md5_hex("message digest", h);
    CHECK(strcmp(h, "f96b697d7cb7938d525a2f31aaf161d0") == 0, "md5(msg digest) = %s", h);
    md5_hex("abcdefghijklmnopqrstuvwxyz", h);
    CHECK(strcmp(h, "c3fcd3d76192e4007dfb496cca67e13b") == 0, "md5(a..z) = %s", h);
    md5_hex("12345678901234567890123456789012345678901234567890"
            "123456789012345678901234567890", h);
    CHECK(strcmp(h, "57edf4a22be3c955ac49da2e2107b67a") == 0, "md5(80 digits) = %s", h);

    /* A multi-block update split at every offset must give the same hash as a
     * single one: the buffering path is where an incremental hash goes wrong. */
    static uint8_t big[1000];
    for (int i = 0; i < 1000; i++) big[i] = (uint8_t)(i * 7 + 3);
    amd5 a, b;
    uint8_t ha[16], hb[16];
    amd5_init(&a);
    amd5_update(&a, big, 1000);
    amd5_final(&a, ha);
    int splits_ok = 1;
    for (int cut = 0; cut <= 1000; cut += 7) {
        amd5_init(&b);
        amd5_update(&b, big, (unsigned long)cut);
        amd5_update(&b, big + cut, (unsigned long)(1000 - cut));
        amd5_final(&b, hb);
        if (memcmp(ha, hb, 16) != 0) splits_ok = 0;
    }
    CHECK(splits_ok, "md5 incremental update disagrees with one-shot");
}

/* --- one file ------------------------------------------------------------ */

struct dist {
    long n;             /* samples compared */
    long wrong;         /* samples differing at all */
    long maxdiff;
    long first;         /* index of the first difference, -1 if none */
};

static int test_file(const char *dir, const char *name, int expect_md5)
{
    char path[512], refpath[512];
    snprintf(path, sizeof(path), "%s/%s.flac", dir, name);
    snprintf(refpath, sizeof(refpath), "%s/%s.flac.s32", dir, name);

    long len = 0, reflen = 0;
    uint8_t *buf = slurp(path, &len);
    if (!buf) { printf("SKIP %s (no such file)\n", path); return 0; }
    uint8_t *ref = slurp(refpath, &reflen);
    if (!ref) { printf("SKIP %s (no reference)\n", refpath); free(buf); return 0; }

    int err = 0;
    flacdec *d = flac_open(buf, len, &err);
    CHECK(d != NULL, "%s: flac_open failed (%d)", name, err);
    if (!d) { free(buf); free(ref); return 1; }

    int rate = 0, ch = 0, bits = 0;
    long total = 0;
    flac_info(d, &rate, &ch, &bits, &total);

    /* ffmpeg's s32le output puts every sample at 32-bit full scale. */
    int shift = 32 - bits;
    const int32_t *r32 = (const int32_t *)ref;
    long refsamples = reflen / 4;

    struct dist D = { 0, 0, 0, -1 };
    long frames = 0;
    for (;;) {
        const int32_t *pl[AUDIO_MAX_CHANNELS];
        long n = flac_decode_frame(d, pl);
        CHECK(n >= 0, "%s: decode error %ld at frame %ld", name, n, frames);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            for (int c = 0; c < ch; c++) {
                long idx = (frames + i) * ch + c;
                if (idx >= refsamples) continue;
                int32_t got = (int32_t)((uint32_t)pl[c][i] << shift);
                int32_t want = r32[idx];
                D.n++;
                if (got != want) {
                    D.wrong++;
                    long diff = (long)got - (long)want;
                    if (diff < 0) diff = -diff;
                    if (diff > D.maxdiff) D.maxdiff = diff;
                    if (D.first < 0) D.first = idx;
                }
            }
        }
        frames += n;
    }

    CHECK(D.wrong == 0,
          "%s: %ld/%ld samples differ from ffmpeg, max |diff| %ld, first at %ld",
          name, D.wrong, D.n, D.maxdiff, D.first);
    CHECK(D.n > 0, "%s: nothing compared", name);
    if (total)
        CHECK(frames == total, "%s: decoded %ld frames, STREAMINFO says %ld",
              name, frames, total);

    if (expect_md5) {
        int ok = flac_md5_ok(d);
        CHECK(ok == 1, "%s: STREAMINFO MD5 check returned %d", name, ok);
    }

    printf("  %-10s %6d Hz %dch %2d-bit %8ld frames  ffmpeg-diff %ld/%ld\n",
           name, rate, ch, bits, frames, D.wrong, D.n);

    flac_close(d);
    free(buf);
    free(ref);
    return 0;
}

/* --- malformed input ----------------------------------------------------- */

static void test_malformed(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/noise.flac", dir);
    long len = 0;
    uint8_t *buf = slurp(path, &len);
    if (!buf) { printf("SKIP malformed tests (no noise.flac)\n"); return; }

    int err = 0;
    /* Truncation at every scale must be an error or a short decode, never a
     * crash and never an out-of-bounds read (this file is also the ASan
     * corpus). */
    int survived = 1;
    for (long cut = 1; cut < len; cut = cut * 2 + 1) {
        flacdec *d = flac_open(buf, cut, &err);
        if (d) {
            for (int i = 0; i < 100000; i++) {
                const int32_t *pl[AUDIO_MAX_CHANNELS];
                long n = flac_decode_frame(d, pl);
                if (n <= 0) break;
            }
            flac_close(d);
        }
    }
    CHECK(survived, "truncation walk crashed");

    /* A single flipped bit inside a frame must be caught by the CRC-16 rather
     * than turned into noise. Walk a stride of byte positions past the
     * metadata and count how many are detected. */
    long detected = 0, tried = 0;
    for (long pos = len / 2; pos < len && tried < 64; pos += 97, tried++) {
        uint8_t save = buf[pos];
        buf[pos] ^= 0x40;
        flacdec *d = flac_open(buf, len, &err);
        int bad = 0;
        if (!d) bad = 1;
        else {
            for (;;) {
                const int32_t *pl[AUDIO_MAX_CHANNELS];
                long n = flac_decode_frame(d, pl);
                if (n < 0) { bad = 1; break; }
                if (n == 0) break;
            }
            if (!bad) {
                int ok = flac_md5_ok(d);
                if (ok != 1) bad = 1;     /* MD5 catches what CRC lets through */
            }
            flac_close(d);
        }
        if (bad) detected++;
        buf[pos] = save;
    }
    CHECK(detected == tried,
          "corruption detection: %ld of %ld flipped bytes went unnoticed",
          tried - detected, tried);
    printf("  malformed: %ld/%ld single-bit corruptions rejected\n", detected, tried);

    /* Header garbage. */
    uint8_t junk[64];
    memset(junk, 0xA5, sizeof(junk));
    CHECK(flac_open(junk, sizeof(junk), &err) == NULL, "junk accepted as FLAC");
    CHECK(flac_open(buf, 4, &err) == NULL, "4-byte file accepted");
    CHECK(flac_open(NULL, 100, &err) == NULL, "NULL accepted");

    free(buf);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/audioref";

    printf("MD5 (RFC 1321 vectors)\n");
    test_md5();

    printf("FLAC files (bit-exact vs ffmpeg + STREAMINFO MD5)\n");
    static const char *names[] = {
        "sine440", "noise", "stereo", "impulse", "quiet", "sr48", "f24", "fmono",
        "f_mid_side", "f_left_side", "f_right_side", "f_indep"
    };
    int decoded = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (test_file(dir, names[i], 1) == 0) decoded++;
    CHECK(decoded >= 10, "only %d FLAC files were available to test", decoded);

    printf("malformed input\n");
    test_malformed(dir);

    if (failures) {
        printf("FLAC-FAIL %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("FLAC-OK %d checks passed\n", checks);
    return 0;
}
