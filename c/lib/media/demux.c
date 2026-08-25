/* c/lib/media/demux.c -- container sniffing, the demuxer object, sample
 * ordering, seeking, and the Annex B rewrite.
 *
 * Everything format-specific lives in mp4.c and mkv.c; this file is what they
 * have in common and what a player talks to.
 */
#include <stdlib.h>
#include <string.h>
#include "media_int.h"
#include "avi.h"
#include "ts.h"
#include "ps.h"
#include "flv.h"

/* ------------------------------------------------------------- sniff ----- */
/* By content. A file called .mp4 that is really Matroska opens as Matroska,
 * and a .bin that is really an MP4 opens too -- the same rule audio_sniff()
 * and Preview's Annex B check follow. */
media_container media_sniff(const uint8_t *d, long n)
{
    if (!d || n < 8) return MEDIA_CONT_UNKNOWN;

    /* EBML: every Matroska and WebM file starts with the EBML header id. */
    if (d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3)
        return MEDIA_CONT_MKV;

    /* ISO-BMFF: a top-level box whose type is one of the ones that can lead a
     * file. `ftyp` normally does; `moov`/`moof`/`styp`/`mdat`/`free`/`skip`
     * can, and a fragment pulled off a DASH server often starts at `styp` or
     * straight at `moof`. Walk at most a few boxes so a stray "ftyp" deep in
     * some other file's payload cannot claim it. */
    {
        long off = 0;
        for (int i = 0; i < 4 && off + 8 <= n; i++) {
            uint64_t sz = ((uint64_t)d[off] << 24) | ((uint64_t)d[off + 1] << 16)
                        | ((uint64_t)d[off + 2] << 8) | d[off + 3];
            const uint8_t *ty = d + off + 4;
            long hdr = 8;
            if (sz == 1) {
                if (off + 16 > n) break;
                sz = 0;
                for (int k = 0; k < 8; k++) sz = (sz << 8) | d[off + 8 + k];
                hdr = 16;
            } else if (sz == 0) {
                sz = (uint64_t)(n - off);
            }
            if (!memcmp(ty, "ftyp", 4) || !memcmp(ty, "styp", 4) ||
                !memcmp(ty, "moov", 4) || !memcmp(ty, "moof", 4))
                return MEDIA_CONT_MP4;
            /* Leading padding/media boxes: keep walking, but only forward. */
            if (memcmp(ty, "free", 4) && memcmp(ty, "skip", 4) &&
                memcmp(ty, "wide", 4) && memcmp(ty, "mdat", 4) &&
                memcmp(ty, "pnot", 4))
                break;
            if (sz < (uint64_t)hdr || sz > (uint64_t)(n - off)) break;
            off += (long)sz;
        }
    }

    /* The four formats added alongside this dispatch (avi.c/ts.c/ps.c/
     * flv.c). Their magic bytes are mutually exclusive with each other, with
     * EBML and with an ISO-BMFF leading box -- container_test.c's own
     * test_sniffs_disjoint() proves the four are pairwise exclusive on real
     * magic, which this order relies on rather than re-deriving. */
    if (avi_sniff(d, n)) return MEDIA_CONT_AVI;
    if (ts_sniff(d, n))  return MEDIA_CONT_TS;
    if (ps_sniff(d, n))  return MEDIA_CONT_PS;
    if (flv_sniff(d, n)) return MEDIA_CONT_FLV;

    return MEDIA_CONT_UNKNOWN;
}

const char *media_container_name(media_container c)
{
    switch (c) {
    case MEDIA_CONT_MP4: return "mp4";
    case MEDIA_CONT_MKV: return "matroska";
    case MEDIA_CONT_AVI: return "avi";
    case MEDIA_CONT_TS:  return "mpegts";
    case MEDIA_CONT_PS:  return "mpeg";
    case MEDIA_CONT_FLV: return "flv";
    default:             return "unknown";
    }
}

