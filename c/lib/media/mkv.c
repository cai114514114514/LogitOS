/* c/lib/media/mkv.c -- Matroska and WebM.
 *
 * WebM is Matroska with a shorter list of allowed codecs, so this is one
 * parser and not two.
 *
 * EBML is the opposite design to MP4. Where a box carries a four-byte type and
 * a 32-bit size, an EBML element carries a VARIABLE-LENGTH id and a
 * VARIABLE-LENGTH size, both self-describing by their leading zero bits, and
 * the size is allowed to be UNKNOWN -- which is the whole point, because it is
 * what lets a live encoder start writing a Cluster before it knows how long
 * the Cluster will be. A parser that assumes a size is always known cannot
 * open a stream recorded from a camera.
 *
 * The other thing MP4 does not have is LACING: several small frames packed
 * into one Block to avoid paying a block header per 24 ms of audio. There are
 * three schemes and they are all in use -- Xiph (255-accumulating byte runs,
 * inherited from Ogg), fixed (equal division), and EBML (a first size then
 * signed deltas). All three are here, because a file with laced audio and a
 * parser without lacing does not fail; it plays the first frame of every
 * twelve and calls the rest a header.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"

/* Level 0/1 ids, in full (an EBML id INCLUDES its length marker). */
#define ID_EBML          0x1A45DFA3u
#define ID_SEGMENT       0x18538067u
#define ID_SEEKHEAD      0x114D9B74u
#define ID_INFO          0x1549A966u
#define ID_TRACKS        0x1654AE6Bu
#define ID_CLUSTER       0x1F43B675u
#define ID_CUES          0x1C53BB6Bu
#define ID_ATTACHMENTS   0x1941A469u
#define ID_CHAPTERS      0x1043A770u
#define ID_TAGS          0x1254C367u

#define ID_TIMESTAMPSCALE 0x2AD7B1u
#define ID_DURATION       0x4489u
#define ID_TRACKENTRY     0xAEu
#define ID_TRACKNUMBER    0xD7u
#define ID_TRACKTYPE      0x83u
#define ID_CODECID        0x86u
#define ID_CODECPRIVATE   0x63A2u
#define ID_DEFAULTDUR     0x23E383u
#define ID_CODECDELAY     0x56AAu
#define ID_VIDEO          0xE0u
#define ID_PIXELWIDTH     0xB0u
#define ID_PIXELHEIGHT    0xBAu
#define ID_DISPLAYWIDTH   0x54B0u
#define ID_DISPLAYHEIGHT  0x54BAu
#define ID_AUDIO          0xE1u
#define ID_SAMPLINGFREQ   0xB5u
#define ID_CHANNELS       0x9Fu
#define ID_BITDEPTH       0x6264u
#define ID_TIMESTAMP      0xE7u
#define ID_SIMPLEBLOCK    0xA3u
#define ID_BLOCKGROUP     0xA0u
#define ID_BLOCK          0xA1u
#define ID_BLOCKDURATION  0x9Bu
#define ID_REFERENCEBLOCK 0xFBu

typedef struct {
    uint32_t  id;
    long long size;        /* payload length; -1 when the file says "unknown" */
} eel;

/* An EBML variable-length integer. `keep_marker` distinguishes an ID (the
 * marker bit is part of the value) from a size (it is not). Returns the number
 * of bytes consumed, or 0 on failure. */
static int vint(br *b, uint64_t *out, int keep_marker, int *unknown)
{
    if (unknown) *unknown = 0;
    if (br_left(b) < 1) { br_fail(b); return 0; }
    uint32_t first = br_u8(b);
    if (first == 0) { br_fail(b); return 0; }   /* > 8 byte lengths do not exist */
    int len = 1;
    uint32_t mask = 0x80;
    while (!(first & mask)) { mask >>= 1; len++; }
    uint64_t v = keep_marker ? first : (first & (mask - 1));
    uint64_t all_ones = keep_marker ? 0 : (mask - 1);
    for (int i = 1; i < len; i++) {
        uint32_t c = br_u8(b);
        v = (v << 8) | c;
        all_ones = (all_ones << 8) | 0xFF;
    }
    if (!br_ok(b)) return 0;
    if (!keep_marker && v == all_ones && unknown) *unknown = 1;
    *out = v;
    return len;
}

