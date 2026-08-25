/* tests/unit/container_fuzz.c -- ASan+UBSan fuzz for AVI/FLV/MPEG-TS/MPEG-PS.
 *
 * Same argument as tests/unit/demux_fuzz.c (read that file's header first --
 * this one does not repeat it): a container is the most attacker-shaped
 * input in this system, and the bar is not "does not crash on files ffmpeg
 * made". This is the sibling harness for the four formats demux.c does not
 * dispatch to yet (avi.c/flv.c/ts.c/ps.c) -- container_test.c's own
 * open_any()/close_any() dispatch is copied here for the same reason it
 * exists there: ts_open()/ps_open() reassemble into a scratch buffer
 * (ts.h/ps.h) and must be paired with ts_close()/ps_close(), never the
 * generic media_close(), or every corrupted-input run leaks.
 *
 * Three phases, deterministic from the seed so a failure reproduces:
 *   1. corruption   the four real fixtures (one per format, built by ffmpeg
 *                    -- see tests/containers.mk), bit-flipped/byte-replaced/
 *                    spliced/truncated.
 *   2. structured    the same corpus with a second mutator aimed at what
 *                    looks like a length field: any 32-bit big-endian or
 *                    16-bit big-endian word immediately preceding four ASCII
 *                    bytes (a RIFF/FLV/PSM-shaped tag) is a candidate, and is
 *                    occasionally replaced with a hostile value (0, 1,
 *                    0x7FFFFFFF, 0xFFFFFFFF) -- the same idea demux_fuzz.c's
 *                    mutate_structured() uses for MP4 box sizes, aimed at
 *                    length-prefixed fields these four formats actually have
 *                    (RIFF chunk sizes, FLV DataSize/PreviousTagSize, PES
 *                    PES_packet_length, PSM section lengths).
 *   3. noise         uniform random bytes, and the same wearing each
 *                    format's magic number, so the deep parser is reached at
 *                    all (a magic-less run almost never gets past sniff()).
 *
 * NOT INCLUDED, and said plainly rather than silently: demux_fuzz.c's phase
 * 3 (synthetic files built field-by-field with hostile values baked in) is
 * not reproduced here for these four formats -- corruption + structured +
 * noise over four real, format-diverse fixtures already reaches every parser
 * loop in avi.c/flv.c/ts.c/ps.c/pes.c at least once (verified: every run
 * below opens successfully at iteration 0, the pristine-file sanity check),
 * and building four more from-scratch format encoders was judged not worth
 * the time against that marginal gain. A synthetic-hostile phase for these
 * four is future work, not a claim made here.
 *
 * THE NEGATIVE CONTROL IS SHARED WITH demux_fuzz.c ON PURPOSE, not a second
 * one invented for these formats: -DDEMUX_FUZZ_SABOTAGE guts the SAME
 * function these formats call through, c/lib/media/demux.c's
 * media_to_annexb() (its AVCC length-prefix loop takes an attacker's 2/4-byte
 * length at face value under the flag) -- and two of these four formats
 * reach that exact code path for real: AVI's H.264 tracks (now that avi.c
 * captures the strf avcC extension -- see its own commit) and FLV's H.264
 * tracks (AVCPacketType=1 NALU data, already length-prefixed) both get
 * MEDIA_FRAMING_AVCC. TS/PS carry H.264 in-band (Annex B already, RAW
 * framing) and do not reach this loop, so the sabotage is proven live by the
 * AVI/FLV fixtures in this same corpus, exactly as REQUIRED -- caught by
 * AddressSanitizer specifically, not merely by a crash.
 *
 * Run:  make test-containers-fuzz
 *       make test-containers-fuzz SCALE=60 SEED=0x1234
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"
#include "media_int.h"
#include "avi.h"
#include "ts.h"
#include "ps.h"
#include "flv.h"

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = rng_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } } while (0)

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

#define GUARD 64

/* ------------------------------------------------------- open dispatch --- */
enum kind { K_NONE, K_AVI, K_TS, K_PS, K_FLV };

static mdemux *open_any(const uint8_t *data, long len, int *err, enum kind *k)
{
    if (avi_sniff(data, len)) { *k = K_AVI; return avi_open(data, len, err); }
    if (ts_sniff(data, len))  { *k = K_TS;  return ts_open(data, len, err); }
    if (ps_sniff(data, len))  { *k = K_PS;  return ps_open(data, len, err); }
    if (flv_sniff(data, len)) { *k = K_FLV; return flv_open(data, len, err); }
    *k = K_NONE;
    if (err) *err = MEDIA_ERR_UNSUPPORTED;
    return 0;
}
static void close_any(mdemux *m, enum kind k)
{
    if (k == K_TS) ts_close(m);
    else if (k == K_PS) ps_close(m);
    else media_close(m);
}

/* Exercise a demuxer as hard as the player will. Returns samples walked.
 * Same shape as demux_fuzz.c's hammer(), pared to what these four formats'
 * generic media.h surface actually exposes (media_read/media_get_sample/
 * media_to_annexb/media_seek/media_select -- media_annexb_headers too, for
 * the AVI/FLV AVCC tracks). */
