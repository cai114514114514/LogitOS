/* c/lib/media/mp4.c -- ISO base media file format (MP4/MOV/M4A), including
 * fragmented MP4.
 *
 * An MP4 is a tree of boxes: [u32 size][u8 type[4]][payload]. `moov` holds the
 * metadata, `mdat` holds the bytes, and NOTHING in the metadata is checked by
 * the format itself -- a chunk offset is a raw file position that the file
 * supplies and the parser dereferences. That is why every offset built here is
 * validated against the file length before it reaches the index, and why every
 * box is read through a `br` window carved out of its parent.
 *
 * Two very different layouts produce the same index:
 *
 *   PROGRESSIVE. `stbl` holds five or six parallel tables that between them
 *   say, for every sample: how big it is, which chunk it is in, where that
 *   chunk is, how long it lasts, how far its presentation time is offset from
 *   its decode time, and whether it is a sync sample. None of them is indexed
 *   by sample number: stts and ctts are RUN-LENGTH ENCODED, stsc is run-length
 *   over chunks, and the sample-to-chunk mapping is implicit in the order. The
 *   loop below walks all of them at once, which is the only way to do it in
 *   one pass, and is where a sample-table bug hides.
 *
 *   FRAGMENTED (`moof` + `mdat`, repeated). There is no sample table at all:
 *   each fragment carries its own `trun` with per-sample durations, sizes and
 *   flags, defaulted from `tfhd` and then from `trex` in the movie header.
 *   This is what a streaming site delivers, and the reason it exists is that
 *   the sample table above cannot be written until the file is finished.
 *
 * Timestamps come out matching ffmpeg's, which means the edit list is applied:
 * a one-entry `elst` with media_time > 0 is how every encoder expresses "the
 * first frames are decoder priming, do not show them", and ignoring it puts
 * audio and video permanently out of step by exactly that offset.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"

#define FOURCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* One box header read out of a window: type, and a sub-window over its
 * payload. Returns 0 when the window has no room for another box. */
typedef struct { uint32_t type; br body; long hdr_at; } mbox;

static int next_box(br *b, mbox *out)
{
    if (br_left(b) < 8) return 0;
    long at = br_tell(b);
    uint64_t sz = br_u32(b);
    uint32_t ty = br_u32(b);
    long hdr = 8;
    if (sz == 1) { sz = br_u64(b); hdr = 16; }
    else if (sz == 0) { sz = (uint64_t)(hdr + br_left(b)); }
    if (!br_ok(b)) return 0;
    if (sz < (uint64_t)hdr) { br_fail(b); return 0; }
    uint64_t payload = sz - (uint64_t)hdr;
    if (payload > (uint64_t)br_left(b)) { br_fail(b); return 0; }
    out->type = ty;
    out->hdr_at = at;
    out->body = br_sub(b, (long)payload);
    return br_ok(b) ? 1 : 0;
}

/* version(8) | flags(24) */
static uint32_t full_box(br *b, uint32_t *flags)
{
    uint32_t v = br_u8(b);
    uint32_t f = br_u24(b);
    if (flags) *flags = f;
    return v;
}

/* ------------------------------------------------------- sample tables --- */
typedef struct {
    const uint8_t *stts; uint32_t stts_n;
    const uint8_t *ctts; uint32_t ctts_n; int ctts_signed;
    const uint8_t *stsc; uint32_t stsc_n;
    const uint8_t *stsz; uint32_t stsz_n; uint32_t stsz_const; int stsz_field;
    const uint8_t *stco; uint32_t stco_n; int co64;
    const uint8_t *stss; uint32_t stss_n;
} stbl_t;

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

/* stsz's variant stz2 packs sizes at 4, 8 or 16 bits. Reading it through one
 * accessor keeps the index loop from growing a second copy of itself. */
static uint32_t stsz_at(const stbl_t *s, uint32_t i)
{
    if (s->stsz_const) return s->stsz_const;
    if (i >= s->stsz_n) return 0;
    switch (s->stsz_field) {
    case 4:  { uint8_t byte = s->stsz[i >> 1];
               return (i & 1) ? (byte & 0x0F) : (byte >> 4); }
    case 8:  return s->stsz[i];
    case 16: return ((uint32_t)s->stsz[i * 2] << 8) | s->stsz[i * 2 + 1];
    default: return rd32(s->stsz + (size_t)i * 4);
    }
}

