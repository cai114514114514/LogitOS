/* c/lib/media/avi.c -- RIFF/AVI demuxer.
 *
 * An AVI file is a RIFF: "RIFF" [u32le size] "AVI " then a flat sequence of
 * chunks and LISTs, exactly like a WAV file's outer shell. Two lists matter:
 * `hdrl` (one `avih` + one `strl` LIST per stream, each `strl` holding
 * `strh`+`strf`) and `movi` (the sample data itself, one chunk per sample
 * named "##xx" where ## is the two-ASCII-digit stream number and xx is
 * "dc"/"db" for video, "wb" for audio). `idx1`, when present, is a flat
 * table mapping every sample to its byte offset.
 *
 * THE OFFSET CONVENTION, established empirically against ffmpeg's own AVI
 * muxer (there is no other way to be sure -- the written spec text is
 * genuinely read two ways in the wild): idx1's dwOffset is relative to the
 * position of the 4-byte "movi" FOURCC ITSELF, not to the first byte after
 * it. That is, if `movi_ref` is the file offset of the ASCII bytes "movi",
 * then a chunk's header (its own ckid+size) is at `movi_ref + dwOffset`, and
 * the sample payload starts 8 bytes after that. Verified against ffprobe's
 * reported packet `pos` on a real ffmpeg-muxed file (see the containers
 * work order): idx1 said dwOffset=4 for the first video chunk, "movi" sat at
 * file offset 10016, and ffprobe reported that packet's byte position as
 * 10016 + 4 + 8 = 10028 -- which is exactly what it printed.
 *
 * THE ZERO-BYTE CHUNK, the other thing that is easy to get wrong and did
 * get this file's first draft wrong: a video chunk of size 0 is not "no
 * data", it is AVI's own "repeat the previous frame" marker (used when a
 * container-level frame rate is finer than the source's, e.g. an
 * externally-timed raw H.264 elementary stream muxed at a nominal higher
 * rate than its actual GOP cadence). It contributes NO sample but DOES
 * still advance the running timestamp by one dwScale tick, exactly like a
 * real chunk would. ffprobe agrees: a file whose idx1 lists 1200 video
 * entries, 1170 of them zero-size, reports exactly 30 video PACKETS -- the
 * dwSampleSize==0 chunk-time-unit accounting below reproduces that count and
 * those dts values exactly (see tests/containers.mk's differential).
 *
 * TIMESTAMPS. Each stream's strh carries (dwScale, dwRate): the track's
 * timescale is dwRate ticks/second and each CHUNK-TIME-UNIT (whether or not
 * it produced a sample) is dwScale ticks. AVI carries no separate
 * presentation timestamp -- pts is defined to equal dts throughout, which is
 * also what every real AVI player does; the format predates B-frame
 * reordering support entirely.
 *
 * CBR AUDIO (dwSampleSize != 0, e.g. classic PCM WAV-in-AVI): one AVI chunk
 * can hold many audio "samples", and duration advances by
 * chunk_bytes/dwSampleSize ticks rather than by one fixed dwScale per chunk.
 * This path exists in the code and is exercised by a hand-built fixture
 * (test_pcm_sample_size in tests/unit/container_test.c) because no positive
 * fixture in the ffmpeg-muxed gate corpus uses it -- H.264 and AAC/MP2 both
 * come out dwSampleSize==0 from ffmpeg's own muxer.
 *
 * OPENDML `indx`/`ix00` (the two-level super-index used above ~1 GiB or
 * with many streams): implemented per the OpenDML AVI-2.0 spec, but NOT
 * gate-verified against a real file -- ffmpeg's avienc never emitted one for
 * any file this small, and none of the source material here needs a >1 GiB
 * fixture to prove a code path. It IS exercised, by construction, in
 * test_opendml_index (tests/unit/container_test.c), which hand-builds a
 * minimal indx/ix00 pair the way tests/unit/gen_laced.py hand-builds
 * Matroska lacing rather than trusting a third implementation to be right.
 *
 * NO INDEX AT ALL: avi_parse scans `movi` chunk by chunk in that case,
 * which is required (a legally-formed AVI can omit AVIF_HASINDEX and rely on
 * a player reading straight through). Keyframe detection there falls back to
 * a heuristic -- see mark_key_scan() -- because idx1's AVIIF_KEYFRAME bit is
 * the only place that fact is normally recorded at all.
 *
 * WHAT THIS DOES NOT DO: multi-part "AVIX" RIFF continuation (the >1 GiB /
 * OpenDML segmented-file convention, a second top-level RIFF chunk after the
 * first) is not chased -- only the first RIFF's own movi is read. A very
 * large capture would demux short, not wrong.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"
#include "avi.h"

#define FOURCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* ------------------------------------------------------- little-endian --- */
/* RIFF is little-endian throughout (unlike ISO-BMFF, which is big-endian),
 * so this file keeps its own readers rather than media_int.h's br_u32/u16. */