/* Read one element header. Returns 1, or 0 at the end of the window. */
static int next_elem(br *b, eel *e)
{
    if (br_left(b) < 2) return 0;
    uint64_t id = 0, sz = 0;
    int unknown = 0;
    if (!vint(b, &id, 1, 0)) return 0;
    if (!vint(b, &sz, 0, &unknown)) return 0;
    if (id > 0xFFFFFFFFull) { br_fail(b); return 0; }
    e->id = (uint32_t)id;
    e->size = unknown ? -1 : (long long)sz;
    if (!unknown && (e->size < 0 || e->size > br_left(b))) {
        /* A truncated final element is the normal shape of a file that was
         * still being written. Give it what is left rather than refusing the
         * whole file -- but never more than there is. */
        e->size = br_left(b);
    }
    return br_ok(b);
}

static int is_level1(uint32_t id)
{
    return id == ID_SEEKHEAD || id == ID_INFO || id == ID_TRACKS ||
           id == ID_CLUSTER || id == ID_CUES || id == ID_ATTACHMENTS ||
           id == ID_CHAPTERS || id == ID_TAGS || id == ID_EBML ||
           id == ID_SEGMENT;
}

static uint64_t rd_uint(br *b, long n)
{
    uint64_t v = 0;
    if (n < 0 || n > 8) { br_fail(b); return 0; }
    for (long i = 0; i < n; i++) v = (v << 8) | br_u8(b);
    return v;
}

static double rd_float(br *b, long n)
{
    if (n == 4) {
        uint32_t bits = br_u32(b);
        float f;
        memcpy(&f, &bits, 4);
        return (double)f;
    }
    if (n == 8) {
        uint64_t bits = br_u64(b);
        double d;
        memcpy(&d, &bits, 8);
        return d;
    }
    br_skip(b, n);
    return 0.0;
}

/* --------------------------------------------------------------- tracks -- */
static media_codec codec_from_id(const char *id, long n)
{
    struct { const char *p; media_codec c; int prefix; } map[] = {
        { "V_MPEG4/ISO/AVC",  MEDIA_CODEC_H264,      0 },
        { "V_MPEGH/ISO/HEVC", MEDIA_CODEC_H265,      0 },
        { "V_VP8",            MEDIA_CODEC_VP8,       0 },
        { "V_VP9",            MEDIA_CODEC_VP9,       0 },
        { "V_AV1",            MEDIA_CODEC_AV1,       0 },
        { "V_MPEG4/ISO/",     MEDIA_CODEC_MPEG4,     1 },
        { "A_MPEG/L3",        MEDIA_CODEC_MP3,       0 },
        { "A_AAC",            MEDIA_CODEC_AAC,       1 },
        { "A_FLAC",           MEDIA_CODEC_FLAC,      0 },
        { "A_PCM/INT/LIT",    MEDIA_CODEC_PCM_S16LE, 0 },
        { "A_PCM/INT/BIG",    MEDIA_CODEC_PCM_S16BE, 0 },
        { "A_OPUS",           MEDIA_CODEC_OPUS,      0 },
        { "A_VORBIS",         MEDIA_CODEC_VORBIS,    0 },
        { "A_AC3",            MEDIA_CODEC_AC3,       1 },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        long l = (long)strlen(map[i].p);
        if (map[i].prefix) {
            if (n >= l && !memcmp(id, map[i].p, (size_t)l)) return map[i].c;
        } else {
            if (n == l && !memcmp(id, map[i].p, (size_t)l)) return map[i].c;
        }
    }
    return MEDIA_CODEC_UNKNOWN;
}

