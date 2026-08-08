/* tests/unit/demux_test.c -- the host driver for the container demuxers.
 *
 * This program does not decide anything. It prints what c/lib/media saw, in a
 * form tests/unit/demux_diff.py can put next to ffprobe's, and it extracts the
 * elementary streams so they can be compared with ffmpeg's own remux BYTE FOR
 * BYTE. The bar next door is bit-exactness against ffmpeg; a demuxer's
 * equivalent of that is: the same sample boundaries, the same timestamps, the
 * same codec-configuration bytes, and the same elementary stream out.
 *
 *   demux_test packets <file>            one line per sample, per track
 *   demux_test info <file>               tracks, geometry, extradata CRC32
 *   demux_test annexb <file> <t> <out>   parameter sets + samples, Annex B
 *   demux_test raw <file> <t> <out>      sample payloads, concatenated
 *   demux_test order <file>              media_read's interleave
 *   demux_test units                     the parser's own unit assertions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"
#include "media_int.h"

static unsigned crc_table[256];
static void crc_init(void)
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}
static unsigned crc32_of(const unsigned char *p, long n)
{
    unsigned c = 0xFFFFFFFFu;
    while (n-- > 0) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static unsigned char *read_all(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *b = malloc((size_t)n);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out_len = n;
    return b;
}

/* ------------------------------------------------------------- checks ---- */
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); fails++; } } while (0)

/* The reader is the whole safety argument, so it gets tested against
 * hand-written cases rather than only through a file. */