static uint32_t rd_le32_p(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t rd_le32(br *b)
{ const uint8_t *p = br_bytes(b, 4); return p ? rd_le32_p(p) : 0; }

static uint16_t rd_le16(br *b)
{ const uint8_t *p = br_bytes(b, 2); return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0; }

static uint32_t rd_fourcc(br *b)
{
    const uint8_t *p = br_bytes(b, 4);
    return p ? (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]) : 0;
}

/* ---------------------------------------------------------- chunk walk --- */
typedef struct { uint32_t id, listtype; br body; int is_list; } achunk;

/* One RIFF chunk: ckid, u32le size, that many bytes. A LIST/RIFF chunk's
 * first 4 payload bytes are its list type, consumed here so `body` always
 * ends up positioned at the list's real children (or, for a leaf chunk, at
 * its raw data). Odd-sized chunks are padded to even -- tolerated rather
 * than required at true end of file, since a well-formed file need not pad
 * its very last chunk. */
static int riff_next(br *b, achunk *out)
{
    if (br_left(b) < 8) return 0;
    out->id = rd_fourcc(b);
    uint32_t sz = rd_le32(b);
    if (!br_ok(b)) return 0;
    out->body = br_sub(b, (long)sz);
    if (!br_ok(&out->body)) { br_fail(b); return 0; }
    out->is_list = (out->id == FOURCC('R','I','F','F') || out->id == FOURCC('L','I','S','T'));
    out->listtype = 0;
    if (out->is_list) {
        out->listtype = rd_fourcc(&out->body);
        if (!br_ok(&out->body)) { br_fail(b); return 0; }
    }
    if (sz & 1) { if (br_left(b) > 0) br_skip(b, 1); }
    return 1;
}

int avi_sniff(const uint8_t *d, long n)
{
    if (!d || n < 12) return 0;
    return d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
           d[8] == 'A' && d[9] == 'V' && d[10] == 'I' && d[11] == ' ';
}

/* ------------------------------------------------------------ headers ---- */
typedef struct {
    uint32_t fcctype;
    uint32_t scale, rate, length, samplesize;
} strh_t;

static void parse_strh(br *b, strh_t *s)
{
    memset(s, 0, sizeof *s);
    s->fcctype = rd_fourcc(b);
    rd_fourcc(b);                  /* fccHandler */
    rd_le32(b);                    /* dwFlags */
    rd_le16(b); rd_le16(b);        /* wPriority, wLanguage */
    rd_le32(b);                    /* dwInitialFrames */
    s->scale = rd_le32(b);
    s->rate  = rd_le32(b);
    rd_le32(b);                    /* dwStart */
    s->length = rd_le32(b);
    rd_le32(b);                    /* dwSuggestedBufferSize */
    rd_le32(b);                    /* dwQuality */
    s->samplesize = rd_le32(b);
    /* rcFrame (8 bytes on disk, despite RECT nominally being 4 LONGs -- the
     * on-the-wire AVISTREAMHEADER is 56 bytes total, confirmed against a
     * real file: 48 bytes of scalar fields above plus an 8-byte rcFrame).
     * Not read; nothing here needs it. */
}