const char *media_codec_name(media_codec c)
{
    switch (c) {
    case MEDIA_CODEC_H264:      return "h264";
    case MEDIA_CODEC_H265:      return "hevc";
    case MEDIA_CODEC_VP8:       return "vp8";
    case MEDIA_CODEC_VP9:       return "vp9";
    case MEDIA_CODEC_AV1:       return "av1";
    case MEDIA_CODEC_MPEG4:     return "mpeg4";
    case MEDIA_CODEC_AAC:       return "aac";
    case MEDIA_CODEC_MP3:       return "mp3";
    case MEDIA_CODEC_FLAC:      return "flac";
    case MEDIA_CODEC_PCM_S16LE: return "pcm_s16le";
    case MEDIA_CODEC_PCM_S16BE: return "pcm_s16be";
    case MEDIA_CODEC_OPUS:      return "opus";
    case MEDIA_CODEC_VORBIS:    return "vorbis";
    case MEDIA_CODEC_AC3:       return "ac3";
    default:                    return "unknown";
    }
}

const char *media_strerror(int e)
{
    switch (e) {
    case MEDIA_OK:               return "ok";
    case MEDIA_ERR_CORRUPT:      return "corrupt";
    case MEDIA_ERR_UNSUPPORTED:  return "unsupported";
    case MEDIA_ERR_OOM:          return "out of memory";
    case MEDIA_ERR_RANGE:        return "out of range";
    default:                     return "error";
    }
}

/* --------------------------------------------------------- index build --- */
mtrack *md_add_track(mdemux *m)
{
    if (m->ntracks >= MEDIA_MAX_TRACKS) return 0;
    mtrack *t = &m->tr[m->ntracks];
    memset(t, 0, sizeof *t);
    t->t.index = m->ntracks;
    t->t.duration = -1;
    t->t.codec_name = "unknown";
    t->t.nal_length_size = 4;
    m->ntracks++;
    return t;
}

int md_push(mtrack *t, long long dts, long long pts, long long off,
            long size, int key)
{
    if (size < 0 || off < 0) return MEDIA_ERR_CORRUPT;
    if (t->n >= MEDIA_MAX_SAMPLES) return MEDIA_ERR_RANGE;
    if (t->n == t->cap) {
        long nc = t->cap ? t->cap * 2 : 256;
        if (nc > MEDIA_MAX_SAMPLES) nc = MEDIA_MAX_SAMPLES;
        msample *ns = (msample *)realloc(t->s, (size_t)nc * sizeof *ns);
        if (!ns) return MEDIA_ERR_OOM;
        t->s = ns; t->cap = nc;
    }
    msample *s = &t->s[t->n++];
    s->dts = dts; s->pts = pts;
    s->off = (uint32_t)(off & 0xFFFFFFFFu);
    s->off_hi = (uint32_t)((unsigned long long)off >> 32);
    s->size = (uint32_t)size;
    s->flags = key ? MS_KEY : 0u;
    return MEDIA_OK;
}

static long long ms_off(const msample *s)
{
    return ((long long)s->off_hi << 32) | (long long)s->off;
}

/* ticks -> nanoseconds, exactly, without a 64-bit overflow on long files and
 * without floating point. Split the tick count into whole seconds and a
 * remainder first: (ticks/ts)*1e9 + (ticks%ts)*1e9/ts. The remainder is under
 * `ts`, so the second product cannot overflow for any timescale below about
 * 9.2e9, which is every real one (the largest in the wild is 90 kHz). */