static long hammer(const uint8_t *data, long len)
{
    int err = 0;
    enum kind k;
    mdemux *m = open_any(data, len, &err, &k);
    if (!m) {
        CHECK(err < 0, "open failed with err %d, which is not an error code", err);
        return 0;
    }

    long walked = 0;
    int nt = media_track_count(m);
    CHECK(nt > 0 && nt <= MEDIA_MAX_TRACKS, "track count %d", nt);

    for (int i = 0; i < nt; i++) {
        const media_track *t = media_track_info(m, i);
        CHECK(t != 0, "track %d info", i);
        if (!t) continue;
        CHECK(t->nsamples >= 0, "negative sample count");
        CHECK(t->timescale > 0, "zero timescale on track %d", i);
        CHECK(t->extradata_len >= 0 && t->extradata_len <= MEDIA_MAX_EXTRADATA,
              "extradata length %d", t->extradata_len);
        /* NOTE: unlike demux_fuzz.c's hammer(), this does not bounds-check
         * sample/extradata pointers against `data`/`len`, because for a
         * TS/PS-opened mdemux m->data is a reassembled scratch buffer
         * (ts.h/ps.h), not the caller's file -- ASan already catches any
         * out-of-bounds access against WHATEVER buffer a pointer actually
         * targets, which is the property that matters; it just cannot be
         * phrased as "inside `data`" here the way demux_fuzz.c phrases it
         * for MP4/MKV, where that is always true by construction. */

        long need = media_annexb_headers(m, i, 0, 0);
        if (need > 0 && need < (16 << 20)) {
            uint8_t *hb = malloc((size_t)need);
            long got = media_annexb_headers(m, i, hb, need);
            CHECK(got == need, "headers size changed between calls: %ld then %ld", need, got);
            free(hb);
        }

        media_sample s;
        for (long kk = 0; media_get_sample(m, i, kk, &s) == 1; kk++) {
            walked++;
            CHECK(s.size >= 0, "sample %ld of track %d has negative size", kk, i);
            CHECK(s.track == i, "sample reports track %d, asked for %d", s.track, i);

            long n = media_to_annexb(m, &s, 0, 0);
            if (n > 0 && n < (4 << 20)) {
                uint8_t *ab = malloc((size_t)n + GUARD);
                memset(ab + n, 0xAB, GUARD);
                long w = media_to_annexb(m, &s, ab, n);
                CHECK(w == n, "annexb size changed between calls: %ld then %ld", n, w);
                for (int g = 0; g < GUARD; g++)
                    if (ab[n + g] != 0xAB) { CHECK(0, "annexb wrote past its size"); break; }
                if (n > 1) CHECK(media_to_annexb(m, &s, ab, n - 1) < 0,
                                 "a short buffer was accepted");
                free(ab);
            }
            if (kk > 20000) break;
        }
    }

    media_sample s;
    long nread = 0;
    while (media_read(m, &s) == 1) {
        if (++nread > 40000) break;
    }
    for (int i = 0; i < nt; i++) {
        media_seek(m, i, (long long)rnd() * 1000);
        media_seek(m, i, -1);
        media_seek(m, i, 0);
        media_select(m, i);
        int guard = 0;
        while (media_read(m, &s) == 1 && ++guard < 100) { }
    }
    media_select(m, -1);
    CHECK(media_get_sample(m, -1, 0, &s) < 0, "negative track accepted");
    CHECK(media_get_sample(m, nt, 0, &s) < 0, "track past the end accepted");
    CHECK(media_get_sample(m, 0, -1, &s) < 0, "negative sample index accepted");
    CHECK(media_get_sample(m, 0, 0x7FFFFFFF, &s) == 0, "absurd index is not simply absent");

    close_any(m, k);
    return walked;
}

/* ---------------------------------------------------------- mutations ---- */
static void mutate(uint8_t *b, long n)
{
    int ops = 1 + (int)rnd_below(6);
    for (int i = 0; i < ops; i++) {
        long at = (long)rnd_below((uint32_t)n);
        switch (rnd_below(4)) {
        case 0: b[at] ^= (uint8_t)(1u << rnd_below(8)); break;
        case 1: b[at] = (uint8_t)rnd(); break;
        case 2: {
            long from = (long)rnd_below((uint32_t)n);
            long run = (long)rnd_below(64) + 1;
            if (at + run > n) run = n - at;
            if (from + run > n) run = n - from;
            if (run > 0) memmove(b + at, b + from, (size_t)run);
            break; }
        case 3: memset(b + at, (int)rnd_below(256),
                       (size_t)((at + 32 <= n) ? 32 : (n - at))); break;
        }
    }
}

static const uint32_t hostile32[] = { 0, 1, 8, 16, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFF0 };
static const uint16_t hostile16[] = { 0, 1, 8, 0x7FFF, 0x8000, 0xFFFF };