static void parse_strf_video(br *b, mtrack *t)
{
    rd_le32(b);                    /* biSize */
    int32_t w = (int32_t)rd_le32(b);
    int32_t h = (int32_t)rd_le32(b);
    rd_le16(b);                    /* biPlanes */
    rd_le16(b);                    /* biBitCount */
    uint32_t comp = rd_fourcc(b);
    if (w > 0 && w <= MEDIA_MAX_DIM) t->t.width = w;
    int32_t ah = h < 0 ? -h : h;    /* negative biHeight = top-down; magnitude only */
    if (ah > 0 && ah <= MEDIA_MAX_DIM) t->t.height = ah;
    switch (comp) {
    case FOURCC('H','2','6','4'): case FOURCC('h','2','6','4'):
    case FOURCC('X','2','6','4'): case FOURCC('x','2','6','4'):
    case FOURCC('A','V','C','1'): case FOURCC('a','v','c','1'):
    case FOURCC('D','A','V','C'): case FOURCC('V','S','S','H'):
        t->t.codec = MEDIA_CODEC_H264; break;
    case FOURCC('H','E','V','C'): case FOURCC('h','e','v','c'):
    case FOURCC('H','2','6','5'): case FOURCC('h','2','6','5'):
        t->t.codec = MEDIA_CODEC_H265; break;
    case FOURCC('V','P','8','0'): t->t.codec = MEDIA_CODEC_VP8; break;
    case FOURCC('V','P','9','0'): t->t.codec = MEDIA_CODEC_VP9; break;
    case FOURCC('A','V','0','1'): t->t.codec = MEDIA_CODEC_AV1; break;
    case FOURCC('F','M','P','4'): case FOURCC('X','V','I','D'):
    case FOURCC('D','I','V','X'): case FOURCC('D','X','5','0'):
        t->t.codec = MEDIA_CODEC_MPEG4; break;
    /* MJPG (Motion JPEG) and everything else this system has no codec
     * enum for: the track is still indexed correctly (geometry, sample
     * boundaries, timestamps) with codec left MEDIA_CODEC_UNKNOWN, exactly
     * how mp4.c leaves an unrecognised fourcc -- a demuxer's job is the
     * index, not the decode. */
    default: break;
    }
    /* BITMAPINFOHEADER's fixed core is 40 bytes; the six fields above
     * (biSize..biCompression) consumed the first 20, so skip the remaining
     * 20 (biSizeImage, biXPelsPerMeter, biYPelsPerMeter, biClrUsed,
     * biClrImportant) and capture whatever the strf chunk still has past
     * that as this track's extradata. Confirmed against a real ffmpeg AVI
     * mux: an H.264 stream's strf carries the SAME avcC-shaped
     * codec-private bytes this project's MP4/FLV remux of the identical
     * clip carries (byte-for-byte, same CRC32) -- this is the same
     * WAVEFORMATEX-style "extra bytes past the fixed header" convention
     * parse_strf_audio already handles for audio, just with no cbSize
     * field of its own (there is nothing else in the chunk to be, once the
     * fixed header is accounted for). Without this, every AVI H.264/H.265
     * track's framing defaulted to RAW even though the sample bytes are
     * this container's own length-prefixed AVCC/HVCC convention, not
     * Annex B -- so media_to_annexb copied them through un-rewritten, with
     * no start codes and no parameter sets, ever. */
    if (br_bytes(b, 20)) {
        long extra = br_left(b);
        if (extra > 0 && extra <= MEDIA_MAX_EXTRADATA) {
            const uint8_t *p = br_bytes(b, extra);
            if (p) { t->t.extradata = p; t->t.extradata_len = (int)extra; }
        }
    }
}

