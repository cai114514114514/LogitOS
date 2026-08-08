/* tests/unit/demux_fuzz.c -- ASan+UBSan fuzz for the container demuxers.
 *
 * A CONTAINER IS THE MOST ATTACKER-SHAPED INPUT IN THIS SYSTEM. It comes off
 * the network. It is a tree of nested lengths, every one of them written by a
 * stranger, and a demuxer's whole job is to walk that tree with a pointer and
 * turn the numbers in it into array subscripts, allocation sizes and file
 * offsets. An MP4 chunk offset is a raw file position the file itself
 * supplies; an EBML element size is a variable-length integer that can claim
 * eight bytes of length; a Matroska lace can claim 256 frames in a block that
 * holds three. So the bar is not "does not crash on the files ffmpeg made".
 *
 * Five phases, all deterministic from the seed so a failure reproduces:
 *
 *   1. corruption   real fixtures with bits flipped, bytes replaced, chunks
 *                   spliced and lengths truncated -- the shapes damage takes.
 *   2. structured   the fields that ARE the attack surface, hit on purpose:
 *                   every 32-bit big-endian word that looks like a box size
 *                   set to 0, 1, 8, 0x7FFFFFFF and 0xFFFFFFFF; every EBML
 *                   size vint set to the unknown-length pattern. Uniform
 *                   random bytes almost never get past a magic number, so
 *                   without this phase the deep parser is never reached.
 *   3. synthetic    boxes and EBML elements built from nothing with hostile
 *                   values: a stsz claiming four billion samples, an stsc with
 *                   samples_per_chunk of zero, a trun whose data offset points
 *                   before the file.
 *   4. random       uniform noise, and noise behind a valid magic prefix.
 *   5. properties   what must hold even on garbage -- a demuxer that opened
 *                   must return samples that lie inside the buffer it was
 *                   given, media_to_annexb must never write past the size it
 *                   asked for, the same bytes must demux identically twice,
 *                   and nothing may be left allocated (ASan's leak checker).
 *
 * Run:  make test-demux-fuzz
 *       make test-demux-fuzz SCALE=60 SEED=0x1234     (deeper)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"

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

/* An output buffer with a guard the caller can check. Under ASan an overrun
 * would be caught anyway; the guard also catches an overrun of a size the
 * function TOLD us it needed, which is a logic bug rather than a memory one. */
#define GUARD 64

/* Exercise a demuxer as hard as the player will, and then harder. Returns the
 * number of samples walked. Everything allocated is released on every path --
 * ASan's leak detector is the point of half this function. */