long long md_ticks_to_ns(long long ticks, unsigned timescale)
{
    if (!timescale) return 0;
    long long ts = (long long)timescale;
    long long whole = ticks / ts, rem = ticks % ts;
    /* SATURATE rather than overflow. A tick count is a number a stranger put
     * in the file, and 2^63 nanoseconds is only 292 years, so a corrupt or
     * hostile timestamp of a few hundred billion "seconds" overflows the
     * multiply -- which is undefined behaviour, not merely a wrong answer.
     * (This is not hypothetical: the fuzzer found it, at scale 40, in a
     * mutated stts run.) Everything downstream compares timestamps, so a
     * saturated one sorts to the end and is never dereferenced. */
    if (whole >  9223372035LL) return  0x7FFFFFFFFFFFFFFFLL;
    if (whole < -9223372035LL) return -0x7FFFFFFFFFFFFFFFLL - 1;
    return whole * 1000000000LL + rem * 1000000000LL / ts;
}

/* Codec name + framing, once the format parser has set codec/extradata. */
void md_finish_track(mtrack *t)
{
    t->t.codec_name = media_codec_name(t->t.codec);
    if (t->t.codec == MEDIA_CODEC_H264 || t->t.codec == MEDIA_CODEC_H265) {
        /* Both containers store these length-prefixed with the parameter sets
         * in the sample description. The one exception is a Matroska track
         * whose CodecPrivate is empty, which some muxers emit for Annex B in
         * the blocks -- detect it rather than trusting the CodecID. */
        if (t->t.extradata_len >= 5 && t->t.extradata[0] == 1) {
            t->t.framing = MEDIA_FRAMING_AVCC;
            if (t->t.codec == MEDIA_CODEC_H264)
                t->t.nal_length_size = (t->t.extradata[4] & 3) + 1;
            else if (t->t.extradata_len >= 23)
                t->t.nal_length_size = (t->t.extradata[21] & 3) + 1;
        } else {
            t->t.framing = MEDIA_FRAMING_RAW;
            t->t.nal_length_size = 0;
        }
    } else {
        t->t.framing = MEDIA_FRAMING_RAW;
        t->t.nal_length_size = 0;
    }
    t->t.nsamples = t->n;
}

/* --------------------------------------------------------- open/close --- */
mdemux *media_open(const uint8_t *data, long len, int *err)
{
    int e = MEDIA_OK;
    if (err) *err = MEDIA_OK;
    if (!data || len <= 0) { if (err) *err = MEDIA_ERR_CORRUPT; return 0; }

    media_container k = media_sniff(data, len);
    if (k == MEDIA_CONT_UNKNOWN) { if (err) *err = MEDIA_ERR_UNSUPPORTED; return 0; }

    mdemux *m = (mdemux *)calloc(1, sizeof *m);
    if (!m) { if (err) *err = MEDIA_ERR_OOM; return 0; }
    m->data = data; m->len = len; m->kind = k;
    /* Matches each format's own _open() wrapper exactly (avi_open/flv_open:
     * 1000; ts_open/ps_open: 90000, the fixed 90 kHz system clock both MPEG
     * transport formats use) -- neither avi_parse/flv_parse/ts_parse/
     * ps_parse sets movie_timescale itself the way mp4_parse/mkv_parse do
     * from their own header fields, so the caller must get this right
     * BEFORE the parse call, not after. */
    m->movie_timescale = (k == MEDIA_CONT_TS || k == MEDIA_CONT_PS) ? 90000 : 1000;
    m->movie_duration = -1;
    m->selected = -1;

    switch (k) {
    case MEDIA_CONT_MP4: e = mp4_parse(m); break;
    case MEDIA_CONT_MKV: e = mkv_parse(m); break;
    case MEDIA_CONT_AVI: e = avi_parse(m); break;
    case MEDIA_CONT_FLV: e = flv_parse(m); break;
    case MEDIA_CONT_TS:
        /* ts_parse REASSIGNS m->data to a scratch buffer it allocates on
         * success (see media_int.h's owns_data field and ts.h) -- set the
         * flag only once success is confirmed, so a failed parse (where
         * m->data is still the caller's own buffer) never gets freed here. */
        e = ts_parse(m);
#ifndef CONTAINERS_CONTROL_NO_OWNS_DATA
        if (e == MEDIA_OK) m->owns_data = 1;
#endif
        break;
    case MEDIA_CONT_PS:
        e = ps_parse(m);
#ifndef CONTAINERS_CONTROL_NO_OWNS_DATA
        if (e == MEDIA_OK) m->owns_data = 1;
#endif
        break;
        /* CONTAINERS_CONTROL_NO_OWNS_DATA: the plausible wrong wiring --
         * dispatch TS/PS through media_open() without ever setting the
         * flag, exactly the state this file was in before this change (and
         * exactly what ts.h's own header comment warned would happen: "not
         * written here because media_int.h and demux.c belong to another
         * workflow"). media_close() then frees only m->tr[i].s, never the
         * reassembled scratch buffer -- a real leak on every TS/PS file the
         * generic path closes, required to be caught by AddressSanitizer's
         * leak detector specifically. See test-containers-negctl. */
    default: e = MEDIA_ERR_UNSUPPORTED; break;
    }
    if (e == MEDIA_OK && m->ntracks == 0) e = MEDIA_ERR_CORRUPT;
    if (e != MEDIA_OK) { media_close(m); if (err) *err = e; return 0; }

    for (int i = 0; i < m->ntracks; i++) md_finish_track(&m->tr[i]);
    return m;
}

