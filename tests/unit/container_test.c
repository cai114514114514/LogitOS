/* tests/unit/container_test.c -- the host driver for the container demuxers
 * this phase adds (AVI, MPEG-TS, MPEG-PS, FLV). Same shape as
 * tests/unit/demux_test.c so tests/unit/demux_diff.py can be pointed at
 * this binary UNCHANGED -- both print identical "packets"/"info"/"order"/
 * "annexb"/"raw" output, the only difference being how the file gets opened
 * (media_open() only knows MP4/Matroska; demux.c belongs to another
 * workflow, so this file does its own by-content dispatch across the four
 * new formats using their own sniff/open entry points).
 *
 *   container_test packets <file>            one line per sample, per track
 *   container_test info <file>               tracks, geometry, extradata CRC32
 *   container_test annexb <file> <t> <out>    parameter sets + samples, Annex B
 *   container_test raw <file> <t> <out>       sample payloads, concatenated
 *   container_test order <file>               media_read's interleave
 *   container_test units                      hand-built fixtures, known-by-
 *                                              construction right answers
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

/* --------------------------------------------------------------- crc32 --- */
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

/* ------------------------------------------------------ open dispatch ---- */
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
    /* ts_open()/ps_open() replace m->data with an internally-reassembled
     * buffer (see ts.h/ps.h) -- media_close() must not be used on those. */
    if (k == K_TS) ts_close(m);
    else if (k == K_PS) ps_close(m);
    else media_close(m);
}

static const char *kind_name(enum kind k)
{
    switch (k) { case K_AVI: return "avi"; case K_TS: return "mpegts";
                 case K_PS: return "mpeg"; case K_FLV: return "flv"; default: return "unknown"; }
}

/* ------------------------------------------------------------- checks ---- */
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); fails++; } } while (0)