static void parse_strf_audio(br *b, mtrack *t)
{
    uint16_t tag = rd_le16(b);
    uint16_t ch  = rd_le16(b);
    uint32_t sr  = rd_le32(b);
    rd_le32(b);                    /* nAvgBytesPerSec */
    rd_le16(b);                    /* nBlockAlign */
    uint16_t bits = rd_le16(b);
    if (ch >= 1 && ch <= 8) t->t.channels = ch;
    if (sr) t->t.rate = (int)sr;
    if (bits) t->t.bits = bits;
    switch (tag) {
    case 0x0001: if (bits == 16) t->t.codec = MEDIA_CODEC_PCM_S16LE; break;
    case 0x0055: t->t.codec = MEDIA_CODEC_MP3; break;
    /* 0x00FF: WAVE_FORMAT_AAC (raw AAC). Confirmed against a real ffmpeg
     * mux: with an ADTS source (`-c:a copy` from an .aac ADTS stream),
     * ffmpeg's AVI muxer writes each audio chunk as a whole self-describing
     * ADTS frame (sync word 0xFFF and all) rather than stripping it the way
     * MP4's esds/avcC convention does -- so no CodecPrivate-style extradata
     * is needed here; MEDIA_FRAMING_RAW (md_finish_track's default for
     * anything that is not H.264/H.265) is already correct. */
    case 0x00FF: t->t.codec = MEDIA_CODEC_AAC; break;
    case 0x2000: t->t.codec = MEDIA_CODEC_AC3; break;
    default: break;
    }
    /* WAVEFORMATEX's cbSize + that many extra bytes, when present (some
     * codecs -- notably a differently-sourced AAC stream carrying its
     * AudioSpecificConfig here instead of ADTS-per-frame -- use it). A bare
     * PCMWAVEFORMAT (16 bytes, no cbSize at all) is legal too, so this is
     * conditional on there being anything left to read. */
    if (br_left(b) >= 2) {
        uint32_t cbsize = rd_le16(b);
        if (cbsize > 0 && cbsize <= MEDIA_MAX_EXTRADATA && br_left(b) >= (long)cbsize) {
            const uint8_t *p = br_bytes(b, (long)cbsize);
            if (p) { t->t.extradata = p; t->t.extradata_len = (int)cbsize; }
        }
    }
}

/* How many dwScale-sized time units one chunk of `bytes` is worth.
 *
 * dwSampleSize == 0 (video, and most audio): the container's own convention
 * is "one chunk is one sample", full stop -- dwScale ticks pass per chunk
 * REGARDLESS of its size, which is also exactly what makes AVI's
 * zero-byte-chunk "repeat the previous frame" marker work (see this file's
 * top comment): a chunk contributes exactly one time unit whether it holds
 * a whole frame or nothing at all.
 *
 * dwSampleSize != 0 (CBR audio, e.g. PCM): chunk boundaries carry no timing
 * information of their own -- one chunk can hold an arbitrary number of
 * samples, glued together however the muxer felt like buffering them -- so
 * dwScale ticks are defined per SAMPLE (dwSampleSize bytes), not per chunk,
 * and a chunk of N bytes is worth N/dwSampleSize time units. This was named
 * in this file's top comment and NOT implemented until this fix (every call
 * site used a flat `+= scale`, which is silently wrong only for the one
 * shape -- CBR audio -- that has no fixture in the ffmpeg-muxed gate corpus
 * to catch it with; see test_pcm_sample_size in tests/unit/container_test.c,
 * hand-built for exactly that reason, the same way tests/unit/gen_laced.py
 * hand-builds Matroska lacing rather than trusting a chunk-based format to
 * exercise a byte-based path by accident). */
static long chunk_time_units(uint32_t samplesize, long bytes)
{
#ifdef AVI_CONTROL_NO_CBR
    /* THE PLAUSIBLE WRONG IMPLEMENTATION: treat every chunk as advancing the
     * timestamp by exactly one dwScale tick, the way a codec with
     * dwSampleSize==0 (H.264, AAC/MP2 -- see the file header) actually
     * works. It is wrong specifically for CBR audio (classic PCM-in-AVI,
     * dwSampleSize != 0), where one chunk can hold MANY samples and the
     * real advance is bytes/dwSampleSize ticks -- test_pcm_sample_size
     * exists to catch exactly this and must redden under this switch. */
    (void)samplesize; (void)bytes;
    return 1;
#else
    if (samplesize == 0) return 1;
    if (bytes <= 0) return 0;
    return bytes / (long)samplesize;
#endif
}

/* --------------------------------------------------------- OpenDML idx --- */
/* AVISUPERINDEX ('indx' inside a strl): a flat array of {qwOffset, dwSize,
 * dwDuration}, each pointing at an AVISTDINDEX ('ix00'/'ix01'/...) chunk
 * elsewhere in the file by ABSOLUTE file offset -- unlike idx1 and unlike
 * AVISTDINDEX's own entries, which are both relative. */
#define SIDX_MAX_PARTS 64
typedef struct {
    long long part_off[SIDX_MAX_PARTS];
    int nparts;
    int have;
} sidx_t;