void media_close(mdemux *m)
{
    if (!m) return;
    for (int i = 0; i < m->ntracks; i++) free(m->tr[i].s);
    /* Only ts_parse/ps_parse ever set this, and only after reassigning
     * `data` from the caller's buffer to a scratch buffer this library
     * allocated -- freeing it here for any other kind would free memory
     * media_open() never owned. See media_int.h's field comment. */
    if (m->owns_data) free((void *)m->data);
    free(m);
}

media_container media_kind(const mdemux *m) { return m ? m->kind : MEDIA_CONT_UNKNOWN; }
int media_track_count(const mdemux *m) { return m ? m->ntracks : 0; }
int media_is_fragmented(const mdemux *m) { return m ? m->fragmented : 0; }

const media_track *media_track_info(const mdemux *m, int i)
{
    if (!m || i < 0 || i >= m->ntracks) return 0;
    return &m->tr[i].t;
}

int media_find_track(const mdemux *m, int type)
{
    if (!m) return -1;
    for (int i = 0; i < m->ntracks; i++)
        if (m->tr[i].t.type == type) return i;
    return -1;
}

long long media_duration_ns(const mdemux *m)
{
    if (!m) return -1;
    long long best = -1;
    /* The movie header's duration is authoritative when it is there; without
     * it, the longest track's last timestamp is the honest answer. */
    if (m->movie_duration > 0)
        best = md_ticks_to_ns(m->movie_duration, m->movie_timescale);
    for (int i = 0; i < m->ntracks; i++) {
        const mtrack *t = &m->tr[i];
        long long d = -1;
        if (t->t.duration > 0) d = md_ticks_to_ns(t->t.duration, t->t.timescale);
        else if (t->n > 0) d = md_ticks_to_ns(t->s[t->n - 1].dts, t->t.timescale);
        if (d > best) best = d;
    }
    return best;
}

int media_select(mdemux *m, int track)
{
    if (!m) return MEDIA_ERR_CORRUPT;
    if (track >= m->ntracks) return MEDIA_ERR_CORRUPT;
    m->selected = track < 0 ? -1 : track;
    return MEDIA_OK;
}

static void fill_sample(const mdemux *m, int ti, long idx, media_sample *out)
{
    const mtrack *t = &m->tr[ti];
    const msample *s = &t->s[idx];
    long long off = ms_off(s);
    out->track = ti;
    out->size = (long)s->size;
    out->file_off = off;
    out->data = m->data + off;
    out->pts_ticks = s->pts;
    out->dts_ticks = s->dts;
    out->pts_ns = md_ticks_to_ns(s->pts, t->t.timescale);
    out->dts_ns = md_ticks_to_ns(s->dts, t->t.timescale);
    out->keyframe = (s->flags & MS_KEY) ? 1 : 0;
}