/* Build the flat index from the six tables. This is the heart of the
 * progressive path and everything about it is run-length: cursors advance
 * together, and the only random access is stsz (by sample) and stco (by
 * chunk), both of which were length-checked when the table was accepted. */
static int build_index(mdemux *m, mtrack *t, const stbl_t *s, long long shift)
{
    uint32_t total = s->stsz_const ? 0 : s->stsz_n;
    if (s->stsz_const) {
        /* With a constant size, stsz still carries the sample count. */
        total = s->stsz_n;
    }
    if (total == 0 || s->stsc_n == 0 || s->stco_n == 0) return MEDIA_OK; /* empty track */
    if (total > MEDIA_MAX_SAMPLES) return MEDIA_ERR_RANGE;

    /* UNCOMPRESSED AUDIO IS CHUNK-BASED, not sample-based. In a PCM track a
     * "sample" is one frame of two bytes, so a two-second stereo file has
     * 88,200 of them -- and an index with 88,200 entries, each pointing two
     * bytes further into the same chunk, is not a sample table, it is a
     * spreadsheet. The container's own grouping is the chunk, every player
     * reads it that way, and so does ffmpeg (which is why its packet count for
     * a .mov is the chunk count). Detect it exactly: a constant sample size,
     * an audio track, and one stts run whose duration is a single tick. */
    int chunky = (t->t.type == MEDIA_TRACK_AUDIO) && s->stsz_const &&
                 s->stts_n == 1 && rd32(s->stts + 4) == 1;

    uint32_t sample = 0;
    long long dts = 0;
    uint32_t stts_i = 0, stts_left = 0; uint32_t stts_delta = 0;
    uint32_t ctts_i = 0, ctts_left = 0; int64_t ctts_off = 0;
    uint32_t stsc_i = 0;
    uint32_t stss_i = 0;

    for (uint32_t chunk = 0; chunk < s->stco_n && sample < total; chunk++) {
        while (stsc_i + 1 < s->stsc_n &&
               chunk + 1 >= rd32(s->stsc + (size_t)(stsc_i + 1) * 12))
            stsc_i++;
        uint32_t spc = rd32(s->stsc + (size_t)stsc_i * 12 + 4);
        if (spc == 0 || spc > total) return MEDIA_ERR_CORRUPT;

        long long off = s->co64 ? (long long)rd64(s->stco + (size_t)chunk * 8)
                                : (long long)rd32(s->stco + (size_t)chunk * 4);

        if (chunky) {
            /* One chunk of PCM can be the whole file, so it is cut into
             * packets rather than handed over whole. 1024 frames is ffmpeg's
             * split and therefore what makes the differential comparable; it
             * is also about 23 ms at 44.1 kHz, which is a sensible unit to
             * hand the sound ring. */
            uint32_t remaining = spc;
            if (remaining > total - sample) remaining = total - sample;
            while (remaining > 0) {
                uint32_t take = remaining > 1024 ? 1024 : remaining;
                long long bytes = (long long)take * s->stsz_const;
                if (off < 0 || off > m->len || bytes > m->len - off)
                    return MEDIA_ERR_CORRUPT;
                int rc = md_push(t, dts - shift, dts - shift, off, (long)bytes, 1);
                if (rc != MEDIA_OK) return rc;
                off += bytes;
                dts += take;                /* one tick per frame, by definition */
                sample += take;
                remaining -= take;
            }
            continue;
        }

        for (uint32_t k = 0; k < spc && sample < total; k++) {
            if (stts_left == 0) {
                if (stts_i >= s->stts_n) {
                    /* Ran out of duration runs with samples left. The stream
                     * is inconsistent; the last delta is the least wrong
                     * answer and is what every player does. */
                    stts_left = total - sample;
                } else {
                    stts_left = rd32(s->stts + (size_t)stts_i * 8);
                    stts_delta = rd32(s->stts + (size_t)stts_i * 8 + 4);
                    stts_i++;
                    if (stts_left == 0) continue;
                }
            }
            if (s->ctts_n) {
                while (ctts_left == 0 && ctts_i < s->ctts_n) {
                    ctts_left = rd32(s->ctts + (size_t)ctts_i * 8);
                    uint32_t raw = rd32(s->ctts + (size_t)ctts_i * 8 + 4);
                    ctts_off = s->ctts_signed ? (int64_t)(int32_t)raw : (int64_t)raw;
                    ctts_i++;
                }
                if (ctts_left == 0) ctts_off = 0;
            }

            uint32_t size = stsz_at(s, sample);
            if (off < 0 || off > m->len || (long long)size > m->len - off)
                return MEDIA_ERR_CORRUPT;

            int key = 1;
            if (s->stss) {
                key = 0;
                while (stss_i < s->stss_n &&
                       rd32(s->stss + (size_t)stss_i * 4) < sample + 1)
                    stss_i++;
                if (stss_i < s->stss_n &&
                    rd32(s->stss + (size_t)stss_i * 4) == sample + 1) key = 1;
            }

#ifdef DEMUX_CONTROL_NO_CTTS
            /* THE NEGATIVE CONTROL, and a real bug: ignore the composition
             * offsets, so presentation time is taken to equal decode time.
             * This is invisible on every stream without B frames -- which is
             * most test material -- and on a stream WITH them it plays the
             * pictures in the wrong order at the wrong times. A suite that
             * cannot tell the difference is not testing timestamps.
             * make test-demux-negctl REQUIRES the differential to fail here. */
            ctts_off = 0;
#endif
            int rc = md_push(t, dts - shift, dts - shift + ctts_off, off, (long)size, key);
            if (rc != MEDIA_OK) return rc;

            off += size;
            dts += stts_delta;
            if (stts_left) stts_left--;
            if (ctts_left) ctts_left--;
            sample++;
        }
    }
    return MEDIA_OK;
}