static long hammer(const uint8_t *data, long len)
{
    int err = 0;
    mdemux *m = media_open(data, len, &err);
    if (!m) {
        CHECK(err < 0, "media_open failed with err %d, which is not an error code", err);
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
        if (t->extradata_len)
            CHECK(t->extradata >= data && t->extradata + t->extradata_len <= data + len,
                  "extradata points outside the file");

        /* The parameter sets. A buffer exactly the size it asked for, so an
         * off-by-one in the Annex B writer lands in ASan's redzone. */
        long need = media_annexb_headers(m, i, 0, 0);
        if (need > 0 && need < (16 << 20)) {
            uint8_t *hb = malloc((size_t)need);
            long got = media_annexb_headers(m, i, hb, need);
            CHECK(got == need, "headers size changed between calls: %ld then %ld", need, got);
            free(hb);
        }

        media_sample s;
        for (long k = 0; media_get_sample(m, i, k, &s) == 1; k++) {
            walked++;
            CHECK(s.data >= data && s.size >= 0 && s.data + s.size <= data + len,
                  "sample %ld of track %d escapes the buffer", k, i);
            CHECK(s.track == i, "sample reports track %d, asked for %d", s.track, i);

            long n = media_to_annexb(m, &s, 0, 0);
            if (n > 0 && n < (4 << 20)) {
                uint8_t *ab = malloc((size_t)n + GUARD);
                memset(ab + n, 0xAB, GUARD);
                long w = media_to_annexb(m, &s, ab, n);
                CHECK(w == n, "annexb size changed between calls: %ld then %ld", n, w);
                for (int g = 0; g < GUARD; g++)
                    if (ab[n + g] != 0xAB) { CHECK(0, "annexb wrote past its size"); break; }
                /* One byte short must be refused, not truncated into. */
                if (n > 1) CHECK(media_to_annexb(m, &s, ab, n - 1) < 0,
                                 "a short buffer was accepted");
                free(ab);
            }
            if (k > 20000) break;          /* a fuzzed index can be enormous */
        }
    }

    /* The interleave, and seeking, which is where a cursor can be left out of
     * range for the next call to dereference. */
    media_sample s;
    long nread = 0;
    while (media_read(m, &s) == 1) {
        CHECK(s.data >= data && s.data + s.size <= data + len, "read escapes the buffer");
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
    /* Out-of-range indices reach a pointer only through here, so ask for
     * several that cannot exist. */
    CHECK(media_get_sample(m, -1, 0, &s) < 0, "negative track accepted");
    CHECK(media_get_sample(m, nt, 0, &s) < 0, "track past the end accepted");
    CHECK(media_get_sample(m, 0, -1, &s) < 0, "negative sample index accepted");
    CHECK(media_get_sample(m, 0, 0x7FFFFFFF, &s) == 0, "absurd index is not simply absent");

    media_close(m);
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
        case 2: {                                   /* splice a run from elsewhere */
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

/* Phase 2: aim at the fields that are the attack surface, rather than at
 * random bytes. Every four-byte word whose successor looks like a printable
 * four-character code is a box size; every byte whose leading-zero count makes
 * it an EBML length marker is a size. */
static const uint32_t hostile[] = { 0, 1, 8, 16, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFF0 };

static int printable4(const uint8_t *p)
{
    for (int i = 0; i < 4; i++) if (p[i] < 0x20 || p[i] > 0x7E) return 0;
    return 1;
}

static void mutate_structured(uint8_t *b, long n)
{
    int hits = 0;
    for (long i = 0; i + 8 <= n && hits < 6; i++) {
        if (printable4(b + i + 4) && !printable4(b + i)) {
            if (rnd_below(6) == 0) {
                uint32_t v = hostile[rnd_below(sizeof hostile / sizeof hostile[0])];
                b[i] = v >> 24; b[i+1] = v >> 16; b[i+2] = v >> 8; b[i+3] = v;
                hits++;
            }
        }
    }
    /* EBML: turn a size vint into the all-ones "unknown length" form, which is
     * the one that makes a parser scan forward for the next element. */
    for (long i = 0; i + 2 <= n && hits < 10; i++) {
        if ((b[i] & 0x80) && rnd_below(200) == 0) { b[i] = 0xFF; hits++; }
        else if ((b[i] & 0xC0) == 0x40 && rnd_below(200) == 0) { b[i] = 0x7F; b[i+1] = 0xFF; hits++; }
    }
}

/* Phase 3: build files rather than damage them. */
static long synth_mp4(uint8_t *b, long cap, uint32_t which)
{
    long at = 0;
    #define PUT32(v) do { b[at++] = (uint8_t)((v) >> 24); b[at++] = (uint8_t)((v) >> 16); \
                          b[at++] = (uint8_t)((v) >> 8); b[at++] = (uint8_t)(v); } while (0)
    #define PUT4(s) do { memcpy(b + at, (s), 4); at += 4; } while (0)
    if (cap < 512) return 0;
    PUT32(16); PUT4("ftyp"); PUT4("isom"); PUT32(0);
    long moov_at = at;
    PUT32(0); PUT4("moov");
    long trak_at = at;
    PUT32(0); PUT4("trak");
    /* mdia > mdhd + hdlr + minf > stbl > the hostile table */
    long mdia_at = at; PUT32(0); PUT4("mdia");
    PUT32(32); PUT4("mdhd"); PUT32(0); PUT32(0); PUT32(0);
    PUT32(which & 1 ? 0 : 1000);            /* timescale, sometimes zero */
    PUT32(1000); PUT32(0);
    PUT32(24); PUT4("hdlr"); PUT32(0); PUT32(0); PUT4("vide"); PUT32(0);
    long minf_at = at; PUT32(0); PUT4("minf");
    long stbl_at = at; PUT32(0); PUT4("stbl");
    PUT32(20); PUT4("stsz"); PUT32(0); PUT32(which & 2 ? 0 : 4);
    PUT32(which & 4 ? 0xFFFFFFFF : 3);      /* sample count */
    PUT32(20); PUT4("stsc"); PUT32(0); PUT32(1);
    PUT32(1); PUT32(which & 8 ? 0 : 2); PUT32(1);
    PUT32(20); PUT4("stco"); PUT32(0); PUT32(1); PUT32(which & 16 ? 0xFFFFFF00 : 0);
    PUT32(24); PUT4("stts"); PUT32(0); PUT32(1); PUT32(3); PUT32(100);
    #define CLOSE(x) do { long e = at; long s = e - (x); \
                          b[(x)]=(uint8_t)(s>>24); b[(x)+1]=(uint8_t)(s>>16); \
                          b[(x)+2]=(uint8_t)(s>>8); b[(x)+3]=(uint8_t)s; } while (0)
    CLOSE(stbl_at); CLOSE(minf_at); CLOSE(mdia_at); CLOSE(trak_at); CLOSE(moov_at);
    PUT32(64); PUT4("mdat");
    memset(b + at, 0x5A, 56); at += 56;
    #undef PUT32
    #undef PUT4
    #undef CLOSE
    return at;
}

static long synth_mkv(uint8_t *b, long cap, uint32_t which)
{
    /* EBML header, Segment (unknown size), Tracks, then a Cluster whose
     * SimpleBlock claims a lace of 255 frames in a handful of bytes. */
    static const uint8_t head[] = {
        0x1A,0x45,0xDF,0xA3, 0x84, 0x42,0x82,0x81,0x01,   /* EBML{DocType len1} */
        0x18,0x53,0x80,0x67, 0xFF                          /* Segment, unknown size */
    };
    if (cap < 128) return 0;
    long at = 0;
    memcpy(b, head, sizeof head); at += sizeof head;
    /* Tracks > TrackEntry > TrackNumber 1, TrackType 2, CodecID */
    static const uint8_t tracks[] = {
        0x16,0x54,0xAE,0x6B, 0x93,
          0xAE, 0x91,
            0xD7,0x81,0x01, 0x83,0x81,0x02,
            0x86,0x89,'A','_','M','P','E','G','/','L','3'
    };
    memcpy(b + at, tracks, sizeof tracks); at += sizeof tracks;
    b[at++] = 0x1F; b[at++] = 0x43; b[at++] = 0xB6; b[at++] = 0x75;
    long clen_at = at; b[at++] = 0x8F;                      /* size 15 */
    b[at++] = 0xE7; b[at++] = 0x81; b[at++] = 0x00;         /* Timestamp 0 */
    b[at++] = 0xA3; b[at++] = 0x8A;                         /* SimpleBlock, 10 bytes */
    b[at++] = 0x81;                                          /* track 1 */
    b[at++] = 0; b[at++] = 0;                                /* rel ts */
    b[at++] = (uint8_t)(0x80 | ((which % 4) << 1));          /* flags: lacing 0..3 */
    b[at++] = 0xFE;                                          /* 255 frames */
    b[at++] = 0xFF; b[at++] = 0xFF; b[at++] = 0xFF; b[at++] = 0xFF;
    (void)clen_at;
    return at;
}

/* ------------------------------------------------------------- phases ---- */
static const char *fixtures[] = {
    "h264-mp3.mp4", "h264-mp3-nobf.mp4", "frag.mp4", "frag-everyframe.mp4",
    "h265.mp4", "aac.m4a", "pcm.mov", "h264-mp3.mkv", "h264-flac.mkv",
    "vp9-opus.webm", "mp3.mka", "laced-xiph.mkv", "laced-fixed.mkv",
    "laced-ebml.mkv", "laced-none.mkv"
};

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 8;
    rng_state = argc > 2 ? strtoull(argv[2], 0, 0) : 0x243F6A8885A308D3ull;
    const char *dir = argc > 3 ? argv[3] : "tests/fixtures/media";
    if (scale < 1) scale = 1;

    long iterations = 0, opened = 0, samples = 0;

    /* 1 + 2: corruption and structured corruption of the real fixtures. */
    for (unsigned f = 0; f < sizeof fixtures / sizeof fixtures[0]; f++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, fixtures[f]);
        long n = 0;
        uint8_t *orig = slurp(path, &n);
        if (!orig) continue;

        /* The pristine file must open and walk cleanly -- if it does not, the
         * fuzz results below mean nothing. */
        samples += hammer(orig, n);
        opened++;

        for (int it = 0; it < scale * 8; it++) {
            long m = n;
            if (rnd_below(4) == 0) m = 1 + (long)rnd_below((uint32_t)n);   /* truncate */
            uint8_t *copy = malloc((size_t)m);
            memcpy(copy, orig, (size_t)m);
            if (it & 1) mutate_structured(copy, m); else mutate(copy, m);
            hammer(copy, m);
            free(copy);
            iterations++;
        }
        free(orig);
    }

    /* 3: synthetic files built to be hostile rather than damaged. */
    for (int it = 0; it < scale * 16; it++) {
        uint8_t *b = malloc(1024);
        long n = (it & 1) ? synth_mp4(b, 1024, rnd()) : synth_mkv(b, 1024, rnd());
        if (n > 0) {
            if (rnd_below(2)) mutate(b, n);
            hammer(b, n);
            iterations++;
        }
        free(b);
    }

    /* 4: noise, and noise wearing a magic number. */
    for (int it = 0; it < scale * 16; it++) {
        long n = 16 + (long)rnd_below(4096);
        uint8_t *b = malloc((size_t)n);
        for (long i = 0; i < n; i++) b[i] = (uint8_t)rnd();
        if (it % 3 == 1) { b[0]=0; b[1]=0; b[2]=0; b[3]=0x18; memcpy(b+4, "ftyp", 4); }
        if (it % 3 == 2) { b[0]=0x1A; b[1]=0x45; b[2]=0xDF; b[3]=0xA3; }
        hammer(b, n);
        free(b);
        iterations++;
    }

    /* 5: determinism. The same bytes must demux to the same thing twice --
     * a parser that reads uninitialised memory usually does not. */
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

    printf("demux fuzz: %ld iterations over %ld fixtures, %ld samples walked, "
           "%d checks, %d failures\n", iterations, opened, samples, checks, failures);
    if (!opened) {
        printf("demux fuzz: NO FIXTURES FOUND in %s -- this run proved nothing\n", dir);
        return 1;
    }
    return failures ? 1 : 0;
}