int media_get_sample(const mdemux *m, int track, long idx, media_sample *out)
{
    if (!m || !out || track < 0 || track >= m->ntracks) return MEDIA_ERR_CORRUPT;
    const mtrack *t = &m->tr[track];
    if (idx < 0) return MEDIA_ERR_CORRUPT;
    if (idx >= t->n) return 0;
    /* The index was validated at build time against the file length, but this
     * is the one place a caller-supplied number reaches a pointer, so check. */
    long long off = ms_off(&t->s[idx]);
    if (off < 0 || off > m->len || (long long)t->s[idx].size > m->len - off)
        return MEDIA_ERR_CORRUPT;
    fill_sample(m, track, idx, out);
    return 1;
}

/* Decode order across tracks: lowest dts wins, ties go to the lower file
 * offset -- which is the interleave the muxer chose, so a well-muxed file
 * comes out in exactly the order it is laid out in. */
int media_read(mdemux *m, media_sample *out)
{
    if (!m || !out) return MEDIA_ERR_CORRUPT;
    int best = -1;
    long long best_ns = 0, best_off = 0;
    for (int i = 0; i < m->ntracks; i++) {
        if (m->selected >= 0 && i != m->selected) continue;
        mtrack *t = &m->tr[i];
        if (t->cursor >= t->n) continue;
        long long ns = md_ticks_to_ns(t->s[t->cursor].dts, t->t.timescale);
        long long off = ms_off(&t->s[t->cursor]);
        if (best < 0 || ns < best_ns || (ns == best_ns && off < best_off)) {
            best = i; best_ns = ns; best_off = off;
        }
    }
    if (best < 0) return 0;
    int rc = media_get_sample(m, best, m->tr[best].cursor, out);
    if (rc == 1) m->tr[best].cursor++;
    return rc;
}

long long media_seek(mdemux *m, int track, long long ns)
{
    if (!m || track < 0 || track >= m->ntracks) return MEDIA_ERR_CORRUPT;
    mtrack *t = &m->tr[track];
    if (t->n == 0) return MEDIA_ERR_CORRUPT;
    long want = 0;
    for (long i = 0; i < t->n; i++) {
        if (!(t->s[i].flags & MS_KEY)) continue;
        if (md_ticks_to_ns(t->s[i].pts, t->t.timescale) <= ns) want = i;
        else break;
    }
    /* Everything restarts from that instant, in DECODE time: an audio sample
     * whose dts is before the video keyframe would be played too early. */
    long long at = t->s[want].dts;
    long long at_ns = md_ticks_to_ns(at, t->t.timescale);
    for (int i = 0; i < m->ntracks; i++) {
        mtrack *o = &m->tr[i];
        long j = 0;
        while (j < o->n && md_ticks_to_ns(o->s[j].dts, o->t.timescale) < at_ns) j++;
        o->cursor = j;
    }
    t->cursor = want;
    return md_ticks_to_ns(t->s[want].pts, t->t.timescale);
}

/* ------------------------------------------------------- Annex B out ----- */
/* A NAL, start-code prefixed. Four-byte start codes throughout: three is legal
 * and shorter, but the decoders here scan for either and a uniform prefix
 * makes the emitted stream trivially comparable with ffmpeg's. */
static long emit_nal(uint8_t *out, long max, long at, const uint8_t *p, long n)
{
    if (n < 0) return MEDIA_ERR_CORRUPT;
    if (out) {
        if (at + 4 + n > max) return MEDIA_ERR_RANGE;
        out[at] = 0; out[at + 1] = 0; out[at + 2] = 0; out[at + 3] = 1;
        memcpy(out + at + 4, p, (size_t)n);
    }
    return at + 4 + n;
}