static void parse_super_index(br *b, sidx_t *out)
{
    memset(out, 0, sizeof *out);
    uint16_t longs_per_entry = rd_le16(b);
    rd_le16(b);                    /* bIndexSubType(8) | bIndexType(8) */
    uint32_t nent = rd_le32(b);
    rd_le32(b);                    /* dwChunkId (redundant with strl order here) */
    rd_le32(b); rd_le32(b); rd_le32(b);  /* dwReserved[3] */
    if (!br_ok(b) || longs_per_entry < 4) return;
    for (uint32_t i = 0; i < nent && out->nparts < SIDX_MAX_PARTS; i++) {
        uint32_t lo = rd_le32(b), hi = rd_le32(b);
        rd_le32(b);                /* dwSize of the ix## chunk, unused: we re-read its own header */
        rd_le32(b);                /* dwDuration */
        /* Any extra longs a future entry format might carry. */
        for (uint32_t k = 4; k < longs_per_entry; k++) rd_le32(b);
        if (!br_ok(b)) return;
        out->part_off[out->nparts++] = ((long long)hi << 32) | (long long)lo;
    }
    out->have = (out->nparts > 0);
}

/* AVISTDINDEX ('ix00', ...): entries relative to qwBaseOffset, each pointing
 * at a chunk HEADER (ckid+size) the same way idx1 does -- entry.dwOffset +
 * qwBaseOffset is the file position of the 4-byte ckid, not of the payload.
 * Bit 31 of dwSize is the ONE place OpenDML inverts idx1's convention: here
 * it means "this sample is NOT a sync sample" (idx1's AVIIF_KEYFRAME meant
 * the positive of that). */
static int walk_std_index(mdemux *m, long long part_off, int stream_no,
                           const strh_t *strh, long long *dts_inout)
{
    if (part_off < 0 || part_off + 8 > m->len) return MEDIA_ERR_CORRUPT;
    br top; br_init(&top, m->data, m->len, 0);
    br_seek(&top, (long)part_off);
    achunk ix;
    if (!riff_next(&top, &ix)) return MEDIA_ERR_CORRUPT;
    /* ckid is meant to be "ix00".."ix99"; only the leading "ix" is load-
     * bearing for recognising the chunk, so that is all that is checked. */
    if (((ix.id >> 24) & 0xFF) != 'i' || ((ix.id >> 16) & 0xFF) != 'x')
        return MEDIA_ERR_CORRUPT;

    br *b = &ix.body;
    uint16_t longs_per_entry = rd_le16(b);
    rd_le16(b);                    /* subtype | type */
    uint32_t nent = rd_le32(b);
    rd_le32(b);                    /* dwChunkId */
    uint32_t base_lo = rd_le32(b), base_hi = rd_le32(b);
    rd_le32(b);                    /* dwReserved3 */
    if (!br_ok(b) || longs_per_entry < 2) return MEDIA_ERR_CORRUPT;
    long long base = ((long long)base_hi << 32) | (long long)base_lo;

    mtrack *t = &m->tr[stream_no];
    uint32_t scale = strh->scale ? strh->scale : 1;
    for (uint32_t i = 0; i < nent; i++) {
        uint32_t off = rd_le32(b), rawsz = rd_le32(b);
        for (uint32_t k = 2; k < longs_per_entry; k++) rd_le32(b);
        if (!br_ok(b)) return MEDIA_ERR_CORRUPT;
        uint32_t size = rawsz & 0x7FFFFFFFu;
        int key = !(rawsz & 0x80000000u);
        long long hdr_off = base + (long long)off;
        long long pay_off = hdr_off + 8;
        if (hdr_off < 0 || hdr_off + 8 > m->len || pay_off + (long long)size > m->len)
            return MEDIA_ERR_CORRUPT;
        if (t->t.type != MEDIA_TRACK_VIDEO) key = 1;
        if (size > 0) {
            int rc = md_push(t, *dts_inout, *dts_inout, pay_off, (long)size, key);
            if (rc != MEDIA_OK) return rc;
        }
        *dts_inout += (long long)scale * chunk_time_units(strh->samplesize, (long)size);
    }
    return MEDIA_OK;
}

/* --------------------------------------------------- keyframe scanning --- */
/* Used only when there is no index at all and video is H.264 (the one codec
 * this system can look inside): an IDR access unit starts with NAL type 5,
 * almost always preceded immediately by SPS(7)/PPS(8) in the same AVI chunk
 * since there is nowhere else to put them (AVI carries no avcC-style
 * out-of-band parameter set box). */