/* ------------------------------------------------------------- stsd ------ */
static void parse_esds(br *b, mtrack *t)
{
    /* ES_Descriptor. Descriptor lengths are 7 bits per byte with a
     * continuation bit -- and a file can encode the same length in one to four
     * bytes, so the reader must accept all of them. */
    full_box(b, 0);
    for (int guard = 0; guard < 8 && br_ok(b) && br_left(b) > 0; guard++) {
        uint32_t tag = br_u8(b);
        uint32_t len = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t c = br_u8(b);
            len = (len << 7) | (c & 0x7F);
            if (!(c & 0x80)) break;
        }
        if (!br_ok(b)) return;
        if (len > (uint32_t)br_left(b)) return;

        if (tag == 0x03) {                    /* ES_Descriptor: skip its header */
            br_u16(b);                        /* ES_ID */
            uint32_t fl = br_u8(b);
            if (fl & 0x80) br_u16(b);         /* dependsOn_ES_ID */
            if (fl & 0x40) { uint32_t n = br_u8(b); br_skip(b, (long)n); }
            if (fl & 0x20) br_u16(b);         /* OCR_ES_Id */
            continue;                         /* and fall into its children */
        }
        if (tag == 0x04) {                    /* DecoderConfigDescriptor */
            uint32_t oti = br_u8(b);
            br_u8(b); br_u24(b); br_u32(b); br_u32(b);
            switch (oti) {
            case 0x40: case 0x66: case 0x67: case 0x68: t->t.codec = MEDIA_CODEC_AAC; break;
            case 0x69: case 0x6B:                       t->t.codec = MEDIA_CODEC_MP3; break;
            case 0x20:                                  t->t.codec = MEDIA_CODEC_MPEG4; break;
            case 0x21:                                  t->t.codec = MEDIA_CODEC_H264; break;
            case 0xA5:                                  t->t.codec = MEDIA_CODEC_AC3; break;
            default: break;
            }
            continue;                         /* children follow */
        }
        if (tag == 0x05) {                    /* DecoderSpecificInfo */
            const uint8_t *p = br_bytes(b, (long)len);
            if (p && len <= MEDIA_MAX_EXTRADATA) {
                t->t.extradata = p;
                t->t.extradata_len = (int)len;
            }
            return;
        }
        br_skip(b, (long)len);                /* SL config and the rest */
    }
}