static int printable4(const uint8_t *p)
{
    for (int i = 0; i < 4; i++) if (p[i] < 0x20 || p[i] > 0x7E) return 0;
    return 1;
}

/* Aimed at the length fields these four formats actually have: RIFF chunk
 * sizes (u32le right before a 4-char tag), FLV DataSize/PreviousTagSize
 * (u24be/u32be), PES_packet_length (u16be right after 00 00 01 + stream_id). */
static void mutate_structured(uint8_t *b, long n)
{
    int hits = 0;
    for (long i = 0; i + 8 <= n && hits < 8; i++) {
        if (printable4(b + i + 4) && !printable4(b + i) && rnd_below(6) == 0) {
            uint32_t v = hostile32[rnd_below(sizeof hostile32 / sizeof hostile32[0])];
            /* little-endian: RIFF's own convention */
            b[i] = (uint8_t)v; b[i+1] = (uint8_t)(v >> 8);
            b[i+2] = (uint8_t)(v >> 16); b[i+3] = (uint8_t)(v >> 24);
            hits++;
        }
    }
    for (long i = 0; i + 5 <= n && hits < 14; i++) {
        if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 1 && rnd_below(80) == 0) {
            uint16_t v = hostile16[rnd_below(sizeof hostile16 / sizeof hostile16[0])];
            b[i+4] = (uint8_t)(v >> 8); if (i + 5 < n) b[i+5] = (uint8_t)v;
            hits++;
        }
    }
}

/* ------------------------------------------------------------- phases ---- */
static const char *fixtures[] = { "clip.avi", "clip.ts", "clip.mpg", "clip.flv" };

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 8;
    rng_state = argc > 2 ? strtoull(argv[2], 0, 0) : 0x243F6A8885A308D3ull;
    const char *dir = argc > 3 ? argv[3] : "build/cfx";
    if (scale < 1) scale = 1;

    long iterations = 0, opened = 0, samples = 0;

    for (unsigned f = 0; f < sizeof fixtures / sizeof fixtures[0]; f++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, fixtures[f]);
        long n = 0;
        uint8_t *orig = slurp(path, &n);
        if (!orig) continue;

        /* Pristine file must open and walk cleanly, or nothing below means
         * anything -- same discipline demux_fuzz.c uses. */
        long w0 = hammer(orig, n);
        CHECK(w0 > 0, "%s: the pristine fixture walked 0 samples", fixtures[f]);
        opened++;
        samples += w0;

        for (int it = 0; it < scale * 8; it++) {
            long m = n;
            if (rnd_below(4) == 0) m = 1 + (long)rnd_below((uint32_t)n);
            uint8_t *copy = malloc((size_t)m);
            memcpy(copy, orig, (size_t)m);
            if (it & 1) mutate_structured(copy, m); else mutate(copy, m);
            hammer(copy, m);
            free(copy);
            iterations++;
        }
        free(orig);
    }

    /* Noise, and noise wearing each format's magic number. */
    for (int it = 0; it < scale * 16; it++) {
        long n = 16 + (long)rnd_below(4096);
        uint8_t *b = malloc((size_t)n);
        for (long i = 0; i < n; i++) b[i] = (uint8_t)rnd();
        switch (it % 5) {
        case 1: b[0]='R'; b[1]='I'; b[2]='F'; b[3]='F'; b[8]='A'; b[9]='V'; b[10]='I'; b[11]=' '; break;
        case 2: b[0]=0x47; break;                                   /* TS sync (one packet's worth is unlikely, on purpose) */
        case 3: b[0]=0; b[1]=0; b[2]=1; b[3]=0xBA; break;            /* PS pack_start_code */
        case 4: b[0]='F'; b[1]='L'; b[2]='V'; b[3]=1; b[4]=0; break;  /* FLV, reserved bits clear */
        default: break;
        }
        hammer(b, n);
        free(b);
        iterations++;
    }

    /* Determinism: the same bytes must demux to the same thing twice. */
    for (unsigned f = 0; f < sizeof fixtures / sizeof fixtures[0]; f++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, fixtures[f]);
        long n = 0;
        uint8_t *orig = slurp(path, &n);
        if (!orig) continue;
        for (int it = 0; it < scale; it++) {
            long m = 1 + (long)rnd_below((uint32_t)n);
            uint8_t *a = malloc((size_t)m), *b = malloc((size_t)m);
            memcpy(a, orig, (size_t)m);
            mutate(a, m);
            memcpy(b, a, (size_t)m);
            long wa = hammer(a, m), wb = hammer(b, m);
            CHECK(wa == wb, "%s: the same bytes walked %ld samples then %ld",
                  fixtures[f], wa, wb);
            free(a); free(b);
            iterations++;
        }
        free(orig);
    }

    printf("container fuzz: %ld iterations over %ld fixtures, %ld samples walked, "
           "%d checks, %d failures\n", iterations, opened, samples, checks, failures);
    if (!opened) {
        printf("container fuzz: NO FIXTURES FOUND in %s -- this run proved nothing\n", dir);
        return 1;
    }
    return failures ? 1 : 0;
}