static int parse_track_entry(mdemux *m, br *b, unsigned timescale)
{
    mtrack *t = md_add_track(m);
    if (!t) return MEDIA_ERR_RANGE;
    t->t.timescale = timescale;
    t->t.type = MEDIA_TRACK_OTHER;

    const char *cid = 0; long cid_n = 0;
    eel e;
    while (next_elem(b, &e)) {
        if (e.size < 0) break;
        br v = br_sub(b, (long)e.size);
        if (!br_ok(&v)) break;
        switch (e.id) {
        case ID_TRACKNUMBER: t->t.id = (int)rd_uint(&v, e.size); break;
        case ID_TRACKTYPE: {
            uint64_t ty = rd_uint(&v, e.size);
            t->t.type = (ty == 1) ? MEDIA_TRACK_VIDEO
                      : (ty == 2) ? MEDIA_TRACK_AUDIO : MEDIA_TRACK_OTHER;
            break; }
        case ID_CODECID: {
            const uint8_t *p = br_bytes(&v, (long)e.size);
            if (p) { cid = (const char *)p; cid_n = (long)e.size;
                     while (cid_n > 0 && cid[cid_n - 1] == 0) cid_n--; }
            break; }
        case ID_DEFAULTDUR:
            /* The nominal duration of ONE frame, in nanoseconds. It is what
             * lets a laced block's frames be spread over time instead of all
             * landing on the block's own stamp: Matroska gives a lace a single
             * timestamp, and this element is the container's own statement of
             * how far apart the frames in it are. mkvmerge writes it for every
             * audio track. Converted to ticks once the timescale is known. */
            t->lace_ticks = (long long)rd_uint(&v, e.size);    /* ns, for now */
            break;
        case ID_CODECDELAY:
            /* Encoder priming, in nanoseconds: the samples at the start of the
             * stream that exist only so the decoder has state and must not be
             * heard. Matroska's answer to MP4's edit list, and leaving it out
             * puts audio permanently early by exactly that much -- 25 ms for
             * LAME, 6.5 ms for Opus, both audible against video. Converted to
             * ticks once the timescale is known. */
            t->delay_ticks = (long long)rd_uint(&v, e.size);   /* ns, for now */
            break;
        case ID_CODECPRIVATE: {
            const uint8_t *p = br_bytes(&v, (long)e.size);
            if (p && e.size <= MEDIA_MAX_EXTRADATA) {
                t->t.extradata = p; t->t.extradata_len = (int)e.size;
            }
            break; }
        case ID_VIDEO: {
            eel w;
            int px = 0, py = 0;
            while (next_elem(&v, &w)) {
                if (w.size < 0) break;
                br s = br_sub(&v, (long)w.size);
                if (w.id == ID_PIXELWIDTH)  px = (int)rd_uint(&s, w.size);
                else if (w.id == ID_PIXELHEIGHT) py = (int)rd_uint(&s, w.size);
                else if (w.id == ID_DISPLAYWIDTH)  { int d = (int)rd_uint(&s, w.size);
                                                     if (d > 0) t->t.width = d; }
                else if (w.id == ID_DISPLAYHEIGHT) { int d = (int)rd_uint(&s, w.size);
                                                     if (d > 0) t->t.height = d; }
            }
            /* Pixel dimensions are what the decoder produces; display ones are
             * a stretch instruction. Prefer pixels when display is absent. */
            if (!t->t.width && px > 0 && px <= MEDIA_MAX_DIM) t->t.width = px;
            if (!t->t.height && py > 0 && py <= MEDIA_MAX_DIM) t->t.height = py;
            break; }
        case ID_AUDIO: {
            eel w;
            while (next_elem(&v, &w)) {
                if (w.size < 0) break;
                br s = br_sub(&v, (long)w.size);
                if (w.id == ID_SAMPLINGFREQ) {
                    double f = rd_float(&s, w.size);
                    if (f > 0 && f < 1000000.0) t->t.rate = (int)(f + 0.5);
                } else if (w.id == ID_CHANNELS) {
                    int c = (int)rd_uint(&s, w.size);
                    if (c >= 1 && c <= 8) t->t.channels = c;
                } else if (w.id == ID_BITDEPTH) {
                    t->t.bits = (int)rd_uint(&s, w.size);
                }
            }
            break; }
        default: break;
        }
    }
    if (cid) t->t.codec = codec_from_id(cid, cid_n);
    return MEDIA_OK;
}

/* ---------------------------------------------------------------- blocks - */
static mtrack *track_by_number(mdemux *m, uint64_t num)
{
    for (int i = 0; i < m->ntracks; i++)
        if ((uint64_t)m->tr[i].t.id == num) return &m->tr[i];
    return 0;
}