static void parse_sample_entry_children(br *b, mtrack *t)
{
    mbox c;
    while (next_box(b, &c)) {
        switch (c.type) {
        case FOURCC('a','v','c','C'):
        case FOURCC('h','v','c','C'):
        case FOURCC('v','p','c','C'):
        case FOURCC('a','v','1','C'):
        case FOURCC('d','O','p','s'): {
            long n = br_left(&c.body);
            const uint8_t *p = br_bytes(&c.body, n);
            if (p && n <= MEDIA_MAX_EXTRADATA) { t->t.extradata = p; t->t.extradata_len = (int)n; }
            break;
        }
        case FOURCC('d','f','L','a'): {       /* FLAC: metadata blocks after a full box */
            full_box(&c.body, 0);
            long n = br_left(&c.body);
            const uint8_t *p = br_bytes(&c.body, n);
            if (p && n <= MEDIA_MAX_EXTRADATA) { t->t.extradata = p; t->t.extradata_len = (int)n; }
            break;
        }
        case FOURCC('e','s','d','s'):
            parse_esds(&c.body, t);
            break;
        case FOURCC('w','a','v','e'): {       /* QuickTime wraps esds in here */
            mbox w;
            while (next_box(&c.body, &w))
                if (w.type == FOURCC('e','s','d','s')) parse_esds(&w.body, t);
            break;
        }
        default: break;
        }
    }
}

static void parse_stsd(br *b, mtrack *t)
{
    full_box(b, 0);
    uint32_t n = br_u32(b);
    if (!br_ok(b) || n == 0) return;
    mbox e;
    if (!next_box(b, &e)) return;             /* the first entry is the one we use */

    switch (e.type) {
    case FOURCC('a','v','c','1'):
    case FOURCC('a','v','c','2'):
    case FOURCC('a','v','c','3'):
    case FOURCC('a','v','c','4'): t->t.codec = MEDIA_CODEC_H264; break;
    case FOURCC('h','e','v','1'):
    case FOURCC('h','v','c','1'): t->t.codec = MEDIA_CODEC_H265; break;
    case FOURCC('v','p','0','8'): t->t.codec = MEDIA_CODEC_VP8; break;
    case FOURCC('v','p','0','9'): t->t.codec = MEDIA_CODEC_VP9; break;
    case FOURCC('a','v','0','1'): t->t.codec = MEDIA_CODEC_AV1; break;
    case FOURCC('m','p','4','v'): t->t.codec = MEDIA_CODEC_MPEG4; break;
    case FOURCC('f','L','a','C'): t->t.codec = MEDIA_CODEC_FLAC; break;
    case FOURCC('O','p','u','s'): t->t.codec = MEDIA_CODEC_OPUS; break;
    case FOURCC('a','c','-','3'): t->t.codec = MEDIA_CODEC_AC3; break;
    case FOURCC('s','o','w','t'): t->t.codec = MEDIA_CODEC_PCM_S16LE; break;
    case FOURCC('t','w','o','s'): t->t.codec = MEDIA_CODEC_PCM_S16BE; break;
    case FOURCC('.','m','p','3'): t->t.codec = MEDIA_CODEC_MP3; break;
    default: break;                            /* mp4a resolves through esds */
    }

    br_skip(&e.body, 6);                       /* reserved */
    br_u16(&e.body);                           /* data_reference_index */

    if (t->t.type == MEDIA_TRACK_VIDEO) {
        br_skip(&e.body, 16);                  /* pre_defined + reserved */
        uint32_t w = br_u16(&e.body), h = br_u16(&e.body);
        if (w && h && w <= MEDIA_MAX_DIM && h <= MEDIA_MAX_DIM) {
            t->t.width = (int)w; t->t.height = (int)h;
        }
        br_skip(&e.body, 4 + 4 + 4);           /* resolutions + reserved */
        br_u16(&e.body);                       /* frame_count */
        br_skip(&e.body, 32);                  /* compressorname */
        br_u16(&e.body);                       /* depth */
        br_u16(&e.body);                       /* pre_defined (-1) */
    } else if (t->t.type == MEDIA_TRACK_AUDIO) {
        uint32_t ver = br_u16(&e.body);
        br_u16(&e.body); br_u32(&e.body);      /* revision, vendor */
        uint32_t ch = br_u16(&e.body);
        uint32_t bits = br_u16(&e.body);
        br_u16(&e.body); br_u16(&e.body);      /* pre_defined, reserved */
        uint32_t sr = br_u32(&e.body) >> 16;   /* 16.16 fixed point */
        if (ch >= 1 && ch <= 8) t->t.channels = (int)ch;
        if (bits) t->t.bits = (int)bits;
        if (sr) t->t.rate = (int)sr;
        /* QuickTime's v1 and v2 sound descriptions carry extra fields before
         * the child boxes; skipping the wrong number lands mid-box. */
        if (ver == 1) br_skip(&e.body, 16);
        else if (ver == 2) {
            br_skip(&e.body, 4);               /* sizeOfStructOnly */
            br_skip(&e.body, 8);               /* audioSampleRate (double) */
            uint32_t ch2 = br_u32(&e.body);
            if (ch2 >= 1 && ch2 <= 8) t->t.channels = (int)ch2;
            br_skip(&e.body, 4 + 4 + 4 + 4);
        }
    } else {
        return;
    }
    parse_sample_entry_children(&e.body, t);
}