static void test_reader(void)
{
    static const uint8_t d[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    br b;
    br_init(&b, d, 8, 100);
    CHECK(br_u32(&b) == 0x01020304u, "u32");
    CHECK(br_tell(&b) == 104, "tell %ld", br_tell(&b));
    CHECK(br_left(&b) == 4, "left");
    br child = br_sub(&b, 4);
    CHECK(br_ok(&child) && child.len == 4 && child.org == 104, "sub window");
    CHECK(br_u16(&child) == 0x0506u, "child u16");
    /* Past the end: fails, stays failed, and returns zeros rather than data. */
    br_init(&b, d, 8, 0);
    br_skip(&b, 6);
    CHECK(br_u32(&b) == 0 && !br_ok(&b), "overrun detected");
    CHECK(br_u8(&b) == 0 && !br_ok(&b), "failure is sticky");
    /* A child larger than its parent gets a BAD reader, not the parent's tail. */
    br_init(&b, d, 8, 0);
    br c2 = br_sub(&b, 9);
    CHECK(!br_ok(&c2), "oversized child rejected");
    CHECK(br_left(&c2) == 0, "bad child has no bytes");
    /* Negative and absurd lengths. */
    br_init(&b, d, 8, 0);
    br_skip(&b, -1);
    CHECK(!br_ok(&b), "negative skip rejected");
    br_init(&b, d, 8, 0);
    br_seek(&b, 9);
    CHECK(!br_ok(&b), "seek past end rejected");
}

static void test_ticks(void)
{
    CHECK(md_ticks_to_ns(0, 1000) == 0, "zero");
    CHECK(md_ticks_to_ns(1, 1000) == 1000000LL, "1 ms");
    CHECK(md_ticks_to_ns(90000, 90000) == 1000000000LL, "one second at 90k");
    CHECK(md_ticks_to_ns(1, 90000) == 11111LL, "sub-tick truncates, not rounds");
    /* Three hours at 90 kHz is 972,000,000 ticks; times 1e9 overflows int64 if
     * the multiply is done first. This is the case the split exists for. */
    long long big = 972000000LL;
    CHECK(md_ticks_to_ns(big, 90000) == 10800000000000LL, "no overflow: %lld",
          md_ticks_to_ns(big, 90000));
    CHECK(md_ticks_to_ns(-1000, 1000) == -1000000000LL, "negative dts");
    CHECK(md_ticks_to_ns(5, 0) == 0, "zero timescale is not a divide by zero");
}

/* A hand-built minimal MP4: enough boxes for one video sample, so the sample
 * table walk is tested against something whose right answer is known by
 * construction rather than by ffmpeg agreeing with us. */
static void put32(unsigned char *p, unsigned v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void test_sniff(void)
{
    unsigned char mp4[16] = { 0,0,0,0x10, 'f','t','y','p', 'i','s','o','m', 0,0,0,0 };
    unsigned char mkv[8]  = { 0x1A,0x45,0xDF,0xA3, 0x01,0x02,0x03,0x04 };
    unsigned char junk[16];
    memset(junk, 0x5A, sizeof junk);
    CHECK(media_sniff(mp4, 16) == MEDIA_CONT_MP4, "ftyp");
    CHECK(media_sniff(mkv, 8) == MEDIA_CONT_MKV, "ebml");
    CHECK(media_sniff(junk, 16) == MEDIA_CONT_UNKNOWN, "junk");
    CHECK(media_sniff(mp4, 4) == MEDIA_CONT_UNKNOWN, "too short");
    CHECK(media_sniff(0, 100) == MEDIA_CONT_UNKNOWN, "null");
    /* An mdat-first file (the shape a writer that streams then patches makes)
     * is still an MP4 once the walk reaches ftyp. */
    unsigned char lead[32];
    memset(lead, 0, sizeof lead);
    put32(lead, 16); memcpy(lead + 4, "free", 4);
    put32(lead + 16, 16); memcpy(lead + 20, "ftyp", 4);
    CHECK(media_sniff(lead, 32) == MEDIA_CONT_MP4, "ftyp after free");
    /* ...but not if the leading box's size runs off the end. */
    put32(lead, 0x7FFFFFFF);
    CHECK(media_sniff(lead, 32) == MEDIA_CONT_UNKNOWN, "absurd leading size");
    /* Neither container opens from nothing. */
    int e = 0;
    CHECK(media_open(0, 0, &e) == 0, "null open");
    CHECK(media_open(junk, 16, &e) == 0 && e == MEDIA_ERR_UNSUPPORTED, "junk open");
}

/* Annex B rewriting, against a length-prefixed sample built by hand. */
static void test_annexb_lengths(void)
{
    /* This exercises media_to_annexb's length walk through a real demuxer
     * object, which is the only way in: the function takes an mdemux. Build
     * the smallest one that can exist. */
    mdemux m;
    memset(&m, 0, sizeof m);
    static const uint8_t payload[] = {
        0, 0, 0, 3, 0x65, 0xAA, 0xBB,        /* 3-byte NAL */
        0, 0, 0, 2, 0x41, 0xCC               /* 2-byte NAL */
    };
    m.data = payload; m.len = sizeof payload; m.ntracks = 1; m.selected = -1;
    m.tr[0].t.codec = MEDIA_CODEC_H264;
    m.tr[0].t.framing = MEDIA_FRAMING_AVCC;
    m.tr[0].t.nal_length_size = 4;

    media_sample s;
    memset(&s, 0, sizeof s);
    s.track = 0; s.data = payload; s.size = sizeof payload; s.file_off = 0;

    long need = media_to_annexb(&m, &s, 0, 0);
    CHECK(need == 4 + 3 + 4 + 2, "annexb size %ld", need);
    unsigned char out[32];
    long n = media_to_annexb(&m, &s, out, sizeof out);
    CHECK(n == need, "annexb write %ld", n);
    static const unsigned char want[] = { 0,0,0,1, 0x65,0xAA,0xBB, 0,0,0,1, 0x41,0xCC };
    CHECK(n == (long)sizeof want && !memcmp(out, want, sizeof want), "annexb bytes");
    /* A buffer one byte too small must refuse, not truncate. */
    CHECK(media_to_annexb(&m, &s, out, n - 1) < 0, "short buffer refused");

    /* A NAL whose length overruns the sample is corrupt, not an invitation to
     * read the next sample's bytes. This is the fuzzer's first find. */
    static const uint8_t bad[] = { 0, 0, 0, 100, 0x65, 0xAA };
    s.data = bad; s.size = sizeof bad;
    m.data = bad; m.len = sizeof bad;
    CHECK(media_to_annexb(&m, &s, 0, 0) == MEDIA_ERR_CORRUPT, "overrunning NAL length");

    /* An avcC saying "1-byte lengths" must be honoured, not assumed to be 4. */
    static const uint8_t one[] = { 2, 0x65, 0xAA, 1, 0x41 };
    s.data = one; s.size = sizeof one;
    m.data = one; m.len = sizeof one;
    m.tr[0].t.nal_length_size = 1;
    CHECK(media_to_annexb(&m, &s, 0, 0) == 4 + 2 + 4 + 1, "1-byte lengths");
    m.tr[0].t.nal_length_size = 3;
    CHECK(media_to_annexb(&m, &s, 0, 0) == MEDIA_ERR_CORRUPT, "3 is not a legal length size");
}

/* avcC and hvcC parameter-set extraction. */
static void test_headers(void)
{
    mdemux m;
    memset(&m, 0, sizeof m);
    m.ntracks = 1; m.selected = -1;
    static const uint8_t avcc[] = {
        1, 0x42, 0x00, 0x1E, 0xFF,          /* version..lengthSizeMinusOne=3 */
        0xE1, 0x00, 0x03, 0x67, 0x42, 0x1E, /* 1 SPS, 3 bytes */
        0x01, 0x00, 0x02, 0x68, 0xCE        /* 1 PPS, 2 bytes */
    };
    m.tr[0].t.codec = MEDIA_CODEC_H264;
    m.tr[0].t.framing = MEDIA_FRAMING_AVCC;
    m.tr[0].t.extradata = avcc;
    m.tr[0].t.extradata_len = sizeof avcc;
    unsigned char out[64];
    long n = media_annexb_headers(&m, 0, out, sizeof out);
    static const unsigned char want[] = { 0,0,0,1, 0x67,0x42,0x1E, 0,0,0,1, 0x68,0xCE };
    CHECK(n == (long)sizeof want && !memcmp(out, want, sizeof want), "avcC headers n=%ld", n);

    /* Truncated: the SPS says 3 bytes and only 2 are there. */
    m.tr[0].t.extradata_len = 10;
    CHECK(media_annexb_headers(&m, 0, out, sizeof out) == MEDIA_ERR_CORRUPT,
          "truncated avcC");
    /* An empty extradata is not AVCC framing at all -- avc3 puts the parameter
     * sets in-band, and md_finish_track has to notice. */
    mtrack t;
    memset(&t, 0, sizeof t);
    t.t.codec = MEDIA_CODEC_H264;
    md_finish_track(&t);
    CHECK(t.t.framing == MEDIA_FRAMING_RAW, "no extradata -> raw framing");
}

static int run_units(void)
{
    fails = 0;
    test_reader();
    test_ticks();
    test_sniff();
    test_annexb_lengths();
    test_headers();
    if (fails) { printf("demux units: %d FAILED\n", fails); return 1; }
    printf("demux units: ok\n");
    return 0;
}

/* --------------------------------------------------------------- main ---- */
int main(int argc, char **argv)
{
    crc_init();
    if (argc >= 2 && !strcmp(argv[1], "units")) return run_units();
    if (argc < 3) {
        printf("usage: demux_test packets|info|order|annexb|raw <file> [track] [out]\n");
        return 2;
    }
    const char *mode = argv[1];
    long len = 0;
    unsigned char *data = read_all(argv[2], &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }

    int err = 0;
    mdemux *m = media_open(data, len, &err);
    if (!m) { fprintf(stderr, "open failed: %s (%d)\n", media_strerror(err), err); return 1; }

    if (!strcmp(mode, "info")) {
        printf("container=%s fragmented=%d duration_ns=%lld tracks=%d\n",
               media_container_name(media_kind(m)), media_is_fragmented(m),
               media_duration_ns(m), media_track_count(m));
        for (int i = 0; i < media_track_count(m); i++) {
            const media_track *t = media_track_info(m, i);
            printf("track=%d type=%s codec=%s w=%d h=%d rate=%d ch=%d "
                   "timescale=%u nsamples=%ld extradata_size=%d extradata_crc=%08x\n",
                   i, t->type == MEDIA_TRACK_VIDEO ? "video"
                    : t->type == MEDIA_TRACK_AUDIO ? "audio" : "other",
                   t->codec_name, t->width, t->height, t->rate, t->channels,
                   t->timescale, t->nsamples, t->extradata_len,
                   crc32_of(t->extradata, t->extradata_len));
        }
    } else if (!strcmp(mode, "packets")) {
        for (int i = 0; i < media_track_count(m); i++) {
            media_sample s;
            for (long k = 0; media_get_sample(m, i, k, &s) == 1; k++)
                printf("P %d %lld %lld %ld %lld %d\n",
                       i, s.pts_ticks, s.dts_ticks, s.size, s.file_off, s.keyframe);
        }
    } else if (!strcmp(mode, "order")) {
        media_sample s;
        while (media_read(m, &s) == 1)
            printf("O %d %lld %lld %ld\n", s.track, s.dts_ns, s.pts_ns, s.size);
    } else if (!strcmp(mode, "extradata")) {
        if (argc < 5) { fprintf(stderr, "need <track> <out>\n"); return 2; }
        const media_track *t = media_track_info(m, atoi(argv[3]));
        if (!t) { fprintf(stderr, "no such track\n"); return 2; }
        FILE *o = fopen(argv[4], "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", argv[4]); return 2; }
        if (t->extradata_len > 0) fwrite(t->extradata, 1, (size_t)t->extradata_len, o);
        fclose(o);
    } else if (!strcmp(mode, "annexb") || !strcmp(mode, "raw")) {
        if (argc < 5) { fprintf(stderr, "need <track> <out>\n"); return 2; }
        int ti = atoi(argv[3]);
        FILE *o = fopen(argv[4], "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", argv[4]); return 2; }
        int annexb = !strcmp(mode, "annexb");
        static unsigned char buf[16 << 20];
        if (annexb) {
            long n = media_annexb_headers(m, ti, buf, (long)sizeof buf);
            if (n < 0) { fprintf(stderr, "headers: %ld\n", n); return 1; }
            fwrite(buf, 1, (size_t)n, o);
        }
        media_sample s;
        for (long k = 0; media_get_sample(m, ti, k, &s) == 1; k++) {
            if (annexb) {
                long n = media_to_annexb(m, &s, buf, (long)sizeof buf);
                if (n < 0) { fprintf(stderr, "sample %ld: %ld\n", k, n); return 1; }
                fwrite(buf, 1, (size_t)n, o);
            } else {
                fwrite(s.data, 1, (size_t)s.size, o);
            }
        }
        fclose(o);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    media_close(m);
    free(data);
    return 0;
}