static int scan_has_idr(const uint8_t *p, long n)
{
    long i = 0;
    while (i + 3 < n) {
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) {
            if (i + 3 < n && (p[i+3] & 0x1F) == 5) return 1;
            i += 3; continue;
        }
        if (i + 4 < n && p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1) {
            if (i + 4 < n && (p[i+4] & 0x1F) == 5) return 1;
            i += 4; continue;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------- indexing -- */
static int stream_no_of(uint32_t ckid, int *is_sample)
{
    char t0 = (char)(ckid >> 24), t1 = (char)(ckid >> 16);
    char c2 = (char)(ckid >> 8),  c3 = (char)ckid;
    *is_sample = (c2 == 'd' && (c3 == 'c' || c3 == 'b')) ||
                 (c2 == 'w' && c3 == 'b') ||
                 (c2 == 't' && c3 == 'x') ||
                 (c2 == 's' && c3 == 'b');
    if (t0 < '0' || t0 > '9' || t1 < '0' || t1 > '9') return -1;
    return (t0 - '0') * 10 + (t1 - '0');
}

static int build_from_idx1(mdemux *m, br *idx1_body, long long movi_ref,
                            const strh_t *strh, int have_strh_mask)
{
    long long dts[MEDIA_MAX_TRACKS];
    memset(dts, 0, sizeof dts);
    (void)have_strh_mask;
    long n = br_left(idx1_body) / 16;
    for (long i = 0; i < n; i++) {
        const uint8_t *e = br_bytes(idx1_body, 16);
        if (!e) break;
        uint32_t ckid  = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3];
        uint32_t flags = rd_le32_p(e + 4);
        uint32_t off   = rd_le32_p(e + 8);
        uint32_t size  = rd_le32_p(e + 12);

        int is_sample;
        int sn = stream_no_of(ckid, &is_sample);
        if (sn < 0 || sn >= m->ntracks || !is_sample) continue; /* 'rec ' marker, ##ix, etc: not a sample */

        long long hdr_off = movi_ref + (long long)off;
        long long pay_off = hdr_off + 8;
        if (hdr_off < 0 || hdr_off + 8 > m->len || pay_off + (long long)size > m->len)
            return MEDIA_ERR_CORRUPT;
        /* Cross-check: the bytes actually at the claimed header are the ckid
         * idx1 claims. A one-byte flip in dwOffset almost always lands on
         * something that is not a valid "##xx" ckid, which is exactly the
         * corruption this test exists to catch -- REPORTED as
         * MEDIA_ERR_CORRUPT, the same way mp4.c's build_index refuses a bad
         * stco entry, not silently dropped from the index. */
        if (m->data[hdr_off] != e[0] || m->data[hdr_off + 1] != e[1] ||
            m->data[hdr_off + 2] != e[2] || m->data[hdr_off + 3] != e[3])
            return MEDIA_ERR_CORRUPT;

        mtrack *t = &m->tr[sn];
        uint32_t scale = strh[sn].scale ? strh[sn].scale : 1;
        int key = (flags & 0x10) ? 1 : 0;          /* AVIIF_KEYFRAME */
        if (t->t.type != MEDIA_TRACK_VIDEO) key = 1;
        if (size > 0) {
            int rc = md_push(t, dts[sn], dts[sn], pay_off, (long)size, key);
            if (rc != MEDIA_OK) return rc;
        }
        dts[sn] += (long long)scale * chunk_time_units(strh[sn].samplesize, (long)size);
    }
    return MEDIA_OK;
}

static int build_from_opendml(mdemux *m, const sidx_t *sidx, const strh_t *strh)
{
    long long dts[MEDIA_MAX_TRACKS];
    memset(dts, 0, sizeof dts);
    for (int sn = 0; sn < m->ntracks; sn++) {
        if (!sidx[sn].have) continue;
        for (int p = 0; p < sidx[sn].nparts; p++) {
            int rc = walk_std_index(m, sidx[sn].part_off[p], sn, &strh[sn], &dts[sn]);
            if (rc != MEDIA_OK) return rc;
        }
    }
    return MEDIA_OK;
}

/* No index of any kind: read `movi` in file order. A 'rec ' LIST (used by
 * some real-time capture writers to group one interleave unit) is not a
 * sample itself -- recurse into it. */
static int scan_movi(mdemux *m, br *movi, const strh_t *strh)
{
    long long dts[MEDIA_MAX_TRACKS];
    int seen_key[MEDIA_MAX_TRACKS];
    memset(dts, 0, sizeof dts);
    memset(seen_key, 0, sizeof seen_key);

    /* An explicit small stack of (br) windows rather than recursion: 'rec '
     * nests at most one level deep in every real writer, but a hostile file
     * could claim otherwise, and this keeps the bound explicit. */
    br stack[8];
    int sp = 0;
    stack[sp++] = *movi;

    while (sp > 0) {
        br *cur = &stack[sp - 1];
        achunk c;
        if (!riff_next(cur, &c)) { sp--; continue; }
        if (c.id == FOURCC('L','I','S','T') && c.listtype == FOURCC('r','e','c',' ')) {
            if (sp < 8) stack[sp++] = c.body;
            continue;
        }
        if (c.is_list) continue;   /* an unexpected LIST inside movi: skip its contents */
        int is_sample;
        int sn = stream_no_of(c.id, &is_sample);
        if (sn < 0 || sn >= m->ntracks || !is_sample) continue;

        long size = c.body.len;
        long long pay_off = c.body.org;
        mtrack *t = &m->tr[sn];
        uint32_t scale = strh[sn].scale ? strh[sn].scale : 1;
        if (size > 0) {
            int key;
            if (t->t.type != MEDIA_TRACK_VIDEO) key = 1;
            else if (t->t.codec == MEDIA_CODEC_H264)
                key = scan_has_idr(m->data + pay_off, size);
            else
                key = !seen_key[sn];    /* unknown video codec: first sample only */
            if (key) seen_key[sn] = 1;
            int rc = md_push(t, dts[sn], dts[sn], pay_off, size, key);
            if (rc != MEDIA_OK) return rc;
        }
        dts[sn] += (long long)scale * chunk_time_units(strh[sn].samplesize, size);
    }
    return MEDIA_OK;
}

/* ------------------------------------------------------------------ top -- */
int avi_parse(mdemux *m)
{
    br top;
    br_init(&top, m->data, m->len, 0);

    achunk riff;
    if (!riff_next(&top, &riff) || riff.id != FOURCC('R','I','F','F') ||
        riff.listtype != FOURCC('A','V','I',' '))
        return MEDIA_ERR_CORRUPT;

    strh_t strh[MEDIA_MAX_TRACKS];
    memset(strh, 0, sizeof strh);
    sidx_t sidx[MEDIA_MAX_TRACKS];
    memset(sidx, 0, sizeof sidx);

    long long movi_ref = -1;
    br movi_scan; int have_movi = 0;
    br idx1_body; int have_idx1 = 0;

    achunk c;
    while (riff_next(&riff.body, &c)) {
        if (c.id == FOURCC('L','I','S','T') && c.listtype == FOURCC('h','d','r','l')) {
            int stream_no = -1;
            achunk h;
            while (riff_next(&c.body, &h)) {
                if (h.id == FOURCC('L','I','S','T') && h.listtype == FOURCC('s','t','r','l')) {
                    stream_no++;
                    if (stream_no >= MEDIA_MAX_TRACKS) continue;
                    mtrack *t = md_add_track(m);
                    if (!t) return MEDIA_ERR_RANGE;
                    t->t.id = stream_no;
                    /* Default before strh is seen, not after: a strl LIST
                     * with no strh chunk at all (corrupt/truncated -- found
                     * by container_fuzz.c's ASan run, "zero timescale on
                     * track 0") left this track's timescale at md_add_track's
                     * memset-zero forever, since the only assignment lived
                     * inside the strh branch below. A caller dividing ticks
                     * by timescale (md_ticks_to_ns) on that track divides by
                     * zero. Every other format here hardcodes a sane
                     * timescale at track-creation time for the same reason
                     * (ts.c/ps.c: 90000, flv.c: 1000); this just makes avi.c
                     * match instead of leaving one path relying on strh
                     * always being present. */
                    t->t.timescale = 1000;
                    achunk s;
                    while (riff_next(&h.body, &s)) {
                        if (s.id == FOURCC('s','t','r','h')) {
                            parse_strh(&s.body, &strh[stream_no]);
                            uint32_t ft = strh[stream_no].fcctype;
                            t->t.type = (ft == FOURCC('v','i','d','s')) ? MEDIA_TRACK_VIDEO
                                      : (ft == FOURCC('a','u','d','s')) ? MEDIA_TRACK_AUDIO
                                      : MEDIA_TRACK_OTHER;
                            t->t.timescale = strh[stream_no].rate ? strh[stream_no].rate : 1000;
                        } else if (s.id == FOURCC('s','t','r','f')) {
                            if (t->t.type == MEDIA_TRACK_VIDEO) parse_strf_video(&s.body, t);
                            else if (t->t.type == MEDIA_TRACK_AUDIO) parse_strf_audio(&s.body, t);
                        } else if (s.id == FOURCC('i','n','d','x')) {
                            parse_super_index(&s.body, &sidx[stream_no]);
                        }
                    }
                    if (strh[stream_no].length) {
                        /* Both operands are attacker-controlled uint32_t
                         * fields (dwLength, dwScale) straight out of strh,
                         * and their product does not fit in `long long`
                         * near the top of that range -- found by
                         * test-containers-fuzz's UBSan run: "signed integer
                         * overflow: 3520188881 * 3520188881". Multiply
                         * unsigned in 64 bits first (that CANNOT overflow --
                         * the largest possible product, UINT32_MAX squared,
                         * is still under UINT64_MAX), then refuse rather
                         * than reinterpret a value too large for the signed
                         * duration field to hold. A header field is a claim,
                         * not a fact (media.h's own words for exactly this
                         * shape of check). */
                        uint64_t len64 = strh[stream_no].length;
                        uint64_t scale64 = strh[stream_no].scale ? strh[stream_no].scale : 1;
                        uint64_t product = len64 * scale64;
                        t->t.duration = (product <= (uint64_t)0x7FFFFFFFFFFFFFFFULL)
                                       ? (long long)product : -1;
                    }
                }
            }
        } else if (c.id == FOURCC('L','I','S','T') && c.listtype == FOURCC('m','o','v','i')) {
            movi_ref = c.body.org;
            movi_scan = c.body;
            have_movi = 1;
        } else if (c.id == FOURCC('i','d','x','1')) {
            idx1_body = c.body;
            have_idx1 = 1;
        }
    }

    if (m->ntracks == 0) return MEDIA_ERR_CORRUPT;
    if (!have_movi) return MEDIA_ERR_CORRUPT;

    int rc;
    if (have_idx1) {
        rc = build_from_idx1(m, &idx1_body, movi_ref, strh, 0);
    } else {
        int any_sidx = 0;
        for (int i = 0; i < m->ntracks; i++) any_sidx |= sidx[i].have;
        rc = any_sidx ? build_from_opendml(m, sidx, strh) : scan_movi(m, &movi_scan, strh);
    }
    if (rc != MEDIA_OK) return rc;

    for (int i = 0; i < m->ntracks; i++)
        if (m->tr[i].n == 0 && m->tr[i].t.duration <= 0) m->tr[i].t.duration = -1;
    return MEDIA_OK;
}

mdemux *avi_open(const uint8_t *data, long len, int *err)
{
    if (err) *err = MEDIA_OK;
    if (!avi_sniff(data, len)) { if (err) *err = MEDIA_ERR_UNSUPPORTED; return 0; }
    mdemux *m = (mdemux *)calloc(1, sizeof *m);
    if (!m) { if (err) *err = MEDIA_ERR_OOM; return 0; }
    m->data = data; m->len = len; m->kind = MEDIA_CONT_AVI;
    m->movie_timescale = 1000;
    m->movie_duration = -1;
    m->selected = -1;

    int e = avi_parse(m);
    if (e == MEDIA_OK && m->ntracks == 0) e = MEDIA_ERR_CORRUPT;
    if (e != MEDIA_OK) { media_close(m); if (err) *err = e; return 0; }
    for (int i = 0; i < m->ntracks; i++) md_finish_track(&m->tr[i]);
    return m;
}