/* One Block or SimpleBlock. `force_key` is -1 for a SimpleBlock (the flag byte
 * decides) and 0/1 for a Block inside a BlockGroup, where the presence of a
 * ReferenceBlock is what decides and the flag byte does not carry it. */
static int parse_block(mdemux *m, br *b, long long cluster_ts, int force_key)
{
    uint64_t num = 0;
    if (!vint(b, &num, 0, 0)) return MEDIA_ERR_CORRUPT;
    int16_t rel = (int16_t)br_u16(b);
    uint32_t flags = br_u8(b);
    if (!br_ok(b)) return MEDIA_ERR_CORRUPT;

    mtrack *t = track_by_number(m, num);
    long long ts = cluster_ts + rel - (t ? t->delay_ticks : 0);
    int key = (force_key >= 0) ? force_key : ((flags & 0x80) ? 1 : 0);
    int lacing = (flags >> 1) & 3;
#ifdef DEMUX_CONTROL_NO_LACING
    /* THE SECOND NEGATIVE CONTROL, and the other real bug: pretend no block is
     * laced. A parser that does this does not fail loudly -- it hands the
     * decoder the first frame of every group with the other eleven glued to
     * the end of it, and audio still comes out, just wrong. Required to be
     * caught by make test-demux-negctl. */
    lacing = 0;
#endif

    /* Frame sizes. Everything but the last is explicit; the last is whatever
     * remains, which is also the only reason a size can be trusted: the sum is
     * checked against the block before any of it is used. */
    long sizes[256];
    int nframes = 1;

    if (lacing) {
        uint32_t nm1 = br_u8(b);
        if (!br_ok(b)) return MEDIA_ERR_CORRUPT;
        nframes = (int)nm1 + 1;
        if (nframes > (int)(sizeof sizes / sizeof sizes[0])) return MEDIA_ERR_RANGE;

        if (lacing == 2) {                        /* fixed */
            long rest = br_left(b);
            if (rest % nframes) return MEDIA_ERR_CORRUPT;
            for (int i = 0; i < nframes; i++) sizes[i] = rest / nframes;
        } else if (lacing == 1) {                 /* Xiph */
            long used = 0;
            for (int i = 0; i < nframes - 1; i++) {
                long s = 0;
                for (;;) {
                    uint32_t c = br_u8(b);
                    if (!br_ok(b)) return MEDIA_ERR_CORRUPT;
                    s += (long)c;
                    if (c != 255) break;
                    if (s > (1L << 28)) return MEDIA_ERR_RANGE;
                }
                sizes[i] = s;
                used += s;
            }
            long rest = br_left(b) - used;
            if (rest < 0) return MEDIA_ERR_CORRUPT;
            sizes[nframes - 1] = rest;
        } else {                                  /* EBML lacing */
            uint64_t first = 0;
            if (!vint(b, &first, 0, 0)) return MEDIA_ERR_CORRUPT;
            if (first > (uint64_t)(1L << 30)) return MEDIA_ERR_RANGE;
            sizes[0] = (long)first;
            long used = sizes[0];
            long prev = sizes[0];
            for (int i = 1; i < nframes - 1; i++) {
                uint64_t raw = 0;
                int len = vint(b, &raw, 0, 0);
                if (!len) return MEDIA_ERR_CORRUPT;
                /* Signed: the bias is 2^(7*len-1) - 1. */
                long long bias = (1LL << (7 * len - 1)) - 1;
                long long d = (long long)raw - bias;
                long s = (long)(prev + d);
                if (s < 0) return MEDIA_ERR_CORRUPT;
                sizes[i] = s;
                used += s;
                prev = s;
            }
            long rest = br_left(b) - used;
            if (rest < 0) return MEDIA_ERR_CORRUPT;
            sizes[nframes - 1] = rest;
        }
    } else {
        sizes[0] = br_left(b);
    }

    for (int i = 0; i < nframes; i++) {
        const uint8_t *p = br_bytes(b, sizes[i]);
        if (!p) return MEDIA_ERR_CORRUPT;
        if (!t) continue;                          /* a track we never saw declared */
        long long off = (long long)(p - m->data);
        /* A laced block carries ONE timestamp for all its frames. Spreading
         * them needs a per-frame duration, and the container's own statement
         * of that is DefaultDuration -- so frame i is the block's stamp plus
         * i durations when the track declares one, and the block's stamp when
         * it does not. Deriving a duration from the payload instead (bytes
         * over sample rate, or an MP3 frame header) would be the decoder's
         * knowledge leaking into the demuxer, and a guess on anything with a
         * variable bit rate. */
        long long fts = ts + (long long)i * t->lace_ticks;
        int rc = md_push(t, fts, fts, off, sizes[i], key);
        if (rc != MEDIA_OK) return rc;
    }
    return MEDIA_OK;
}