/* ------------------------------------------------------------- trak ------ */
/* Returns the dts shift the edit list asks for, in media timescale ticks. */
static long long parse_elst(br *b, unsigned media_ts, unsigned movie_ts)
{
    uint32_t flags;
    uint32_t ver = full_box(b, &flags);
    uint32_t n = br_u32(b);
    long long empty = 0;                       /* movie timescale */
    for (uint32_t i = 0; i < n && br_ok(b); i++) {
        long long dur, mt;
        if (ver == 1) { dur = (long long)br_u64(b); mt = (long long)(int64_t)br_u64(b); }
        else          { dur = (long long)br_u32(b); mt = (long long)(int32_t)br_u32(b); }
        br_u32(b);                             /* media_rate */
        if (!br_ok(b)) break;
        if (mt < 0) { empty += dur; continue; }/* a leading empty edit = a delay */
        /* The first real edit sets the start; later ones are a cut list this
         * player does not honour, and pretending otherwise would be worse. */
        long long empty_media = movie_ts ? empty * (long long)media_ts / (long long)movie_ts : 0;
        return mt - empty_media;
    }
    return movie_ts ? -(empty * (long long)media_ts / (long long)movie_ts) : 0;
}

static int parse_trak(mdemux *m, br *b)
{
    mtrack *t = md_add_track(m);
    if (!t) return MEDIA_ERR_RANGE;
    t->t.timescale = m->movie_timescale;

    stbl_t s;
    memset(&s, 0, sizeof s);
    s.stsz_field = 32;
    long long shift = 0;
    int have_elst = 0;
    br elst_body; br_init(&elst_body, 0, 0, 0);

    mbox tb;
    while (next_box(b, &tb)) {
        if (tb.type == FOURCC('t','k','h','d')) {
            uint32_t ver = full_box(&tb.body, 0);
            if (ver == 1) { br_u64(&tb.body); br_u64(&tb.body); }
            else          { br_u32(&tb.body); br_u32(&tb.body); }
            t->t.id = (int)br_u32(&tb.body);
        } else if (tb.type == FOURCC('e','d','t','s')) {
            mbox eb;
            while (next_box(&tb.body, &eb))
                if (eb.type == FOURCC('e','l','s','t')) { elst_body = eb.body; have_elst = 1; }
        } else if (tb.type == FOURCC('m','d','i','a')) {
            mbox mb;
            while (next_box(&tb.body, &mb)) {
                if (mb.type == FOURCC('m','d','h','d')) {
                    uint32_t ver = full_box(&mb.body, 0);
                    if (ver == 1) { br_u64(&mb.body); br_u64(&mb.body);
                                    t->t.timescale = br_u32(&mb.body);
                                    t->t.duration = (long long)br_u64(&mb.body); }
                    else          { br_u32(&mb.body); br_u32(&mb.body);
                                    t->t.timescale = br_u32(&mb.body);
                                    t->t.duration = (long long)br_u32(&mb.body); }
                    if (t->t.timescale == 0) t->t.timescale = 1000;
                } else if (mb.type == FOURCC('h','d','l','r')) {
                    full_box(&mb.body, 0);
                    br_u32(&mb.body);          /* pre_defined */
                    uint32_t h = br_u32(&mb.body);
                    if (h == FOURCC('v','i','d','e')) t->t.type = MEDIA_TRACK_VIDEO;
                    else if (h == FOURCC('s','o','u','n')) t->t.type = MEDIA_TRACK_AUDIO;
                    else t->t.type = MEDIA_TRACK_OTHER;
                } else if (mb.type == FOURCC('m','i','n','f')) {
                    mbox nb;
                    while (next_box(&mb.body, &nb)) {
                        if (nb.type != FOURCC('s','t','b','l')) continue;
                        mbox sb;
                        while (next_box(&nb.body, &sb)) {
                            switch (sb.type) {
                            case FOURCC('s','t','s','d'):
                                parse_stsd(&sb.body, t); break;
                            case FOURCC('s','t','t','s'): {
                                full_box(&sb.body, 0);
                                uint32_t n = br_u32(&sb.body);
                                const uint8_t *p = br_bytes(&sb.body, (long)n * 8);
                                if (p) { s.stts = p; s.stts_n = n; }
                                break; }
                            case FOURCC('c','t','t','s'): {
                                uint32_t ver = full_box(&sb.body, 0);
                                uint32_t n = br_u32(&sb.body);
                                const uint8_t *p = br_bytes(&sb.body, (long)n * 8);
                                if (p) { s.ctts = p; s.ctts_n = n; s.ctts_signed = (ver == 1); }
                                break; }
                            case FOURCC('s','t','s','c'): {
                                full_box(&sb.body, 0);
                                uint32_t n = br_u32(&sb.body);
                                const uint8_t *p = br_bytes(&sb.body, (long)n * 12);
                                if (p) { s.stsc = p; s.stsc_n = n; }
                                break; }
                            case FOURCC('s','t','s','z'): {
                                full_box(&sb.body, 0);
                                uint32_t cs = br_u32(&sb.body);
                                uint32_t n = br_u32(&sb.body);
                                s.stsz_n = n; s.stsz_const = cs; s.stsz_field = 32;
                                if (!cs) {
                                    const uint8_t *p = br_bytes(&sb.body, (long)n * 4);
                                    if (p) s.stsz = p; else s.stsz_n = 0;
                                }
                                break; }
                            case FOURCC('s','t','z','2'): {
                                full_box(&sb.body, 0);
                                br_u24(&sb.body);
                                uint32_t fs = br_u8(&sb.body);
                                uint32_t n = br_u32(&sb.body);
                                long bytes = (fs == 4) ? (long)((n + 1) / 2)
                                           : (fs == 8) ? (long)n
                                           : (fs == 16) ? (long)n * 2 : -1;
                                if (bytes < 0) break;
                                const uint8_t *p = br_bytes(&sb.body, bytes);
                                if (p) { s.stsz = p; s.stsz_n = n; s.stsz_const = 0;
                                         s.stsz_field = (int)fs; }
                                break; }
                            case FOURCC('s','t','c','o'): {
                                full_box(&sb.body, 0);
                                uint32_t n = br_u32(&sb.body);
                                const uint8_t *p = br_bytes(&sb.body, (long)n * 4);
                                if (p) { s.stco = p; s.stco_n = n; s.co64 = 0; }
                                break; }
                            case FOURCC('c','o','6','4'): {
                                full_box(&sb.body, 0);
                                uint32_t n = br_u32(&sb.body);
                                const uint8_t *p = br_bytes(&sb.body, (long)n * 8);
                                if (p) { s.stco = p; s.stco_n = n; s.co64 = 1; }
                                break; }
                            case FOURCC('s','t','s','s'): {
                                full_box(&sb.body, 0);
                                uint32_t n = br_u32(&sb.body);
                                const uint8_t *p = br_bytes(&sb.body, (long)n * 4);
                                if (p) { s.stss = p; s.stss_n = n; }
                                break; }
                            default: break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (have_elst) shift = parse_elst(&elst_body, t->t.timescale, m->movie_timescale);
    return build_index(m, t, &s, shift);
}

/* --------------------------------------------------------- fragments ----- */
typedef struct {
    uint32_t track_id;
    uint32_t duration, size, flags;
} trex_t;

#define TFHD_BASE_OFFSET   0x000001
#define TFHD_STSD_INDEX    0x000002
#define TFHD_DEF_DURATION  0x000008
#define TFHD_DEF_SIZE      0x000010
#define TFHD_DEF_FLAGS     0x000020
#define TFHD_BASE_IS_MOOF  0x020000

#define TRUN_DATA_OFFSET   0x000001
#define TRUN_FIRST_FLAGS   0x000004
#define TRUN_SAMPLE_DUR    0x000100
#define TRUN_SAMPLE_SIZE   0x000200
#define TRUN_SAMPLE_FLAGS  0x000400
#define TRUN_SAMPLE_CTS    0x000800

/* ffmpeg's rule, and the right one: a sample is a sync point unless it says it
 * is not, or says something depends on it having a predecessor. */
#define SF_NON_SYNC    0x00010000u
#define SF_DEPENDS_YES 0x01000000u

static mtrack *track_by_id(mdemux *m, uint32_t id)
{
    for (int i = 0; i < m->ntracks; i++)
        if ((uint32_t)m->tr[i].t.id == id) return &m->tr[i];
    return 0;
}

static int parse_traf(mdemux *m, br *b, long long moof_off,
                      const trex_t *trex, int ntrex, long long *run_dts)
{
    uint32_t track_id = 0, dflags = 0;
    uint32_t ddur = 0, dsize = 0;
    long long base = moof_off;
    int have_base = 0;
    long long dts = 0;
    int have_tfdt = 0;

    /* tfhd must be read before any trun, and the spec puts it first. Two
     * passes rather than trusting that: read the header boxes, then the runs.
     * A file that reversed them would otherwise index at offset zero. */
    br scan = *b;
    mbox fb;
    while (next_box(&scan, &fb)) {
        if (fb.type == FOURCC('t','f','h','d')) {
            uint32_t fl;
            full_box(&fb.body, &fl);
            track_id = br_u32(&fb.body);
            if (fl & TFHD_BASE_OFFSET) { base = (long long)br_u64(&fb.body); have_base = 1; }
            if (fl & TFHD_STSD_INDEX)  br_u32(&fb.body);
            if (fl & TFHD_DEF_DURATION) ddur = br_u32(&fb.body);
            if (fl & TFHD_DEF_SIZE)     dsize = br_u32(&fb.body);
            if (fl & TFHD_DEF_FLAGS)    dflags = br_u32(&fb.body);
            if (!have_base && (fl & TFHD_BASE_IS_MOOF)) base = moof_off;
            if (!br_ok(&fb.body)) return MEDIA_ERR_CORRUPT;
        } else if (fb.type == FOURCC('t','f','d','t')) {
            uint32_t ver = full_box(&fb.body, 0);
            dts = (ver == 1) ? (long long)br_u64(&fb.body) : (long long)br_u32(&fb.body);
            have_tfdt = 1;
        }
    }
    if (!track_id) return MEDIA_OK;            /* a traf for nothing: ignore it */

    mtrack *t = track_by_id(m, track_id);
    if (!t) return MEDIA_OK;                   /* a track the moov never declared */

    for (int i = 0; i < ntrex; i++) {
        if (trex[i].track_id != track_id) continue;
        if (!ddur)   ddur = trex[i].duration;
        if (!dsize)  dsize = trex[i].size;
        if (!dflags) dflags = trex[i].flags;
    }

    int ti = t->t.index;
    if (!have_tfdt) dts = run_dts[ti];         /* continue where the last fragment ended */

    long long data_off = base;
    mbox rb;
    while (next_box(b, &rb)) {
        if (rb.type != FOURCC('t','r','u','n')) continue;
        uint32_t fl;
        uint32_t ver = full_box(&rb.body, &fl);
        uint32_t cnt = br_u32(&rb.body);
        if (!br_ok(&rb.body)) return MEDIA_ERR_CORRUPT;
        if (cnt > MEDIA_MAX_SAMPLES) return MEDIA_ERR_RANGE;
        if (fl & TRUN_DATA_OFFSET) data_off = base + (long long)(int32_t)br_u32(&rb.body);
        uint32_t first_flags = dflags;
        if (fl & TRUN_FIRST_FLAGS) first_flags = br_u32(&rb.body);
        if (!br_ok(&rb.body)) return MEDIA_ERR_CORRUPT;

        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t dur = ddur, size = dsize, sfl = (i == 0) ? first_flags : dflags;
            int64_t cts = 0;
            if (fl & TRUN_SAMPLE_DUR)   dur = br_u32(&rb.body);
            if (fl & TRUN_SAMPLE_SIZE)  size = br_u32(&rb.body);
            if (fl & TRUN_SAMPLE_FLAGS) sfl = br_u32(&rb.body);
            if (fl & TRUN_SAMPLE_CTS) {
                uint32_t raw = br_u32(&rb.body);
                cts = (ver >= 1) ? (int64_t)(int32_t)raw : (int64_t)raw;
            }
            if (!br_ok(&rb.body)) return MEDIA_ERR_CORRUPT;
            if (data_off < 0 || data_off > m->len || (long long)size > m->len - data_off)
                return MEDIA_ERR_CORRUPT;

            int key = !(sfl & (SF_NON_SYNC | SF_DEPENDS_YES));
            int rc = md_push(t, dts, dts + cts, data_off, (long)size, key);
            if (rc != MEDIA_OK) return rc;
            data_off += size;
            dts += dur;
        }
    }
    run_dts[ti] = dts;
    return MEDIA_OK;
}

/* ------------------------------------------------------------- top ------- */
int mp4_parse(mdemux *m)
{
    br top;
    br_init(&top, m->data, m->len, 0);

    trex_t trex[MEDIA_MAX_TRACKS];
    int ntrex = 0;
    long long run_dts[MEDIA_MAX_TRACKS];
    for (int i = 0; i < MEDIA_MAX_TRACKS; i++) run_dts[i] = 0;

    int saw_moov = 0;
    mbox b;
    while (next_box(&top, &b)) {
        if (b.type == FOURCC('m','o','o','v')) {
            saw_moov = 1;
            mbox mb;
            while (next_box(&b.body, &mb)) {
                if (mb.type == FOURCC('m','v','h','d')) {
                    uint32_t ver = full_box(&mb.body, 0);
                    if (ver == 1) { br_u64(&mb.body); br_u64(&mb.body);
                                    m->movie_timescale = br_u32(&mb.body);
                                    m->movie_duration = (long long)br_u64(&mb.body); }
                    else          { br_u32(&mb.body); br_u32(&mb.body);
                                    m->movie_timescale = br_u32(&mb.body);
                                    m->movie_duration = (long long)br_u32(&mb.body); }
                    if (!m->movie_timescale) m->movie_timescale = 1000;
                    if (m->movie_duration == 0 || m->movie_duration == 0xFFFFFFFFLL)
                        m->movie_duration = -1;
                } else if (mb.type == FOURCC('t','r','a','k')) {
                    int rc = parse_trak(m, &mb.body);
                    if (rc != MEDIA_OK) return rc;
                } else if (mb.type == FOURCC('m','v','e','x')) {
                    mbox xb;
                    while (next_box(&mb.body, &xb)) {
                        if (xb.type == FOURCC('m','e','h','d')) {
                            uint32_t ver = full_box(&xb.body, 0);
                            long long d = (ver == 1) ? (long long)br_u64(&xb.body)
                                                     : (long long)br_u32(&xb.body);
                            if (d > 0 && m->movie_duration <= 0) m->movie_duration = d;
                        } else if (xb.type == FOURCC('t','r','e','x') &&
                                   ntrex < MEDIA_MAX_TRACKS) {
                            full_box(&xb.body, 0);
                            trex_t *x = &trex[ntrex];
                            x->track_id = br_u32(&xb.body);
                            br_u32(&xb.body);           /* default_sample_description_index */
                            x->duration = br_u32(&xb.body);
                            x->size = br_u32(&xb.body);
                            x->flags = br_u32(&xb.body);
                            if (br_ok(&xb.body)) ntrex++;
                        }
                    }
                }
            }
        } else if (b.type == FOURCC('m','o','o','f')) {
            /* A fragment can precede the moov in a live stream, but our tracks
             * come from the moov, so a moof before it has nothing to attach
             * to. Real fragmented files carry an init segment first. */
            m->fragmented = 1;
            long long moof_off = b.hdr_at;
            mbox fb;
            while (next_box(&b.body, &fb)) {
                if (fb.type != FOURCC('t','r','a','f')) continue;
                int rc = parse_traf(m, &fb.body, moof_off, trex, ntrex, run_dts);
                if (rc != MEDIA_OK) return rc;
            }
        }
    }
    if (!saw_moov) return MEDIA_ERR_CORRUPT;
    return MEDIA_OK;
}