long media_annexb_headers(const mdemux *m, int track, uint8_t *out, long max)
{
    if (!m || track < 0 || track >= m->ntracks) return MEDIA_ERR_CORRUPT;
    const media_track *t = &m->tr[track].t;
    if (t->framing != MEDIA_FRAMING_AVCC) return 0;

    br b;
    br_init(&b, t->extradata, t->extradata_len, 0);
    long at = 0;

    if (t->codec == MEDIA_CODEC_H264) {
        br_skip(&b, 5);                       /* version, profile, compat, level, lenSize */
        for (int pass = 0; pass < 2; pass++) {
            /* SPS count is 5 bits with 3 reserved above it; PPS count is 8. */
            uint32_t cnt = br_u8(&b);
            if (pass == 0) cnt &= 0x1F;
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t n = br_u16(&b);
                const uint8_t *p = br_bytes(&b, (long)n);
                if (!p) return MEDIA_ERR_CORRUPT;
                at = emit_nal(out, max, at, p, (long)n);
                if (at < 0) return at;
            }
            if (!br_ok(&b)) return MEDIA_ERR_CORRUPT;
        }
        return at;
    }

    /* hvcC: a fixed 22-byte header, then numOfArrays arrays of NALs (VPS, SPS,
     * PPS, and sometimes prefix SEI), each with its own count. */
    br_skip(&b, 22);
    uint32_t narr = br_u8(&b);
    for (uint32_t a = 0; a < narr; a++) {
        br_u8(&b);                            /* completeness | nal type */
        uint32_t cnt = br_u16(&b);
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t n = br_u16(&b);
            const uint8_t *p = br_bytes(&b, (long)n);
            if (!p) return MEDIA_ERR_CORRUPT;
            at = emit_nal(out, max, at, p, (long)n);
            if (at < 0) return at;
        }
        if (!br_ok(&b)) return MEDIA_ERR_CORRUPT;
    }
    return at;
}

long media_to_annexb(const mdemux *m, const media_sample *s, uint8_t *out, long max)
{
    if (!m || !s || s->track < 0 || s->track >= m->ntracks) return MEDIA_ERR_CORRUPT;
    const media_track *t = &m->tr[s->track].t;
    if (t->framing != MEDIA_FRAMING_AVCC) {
        if (out) {
            if (s->size > max) return MEDIA_ERR_RANGE;
            memcpy(out, s->data, (size_t)s->size);
        }
        return s->size;
    }

    int ls = t->nal_length_size;
    if (ls != 1 && ls != 2 && ls != 4) return MEDIA_ERR_CORRUPT;

    br b;
    br_init(&b, s->data, s->size, s->file_off);
    long at = 0;
    while (br_left(&b) > 0) {
        uint32_t n = 0;
        if (ls == 1) n = br_u8(&b);
        else if (ls == 2) n = br_u16(&b);
        else n = br_u32(&b);
        if (!br_ok(&b)) return MEDIA_ERR_CORRUPT;
        /* A length prefix that overruns the sample is the classic
         * malformed-file case and the one a fuzzer finds in five seconds. It
         * is corrupt -- not a reason to read the next sample's bytes, and not
         * a reason to copy them into the caller's buffer.
         *
         * This is the bound make test-demux-fuzz-negctl removes: under
         * -DDEMUX_FUZZ_SABOTAGE the length is taken at face value, which is a
         * heap over-read driven directly by four attacker-chosen bytes. That
         * build is REQUIRED to be caught by AddressSanitizer; if it is not,
         * the fuzzer's clean runs are proving nothing. */
#ifdef DEMUX_FUZZ_SABOTAGE
        const uint8_t *p = b.base + b.pos;
        b.pos += (long)n;
#else
        const uint8_t *p = br_bytes(&b, (long)n);
        if (!p) return MEDIA_ERR_CORRUPT;
#endif
        at = emit_nal(out, max, at, p, (long)n);
        if (at < 0) return at;
    }
    return at;
}