static int parse_cluster(mdemux *m, br *b)
{
    long long cluster_ts = 0;
    eel e;
    while (next_elem(b, &e)) {
        if (e.size < 0) break;                     /* unknown-size child: stop */
        if (e.id == ID_TIMESTAMP) {
            br v = br_sub(b, (long)e.size);
            cluster_ts = (long long)rd_uint(&v, e.size);
        } else if (e.id == ID_SIMPLEBLOCK) {
            br v = br_sub(b, (long)e.size);
            if (!br_ok(&v)) break;
            int rc = parse_block(m, &v, cluster_ts, -1);
            if (rc != MEDIA_OK) return rc;
        } else if (e.id == ID_BLOCKGROUP) {
            br g = br_sub(b, (long)e.size);
            if (!br_ok(&g)) break;
            /* The Block comes before the ReferenceBlock as often as after, so
             * find out whether there is a reference FIRST and then parse. */
            br scan = g;
            int key = 1;
            eel w;
            while (next_elem(&scan, &w)) {
                if (w.size < 0) break;
                if (w.id == ID_REFERENCEBLOCK) key = 0;
                br_skip(&scan, (long)w.size);
            }
            while (next_elem(&g, &w)) {
                if (w.size < 0) break;
                br v = br_sub(&g, (long)w.size);
                if (w.id != ID_BLOCK) continue;
                if (!br_ok(&v)) break;
                int rc = parse_block(m, &v, cluster_ts, key);
                if (rc != MEDIA_OK) return rc;
            }
        } else {
            br_skip(b, (long)e.size);
        }
    }
    return MEDIA_OK;
}