/* --------------------------------------------------------- bytebuffer ---- */
typedef struct { uint8_t *d; long n, cap; } bb;
static void bb_reserve(bb *b, long extra)
{
    if (b->n + extra <= b->cap) return;
    long nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->n + extra) nc *= 2;
    b->d = realloc(b->d, (size_t)nc); b->cap = nc;
}
static void bb_bytes(bb *b, const void *p, long n) { bb_reserve(b, n); memcpy(b->d + b->n, p, (size_t)n); b->n += n; }
static void bb_u8(bb *b, unsigned v) { uint8_t c = (uint8_t)v; bb_bytes(b, &c, 1); }
static void bb_u16le(bb *b, unsigned v) { uint8_t c[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; bb_bytes(b, c, 2); }
static void bb_u32le(bb *b, unsigned v) { uint8_t c[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; bb_bytes(b, c, 4); }
static void bb_u16be(bb *b, unsigned v) { uint8_t c[2] = { (uint8_t)(v >> 8), (uint8_t)v }; bb_bytes(b, c, 2); }
static void bb_u24be(bb *b, unsigned v) { uint8_t c[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bb_bytes(b, c, 3); }
static void bb_u32be(bb *b, unsigned v) { uint8_t c[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bb_bytes(b, c, 4); }
static void bb_str(bb *b, const char *s) { bb_bytes(b, s, (long)strlen(s)); }
static void bb_free(bb *b) { free(b->d); b->d = 0; b->n = b->cap = 0; }
/* fourcc + u32le(size) + payload, padded to even -- one RIFF chunk. */
static void bb_riff_chunk(bb *out, const char *fourcc, const bb *payload)
{
    bb_str(out, fourcc); bb_u32le(out, (unsigned)payload->n); bb_bytes(out, payload->d, payload->n);
    if (payload->n & 1) bb_u8(out, 0);
}

/* ------------------------------------------------------------- AVI ------- */
/* AVISTREAMHEADER (56 bytes on disk): fcctype, fcchandler, flags, priority+
 * language, initialframes, scale, rate, start, length, suggestedbuffersize,
 * quality, samplesize, rcFrame(8). */
static void bb_strh(bb *b, const char *type, uint32_t scale, uint32_t rate,
                     uint32_t length, uint32_t samplesize)
{
    bb strh; memset(&strh, 0, sizeof strh);
    bb_str(&strh, type);
    bb_u32le(&strh, 0);                 /* fccHandler */
    bb_u32le(&strh, 0);                 /* dwFlags */
    bb_u16le(&strh, 0); bb_u16le(&strh, 0);
    bb_u32le(&strh, 0);                 /* dwInitialFrames */
    bb_u32le(&strh, scale);
    bb_u32le(&strh, rate);
    bb_u32le(&strh, 0);                 /* dwStart */
    bb_u32le(&strh, length);
    bb_u32le(&strh, 0);                 /* dwSuggestedBufferSize */
    bb_u32le(&strh, 0);                 /* dwQuality */
    bb_u32le(&strh, samplesize);
    bb_u32le(&strh, 0); bb_u32le(&strh, 0);   /* rcFrame */
    bb_riff_chunk(b, "strh", &strh);
    bb_free(&strh);
}

static void bb_strf_audio(bb *b, uint16_t tag, uint16_t ch, uint32_t sr, uint16_t bits)
{
    bb strf; memset(&strf, 0, sizeof strf);
    bb_u16le(&strf, tag);
    bb_u16le(&strf, ch);
    bb_u32le(&strf, sr);
    bb_u32le(&strf, sr * ch * (bits / 8));   /* nAvgBytesPerSec */
    bb_u16le(&strf, (uint16_t)(ch * (bits / 8)));  /* nBlockAlign */
    bb_u16le(&strf, bits);
    bb_riff_chunk(b, "strf", &strf);
    bb_free(&strf);
}

/* A minimal but complete AVI: one audio stream, dwSampleSize = `samplesize`
 * (0 = one chunk one sample; nonzero = CBR, byte-based timing -- this is
 * test_pcm_sample_size's whole reason to exist, see avi.c's chunk_time_units).
 * `chunks[i]` are the raw payload bytes of movi chunk i, all on stream 0. */
static bb build_avi_pcm(uint32_t rate, uint32_t samplesize,
                         const uint8_t *const *chunks, const long *sizes, int n)
{
    bb strl; memset(&strl, 0, sizeof strl);
    bb_strh(&strl, "auds", 1, rate, 0, samplesize);
    bb_strf_audio(&strl, 1 /* PCM */, 1, rate, 16);
    bb strl_list; memset(&strl_list, 0, sizeof strl_list);
    bb_str(&strl_list, "strl"); bb_bytes(&strl_list, strl.d, strl.n);
    bb strl_chunk; memset(&strl_chunk, 0, sizeof strl_chunk);
    bb_riff_chunk(&strl_chunk, "LIST", &strl_list);
    bb_free(&strl); bb_free(&strl_list);

    bb avih; memset(&avih, 0, sizeof avih);
    for (int i = 0; i < 10; i++) bb_u32le(&avih, 0);   /* AVIMAINHEADER, fields unread by avi.c */
    bb avihc; memset(&avihc, 0, sizeof avihc);
    bb_riff_chunk(&avihc, "avih", &avih);
    bb_free(&avih);

    bb hdrl_list; memset(&hdrl_list, 0, sizeof hdrl_list);
    bb_str(&hdrl_list, "hdrl");
    bb_bytes(&hdrl_list, avihc.d, avihc.n);
    bb_bytes(&hdrl_list, strl_chunk.d, strl_chunk.n);
    bb_free(&avihc); bb_free(&strl_chunk);
    bb hdrl; memset(&hdrl, 0, sizeof hdrl);
    bb_riff_chunk(&hdrl, "LIST", &hdrl_list);
    bb_free(&hdrl_list);

    bb movi_list; memset(&movi_list, 0, sizeof movi_list);
    bb_str(&movi_list, "movi");
    long *chunk_off = malloc(sizeof(long) * (size_t)n);   /* offset of ckid, relative to "movi" fourcc */
    for (int i = 0; i < n; i++) {
        /* idx1's dwOffset is relative to the "movi" FOURCC itself, and
         * movi_list already opens with those 4 bytes (position 0..3) -- so
         * the position about to be written (movi_list.n) IS that relative
         * offset already; no extra +4 (the first chunk's dwOffset is 4,
         * i.e. right after "movi", which movi_list.n already equals here). */
        chunk_off[i] = movi_list.n;
        bb one; one.d = (uint8_t *)chunks[i]; one.n = sizes[i]; one.cap = sizes[i];
        bb_riff_chunk(&movi_list, "00wb", &one);
    }
    bb movi; memset(&movi, 0, sizeof movi);
    bb_riff_chunk(&movi, "LIST", &movi_list);
    bb_free(&movi_list);

    bb idx1; memset(&idx1, 0, sizeof idx1);
    for (int i = 0; i < n; i++) {
        bb_str(&idx1, "00wb");
        bb_u32le(&idx1, 0x10);              /* AVIIF_KEYFRAME, harmless for audio */
        bb_u32le(&idx1, (unsigned)chunk_off[i]);
        bb_u32le(&idx1, (unsigned)sizes[i]);
    }
    free(chunk_off);
    bb idx1c; memset(&idx1c, 0, sizeof idx1c);
    bb_riff_chunk(&idx1c, "idx1", &idx1);
    bb_free(&idx1);

    bb riff_body; memset(&riff_body, 0, sizeof riff_body);
    bb_str(&riff_body, "AVI ");
    bb_bytes(&riff_body, hdrl.d, hdrl.n);
    bb_bytes(&riff_body, movi.d, movi.n);
    bb_bytes(&riff_body, idx1c.d, idx1c.n);
    bb_free(&hdrl); bb_free(&movi); bb_free(&idx1c);

    bb out; memset(&out, 0, sizeof out);
    bb_riff_chunk(&out, "RIFF", &riff_body);
    bb_free(&riff_body);
    return out;
}

static void test_avi_pcm_sample_size(void)
{
    /* Three chunks of a mono 16-bit stream at 8000 Hz: 8, 4, 0 bytes --
     * 4, 2 and 0 SAMPLES. dwSampleSize=2 (16-bit mono). With the bug (flat
     * dwScale-per-chunk, dwScale=1) every chunk would advance dts by 1 tick
     * regardless of size, giving dts 0,1,2 -- wrong. Correct is 0,4,6. */
    uint8_t c0[8] = {0}, c1[4] = {0};
    const uint8_t *chunks[3] = { c0, c1, (const uint8_t *)"" };
    long sizes[3] = { 8, 4, 0 };
    bb avi = build_avi_pcm(8000, 2, chunks, sizes, 3);

    int err = 0;
    mdemux *m = avi_open(avi.d, avi.n, &err);
    CHECK(m != 0, "avi open: %s", media_strerror(err));
    if (m) {
        CHECK(media_track_count(m) == 1, "one track");
        const media_track *t = media_track_info(m, 0);
        CHECK(t && t->codec == MEDIA_CODEC_PCM_S16LE, "codec pcm16le");
        CHECK(t && t->timescale == 8000, "timescale = dwRate, got %u", t ? t->timescale : 0);
        media_sample s;
        CHECK(media_get_sample(m, 0, 0, &s) == 1 && s.dts_ticks == 0, "chunk0 dts=0 got %lld",
              media_get_sample(m, 0, 0, &s) == 1 ? s.dts_ticks : -1LL);
        CHECK(media_get_sample(m, 0, 1, &s) == 1 && s.dts_ticks == 4, "chunk1 dts=4 got %lld",
              media_get_sample(m, 0, 1, &s) == 1 ? s.dts_ticks : -1LL);
        /* chunk2 is zero bytes: 0/2 = 0 samples, contributes NO sample (avi.c
         * skips md_push for size==0) and, being CBR, no time either. Only two
         * samples exist. */
        CHECK(media_get_sample(m, 0, 2, &s) == 0, "no third sample (zero-byte CBR chunk)");
        media_close(m);
    }
    bb_free(&avi);
}

/* AVISUPERINDEX (indx) with ONE part pointing at one AVISTDINDEX (ix00),
 * itself pointing at two movi chunks -- the OpenDML path, no idx1 at all. */
static void test_avi_opendml_index(void)
{
    uint8_t pay0[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t pay1[2] = { 0x55, 0x66 };

    /* movi first: we need real chunk header offsets (ix00 entries are
     * relative to a base, pointing at the ckid, exactly like idx1). */
    bb movi_list; memset(&movi_list, 0, sizeof movi_list);
    bb_str(&movi_list, "movi");
    long off0 = movi_list.n;                 /* relative to the START OF movi_list's payload,
                                                 i.e. right after "movi" -- matches c.body.org
                                                 basis avi.c uses since movi_ref = LIST payload start */
    { bb one; one.d = pay0; one.n = 4; one.cap = 4; bb_riff_chunk(&movi_list, "00dc", &one); }
    long off1 = movi_list.n;
    { bb one; one.d = pay1; one.n = 2; one.cap = 2; bb_riff_chunk(&movi_list, "00dc", &one); }
    bb movi; memset(&movi, 0, sizeof movi);
    bb_riff_chunk(&movi, "LIST", &movi_list);
    bb_free(&movi_list);

    /* Everything before movi in the final file: RIFF+AVI__+hdrl. We need the
     * absolute file offset of "movi"'s payload (right after the LIST header's
     * 12 bytes: "LIST"+size+"movi") to turn off0/off1 into AVISTDINDEX's
     * ABSOLUTE qwBaseOffset + relative entries. Build hdrl first with a
     * placeholder strl (indx filled in once we know the ix00 chunk's own
     * absolute file position, which depends on hdrl's own length -- so hdrl
     * is built WITHOUT indx, an ix00 chunk is appended after movi, and indx
     * is spliced into strl only in the final assembly below.) */
    bb strh; memset(&strh, 0, sizeof strh);
    bb_strh(&strh, "vids", 1, 15, 0, 0);
    bb strl_list; memset(&strl_list, 0, sizeof strl_list);
    bb_str(&strl_list, "strl"); bb_bytes(&strl_list, strh.d, strh.n);
    bb_free(&strh);
    /* indx (AVISUPERINDEX): longsperentry=4, indextype=1(chunks)|subtype=0,
     * nentriesinuse=1, chunkid, reserved[3], then one entry
     * {qwOffset(lo,hi), dwSize, dwDuration}. qwOffset is filled in below,
     * once the RIFF/AVI_/hdrl/movi prefix length is known -- start at a
     * fixed placeholder and patch the byte offset afterward. */
    long indx_qwoffset_patch_at;
    bb indx; memset(&indx, 0, sizeof indx);
    bb_u16le(&indx, 4);                       /* wLongsPerEntry */
    bb_u8(&indx, 0); bb_u8(&indx, 1);          /* bIndexSubType, bIndexType=AVI_INDEX_OF_CHUNKS */
    bb_u32le(&indx, 1);                        /* nEntriesInUse */
    bb_str(&indx, "00dc");                    /* dwChunkId */
    bb_u32le(&indx, 0); bb_u32le(&indx, 0); bb_u32le(&indx, 0);  /* dwReserved[3] */
    indx_qwoffset_patch_at = indx.n;
    bb_u32le(&indx, 0); bb_u32le(&indx, 0);    /* qwOffset lo,hi -- patched below */
    bb_u32le(&indx, 0);                        /* dwSize of the ix00 chunk -- unread by avi.c */
    bb_u32le(&indx, 2);                        /* dwDuration */
    bb indxc; memset(&indxc, 0, sizeof indxc);
    bb_riff_chunk(&indxc, "indx", &indx);
    bb_free(&indx);
    bb_bytes(&strl_list, indxc.d, indxc.n);
    long indx_hdr_off_in_strl = strl_list.n - indxc.n;  /* where "indx" chunk starts inside strl_list */
    bb_free(&indxc);
    bb strl_chunk; memset(&strl_chunk, 0, sizeof strl_chunk);
    bb_riff_chunk(&strl_chunk, "LIST", &strl_list);
    bb_free(&strl_list);

    bb avih; memset(&avih, 0, sizeof avih);
    for (int i = 0; i < 10; i++) bb_u32le(&avih, 0);
    bb avihc; memset(&avihc, 0, sizeof avihc);
    bb_riff_chunk(&avihc, "avih", &avih);
    bb_free(&avih);
    bb hdrl_list; memset(&hdrl_list, 0, sizeof hdrl_list);
    bb_str(&hdrl_list, "hdrl");
    bb_bytes(&hdrl_list, avihc.d, avihc.n);
    bb_bytes(&hdrl_list, strl_chunk.d, strl_chunk.n);
    bb_free(&avihc); bb_free(&strl_chunk);
    bb hdrl; memset(&hdrl, 0, sizeof hdrl);
    bb_riff_chunk(&hdrl, "LIST", &hdrl_list);
    bb_free(&hdrl_list);

    /* ix00 (AVISTDINDEX): wLongsPerEntry=2, subtype/type, nEntriesInUse=2,
     * dwChunkId, qwBaseOffset(=absolute file offset of "movi"'s payload, i.e.
     * the same basis idx1 uses), dwReserved3, then 2 entries {dwOffset
     * (relative to base, pointing at the ckid), dwSize | KEYFRAME-inverted
     * bit 31 clear = sync sample}. */
    bb ix00; memset(&ix00, 0, sizeof ix00);
    bb_u16le(&ix00, 2);
    bb_u8(&ix00, 0); bb_u8(&ix00, 1);
    bb_u32le(&ix00, 2);
    bb_str(&ix00, "00dc");
    long ix00_base_patch_at = ix00.n;
    bb_u32le(&ix00, 0); bb_u32le(&ix00, 0);   /* qwBaseOffset lo,hi -- patched below */
    bb_u32le(&ix00, 0);                        /* dwReserved3 */
    bb_u32le(&ix00, (unsigned)off0); bb_u32le(&ix00, 4);
    bb_u32le(&ix00, (unsigned)off1); bb_u32le(&ix00, 2);
    bb ix00c; memset(&ix00c, 0, sizeof ix00c);
    bb_riff_chunk(&ix00c, "ix00", &ix00);
    bb_free(&ix00);

    /* Assemble: RIFF/AVI_ header (12) + hdrl + movi + ix00. Everything before
     * "movi"'s LIST payload: 12 (RIFF hdr) + hdrl.n, then LIST+size+"movi" =
     * 12 more bytes before the payload itself. */
    /* movi's own chunk starts right after "RIFF"+size(8) + "AVI "(4) + hdrl(hdrl.n);
     * its "movi" TEXT (== movi_ref, the base idx1/ix00 offsets are relative
     * to) sits 8 bytes further in (past movi's own "LIST"+size header); the
     * whole movi chunk (header + payload) is movi.n bytes. */
    long movi_chunk_abs = 12 + hdrl.n;
    long movi_payload_abs = movi_chunk_abs + 8;
    long ix00_abs = movi_chunk_abs + movi.n;
    (void)indx_hdr_off_in_strl;   /* only the direct byte-search patch below is used */

    /* Simpler and less error-prone than re-deriving nested offsets: patch by
     * SEARCHING hdrl.d for the "indx" fourcc (unique in this hand-built
     * file) and writing qwOffset right after its fixed-position field. */
    long indx_pos = -1;
    for (long i = 0; i + 4 <= hdrl.n; i++)
        if (!memcmp(hdrl.d + i, "indx", 4)) { indx_pos = i; break; }
    CHECK(indx_pos >= 0, "found indx chunk to patch");
    if (indx_pos >= 0) {
        /* indx payload starts at indx_pos+8; qwOffset is at
         * indx_qwoffset_patch_at within that payload. */
        long p = indx_pos + 8 + indx_qwoffset_patch_at;
        hdrl.d[p + 0] = (uint8_t)(ix00_abs);       hdrl.d[p + 1] = (uint8_t)(ix00_abs >> 8);
        hdrl.d[p + 2] = (uint8_t)(ix00_abs >> 16); hdrl.d[p + 3] = (uint8_t)(ix00_abs >> 24);
        hdrl.d[p + 4] = 0; hdrl.d[p + 5] = 0; hdrl.d[p + 6] = 0; hdrl.d[p + 7] = 0;
    }
    /* Patch ix00's qwBaseOffset the same direct way. */
    {
        long p = 8 + ix00_base_patch_at;   /* ix00c = "ix00"+size(8) then payload */
        ix00c.d[p + 0] = (uint8_t)(movi_payload_abs);       ix00c.d[p + 1] = (uint8_t)(movi_payload_abs >> 8);
        ix00c.d[p + 2] = (uint8_t)(movi_payload_abs >> 16); ix00c.d[p + 3] = (uint8_t)(movi_payload_abs >> 24);
        ix00c.d[p + 4] = 0; ix00c.d[p + 5] = 0; ix00c.d[p + 6] = 0; ix00c.d[p + 7] = 0;
    }

    bb riff_body; memset(&riff_body, 0, sizeof riff_body);
    bb_str(&riff_body, "AVI ");
    bb_bytes(&riff_body, hdrl.d, hdrl.n);
    bb_bytes(&riff_body, movi.d, movi.n);
    bb_bytes(&riff_body, ix00c.d, ix00c.n);
    bb_free(&hdrl); bb_free(&movi); bb_free(&ix00c);
    bb out; memset(&out, 0, sizeof out);
    bb_riff_chunk(&out, "RIFF", &riff_body);
    bb_free(&riff_body);

    int err = 0;
    mdemux *m = avi_open(out.d, out.n, &err);
    CHECK(m != 0, "opendml avi open: %s", media_strerror(err));
    if (m) {
        CHECK(media_track_count(m) == 1, "one track");
        media_sample s;
        CHECK(media_get_sample(m, 0, 0, &s) == 1 && s.size == 4 && !memcmp(s.data, pay0, 4),
              "opendml sample0 bytes");
        CHECK(media_get_sample(m, 0, 1, &s) == 1 && s.size == 2 && !memcmp(s.data, pay1, 2),
              "opendml sample1 bytes");
        CHECK(media_get_sample(m, 0, 2, &s) == 0, "exactly two samples");
        media_close(m);
    }
    bb_free(&out);
}

/* -------------------------------------------------------------- TS ------- */
static void ts_pack(bb *out, int pid, int pusi, int cc, int afc,
                     const uint8_t *payload, int plen, int adapt_stuff)
{
    bb_u8(out, 0x47);
    bb_u8(out, (unsigned)((pusi << 6) | ((pid >> 8) & 0x1F)));
    bb_u8(out, (unsigned)(pid & 0xFF));
    bb_u8(out, (unsigned)((afc << 4) | (cc & 0xF)));
    int used = 0;
    if (afc == 2 || afc == 3) {
        bb_u8(out, (unsigned)adapt_stuff);
        if (adapt_stuff > 0) {
            bb_u8(out, 0x00);                 /* flags: no discontinuity, no random access */
            for (int i = 0; i < adapt_stuff - 1; i++) bb_u8(out, 0xFF);
        }
        used = 1 + adapt_stuff;
    }
    if (afc == 1 || afc == 3) {
        int room = 184 - used;
        int take = plen < room ? plen : room;
        bb_bytes(out, payload, take);
        for (int i = take; i < room; i++) bb_u8(out, 0xFF);
    } else {
        for (int i = used; i < 184; i++) bb_u8(out, 0xFF);
    }
}

/* One PAT packet naming PMT PID 0x100 for program 1. */
static void ts_pat_packet(bb *out, int cc)
{
    uint8_t sec[64]; int n = 0;
    sec[n++] = 0x00;                          /* table_id */
    int seclen_at = n; sec[n++] = 0; sec[n++] = 0;   /* section_length, patched below */
    sec[n++] = 0; sec[n++] = 1;               /* transport_stream_id */
    sec[n++] = 0xC1;                          /* version/current_next */
    sec[n++] = 0; sec[n++] = 0;               /* section_number, last_section_number */
    sec[n++] = 0x00; sec[n++] = 0x01;         /* program_number = 1 */
    sec[n++] = 0xE1; sec[n++] = 0x00;         /* PMT PID = 0x100 */
    sec[n++] = 0; sec[n++] = 0; sec[n++] = 0; sec[n++] = 0;   /* CRC32 (unchecked here) */
    int seclen = n - (seclen_at + 2);
    sec[seclen_at] = (uint8_t)(0xB0 | ((seclen >> 8) & 0x0F));
    sec[seclen_at + 1] = (uint8_t)seclen;

    uint8_t payload[184]; int pn = 0;
    payload[pn++] = 0x00;                     /* pointer_field */
    memcpy(payload + pn, sec, (size_t)n); pn += n;
    ts_pack(out, 0x0000, 1, cc, 1, payload, pn, 0);
}

/* One PMT packet on PID 0x100 naming one elementary stream. */
static void ts_pmt_packet(bb *out, int cc, unsigned stream_type, unsigned epid)
{
    uint8_t sec[64]; int n = 0;
    sec[n++] = 0x02;
    int seclen_at = n; sec[n++] = 0; sec[n++] = 0;
    sec[n++] = 0x00; sec[n++] = 0x01;         /* program_number */
    sec[n++] = 0xC1;
    sec[n++] = 0; sec[n++] = 0;
    sec[n++] = 0xE1; sec[n++] = 0xFF;         /* PCR_PID (unused here) */
    sec[n++] = 0xF0; sec[n++] = 0x00;         /* program_info_length = 0 */
    sec[n++] = (uint8_t)stream_type;
    sec[n++] = (uint8_t)(0xE0 | ((epid >> 8) & 0x1F));
    sec[n++] = (uint8_t)epid;
    sec[n++] = 0xF0; sec[n++] = 0x00;         /* ES_info_length = 0 */
    sec[n++] = 0; sec[n++] = 0; sec[n++] = 0; sec[n++] = 0;
    int seclen = n - (seclen_at + 2);
    sec[seclen_at] = (uint8_t)(0xB0 | ((seclen >> 8) & 0x0F));
    sec[seclen_at + 1] = (uint8_t)seclen;

    uint8_t payload[184]; int pn = 0;
    payload[pn++] = 0x00;
    memcpy(payload + pn, sec, (size_t)n); pn += n;
    ts_pack(out, 0x0100, 1, cc, 1, payload, pn, 0);
}

/* One PES-starting packet: stream_id, PTS-only, then `n` bytes of ES data
 * (must fit in one packet's remaining room for this hand-built fixture). */
static void ts_pes_packet(bb *out, int pid, int cc, unsigned stream_id,
                           long long pts, const uint8_t *es, int n)
{
    uint8_t pes[32]; int k = 0;
    pes[k++] = 0; pes[k++] = 0; pes[k++] = 1; pes[k++] = (uint8_t)stream_id;
    pes[k++] = 0; pes[k++] = 0;               /* PES_packet_length = 0 (unbounded) */
    pes[k++] = 0x80;                          /* '10' + flags */
    pes[k++] = 0x80;                          /* PTS_DTS_flags = '10' (PTS only) */
    pes[k++] = 5;                             /* PES_header_data_length */
    uint64_t p = (uint64_t)pts;
    pes[k++] = (uint8_t)(0x21 | ((p >> 29) & 0x0E));
    pes[k++] = (uint8_t)(p >> 22);
    pes[k++] = (uint8_t)(0x01 | ((p >> 14) & 0xFE));
    pes[k++] = (uint8_t)(p >> 7);
    pes[k++] = (uint8_t)(0x01 | ((p << 1) & 0xFE));
    uint8_t payload[184]; int pn = 0;
    memcpy(payload + pn, pes, (size_t)k); pn += k;
    memcpy(payload + pn, es, (size_t)n); pn += n;
    /* A PES payload's real end is signalled ONLY by the next PUSI -- ts.c
     * does not trust PES_packet_length (see its header comment), so 0xFF is
     * just another payload byte to it. Padding the payload area itself
     * (afc=1) would hand ts.c bytes it has no way to tell apart from real ES
     * data; a real muxer consumes the leftover room with a genuine
     * adaptation field (afc=3) instead, exactly as this does when the PES
     * does not fill the whole 184-byte packet. */
    if (pn < 184) {
        int adapt_stuff = 183 - pn;   /* 184 - 1(length byte) - pn: room lands on pn exactly */
        ts_pack(out, pid, 1, cc, 3, payload, pn, adapt_stuff);
    } else {
        ts_pack(out, pid, 1, cc, 1, payload, pn, 0);
    }
}

static void test_ts_basic_and_continuity(void)
{
    uint8_t es0[16], es1[16];
    for (int i = 0; i < 16; i++) { es0[i] = (uint8_t)i; es1[i] = (uint8_t)(0x40 + i); }

    bb out; memset(&out, 0, sizeof out);
    ts_pat_packet(&out, 0);
    ts_pmt_packet(&out, 0, 0x0F /* AAC */, 0x101);
    ts_pes_packet(&out, 0x101, 0, 0xC0, 90000, es0, 16);
    ts_pes_packet(&out, 0x101, 1, 0xC0, 93600, es1, 16);   /* cc 0->1: fine */

    int err = 0;
    CHECK(ts_sniff(out.d, out.n), "ts_sniff true on a well-formed stream");
    mdemux *m = ts_open(out.d, out.n, &err);
    CHECK(m != 0, "ts open: %s", media_strerror(err));
    if (m) {
        CHECK(media_track_count(m) == 1, "one track, got %d", media_track_count(m));
        const media_track *t = media_track_info(m, 0);
        CHECK(t && t->codec == MEDIA_CODEC_AAC, "codec aac");
        CHECK(t && t->timescale == 90000, "timescale 90k");
        media_sample s;
        CHECK(media_get_sample(m, 0, 0, &s) == 1 && s.size == 16 && !memcmp(s.data, es0, 16),
              "sample0 bytes");
        CHECK(media_get_sample(m, 0, 0, &s) == 1 && s.pts_ticks == 90000, "sample0 pts=90000 got %lld", s.pts_ticks);
        CHECK(media_get_sample(m, 0, 1, &s) == 1 && s.pts_ticks == 93600, "sample1 pts=93600 got %lld", s.pts_ticks);
        ts_close(m);
    }
    bb_free(&out);

    /* THE NEGATIVE CONTROL: drop the middle packet's continuity by writing
     * cc=1 then cc=3 (a gap -- packet 2 is missing), which must be reported,
     * not silently accepted. */
    bb bad; memset(&bad, 0, sizeof bad);
    ts_pat_packet(&bad, 0);
    ts_pmt_packet(&bad, 0, 0x0F, 0x101);
    ts_pes_packet(&bad, 0x101, 0, 0xC0, 90000, es0, 16);
    ts_pes_packet(&bad, 0x101, 3 /* should be 1 */, 0xC0, 93600, es1, 16);
    int err2 = 0;
    mdemux *m2 = ts_open(bad.d, bad.n, &err2);
    CHECK(m2 == 0 && err2 == MEDIA_ERR_CORRUPT, "continuity gap is reported, not silently skipped (err=%d)", err2);
    if (m2) ts_close(m2);
    bb_free(&bad);
}

/* -------------------------------------------------------------- PS ------- */
/* pack_start_code + b0 (top bits '01' selects the MPEG-2 shape) + 9 more
 * bytes (SCR/mux_rate, not decoded by ps.c at all -- see media_int.h's `br`
 * comment on trusting only what a field is FOR) whose last byte's low 3
 * bits are pack_stuffing_length; 0 here, so 10 bytes total after the start
 * code, matching ps.c's `br_bytes(&top, 9)` after reading b0 itself. */
static void ps_pack_header_mpeg2(bb *out)
{
    bb_u8(out, 0); bb_u8(out, 0); bb_u8(out, 1); bb_u8(out, 0xBA);
    bb_u8(out, 0x44);
    /* 9 more bytes after b0 -- ISO/IEC 13818-1 Table 2-33's MPEG-2
     * pack_header is 80 bits (10 bytes) after the start code, and ps.c's
     * `br_bytes(&top, 9)` is exactly that spec, not an arbitrary count. */
    bb_u8(out, 0x00); bb_u8(out, 0x04); bb_u8(out, 0x00);
    bb_u8(out, 0x04); bb_u8(out, 0x01); bb_u8(out, 0x01); bb_u8(out, 0x89);
    bb_u8(out, 0x01);
    bb_u8(out, 0xF8);   /* marker|marker|reserved(5)|pack_stuffing_length=0 */
}

static void ps_pes(bb *out, unsigned stream_id, long long pts, int has_pts,
                    const uint8_t *es, int n)
{
    bb_u8(out, 0); bb_u8(out, 0); bb_u8(out, 1); bb_u8(out, (unsigned)stream_id);
    int optlen = has_pts ? 5 : 0;
    unsigned pktlen = (unsigned)(3 + optlen + n);
    bb_u16be(out, pktlen);
    bb_u8(out, 0x80);
    bb_u8(out, (unsigned)(has_pts ? 0x80 : 0x00));
    bb_u8(out, (unsigned)optlen);
    if (has_pts) {
        uint64_t p = (uint64_t)pts;
        bb_u8(out, (unsigned)(0x21 | ((p >> 29) & 0x0E)));
        bb_u8(out, (unsigned)(p >> 22));
        bb_u8(out, (unsigned)(0x01 | ((p >> 14) & 0xFE)));
        bb_u8(out, (unsigned)(p >> 7));
        bb_u8(out, (unsigned)(0x01 | ((p << 1) & 0xFE)));
    }
    bb_bytes(out, es, n);
}

static void test_ps_basic_and_truncation(void)
{
    static const uint8_t h264_idr_au[] = { 0,0,0,1, 0x67,1,2,3, 0,0,0,1, 0x65,4,5,6,7,8 };  /* SPS + IDR slice */
    static const uint8_t aac_frame[16] = { 0xFF,0xF1,0x50,0x80,0,0x1F,0xFC, 1,2,3,4,5,6,7,8,9 };

    bb out; memset(&out, 0, sizeof out);
    ps_pack_header_mpeg2(&out);
    ps_pes(&out, 0xE0, 45000, 1, h264_idr_au, (int)sizeof h264_idr_au);
    ps_pes(&out, 0xC0, 45000, 1, aac_frame, (int)sizeof aac_frame);
    bb_u8(&out, 0); bb_u8(&out, 0); bb_u8(&out, 1); bb_u8(&out, 0xB9);   /* program end */

    CHECK(ps_sniff(out.d, out.n), "ps_sniff true");
    int err = 0;
    mdemux *m = ps_open(out.d, out.n, &err);
    CHECK(m != 0, "ps open: %s", media_strerror(err));
    if (m) {
        CHECK(media_track_count(m) == 2, "two tracks, got %d", media_track_count(m));
        int vi = media_find_track(m, MEDIA_TRACK_VIDEO);
        int ai = media_find_track(m, MEDIA_TRACK_AUDIO);
        CHECK(vi >= 0 && ai >= 0, "found video and audio tracks");
        if (vi >= 0) {
            media_sample s;
            CHECK(media_get_sample(m, vi, 0, &s) == 1 && s.size == (long)sizeof h264_idr_au,
                  "video sample size");
            CHECK(media_get_sample(m, vi, 0, &s) == 1 && s.keyframe == 1, "IDR scan finds the keyframe");
        }
        ps_close(m);
    }
    bb_free(&out);

    /* NEGATIVE CONTROL: a truncated pack -- the video PES declares a length
     * longer than the bytes actually present. Must be reported, not padded
     * or silently truncated. */
    bb bad; memset(&bad, 0, sizeof bad);
    ps_pack_header_mpeg2(&bad);
    bb_u8(&bad, 0); bb_u8(&bad, 0); bb_u8(&bad, 1); bb_u8(&bad, 0xE0);
    bb_u16be(&bad, 5000);              /* claims 5000 bytes; file has far fewer */
    bb_u8(&bad, 0x80); bb_u8(&bad, 0x00); bb_u8(&bad, 0);
    bb_bytes(&bad, h264_idr_au, (long)sizeof h264_idr_au);
    int err2 = 0;
    mdemux *m2 = ps_open(bad.d, bad.n, &err2);
    CHECK(m2 == 0 && err2 == MEDIA_ERR_CORRUPT, "truncated pack is reported (err=%d)", err2);
    if (m2) ps_close(m2);
    bb_free(&bad);
}

/* -------------------------------------------------------------- FLV ------ */
static void flv_tag(bb *out, unsigned tagtype, long long ts_ms, const uint8_t *data, int n)
{
    bb_u8(out, tagtype);
    bb_u24be(out, (unsigned)n);
    bb_u24be(out, (unsigned)(ts_ms & 0xFFFFFF));
    bb_u8(out, (unsigned)((ts_ms >> 24) & 0xFF));
    bb_u24be(out, 0);
    bb_bytes(out, data, n);
    bb_u32be(out, (unsigned)(11 + n));
}

static void test_flv_basic_and_tagsize(void)
{
    static const uint8_t avcc[] = { 1, 0x42,0x00,0x1E, 0xFF,
        0xE1, 0x00,0x03, 0x67,0x42,0x1E, 0x01, 0x00,0x02, 0x68,0xCE };
    static const uint8_t nalu[] = { 0,0,0,3, 0x65,0xAA,0xBB };
    static const uint8_t asc[] = { 0x12, 0x10 };            /* AAC-LC 44100 stereo */
    static const uint8_t aacframe[] = { 1,2,3,4,5,6 };

    bb out; memset(&out, 0, sizeof out);
    bb_str(&out, "FLV"); bb_u8(&out, 1); bb_u8(&out, 0x05); bb_u32be(&out, 9);
    bb_u32be(&out, 0);                                       /* PreviousTagSize0 */
    { uint8_t v[1 + (long)sizeof avcc]; v[0] = 0x17;          /* key|AVC */
      /* FrameType|CodecID(1) + AVCPacketType(1) + CompositionTime(3, not 2 --
       * it undersized this by a byte and glued avcc[0] into the timestamp
       * field, which is what made the framing/extradata-size checks below
       * fail), then avcC. */
      uint8_t body[5 + sizeof avcc]; body[0]=0x17; body[1]=0; body[2]=0; body[3]=0; body[4]=0;
      memcpy(body+5, avcc, sizeof avcc);
      flv_tag(&out, 9, 0, body, (int)sizeof body); (void)v; }
    { uint8_t body[2 + sizeof asc]; body[0] = 0xAF; body[1] = 0; memcpy(body+2, asc, sizeof asc);
      flv_tag(&out, 8, 0, body, (int)sizeof body); }
    { uint8_t body[5 + sizeof nalu]; body[0]=0x17; body[1]=1; body[2]=0;body[3]=0;body[4]=0;
      memcpy(body+5, nalu, sizeof nalu);
      flv_tag(&out, 9, 33, body, (int)sizeof body); }
    { uint8_t body[2 + sizeof aacframe]; body[0]=0xAF; body[1]=1; memcpy(body+2, aacframe, sizeof aacframe);
      flv_tag(&out, 8, 23, body, (int)sizeof body); }

    CHECK(flv_sniff(out.d, out.n), "flv_sniff true");
    int err = 0;
    mdemux *m = flv_open(out.d, out.n, &err);
    CHECK(m != 0, "flv open: %s", media_strerror(err));
    if (m) {
        CHECK(media_track_count(m) == 2, "two tracks got %d", media_track_count(m));
        int vi = media_find_track(m, MEDIA_TRACK_VIDEO);
        int ai = media_find_track(m, MEDIA_TRACK_AUDIO);
        if (vi >= 0) {
            const media_track *t = media_track_info(m, vi);
            CHECK(t->codec == MEDIA_CODEC_H264, "video codec h264");
            CHECK(t->framing == MEDIA_FRAMING_AVCC, "avcC framing auto-detected");
            CHECK(t->extradata_len == (int)sizeof avcc, "avcC extradata size");
            media_sample s;
            CHECK(media_get_sample(m, vi, 0, &s) == 1 && s.size == (long)sizeof nalu, "one video sample");
            CHECK(media_get_sample(m, vi, 0, &s) == 1 && s.keyframe == 1, "keyframe flag from FrameType");
            CHECK(media_get_sample(m, vi, 0, &s) == 1 && s.dts_ticks == 33, "video ts ms");
        }
        if (ai >= 0) {
            const media_track *t = media_track_info(m, ai);
            CHECK(t->codec == MEDIA_CODEC_AAC, "audio codec aac");
            CHECK(t->rate == 44100 && t->channels == 2, "asc decode: rate=%d ch=%d", t->rate, t->channels);
        }
        media_close(m);
    }
    bb_free(&out);

    /* NEGATIVE CONTROL: wrong PreviousTagSize before the second tag. */
    bb bad; memset(&bad, 0, sizeof bad);
    bb_str(&bad, "FLV"); bb_u8(&bad, 1); bb_u8(&bad, 0x01); bb_u32be(&bad, 9);
    bb_u32be(&bad, 0);
    { uint8_t body[2] = { 0x27, 1 }; flv_tag(&bad, 9, 0, body, 2); }
    /* Corrupt the trailing PreviousTagSize we just wrote (last 4 bytes). */
    bad.d[bad.n - 1] ^= 0xFF;
    { uint8_t body[2] = { 0x27, 1 }; flv_tag(&bad, 9, 1, body, 2); }
    int err2 = 0;
    mdemux *m2 = flv_open(bad.d, bad.n, &err2);
    CHECK(m2 == 0 && err2 == MEDIA_ERR_CORRUPT, "wrong PreviousTagSize is reported (err=%d)", err2);
    if (m2) media_close(m2);
    bb_free(&bad);
}

/* --------------------------------------------------------------- sniff --- */
static void test_sniffs_disjoint(void)
{
    /* Each format's magic must not falsely trigger another's sniff, and a
     * short/junk buffer must trigger none. */
    uint8_t junk[16]; memset(junk, 0x5A, sizeof junk);
    CHECK(!avi_sniff(junk, 16) && !ts_sniff(junk, 16) && !ps_sniff(junk, 16) && !flv_sniff(junk, 16),
          "junk sniffs as nothing");
    uint8_t riff[12] = { 'R','I','F','F', 0,0,0,0, 'A','V','I',' ' };
    CHECK(avi_sniff(riff, 12) && !ts_sniff(riff,12) && !ps_sniff(riff,12) && !flv_sniff(riff,12), "avi only");
    uint8_t ps4[4] = { 0,0,1,0xBA };
    CHECK(ps_sniff(ps4, 4) && !avi_sniff(ps4,4) && !ts_sniff(ps4,4) && !flv_sniff(ps4,4), "ps only");
    uint8_t flv9[9] = { 'F','L','V',1,5, 0,0,0,9 };
    CHECK(flv_sniff(flv9, 9) && !avi_sniff(flv9,9) && !ts_sniff(flv9,9) && !ps_sniff(flv9,9), "flv only");
    /* One 0x47 alone is not a TS -- the header's own explicit requirement. */
    uint8_t one47[8] = { 0x47,0,0,0,0,0,0,0 };
    CHECK(!ts_sniff(one47, 8), "a single 0x47 is not sniffed as TS");
}

static int run_units(void)
{
    fails = 0;
    crc_init();
    test_sniffs_disjoint();
    test_avi_pcm_sample_size();
    test_avi_opendml_index();
    test_ts_basic_and_continuity();
    test_ps_basic_and_truncation();
    test_flv_basic_and_tagsize();
    if (fails) { printf("container units: %d FAILED\n", fails); return 1; }
    printf("container units: ok\n");
    return 0;
}

/* ---------------------------------------------------- through demux.c ---- */
/* THE GATE THAT ACTUALLY PROVES demux.c'S GENERIC DISPATCH REACHES THESE
 * FOUR FORMATS, not merely that avi.c/ts.c/ps.c/flv.c work when called
 * directly. This tree has been burned by exactly the shape this guards
 * against before -- the WPT runner that linked css_apply()/layout_page() and
 * never called them, and the cookie transport that computed the right
 * same-site value in one caller and never in the other -- "linking a TU is
 * not running it; testing one caller is not testing the rule."
 *
 * Opens the SAME file two ways -- format-specific (avi_open/ts_open/
 * ps_open/flv_open, already proven correct against ffprobe by
 * test-containers-diff) and generic (media_open(), demux.c's public API,
 * the ONE a player is documented to use) -- and requires every observable
 * field to agree exactly: media_kind(), track count/type/codec/timescale/
 * nsamples, and every sample's pts/dts/size/keyframe in decode order via
 * BOTH media_get_sample() and media_read(). It also closes the generic
 * mdemux with the generic media_close() (never ts_close()/ps_close()),
 * which is what actually exercises the owns_data flag media_close() now
 * checks -- a real free, run under ASan by test-containers-fuzz too. */
static int verify_generic_one(const char *path)
{
    long len = 0;
    unsigned char *data = read_all(path, &len);
    if (!data) { printf("FAIL cannot read %s\n", path); return 1; }

    int err1 = 0; enum kind k;
    mdemux *direct = open_any(data, len, &err1, &k);
    if (!direct) { printf("FAIL %s: direct open failed: %s\n", path, media_strerror(err1)); free(data); return 1; }

    int err2 = 0;
    mdemux *generic = media_open(data, len, &err2);
    if (!generic) { printf("FAIL %s: media_open() (generic) failed: %s -- direct open succeeded\n",
                            path, media_strerror(err2)); close_any(direct, k); free(data); return 1; }

    int local_fails = 0;
    #define GCHECK(cond, ...) do { if (!(cond)) { \
            printf("FAIL %s ", path); printf(__VA_ARGS__); printf("\n"); local_fails++; } } while (0)

    GCHECK(!strcmp(media_container_name(media_kind(generic)), kind_name(k)),
           "media_kind() via generic path: %s, direct path: %s",
           media_container_name(media_kind(generic)), kind_name(k));
    GCHECK(media_track_count(generic) == media_track_count(direct),
           "track count via generic: %d, direct: %d", media_track_count(generic), media_track_count(direct));

    int nt = media_track_count(direct);
    if (media_track_count(generic) < nt) nt = media_track_count(generic);
    for (int i = 0; i < nt; i++) {
        const media_track *td = media_track_info(direct, i);
        const media_track *tg = media_track_info(generic, i);
        GCHECK(td && tg && td->type == tg->type && td->codec == tg->codec &&
               td->timescale == tg->timescale && td->nsamples == tg->nsamples,
               "track %d mismatch: direct type=%d codec=%d ts=%u n=%ld, "
               "generic type=%d codec=%d ts=%u n=%ld", i,
               td ? td->type : -1, td ? td->codec : -1, td ? td->timescale : 0, td ? td->nsamples : -1,
               tg ? tg->type : -1, tg ? tg->codec : -1, tg ? tg->timescale : 0, tg ? tg->nsamples : -1);
        if (!td || !tg) continue;

        long n = td->nsamples < tg->nsamples ? td->nsamples : tg->nsamples;
        for (long kk = 0; kk < n; kk++) {
            media_sample sd, sg;
            int rd = media_get_sample(direct, i, kk, &sd);
            int rg = media_get_sample(generic, i, kk, &sg);
            GCHECK(rd == 1 && rg == 1, "track %d sample %ld: direct rc=%d generic rc=%d", i, kk, rd, rg);
            if (rd != 1 || rg != 1) continue;
            GCHECK(sd.pts_ticks == sg.pts_ticks && sd.dts_ticks == sg.dts_ticks &&
                   sd.size == sg.size && sd.keyframe == sg.keyframe,
                   "track %d sample %ld: direct pts=%lld dts=%lld size=%ld key=%d, "
                   "generic pts=%lld dts=%lld size=%ld key=%d", i, kk,
                   sd.pts_ticks, sd.dts_ticks, sd.size, sd.keyframe,
                   sg.pts_ticks, sg.dts_ticks, sg.size, sg.keyframe);
            /* This IS the "a sample came out through demux" proof, not a
             * count: the actual payload bytes read back through the
             * generic path's media_sample.data must be the same bytes the
             * direct path's own pointer names. */
            GCHECK(sd.size == sg.size && !memcmp(sd.data, sg.data, (size_t)sd.size),
                   "track %d sample %ld: payload bytes differ between direct and generic", i, kk);
        }
    }

    /* media_read()'s decode-order interleave, generic path only -- must
     * walk exactly total-sample-count samples and never fault. */
    long total = 0;
    for (int i = 0; i < media_track_count(direct); i++) {
        const media_track *td = media_track_info(direct, i);
        if (td) total += td->nsamples;
    }
    media_sample s; long walked = 0;
    while (media_read(generic, &s) == 1) walked++;
    GCHECK(walked == total, "media_read() via generic path walked %ld, direct track sample total is %ld",
           walked, total);

    #undef GCHECK
    close_any(direct, k);
    media_close(generic);      /* THE generic close -- exercises owns_data for TS/PS */
    free(data);
    return local_fails;
}

static int run_verify_generic(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: container_test verify-generic <file>...\n"); return 2; }
    int total_fails = 0, n = 0;
    for (int i = 2; i < argc; i++) {
        int f = verify_generic_one(argv[i]);
        total_fails += f;
        n++;
        printf("  %-24s %s\n", argv[i], f ? "MISMATCH" : "ok (direct == generic, byte for byte)");
    }
    if (total_fails) { printf("verify-generic: %d mismatch(es) over %d file(s)\n", total_fails, n); return 1; }
    printf("verify-generic: %d file(s), media_open()/media_read()/media_get_sample()/"
           "media_close() (demux.c's generic path) match the direct path exactly\n", n);
    return 0;
}

/* --------------------------------------------------------------- main ---- */
int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "verify-generic")) { crc_init(); return run_verify_generic(argc, argv); }
    crc_init();
    if (argc >= 2 && !strcmp(argv[1], "units")) return run_units();
    if (argc < 3) {
        printf("usage: container_test packets|info|order|annexb|raw <file> [track] [out]\n");
        return 2;
    }
    const char *mode = argv[1];
    long len = 0;
    unsigned char *data = read_all(argv[2], &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }

    int err = 0;
    enum kind k;
    mdemux *m = open_any(data, len, &err, &k);
    if (!m) { fprintf(stderr, "open failed: %s (%d)\n", media_strerror(err), err); return 1; }

    if (!strcmp(mode, "info")) {
        printf("container=%s fragmented=%d duration_ns=%lld tracks=%d\n",
               kind_name(k), media_is_fragmented(m), media_duration_ns(m), media_track_count(m));
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
            for (long kk = 0; media_get_sample(m, i, kk, &s) == 1; kk++)
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
        for (long kk = 0; media_get_sample(m, ti, kk, &s) == 1; kk++) {
            if (annexb) {
                long n = media_to_annexb(m, &s, buf, (long)sizeof buf);
                if (n < 0) { fprintf(stderr, "sample %ld: %ld\n", kk, n); return 1; }
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

    close_any(m, k);
    free(data);
    return 0;
}