/* ------------------------------------------------------------------ top -- */
int mkv_parse(mdemux *m)
{
    br top;
    br_init(&top, m->data, m->len, 0);

    unsigned long long time_scale = 1000000ULL;   /* ns per tick; the default */
    unsigned timescale = 1000;                     /* ticks per second */
    double duration_ticks = 0;
    int saw_tracks = 0;

    eel e;
    if (!next_elem(&top, &e) || e.id != ID_EBML) return MEDIA_ERR_CORRUPT;
    br_skip(&top, e.size < 0 ? 0 : (long)e.size);

    /* Find the Segment. A file may carry more than one; the first is the one
     * every player reads. */
    br seg; br_init(&seg, 0, 0, 0);
    int found = 0;
    while (next_elem(&top, &e)) {
        if (e.id == ID_SEGMENT) {
            long n = (e.size < 0) ? br_left(&top) : (long)e.size;
            seg = br_sub(&top, n);
            found = br_ok(&seg);
            break;
        }
        if (e.size < 0) break;
        br_skip(&top, (long)e.size);
    }
    if (!found) return MEDIA_ERR_CORRUPT;

    /* Pass 1: Info and Tracks. Clusters are indexed in pass 2 because a
     * Cluster can legally precede Tracks only in a stream we do not accept,
     * and doing it in two passes means the track table is complete before the
     * first block is attributed to it. */
    br p1 = seg;
    while (next_elem(&p1, &e)) {
        long n = (e.size < 0) ? br_left(&p1) : (long)e.size;
        br v = br_sub(&p1, n);
        if (!br_ok(&v)) break;
        if (e.id == ID_INFO) {
            eel w;
            while (next_elem(&v, &w)) {
                if (w.size < 0) break;
                br s = br_sub(&v, (long)w.size);
                if (w.id == ID_TIMESTAMPSCALE) {
                    unsigned long long ts = rd_uint(&s, w.size);
                    if (ts >= 1 && ts <= 1000000000ULL) time_scale = ts;
                } else if (w.id == ID_DURATION) {
                    duration_ticks = rd_float(&s, w.size);
                }
            }
        } else if (e.id == ID_TRACKS) {
            saw_tracks = 1;
            eel w;
            while (next_elem(&v, &w)) {
                if (w.size < 0) break;
                br s = br_sub(&v, (long)w.size);
                if (w.id != ID_TRACKENTRY) continue;
                /* timescale is not final until Info has been read; Info comes
                 * first in every muxer's output, and it is patched below in
                 * any case. */
                int rc = parse_track_entry(m, &s, 1000);
                if (rc != MEDIA_OK) return rc;
            }
        }
        if (e.size < 0) break;
    }
    if (!saw_tracks) return MEDIA_ERR_CORRUPT;

    /* ticks per second. 1e9/time_scale is exact for every scale a muxer uses
     * (1e6 -> 1000 Hz, the ffmpeg default). When it is not, fall back to
     * nanoseconds and let the timestamps be ns. */
    if (1000000000ULL % time_scale == 0) timescale = (unsigned)(1000000000ULL / time_scale);
    else timescale = 1000000000u;
    for (int i = 0; i < m->ntracks; i++) {
        m->tr[i].t.timescale = timescale;
        /* CodecDelay was read in nanoseconds; it is used in ticks. Round to
         * nearest, away from zero -- Opus's 6.5 ms at a 1 ms tick has to
         * become 7 and not 6, which is also what ffmpeg's rescale does. */
        if (m->tr[i].delay_ticks) {
            long long ns = m->tr[i].delay_ticks;
            m->tr[i].delay_ticks = (ns * (long long)timescale + 500000000LL) / 1000000000LL;
        }
        /* DefaultDuration truncates rather than rounds: it is a count of ticks
         * and a frame lasting 1.9 of them advances the next by 1. */
        if (m->tr[i].lace_ticks)
            m->tr[i].lace_ticks /= (long long)time_scale;
    }
    m->movie_timescale = timescale;
    /* Duration is the one field in Matroska that is a FLOAT, and it comes off
     * the wire. A double from a hostile file can be 1e300, or a NaN, and
     * casting either to long long is undefined behaviour rather than merely a
     * wrong answer -- so the value is range-checked BEFORE the conversion, not
     * after. (The fuzzer found this: 1.8e22 out of a mutated Duration
     * element.) NaN fails every comparison, so it is excluded by the same
     * test. */
    if (duration_ticks > 0) {
        double scale = (timescale == 1000000000u) ? (double)time_scale : 1.0;
        double d = duration_ticks * scale;
        if (d > 0 && d < 9.0e18) m->movie_duration = (long long)d;
    }

    /* Pass 2: the clusters. */
    br p2 = seg;
    while (next_elem(&p2, &e)) {
        if (e.id == ID_CLUSTER) {
            long n;
            if (e.size >= 0) n = (long)e.size;
            else {
                /* Unknown-size Cluster: it runs to the next level-1 element.
                 * This is what a file recorded live looks like, and the only
                 * way to find the end is to look for the next id. */
                long start = p2.pos, scan = start;
                long end = p2.len;
                while (scan + 4 <= p2.len) {
                    uint32_t id4 = ((uint32_t)p2.base[scan] << 24) |
                                   ((uint32_t)p2.base[scan + 1] << 16) |
                                   ((uint32_t)p2.base[scan + 2] << 8) | p2.base[scan + 3];
                    if (is_level1(id4)) { end = scan; break; }
                    scan++;
                }
                n = end - start;
                if (n < 0) n = 0;
            }
            br v = br_sub(&p2, n);
            if (!br_ok(&v)) break;
            int rc = parse_cluster(m, &v);
            if (rc != MEDIA_OK) return rc;
        } else {
            if (e.size < 0) break;
            br_skip(&p2, (long)e.size);
        }
    }

    for (int i = 0; i < m->ntracks; i++) {
        mtrack *t = &m->tr[i];
        if (t->n > 0) t->t.duration = t->s[t->n - 1].pts;
    }
    return MEDIA_OK;
}
