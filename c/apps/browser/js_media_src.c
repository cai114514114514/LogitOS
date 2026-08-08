/* Media Source Extensions: the engine. See js_media.h for what this is and why.
 *
 * Nothing in this file knows about JavaScript. js_media.c owns the JS objects,
 * the events and the exceptions; this owns the bytes, the demuxer, the two
 * decoders, the sound card and the clock. The split is what lets the whole
 * thing be a host unit test (tests/unit/mse_test.c drives exactly these
 * functions with a stepped clock and a recording blitter) instead of a QEMU
 * boot with a stopwatch.
 *
 * THE ONE IDEA THIS FILE IS BUILT ON. c/lib/media's media_open() takes a whole
 * file, and MSE is the opposite: segments arrive over time and the first one
 * may be three network reads long. But an fMP4 INIT segment followed by media
 * segments IS a valid fragmented MP4 -- that is what a DASH client concatenates
 * when it saves a stream to disk -- so an append accumulates into ONE growing
 * buffer and the demuxer is re-opened over the longest prefix that ends on a
 * top-level box boundary. A partial box is simply not part of that prefix yet,
 * which is why a split segment behaves exactly like a whole one and why nothing
 * here ever has to understand moof internals.
 *
 * WHAT THAT COSTS, said out loud: a re-open re-parses the prefix, so N segments
 * cost O(N^2) bytes of parsing if you re-open per segment. So re-opening is
 * LAZY -- it happens when someone asks a question the current parse cannot
 * answer (the player wants a sample past the end, or something reads
 * `buffered`) -- and the count is in mel_stats.reparses so the cost is a number
 * a test can bound rather than a footnote. The alternative (teaching mp4.c to
 * accept fragments incrementally) is an API change in a file this line does not
 * own; it is the right long-term fix and it is written down in the report.
 *
 * EVERY APPENDED BYTE IS UNTRUSTED. It came from the network through
 * appendBuffer, which any page can call with anything. The box walk below is
 * bounded by the buffer length at every step and refuses a size field that
 * would move the cursor backwards or past the end; beyond that the demuxer's
 * own fuzzed bounds checking (make test-demux-fuzz) is the wall.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "js_media.h"
#include "media.h"
#include "h264.h"
#include "h265.h"
#include "audio.h"
#include "aac.h"
#include "mp3.h"

/* ============================================================ limits ==== */
/* Every one of these bounds an allocation driven by something a page said. */
#define MSE_MAX_SB          4          /* video + audio + slack */
#define MSE_MAX_ELEM        8
#define MSE_MAX_RANGES      16
#define MSE_MAX_STAMPS      32         /* timestampOffset checkpoints */
#define MSE_MAX_REMOVES     8
#define MSE_MAX_URLS        16
#define MSE_BUF_CAP    (48L * 1024 * 1024)   /* QuotaExceededError above this */
#define MSE_NALBUF     (1L << 22)      /* ceiling on ONE access unit */
#define MSE_PTSQ        64             /* decode-order stamp queue (HEVC only) */
#define AV_LEAD_NS      150000000LL    /* how far audio may run ahead: preview's */
#define ABUF_FRAMES     1024
/* Two samples' worth of gap is still one buffered range. The spec calls this
 * the "fudge factor" and defines it as 2/frame-rate; we do not know the frame
 * rate before the first fragment, so 100 ms stands in and is stated. */
#define RANGE_GAP_NS    100000000LL

/* ===================================================== the platform ==== */
static unsigned long long plat_now_default(void) { return 0; }
static const struct media_platform g_plat_null = { plat_now_default, 0,0,0,0, 0,0,0,0,0 };
static const struct media_platform *g_plat = &g_plat_null;

void media_set_platform(const struct media_platform *p)
{
    g_plat = p ? p : &g_plat_null;
}
static unsigned long long now_ns(void)
{
    return g_plat->now_ns ? g_plat->now_ns() : 0;
}

/* ================================================ the codec answer ==== */
/* THE FUNCTION THIS WHOLE FILE EXISTS TO GET RIGHT.
 *
 * Every entry below names the gate in c/lib that decides it. Nothing here is a
 * guess about a decoder's ambitions: if a profile reaches an
 * H264_ERR_UNSUPPORTED / H265_ERR_UNSUPPORTED / AUDIO_ERR_UNSUPPORTED return
 * for a stream that really uses it, the answer is NO.
 *
 * H.264 -- c/lib/video/h264_nal.c h264_parse_sps():
 *     profile_idc 66 (Baseline), 77 (Main) and the High family are parsed;
 *     the High family is then gated to chroma_format_idc == 1 and 8-bit, so
 *     High 10 (110), High 4:2:2 (122) and High 4:4:4 (244) are REFUSED even
 *     though their SPS parses. Extended (88) parses too, but the two things
 *     that make a stream Extended -- SP/SI slices and data partitioning -- are
 *     both refused (h264_nal.c slice_type >= 3, h264.c NAL types 2..4), so
 *     claiming 88 would be a lie for any stream that uses the profile.
 *     => yes to 42.. (66), 4D.. (77), 64.. (100). No to everything else.
 *     Level: accepted up to 5.1 (0x33), which is what bilibili's avc1.640033
 *     asks for and the largest picture this browser's arena can hold a DPB of.
 *
 * H.265 -- c/lib/video/h265_nal.c: the gate is on the DECLARED SAMPLE DEPTH
 *     (8..10, luma == chroma) and 4:2:0, deliberately not on
 *     general_profile_idc. Main (1) and Main 10 (2) are exactly the profiles
 *     that stay inside that; Rext (4) and the screen-content profiles are not.
 *     => yes to hvc1/hev1 profile 1 and 2, no to the rest.
 *
 * AAC -- c/lib/audio/aac.c: AAC-LC only. Object types 5 and 29 (HE-AAC v1/v2)
 *     are refused ON PURPOSE, because decoding the core alone is the right
 *     samples at half the rate, which sounds nearly right and is wrong.
 *     => yes to mp4a.40.2, no to mp4a.40.5 / .29.
 * MP3-in-MP4 (mp4a.40.34, and the mp4a.69/mp4a.6B aliases) -- c/lib/audio/mp3.c
 *     mp3_decode() is frame-incremental, which is the shape MSE needs.
 *     => yes.
 *
 * AV1, VP9, VP8, Opus, Vorbis, AC-3, FLAC-in-MP4: NO. There is no AV1, VP9 or
 *     VP8 decoder in this tree at all, and no Opus one. c/lib/audio does have
 *     Vorbis and FLAC, but neither ships in the fMP4 a DASH/MSE player
 *     delivers, and answering yes to a combination we have never demuxed is
 *     the same lie in a quieter voice. av01 is the one that matters and the one
 *     tests/unit/mse_test.c asserts on both ways.
 *
 * CONTAINERS. video/mp4, audio/mp4 and their aliases only. WebM is refused
 *     whole -- c/lib/media demuxes Matroska perfectly well, but every codec
 *     that ships in a WebM (VP8/VP9/AV1/Opus/Vorbis) is one we cannot decode,
 *     so a yes could never be honoured.
 */

static int ci_eq_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}
static int hex2(const char *s, int *out)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + d;
    }
    *out = v;
    return 1;
}
static int dec_int(const char *s, const char *end, int *out)
{
    int v = 0, any = 0;
    while (s < end && *s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; if (v > 100000) return 0; }
    *out = v;
    return any && s == end;
}

/* One codec string, e.g. "avc1.640033" / "mp4a.40.2" / "hvc1.1.6.L120.90". */
static int codec_ok(const char *c, int len)
{
    char b[64];
    if (len <= 0 || len >= (int)sizeof b) return 0;
    memcpy(b, c, (size_t)len);
    b[len] = 0;

#ifdef MSE_CONTROL_CLAIM_AV1
    /* THE NEGATIVE CONTROL, and it is the precise bug this file exists to not
     * have. There is no AV1 decoder in this tree. A build that says yes anyway
     * does not gain a codec: it makes a real site hand us av01 instead of the
     * H.264 it also offered, addSourceBuffer succeeds, the bytes append, and
     * NOTHING EVER DECODES -- a failure that surfaces frames later and reads as
     * a decoder bug. make test-mse-negctl builds with this and REQUIRES the
     * suite to fail. */
    if (ci_eq_n(b, "av01", 4)) return 1;
#endif

    /* --- H.264 ------------------------------------------------------------ */
    if (ci_eq_n(b, "avc1", 4) || ci_eq_n(b, "avc3", 4)) {
        /* avc1.PPCCLL -- profile_idc, constraint flags, level_idc, all hex. */
        if (b[4] != '.') return 0;
        const char *p = b + 5;
        int n = len - 5;
        int prof, cons, lev;
        if (n != 6) return 0;
        if (!hex2(p, &prof) || !hex2(p + 2, &cons) || !hex2(p + 4, &lev)) return 0;
        (void)cons;
        if (prof != 66 && prof != 77 && prof != 100) return 0;
        if (lev > 51) return 0;
        return 1;
    }
    /* --- H.265 ------------------------------------------------------------ */
    if (ci_eq_n(b, "hvc1", 4) || ci_eq_n(b, "hev1", 4)) {
        /* hvc1.P.CCCC.TLLL.CC... -- the first field is the profile, optionally
         * prefixed with the profile SPACE as a letter ("A1" = space 1). A
         * non-zero profile space is not the common profiles, so it is a no. */
        if (b[4] != '.') return 0;
        const char *p = b + 5;
        if ((*p >= 'A' && *p <= 'D') || (*p >= 'a' && *p <= 'd')) return 0;
        const char *e = p;
        while (*e && *e != '.') e++;
        int prof;
        if (!dec_int(p, e, &prof)) return 0;
        return prof == 1 || prof == 2;         /* Main, Main 10 */
    }
    /* --- audio ------------------------------------------------------------ */
    if (ci_eq_n(b, "mp4a", 4)) {
        if (b[4] != '.') return 0;
        const char *p = b + 5;
        const char *e = p;
        while (*e && *e != '.') e++;
        int oti;
        if (!dec_int(p, e, &oti)) {
            /* mp4a.40 with a hex OTI: mp4a.69 / mp4a.6B are MPEG-2/1 audio. */
            if (hex2(p, &oti) && (p[2] == 0 || p[2] == '.'))
                return oti == 0x69 || oti == 0x6B;
            return 0;
        }
        if (oti == 0x69 || oti == 0x6B) return 1;      /* MP3 spelled decimal */
        if (oti != 40) return 0;                       /* not MPEG-4 audio */
        if (!*e) return 0;                             /* no audioObjectType */
        const char *q = e + 1;
        const char *qe = q;
        while (*qe && *qe != '.') qe++;
        int aot;
        if (!dec_int(q, qe, &aot)) return 0;
        if (aot == 2) return 1;                        /* AAC-LC */
        if (aot == 34) return 1;                       /* MPEG-1 Layer 3 */
        return 0;                                      /* 5/29 HE-AAC: refused */
    }
    /* Everything else -- av01, vp09, vp8, opus, vorbis, ac-3, flac, ... */
    return 0;
}

int mse_type_supported(const char *type)
{
    if (!type || !*type) return 0;
    /* container */
    const char *p = type;
    while (*p == ' ') p++;
    int mp4 = ci_eq_n(p, "video/mp4", 9) || ci_eq_n(p, "audio/mp4", 9) ||
              ci_eq_n(p, "video/x-m4v", 11) || ci_eq_n(p, "audio/x-m4a", 11) ||
              ci_eq_n(p, "audio/mpeg", 10) || ci_eq_n(p, "audio/aac", 9);
    if (!mp4) return 0;
    /* audio/mpeg and audio/aac carry no codecs parameter and are unambiguous;
     * everything else must say what is inside it. */
    int bare_ok = ci_eq_n(p, "audio/mpeg", 10) || ci_eq_n(p, "audio/aac", 9);

    /* find codecs="..." */
    const char *c = 0;
    for (const char *s = type; *s; s++) {
        if ((*s == 'c' || *s == 'C') && ci_eq_n(s, "codecs", 6)) {
            const char *q = s + 6;
            while (*q == ' ') q++;
            if (*q != '=') continue;
            q++;
            while (*q == ' ') q++;
            c = q;
            break;
        }
    }
    if (!c) return bare_ok;

    char quote = 0;
    if (*c == '"' || *c == '\'') { quote = *c; c++; }
    /* Split on commas; every codec must be supported. An empty list is a no. */
    int any = 0;
    while (*c) {
        const char *s = c;
        while (*s && *s != ',' && *s != ';' && *s != quote) s++;
        const char *t = s;
        while (t > c && t[-1] == ' ') t--;
        const char *h = c;
        while (h < t && *h == ' ') h++;
        if (t > h) {
            any = 1;
            if (!codec_ok(h, (int)(t - h))) return 0;
        }
        if (!*s || *s == ';' || *s == quote) break;
        c = s + 1;
    }
    return any;
}

/* ============================================ ranges + stamp tables ==== */
struct nrange { long long start, end; };

static void range_add(struct nrange *r, int *n, long long s, long long e)
{
    if (e < s) return;
    for (int i = 0; i < *n; i++) {
        if (s <= r[i].end + RANGE_GAP_NS && e + RANGE_GAP_NS >= r[i].start) {
            if (s < r[i].start) r[i].start = s;
            if (e > r[i].end) r[i].end = e;
            /* merge forward */
            while (i + 1 < *n && r[i].end + RANGE_GAP_NS >= r[i + 1].start) {
                if (r[i + 1].end > r[i].end) r[i].end = r[i + 1].end;
                for (int k = i + 1; k + 1 < *n; k++) r[k] = r[k + 1];
                (*n)--;
            }
            return;
        }
        if (e + RANGE_GAP_NS < r[i].start) {
            if (*n >= MSE_MAX_RANGES) return;
            for (int k = *n; k > i; k--) r[k] = r[k - 1];
            r[i].start = s; r[i].end = e;
            (*n)++;
            return;
        }
    }
    if (*n >= MSE_MAX_RANGES) return;
    r[*n].start = s; r[*n].end = e;
    (*n)++;
}

/* ==================================================== SourceBuffer ==== */
struct stamp { long index; long long offset_ns; };

struct sbuf {
    msource  *ms;
    char      type[96];
    int       mode;
    int       updating;
    int       removed_self;

    unsigned char *data;
    long      len, cap;
    long      parsed;           /* prefix ending on a top-level box boundary */
    long      scan;             /* how far the box walk has got */
    int       saw_moov;

    mdemux   *dm;
    int       dirty;            /* the parse is behind `parsed` */
    int       vtrack, atrack;
    long      nsamples_v, nsamples_a;

    /* timestampOffset checkpoints: which offset applied to which samples. */
    long long tsoffset_ns;
    struct stamp vstamp[MSE_MAX_STAMPS]; int nvstamp;
    struct stamp astamp[MSE_MAX_STAMPS]; int nastamp;

    /* sequence mode's own cursor */
    long long seq_ns;

    /* what remove() evicted, in presentation ns */
    struct nrange removed[MSE_MAX_REMOVES]; int nremoved;

    struct nrange ranges[MSE_MAX_RANGES]; int nranges;

    long long appends, bytes_appended, reparses;
};

struct msource {
    int      state;
    double   duration;              /* seconds, -1 = unset */
    sbuf    *sb[MSE_MAX_SB];
    int      nsb;
    melem   *el;
    unsigned url_id;
    char     end_error[32];
};

/* ---- the box walk ---- */
/* Advance sb->scan/sb->parsed over every COMPLETE top-level box. Returns 1 when
 * `parsed` moved. Bounded at every step: a size that does not advance the
 * cursor, or that would run past the buffer, ends the walk rather than being
 * believed. */
static int walk_boxes(sbuf *sb)
{
    long moved = 0;
    while (sb->scan + 8 <= sb->len) {
        const unsigned char *p = sb->data + sb->scan;
        uint64_t sz = ((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16) |
                      ((uint64_t)p[2] << 8) | p[3];
        long hdr = 8;
        if (sz == 1) {
            if (sb->scan + 16 > sb->len) break;
            sz = 0;
            for (int i = 0; i < 8; i++) sz = (sz << 8) | p[8 + i];
            hdr = 16;
        } else if (sz == 0) {
            break;                       /* "to end of file": never complete */
        }
        if (sz < (uint64_t)hdr) break;                    /* would not advance */
        if (sz > (uint64_t)(sb->len - sb->scan)) break;   /* not all here yet */
        if (!memcmp(p + 4, "moov", 4)) sb->saw_moov = 1;
        sb->scan += (long)sz;
        sb->parsed = sb->scan;
        moved = 1;
    }
    return moved != 0;
}

static long long sample_dur_ns(const mdemux *m, int track, long idx)
{
    media_sample a, b;
    if (media_get_sample(m, track, idx, &a) != 1) return 0;
    if (media_get_sample(m, track, idx + 1, &b) == 1 && b.pts_ns > a.pts_ns)
        return b.pts_ns - a.pts_ns;
    if (idx > 0 && media_get_sample(m, track, idx - 1, &b) == 1 && a.pts_ns > b.pts_ns)
        return a.pts_ns - b.pts_ns;
    return 0;
}

static long long stamp_offset(const struct stamp *t, int n, long idx)
{
    long long off = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].index <= idx) off = t[i].offset_ns;
        else break;
    }
    return off;
}
static void stamp_push(struct stamp *t, int *n, long idx, long long off)
{
    if (*n > 0 && t[*n - 1].index == idx) { t[*n - 1].offset_ns = off; return; }
    if (*n > 0 && t[*n - 1].offset_ns == off) return;
    if (*n >= MSE_MAX_STAMPS) { t[*n - 1].offset_ns = off; return; }
    t[*n].index = idx; t[*n].offset_ns = off;
    (*n)++;
}

/* Presentation time of sample `idx` on `track`, with the offset that was in
 * force when it was appended. */
static long long pres_ns(const sbuf *sb, int track, long idx, const media_sample *s)
{
    long long off = (track == sb->vtrack)
                        ? stamp_offset(sb->vstamp, sb->nvstamp, idx)
                        : stamp_offset(sb->astamp, sb->nastamp, idx);
    return s->pts_ns + off;
}

static int in_removed(const sbuf *sb, long long t)
{
    for (int i = 0; i < sb->nremoved; i++)
        if (t >= sb->removed[i].start && t < sb->removed[i].end) return 1;
    return 0;
}

static void rebuild_ranges(sbuf *sb)
{
    sb->nranges = 0;
    if (!sb->dm) return;
    int tracks[2] = { sb->vtrack, sb->atrack };
    for (int ti = 0; ti < 2; ti++) {
        int t = tracks[ti];
        if (t < 0) continue;
        const media_track *info = media_track_info(sb->dm, t);
        if (!info) continue;
        media_sample s;
        for (long k = 0; k < info->nsamples; k++) {
            if (media_get_sample(sb->dm, t, k, &s) != 1) break;
            long long a = pres_ns(sb, t, k, &s);
            long long d = sample_dur_ns(sb->dm, t, k);
            if (in_removed(sb, a)) continue;
            range_add(sb->ranges, &sb->nranges, a, a + d);
        }
        /* One track is enough to describe this buffer: with both present they
         * are the same media and intersecting them would report a range the
         * VIDEO does not have just because the audio does. Video wins. */
        if (t == sb->vtrack) break;
    }
}

/* Re-open the demuxer over the parsed prefix. This is the O(prefix) step; it
 * runs only when something needs an answer the current parse cannot give. */
static int reparse(sbuf *sb)
{
    if (!sb->dirty) return sb->dm != 0;
    sb->dirty = 0;
    if (!sb->saw_moov || sb->parsed <= 0) return 0;

    mdemux *old = sb->dm;
    int err = 0;
    mdemux *m = media_open(sb->data, sb->parsed, &err);
    if (!m) {
        /* An init segment on its own (moov with mvex and no fragment) is a
         * valid thing to have appended and not yet a playable file. Keep the
         * previous parse rather than throwing the buffer away. */
        return old != 0;
    }
    if (old) media_close(old);
    sb->dm = m;
    sb->reparses++;
    sb->vtrack = media_find_track(m, MEDIA_TRACK_VIDEO);
    sb->atrack = media_find_track(m, MEDIA_TRACK_AUDIO);
    const media_track *vt = sb->vtrack >= 0 ? media_track_info(m, sb->vtrack) : 0;
    const media_track *at = sb->atrack >= 0 ? media_track_info(m, sb->atrack) : 0;
    long nv = vt ? vt->nsamples : 0, na = at ? at->nsamples : 0;

    /* SEQUENCE MODE. The spec's rule is that each appended segment is placed
     * immediately after the previous one regardless of its own timestamps. So
     * the offset for the samples this parse revealed is whatever moves their
     * first timestamp to the running cursor. */
    if (sb->mode == MSE_MODE_SEQUENCE) {
        media_sample s;
        if (nv > sb->nsamples_v && sb->vtrack >= 0 &&
            media_get_sample(m, sb->vtrack, sb->nsamples_v, &s) == 1) {
            long long off = sb->seq_ns - s.pts_ns;
            stamp_push(sb->vstamp, &sb->nvstamp, sb->nsamples_v, off);
            if (media_get_sample(m, sb->vtrack, nv - 1, &s) == 1)
                sb->seq_ns = s.pts_ns + off + sample_dur_ns(m, sb->vtrack, nv - 1);
        }
        if (na > sb->nsamples_a && sb->atrack >= 0 &&
            media_get_sample(m, sb->atrack, sb->nsamples_a, &s) == 1) {
            long long off = sb->seq_ns - s.pts_ns;
            /* With no video track the audio drives the cursor. */
            if (sb->vtrack < 0) {
                stamp_push(sb->astamp, &sb->nastamp, sb->nsamples_a, off);
                if (media_get_sample(m, sb->atrack, na - 1, &s) == 1)
                    sb->seq_ns = s.pts_ns + off + sample_dur_ns(m, sb->atrack, na - 1);
            } else {
                stamp_push(sb->astamp, &sb->nastamp, sb->nsamples_a,
                           stamp_offset(sb->vstamp, sb->nvstamp, sb->nsamples_v));
            }
        }
    }
    sb->nsamples_v = nv;
    sb->nsamples_a = na;
    rebuild_ranges(sb);
    return 1;
}

static void sb_free(sbuf *sb)
{
    if (!sb) return;
    if (sb->dm) media_close(sb->dm);
    if (sb->data) free(sb->data);
    free(sb);
}

const char *sb_type(const sbuf *sb) { return sb ? sb->type : ""; }
long sb_bytes(const sbuf *sb) { return sb ? sb->len : 0; }
int  sb_mode(const sbuf *sb) { return sb ? sb->mode : MSE_MODE_SEGMENTS; }
double sb_timestamp_offset(const sbuf *sb)
{
    return sb ? (double)sb->tsoffset_ns / 1e9 : 0.0;
}

void sb_set_mode(sbuf *sb, int mode)
{
    if (!sb) return;
    sb->mode = mode ? MSE_MODE_SEQUENCE : MSE_MODE_SEGMENTS;
    if (sb->mode == MSE_MODE_SEQUENCE && sb->seq_ns == 0 && sb->nranges > 0)
        sb->seq_ns = sb->ranges[sb->nranges - 1].end;
}

void sb_set_timestamp_offset(sbuf *sb, double sec)
{
    if (!sb) return;
    sb->tsoffset_ns = (long long)(sec * 1e9);
    if (sb->mode == MSE_MODE_SEQUENCE) { sb->seq_ns = sb->tsoffset_ns; return; }
    reparse(sb);
    stamp_push(sb->vstamp, &sb->nvstamp, sb->nsamples_v, sb->tsoffset_ns);
    stamp_push(sb->astamp, &sb->nastamp, sb->nsamples_a, sb->tsoffset_ns);
}

int sb_append(sbuf *sb, const unsigned char *data, long n)
{
    if (!sb || !data || n < 0) return MSE_E_INVALIDSTATE;
    if (sb->removed_self) return MSE_E_INVALIDSTATE;
    if (!sb->ms || sb->ms->state == MSE_CLOSED) return MSE_E_INVALIDSTATE;
    if (n == 0) return MSE_OK;
    if (sb->len + n > MSE_BUF_CAP) return MSE_E_QUOTA;

    if (sb->len + n > sb->cap) {
        long want = sb->cap ? sb->cap : (256L * 1024);
        while (want < sb->len + n) want *= 2;
        unsigned char *nd = realloc(sb->data, (size_t)want);
        if (!nd) return MSE_E_OOM;
        /* The demuxer holds pointers INTO the old block, so it is dead the
         * moment realloc may have moved it. Dropping it here (rather than
         * hoping) is the whole reason the parse is allowed to be lazy. */
        if (sb->dm) { media_close(sb->dm); sb->dm = 0; }
        sb->data = nd;
        sb->cap = want;
        sb->dirty = 1;
    }
    memcpy(sb->data + sb->len, data, (size_t)n);
    sb->len += n;
    sb->appends++;
    sb->bytes_appended += n;

    if (walk_boxes(sb)) sb->dirty = 1;
    if (sb->ms->state == MSE_ENDED) sb->ms->state = MSE_OPEN;   /* spec: re-open */

    /* Sequence mode needs the offset computed at append time (the running
     * cursor is per-append), so it cannot be lazy. Segments mode can. */
    if (sb->mode == MSE_MODE_SEQUENCE) reparse(sb);
    return MSE_OK;
}

int sb_remove(sbuf *sb, double start_sec, double end_sec)
{
    if (!sb) return MSE_E_INVALIDSTATE;
    if (!(end_sec > start_sec)) return MSE_E_INVALIDSTATE;
    if (sb->nremoved >= MSE_MAX_REMOVES) return MSE_E_QUOTA;
    sb->removed[sb->nremoved].start = (long long)(start_sec * 1e9);
    sb->removed[sb->nremoved].end   = (long long)(end_sec * 1e9);
    sb->nremoved++;
    reparse(sb);
    rebuild_ranges(sb);
    return MSE_OK;
}

int sb_abort(sbuf *sb)
{
    if (!sb) return MSE_E_INVALIDSTATE;
    /* No partial-append state to throw away: an append that did not complete a
     * box simply is not part of `parsed` yet, and the bytes stay because the
     * next append continues the same segment. What abort resets is the
     * timestampOffset-driven parser state, which is what the spec says. */
    sb->updating = 0;
    if (sb->mode == MSE_MODE_SEQUENCE) sb->seq_ns = sb->tsoffset_ns;
    return MSE_OK;
}

int sb_buffered_count(const sbuf *sb)
{
    if (!sb) return 0;
    reparse((sbuf *)sb);
    return sb->nranges;
}
int sb_buffered_range(const sbuf *sb, int i, double *start, double *end)
{
    if (!sb) return 0;
    reparse((sbuf *)sb);
    if (i < 0 || i >= sb->nranges) return 0;
    if (start) *start = (double)sb->ranges[i].start / 1e9;
    if (end)   *end   = (double)sb->ranges[i].end / 1e9;
    return 1;
}

/* ====================================================== MediaSource ==== */
msource *mse_new(void)
{
    msource *ms = calloc(1, sizeof *ms);
    if (!ms) return 0;
    ms->state = MSE_CLOSED;
    ms->duration = -1;
    return ms;
}

/* Forward: freeing a source has to break BOTH links -- the element's pointer
 * back to it and the object-URL registry's -- or the next thing that walks
 * either finds a freed block. (It did; ASan caught it in test_decodes.) */
static void mse_forget_urls(msource *ms);

void mse_free(msource *ms)
{
    if (!ms) return;
    mse_detach(ms);
    mse_forget_urls(ms);
    for (int i = 0; i < ms->nsb; i++) sb_free(ms->sb[i]);
    free(ms);
}

int    mse_state(const msource *ms) { return ms ? ms->state : MSE_CLOSED; }
double mse_duration(const msource *ms) { return ms ? ms->duration : -1; }
int    mse_sb_count(const msource *ms) { return ms ? ms->nsb : 0; }
sbuf  *mse_sb_at(const msource *ms, int i)
{
    if (!ms || i < 0 || i >= ms->nsb) return 0;
    return ms->sb[i];
}

int mse_set_duration(msource *ms, double sec)
{
    if (!ms || ms->state != MSE_OPEN) return MSE_E_INVALIDSTATE;
    if (sec < 0) return MSE_E_INVALIDSTATE;
    ms->duration = sec;
    return MSE_OK;
}

int mse_end_of_stream(msource *ms, const char *err)
{
    if (!ms || ms->state != MSE_OPEN) return MSE_E_INVALIDSTATE;
    ms->state = MSE_ENDED;
    ms->end_error[0] = 0;
    if (err) {
        int i = 0;
        while (err[i] && i < (int)sizeof ms->end_error - 1) { ms->end_error[i] = err[i]; i++; }
        ms->end_error[i] = 0;
    }
    /* With no explicit duration, the end of the buffered media IS the duration
     * -- which is what a player reads to draw its scrub bar. */
    if (ms->duration < 0) {
        double best = 0;
        for (int i = 0; i < ms->nsb; i++) {
            int n = sb_buffered_count(ms->sb[i]);
            double s, e;
            if (n > 0 && sb_buffered_range(ms->sb[i], n - 1, &s, &e) && e > best) best = e;
        }
        if (best > 0) ms->duration = best;
    }
    return MSE_OK;
}

sbuf *mse_add_source_buffer(msource *ms, const char *type, int *err)
{
    if (err) *err = MSE_OK;
    if (!ms || !type) { if (err) *err = MSE_E_INVALIDSTATE; return 0; }
    if (!mse_type_supported(type)) { if (err) *err = MSE_E_NOTSUPPORTED; return 0; }
    if (ms->state != MSE_OPEN || ms->nsb >= MSE_MAX_SB) {
        if (err) *err = MSE_E_INVALIDSTATE;
        return 0;
    }
    sbuf *sb = calloc(1, sizeof *sb);
    if (!sb) { if (err) *err = MSE_E_OOM; return 0; }
    sb->ms = ms;
    sb->vtrack = sb->atrack = -1;
    int i = 0;
    while (type[i] && i < (int)sizeof sb->type - 1) { sb->type[i] = type[i]; i++; }
    sb->type[i] = 0;
    ms->sb[ms->nsb++] = sb;
    return sb;
}

int mse_remove_source_buffer(msource *ms, sbuf *sb)
{
    if (!ms || !sb) return MSE_E_INVALIDSTATE;
    for (int i = 0; i < ms->nsb; i++) {
        if (ms->sb[i] != sb) continue;
        for (int k = i; k + 1 < ms->nsb; k++) ms->sb[k] = ms->sb[k + 1];
        ms->nsb--;
        sb->removed_self = 1;
        sb_free(sb);
        return MSE_OK;
    }
    return MSE_E_INVALIDSTATE;
}

/* ---- object URLs ---- */
static struct { unsigned id; msource *ms; } g_urls[MSE_MAX_URLS];
static unsigned g_url_next = 1;

static int url_id_of(const char *url)
{
    if (!url) return 0;
    const char *p = url;
    if (!ci_eq_n(p, "blob:logit/", 11)) return 0;
    p += 11;
    int v = 0, any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    return any && !*p ? v : 0;
}

int mse_object_url(msource *ms, char *out, int max)
{
    if (!ms || !out || max < 24) return 0;
    if (!ms->url_id) {
        int slot = -1;
        for (int i = 0; i < MSE_MAX_URLS; i++) if (!g_urls[i].ms) { slot = i; break; }
        if (slot < 0) return 0;
        ms->url_id = g_url_next++;
        g_urls[slot].id = ms->url_id;
        g_urls[slot].ms = ms;
    }
    /* "blob:logit/<n>" -- a real Blob URL's opaque part, spelled so a page that
     * logs it sees something recognisable. */
    const char *pre = "blob:logit/";
    int o = 0;
    while (pre[o]) { out[o] = pre[o]; o++; }
    char d[12];
    int n = 0;
    unsigned v = ms->url_id;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n > 0 && o < max - 1) out[o++] = d[--n];
    out[o] = 0;
    return 1;
}

msource *mse_from_object_url(const char *url)
{
    int id = url_id_of(url);
    if (!id) return 0;
    for (int i = 0; i < MSE_MAX_URLS; i++)
        if (g_urls[i].ms && g_urls[i].id == (unsigned)id) return g_urls[i].ms;
    return 0;
}

static void mse_forget_urls(msource *ms)
{
    for (int i = 0; i < MSE_MAX_URLS; i++)
        if (g_urls[i].ms == ms) { g_urls[i].ms = 0; g_urls[i].id = 0; }
}

void mse_revoke_object_url(const char *url)
{
    int id = url_id_of(url);
    if (!id) return;
    for (int i = 0; i < MSE_MAX_URLS; i++)
        if (g_urls[i].id == (unsigned)id) { g_urls[i].ms = 0; g_urls[i].id = 0; }
}

/* ================================================= the media element ==== */
enum { HAVE_NOTHING = 0, HAVE_METADATA = 1, HAVE_CURRENT_DATA = 2,
       HAVE_FUTURE_DATA = 3, HAVE_ENOUGH_DATA = 4 };
enum { NETWORK_EMPTY = 0, NETWORK_IDLE = 1, NETWORK_LOADING = 2, NETWORK_NO_SOURCE = 3 };

struct melem {
    /* Keyed by an INTEGER, not by a `struct node *`. The bindings and the
     * painter approach the same <video> from opposite sides -- JS holds an
     * element wrapper, browser_paint.c holds a DOM node -- and js_dom.c
     * (another line's file) exports no way to turn one into the other. So the
     * element carries the key itself, in a data- attribute: the JS shim stamps
     * it, and js_media.c reads it back off the node with dom_attr(). Public
     * surface on both sides, and it survives DOM mutation, which an index into
     * document order would not.
     *
     * The ask that would delete this: `struct node *js_dom_node_of(JSValueConst)`
     * exported from js_dom.c. Written down in the handover rather than done. */
    int          key;
    int          used;
    msource     *ms;

    int    paused, ended, seeking, muted, playing;
    double volume;
    int    ready_state, net_state;
    int    err_code;
    char   err_msg[96];

    /* video decode */
    int        vcodec;                 /* MEDIA_CODEC_H264 / H265 / 0 */
    h264dec   *d4;
    h265dec   *d5;
    int        hdr_sent;
    long       vcursor;                /* next video sample index */
    sbuf      *vsb;
    long long  vfirst_ns;              /* the presentation time we anchor on */

    /* audio decode */
    int        acodec;
    aacdec    *aac;
    mp3dec    *mp3;
    sbuf      *asb;
    long       acursor;
    int        snd, arate, ach;
    long long  aframes_written, awritten_ns;
    long long  afirst_ns;              /* presentation time of the FIRST audio
                                        * sample handed to the card */
    int        aanchored;
    int        adone;

    avclock    clk;
    long long  current_ns;
    long long  last_timeupdate_ns;
    int        pending_wait;           /* a decoded picture is held, not yet due */
    long long  wait_pts;
    int        anchored;               /* vfirst_ns has been set from a real pts */
    int        waiting;                /* underrunning: `waiting` fired, `canplay` owed */
    unsigned   events;                 /* MEV_* the bindings have not fired yet */

    /* HEVC only: presentation stamps in DECODE order, waiting for the picture
     * they belong to. h264 needs none of this -- h264_decode_pts carries the
     * value through the DPB -- and that asymmetry is deliberate, not an
     * oversight: see the handover note in js_media.h. */
    long long  vpend[MSE_PTSQ];
    int        vpend_head, vpend_n;

    /* The Annex B staging buffer, PER ELEMENT and not a shared static: `noff`
     * persists across pump calls (a decoder emits a picture part-way through an
     * access unit), so two <video>s on one page sharing one buffer would each
     * resume into the other's bytes. Grown to fit, never pre-reserved. */
    unsigned char *nal;
    long   ncap, nlen, noff;
    long long cur_at;                  /* presentation time of the access unit
                                        * currently staged in `nal` */

    /* the picture */
    unsigned char *rgba;
    int    fw, fh, fcap;
    int    have_frame, frame_new;

    /* where the painter last put us, in device pixels */
    int    bx, by, bw, bh, cx, cy, cw, ch, box_valid;

    struct mel_stats st;
};

static melem g_el[MSE_MAX_ELEM];

melem *mel_for_key(int key, int create)
{
    if (key <= 0) return 0;
    for (int i = 0; i < MSE_MAX_ELEM; i++)
        if (g_el[i].used && g_el[i].key == key) return &g_el[i];
    if (!create) return 0;
    for (int i = 0; i < MSE_MAX_ELEM; i++) {
        if (g_el[i].used) continue;
        memset(&g_el[i], 0, sizeof g_el[i]);
        g_el[i].used = 1;
        g_el[i].key = key;
        g_el[i].paused = 1;
        g_el[i].volume = 1.0;
        g_el[i].vsb = g_el[i].asb = 0;
        g_el[i].snd = -1;
        g_el[i].net_state = NETWORK_EMPTY;
        return &g_el[i];
    }
    return 0;
}

static void mel_teardown_decoders(melem *el)
{
    if (el->d4) { h264_close(el->d4); el->d4 = 0; }
    if (el->d5) { h265_close(el->d5); el->d5 = 0; }
    if (el->aac) { aac_close(el->aac); el->aac = 0; }
    if (el->mp3) { mp3_close(el->mp3); el->mp3 = 0; }
    if (el->snd >= 0 && g_plat->snd_close) g_plat->snd_close(el->snd, 0);
    el->snd = -1;
    el->hdr_sent = 0;
}

void mel_free_all(void)
{
    for (int i = 0; i < MSE_MAX_ELEM; i++) {
        if (!g_el[i].used) continue;
        mel_teardown_decoders(&g_el[i]);
        if (g_el[i].rgba) free(g_el[i].rgba);
        if (g_el[i].nal) free(g_el[i].nal);
        if (g_el[i].ms) { g_el[i].ms->el = 0; g_el[i].ms->state = MSE_CLOSED; }
        memset(&g_el[i], 0, sizeof g_el[i]);
    }
    for (int i = 0; i < MSE_MAX_URLS; i++) { g_urls[i].ms = 0; g_urls[i].id = 0; }
}

int mse_attach(msource *ms, melem *el)
{
    if (!ms || !el) return MSE_E_INVALIDSTATE;
    if (ms->el && ms->el != el) return MSE_E_INVALIDSTATE;
    ms->el = el;
    el->ms = ms;
    ms->state = MSE_OPEN;                 /* what makes `sourceopen` fire */
    el->net_state = NETWORK_LOADING;
    el->err_code = 0;
    el->err_msg[0] = 0;
    return MSE_OK;
}

void mse_detach(msource *ms)
{
    if (!ms) return;
    if (ms->el) ms->el->ms = 0;
    ms->el = 0;
    ms->state = MSE_CLOSED;
}

int mel_attach_url(melem *el, const char *url)
{
    if (!el) return MSE_E_INVALIDSTATE;
    msource *ms = mse_from_object_url(url);
    if (!ms) {
        /* A src this browser's media stack cannot load. MEDIA_ERR_SRC_NOT_
         * SUPPORTED is the honest code: there is no progressive <video src>
         * loader here, only MSE. Saying so beats a black box. */
        el->err_code = 4;
        {
            const char *m = "only a MediaSource object URL is a loadable src here";
            int i = 0;
            while (m[i] && i < (int)sizeof el->err_msg - 1) { el->err_msg[i] = m[i]; i++; }
            el->err_msg[i] = 0;
        }
        el->net_state = NETWORK_NO_SOURCE;
        return MSE_E_NOTSUPPORTED;
    }
    return mse_attach(ms, el);
}

void mel_load(melem *el)
{
    if (!el) return;
    mel_teardown_decoders(el);
    el->vcursor = el->acursor = 0;
    el->current_ns = 0;
    el->vfirst_ns = 0;
    el->anchored = 0;
    el->waiting = 0;
    el->nlen = el->noff = 0;
    el->vpend_head = el->vpend_n = 0;
    el->aframes_written = 0;
    el->awritten_ns = 0;
    el->afirst_ns = 0;
    el->aanchored = 0;
    el->adone = 0;
    el->vcodec = el->acodec = 0;
    el->ready_state = HAVE_NOTHING;
    el->have_frame = 0;
    el->ended = 0;
    el->paused = 1;
    el->playing = 0;
    memset(&el->clk, 0, sizeof el->clk);
    memset(&el->st, 0, sizeof el->st);
}

int    mel_paused(const melem *el) { return el ? el->paused : 1; }
int    mel_ended(const melem *el) { return el ? el->ended : 0; }
int    mel_seeking(const melem *el) { return el ? el->seeking : 0; }
double mel_current_time(const melem *el) { return el ? (double)el->current_ns / 1e9 : 0.0; }
int    mel_ready_state(const melem *el) { return el ? el->ready_state : HAVE_NOTHING; }
int    mel_network_state(const melem *el) { return el ? el->net_state : NETWORK_EMPTY; }
double mel_volume(const melem *el) { return el ? el->volume : 1.0; }
int    mel_muted(const melem *el) { return el ? el->muted : 0; }
int    mel_video_width(const melem *el) { return el ? el->fw : 0; }
int    mel_video_height(const melem *el) { return el ? el->fh : 0; }
int    mel_error(const melem *el) { return el ? el->err_code : 0; }
const char *mel_error_message(const melem *el) { return el ? el->err_msg : ""; }

void mel_set_volume(melem *el, double v)
{
    if (!el) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    el->volume = v;
}
void mel_set_muted(melem *el, int m) { if (el) el->muted = m ? 1 : 0; }

void mel_get_stats(const melem *el, struct mel_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!el) return;
    *out = el->st;
    out->resyncs = el->clk.resyncs;
    out->frames_dropped = el->clk.frames_dropped;
    out->drift_mean_ns = el->clk.drift_n ? el->clk.drift_sum_ns / el->clk.drift_n : 0;
    out->drift_max_ns = el->clk.drift_max_ns;
    out->drift_min_ns = el->clk.drift_min_ns;
    if (el->ms) {
        for (int i = 0; i < el->ms->nsb; i++) {
            out->appends += el->ms->sb[i]->appends;
            out->bytes_appended += el->ms->sb[i]->bytes_appended;
            out->reparses += el->ms->sb[i]->reparses;
        }
    }
}

double mel_duration(const melem *el)
{
    if (!el) return -1;
    if (el->ms && el->ms->duration >= 0) return el->ms->duration;
    int n = mel_buffered_count(el);
    double s, e;
    if (n > 0 && mel_buffered_range(el, n - 1, &s, &e)) return e;
    return -1;
}

int mel_buffered_count(const melem *el)
{
    if (!el || !el->ms) return 0;
    /* The element's buffered is the INTERSECTION of its source buffers: a
     * moment is playable only when every track has it. With one buffer it is
     * that buffer's ranges, which is the case a single-file MSE page hits. */
    if (el->ms->nsb == 1) return sb_buffered_count(el->ms->sb[0]);
    int best = 0;
    for (int i = 0; i < el->ms->nsb; i++) {
        int n = sb_buffered_count(el->ms->sb[i]);
        if (i == 0 || n < best) best = n;
    }
    return best;
}

int mel_buffered_range(const melem *el, int i, double *start, double *end)
{
    if (!el || !el->ms || el->ms->nsb == 0) return 0;
    double s = 0, e = 0;
    if (!sb_buffered_range(el->ms->sb[0], i, &s, &e)) return 0;
    for (int k = 1; k < el->ms->nsb; k++) {
        double s2, e2;
        if (!sb_buffered_range(el->ms->sb[k], i, &s2, &e2)) return 0;
        if (s2 > s) s = s2;
        if (e2 < e) e = e2;
    }
    if (e < s) e = s;
    if (start) *start = s;
    if (end) *end = e;
    return 1;
}

/* ---- picking the tracks ---- */
static void mel_bind_tracks(melem *el)
{
    if (!el->ms) return;
    el->vsb = el->asb = 0;
    for (int i = 0; i < el->ms->nsb; i++) {
        sbuf *sb = el->ms->sb[i];
        reparse(sb);
        if (!el->vsb && sb->vtrack >= 0) el->vsb = sb;
        if (!el->asb && sb->atrack >= 0) el->asb = sb;
    }
}

/* ---- colour: BT.601 studio swing, integer only ---- */
static int clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void yuv420_to_rgba(const unsigned char *yp, const unsigned char *up,
                           const unsigned char *vp, int sy, int sc,
                           int w, int h, unsigned char *dst)
{
    for (int y = 0; y < h; y++) {
        const unsigned char *ly = yp + (long)y * sy;
        const unsigned char *lu = up + (long)(y / 2) * sc;
        const unsigned char *lv = vp + (long)(y / 2) * sc;
        unsigned char *o = dst + (long)y * w * 4;
        for (int x = 0; x < w; x++) {
            int c = ly[x] - 16, d = lu[x / 2] - 128, e = lv[x / 2] - 128;
            o[0] = (unsigned char)clip8((298 * c + 409 * e + 128) >> 8);
            o[1] = (unsigned char)clip8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[2] = (unsigned char)clip8((298 * c + 516 * d + 128) >> 8);
            o[3] = 255;
            o += 4;
        }
    }
}

static int mel_store_frame(melem *el, const unsigned char *y, const unsigned char *u,
                           const unsigned char *v, int sy, int sc, int w, int h)
{
    if (w <= 0 || h <= 0 || w > MEDIA_MAX_DIM || h > MEDIA_MAX_DIM) return 0;
    long need = (long)w * h * 4;
    if (need > el->fcap) {
        unsigned char *nb = realloc(el->rgba, (size_t)need);
        if (!nb) return 0;
        el->rgba = nb;
        el->fcap = (int)need;
    }
    yuv420_to_rgba(y, u, v, sy, sc, w, h, el->rgba);
    el->fw = w; el->fh = h;
    el->have_frame = 1;
    el->frame_new = 1;
    return 1;
}

/* ---- open the decoder the container asked for ---- */
static int mel_open_video(melem *el)
{
    if (!el->vsb || el->vsb->vtrack < 0 || !el->vsb->dm) return 0;
    const media_track *t = media_track_info(el->vsb->dm, el->vsb->vtrack);
    if (!t) return 0;
    if (el->vcodec == (int)t->codec && (el->d4 || el->d5)) return 1;
    if (t->codec != MEDIA_CODEC_H264 && t->codec != MEDIA_CODEC_H265) {
        el->err_code = 4;
        {
            const char *m = "no decoder for this video codec";
            int i = 0;
            while (m[i] && i < (int)sizeof el->err_msg - 1) { el->err_msg[i] = m[i]; i++; }
            el->err_msg[i] = 0;
        }
        return 0;
    }
    el->vcodec = (int)t->codec;
    if (t->codec == MEDIA_CODEC_H265) el->d5 = h265_open();
    else                              el->d4 = h264_open();
    if (!el->d4 && !el->d5) { el->err_code = 3; return 0; }
    el->hdr_sent = 0;
    if (t->width > 0 && t->height > 0 && !el->have_frame) { el->fw = t->width; el->fh = t->height; }
    return 1;
}

static int mel_open_audio(melem *el)
{
    if (!el->asb || el->asb->atrack < 0 || !el->asb->dm) return 0;
    const media_track *t = media_track_info(el->asb->dm, el->asb->atrack);
    if (!t) return 0;
    if (el->acodec == (int)t->codec && (el->aac || el->mp3)) return 1;
    if (t->codec == MEDIA_CODEC_AAC) {
        int err = 0;
        el->aac = aac_open_asc(t->extradata, t->extradata_len, &err);
        if (!el->aac) return 0;
        aac_info(el->aac, &el->arate, &el->ach);
    } else if (t->codec == MEDIA_CODEC_MP3) {
        el->mp3 = mp3_open();
        if (!el->mp3) return 0;
        el->arate = t->rate;
        el->ach = t->channels;
    } else {
        return 0;                       /* named on the status line, not fatal */
    }
    el->acodec = (int)t->codec;
    return 1;
}

/* ---- audio playout: write ahead of the picture, but only so far ---- */
static void mel_pump_audio(melem *el, long long upto_ns)
{
    if (!el->asb || (!el->aac && !el->mp3)) return;
    if (el->adone) return;
    sbuf *sb = el->asb;
    if (!sb->dm) return;
    const media_track *t = media_track_info(sb->dm, sb->atrack);
    if (!t) return;

    static short pcm[ABUF_FRAMES * 2];
    /* THE BOUND IS IN ABSOLUTE MEDIA TIME, and getting that wrong deadlocks the
     * player. `awritten_ns` counts from the FIRST AUDIO SAMPLE; the video's
     * presentation times count from the first PICTURE, and on a stream with B
     * frames those differ by the composition offset -- 133 ms on the DASH
     * fixture. Comparing one against the other wrote audio only ~17 ms past the
     * picture, so the card's play cursor (which IS the master clock) caught up
     * and stopped; video then waited for a clock that could only advance if
     * more audio were written, which needed the video to advance. On the
     * machine that looked like a decoder that died after five frames. */
    while (el->afirst_ns + el->awritten_ns < upto_ns) {
        if (el->acursor >= t->nsamples) {
            reparse(sb);
            t = media_track_info(sb->dm, sb->atrack);
            if (!t || el->acursor >= t->nsamples) break;
        }
        media_sample s;
        if (media_get_sample(sb->dm, sb->atrack, el->acursor, &s) != 1) break;
        long long at = pres_ns(sb, sb->atrack, el->acursor, &s);
        el->acursor++;
        if (in_removed(sb, at)) continue;

        const float *f = 0;
        int nsmp = 0, ch = 0, rate = 0, got = 0;
        if (el->aac) {
            aacframe fr;
            int rc = aac_decode_raw(el->aac, s.data, s.size, &fr, &got);
            if (rc < 0) continue;
            if (!got) continue;
            f = fr.pcm; nsmp = fr.nsamples; ch = fr.channels; rate = fr.rate;
        } else {
            mp3frame fr;
            long off = 0;
            while (off < s.size) {
                int g = 0;
                int rc = mp3_decode(el->mp3, s.data + off, s.size - off, &fr, &g);
                if (rc <= 0) break;
                off += rc;
                if (g) { f = fr.pcm; nsmp = fr.nsamples; ch = fr.channels; rate = fr.rate; got = 1; break; }
            }
            if (!got) continue;
        }
        if (ch > 2) ch = 2;
        if (el->snd < 0 && g_plat->snd_open && rate > 0) {
            el->arate = rate; el->ach = ch;
            el->snd = g_plat->snd_open(rate, ch);
        }
        if (el->arate <= 0) el->arate = rate;
        if (el->ach <= 0) el->ach = ch;

        /* float -> s16 with the element's volume folded in. Muting writes
         * SILENCE rather than not writing: the card's play cursor is the master
         * clock, and a muted element that stopped feeding it would stop the
         * clock and freeze the video. */
        double vol = el->muted ? 0.0 : el->volume;
        int n = nsmp;
        if (n > ABUF_FRAMES) n = ABUF_FRAMES;
        for (int i = 0; i < n; i++)
            for (int c = 0; c < ch; c++) {
                double v = (double)f[(long)i * ch + c] * vol * 32767.0;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[i * ch + c] = (short)v;
            }
        if (el->snd >= 0 && g_plat->snd_write) {
            int want = n * ch * 2, off = 0;
            while (off < want) {
                int room = g_plat->snd_avail ? g_plat->snd_avail(el->snd) : want;
                if (room <= 0) break;
                int k = g_plat->snd_write(el->snd, (const char *)pcm + off, want - off);
                if (k <= 0) break;
                off += k;
            }
        }
        if (!el->aanchored) { el->afirst_ns = at; el->aanchored = 1; }
        el->aframes_written += n;
        el->st.audio_frames_written += n;
        if (el->arate > 0)
            el->awritten_ns = el->aframes_written * 1000000000LL / el->arate;
        (void)at;
    }
    if (el->asb) {
        const media_track *t2 = media_track_info(sb->dm, sb->atrack);
        if (t2 && el->acursor >= t2->nsamples && el->ms && el->ms->state == MSE_ENDED)
            el->adone = 1;
    }
}

/* WHERE THE CARD IS, IN THE FILE'S OWN TIMELINE. It is `afirst + played`, and
 * the anchor has to be the first AUDIO sample's presentation time, not the
 * first video frame's. Those differ by exactly the composition offset a stream
 * with B frames has -- 133 ms on this fixture -- and using the video anchor
 * made every picture look 133 ms late, so a quarter of them were dropped and
 * the drift reported -43 ms while nothing was actually out of sync. */
static long long mel_audio_played_ns(melem *el)
{
    if (el->snd < 0 || !g_plat->snd_played || el->arate <= 0 || !el->aanchored) return -1;
    long long fr = g_plat->snd_played(el->snd);
    if (fr < 0) return -1;
    return el->afirst_ns + fr * 1000000000LL / el->arate;
}

/* ---- present the current frame into the box the painter last reported ---- */
static void mel_present(melem *el)
{
    if (!el->box_valid || !el->have_frame || !g_plat->blit) return;
    if (g_plat->clip) g_plat->clip(el->cx, el->cy, el->cw, el->ch);
    g_plat->blit(el->bx, el->by, el->bw, el->bh, el->rgba, el->fw, el->fh);
    if (g_plat->clip) g_plat->clip(0, 0, 0, 0);
    if (g_plat->flush) g_plat->flush();
}

void media_paint_key(int key, int x, int y, int w, int h,
                     int clip_x, int clip_y, int clip_w, int clip_h)
{
    melem *el = mel_for_key(key, 0);
    if (!el) {
        if (g_plat->fill) g_plat->fill(x, y, w, h, 0x000000);
        return;
    }
    el->bx = x; el->by = y; el->bw = w; el->bh = h;
    el->cx = clip_x; el->cy = clip_y; el->cw = clip_w; el->ch = clip_h;
    el->box_valid = 1;
    if (el->have_frame && g_plat->blit) {
        /* Aspect-fit inside the border box, letterboxed on black -- what a
         * <video> does with object-fit: contain, which is the default. */
        int dw = w, dh = (int)((long long)w * el->fh / (el->fw ? el->fw : 1));
        if (dh > h) { dh = h; dw = (int)((long long)h * el->fw / (el->fh ? el->fh : 1)); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (g_plat->fill && (dw < w || dh < h)) g_plat->fill(x, y, w, h, 0x000000);
        g_plat->blit(x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, el->rgba, el->fw, el->fh);
    } else if (g_plat->fill) {
        g_plat->fill(x, y, w, h, 0x000000);
    }
}

/* ---- playback control ---- */
int mel_play(melem *el)
{
    if (!el) return MSE_E_INVALIDSTATE;
    el->paused = 0;
    el->ended = 0;
    if (!el->clk.started) avclock_init(&el->clk, el->asb != 0);
    return MSE_OK;
}
void mel_pause(melem *el) { if (el) { el->paused = 1; el->playing = 0; } }

int mel_seek(melem *el, double sec)
{
    if (!el) return MSE_E_INVALIDSTATE;
    long long want = (long long)(sec * 1e9);
    if (want < 0) want = 0;
    mel_bind_tracks(el);
    /* A seek restarts decode at the last keyframe at or before the target, and
     * every decoder here is stateful -- so it is a teardown, not a rewind. */
    el->seeking = 1;
    if (el->vsb && el->vsb->dm && el->vsb->vtrack >= 0) {
        const media_track *t = media_track_info(el->vsb->dm, el->vsb->vtrack);
        long best = 0;
        media_sample s;
        for (long k = 0; t && k < t->nsamples; k++) {
            if (media_get_sample(el->vsb->dm, el->vsb->vtrack, k, &s) != 1) break;
            if (!s.keyframe) continue;
            if (pres_ns(el->vsb, el->vsb->vtrack, k, &s) <= want) best = k;
            else break;
        }
        el->vcursor = best;
    }
    if (el->asb && el->asb->dm && el->asb->atrack >= 0) {
        const media_track *t = media_track_info(el->asb->dm, el->asb->atrack);
        long best = 0;
        media_sample s;
        for (long k = 0; t && k < t->nsamples; k++) {
            if (media_get_sample(el->asb->dm, el->asb->atrack, k, &s) != 1) break;
            if (pres_ns(el->asb, el->asb->atrack, k, &s) <= want) best = k;
            else break;
        }
        el->acursor = best;
    }
    if (el->d4) { h264_close(el->d4); el->d4 = h264_open(); }
    if (el->d5) { h265_close(el->d5); el->d5 = h265_open(); }
    el->hdr_sent = 0;
    el->nlen = el->noff = 0;
    el->vpend_head = el->vpend_n = 0;
    el->waiting = 0;
    el->current_ns = want;
    el->vfirst_ns = want;
    el->anchored = 1;
    el->aframes_written = 0;
    el->awritten_ns = 0;
    el->afirst_ns = 0;
    el->aanchored = 0;
    el->adone = 0;
    el->ended = 0;
    avclock_init(&el->clk, el->asb != 0);
    return MSE_OK;
}

/* ---- the video step ----------------------------------------------------
 * Feed access units until the decoder emits a picture, then ask the clock what
 * to do with it. Returns 1 when a NEW picture was stored.
 *
 * TWO RULES FROM PREVIEW, AND THEY ARE NOT OPTIONAL:
 *   - A decoder emits a picture when it sees the START OF THE NEXT one, so a
 *     call that hands over access unit N+1 typically returns picture N having
 *     consumed only a few bytes. The loop must keep feeding from the same
 *     buffer until it is empty; moving on to N+2 throws away half the stream
 *     and looks exactly like a stream that plays at half the frame rate.
 *   - A late frame is DECODED and not painted. Skipping the decode corrupts
 *     everything until the next keyframe, because a P frame is the next one's
 *     reference. */
static int nal_room(melem *el, long need)
{
    if (need <= el->ncap) return 1;
    if (need > MSE_NALBUF) return 0;             /* an absurd access unit */
    long want = el->ncap ? el->ncap : (256L * 1024);
    while (want < need) want *= 2;
    unsigned char *nb = realloc(el->nal, (size_t)want);
    if (!nb) return 0;
    el->nal = nb;
    el->ncap = want;
    return 1;
}

/* THE THREE OUTCOMES OF A DECODE CALL, AND THEY ARE NOT TWO.
 *
 *   used > 0            bytes were taken; advance by them, picture or not.
 *   used == 0 && got    a picture came out of the DPB and NOTHING was consumed.
 *                       The same bytes must be handed over again. This is the
 *                       normal way a reordered stream drains: one call in,
 *                       several pictures out.
 *   used == 0 && !got   no progress at all -- the buffer does not contain a
 *                       whole access unit the decoder can use. Skip it, or the
 *                       loop spins for ever.
 *
 * Folding the middle case into the last one costs whole access units: the
 * fixture decoded 54 of its 60 pictures and every counter looked healthy,
 * because the frames were not corrupted, they were never fed. */
static void nal_advance(melem *el, int used, int got)
{
    if (used > 0) el->noff += used;
    else if (!got) el->noff = el->nlen;
}

/* A picture came out of a decoder: store it, stamp it, ask the clock. Returns
 * AV_SHOW / AV_WAIT / AV_DROP, or -1 when the frame could not be stored. */
static int mel_emit(melem *el, long long pts, long long now,
                    const unsigned char *y, const unsigned char *u,
                    const unsigned char *v, int sy, int sc, int w, int h)
{
    if (!mel_store_frame(el, y, u, v, sy, sc, w, h)) return -1;
    el->st.frames_decoded++;
    if (!el->anchored) { el->vfirst_ns = pts; el->anchored = 1; }
    el->current_ns = pts;

    long long played = mel_audio_played_ns(el);
    if (played >= 0) avclock_audio(&el->clk, played);
    long long sleep_ns = 0;
    int what = avclock_frame(&el->clk, pts, now, &sleep_ns);
    if (what == AV_SHOW) {
        el->st.frames_shown++;
        el->ready_state = HAVE_ENOUGH_DATA;
    } else {
        el->frame_new = 0;
        el->wait_pts = pts;
    }
    return what;
}

/* An EARLY frame is held, not thrown away. mel_emit already advanced the
 * decoder past it (decode runs ahead, display does not), so without this the
 * picture would sit in el->rgba, never be painted, and the NEXT one would take
 * its slot -- half the frames shown, at the right times, which looks like a
 * slow decoder rather than a bug. Returns 1 when the held frame became due. */
static int mel_retry_held(melem *el, long long now)
{
    if (!el->pending_wait) return 0;
    long long played = mel_audio_played_ns(el);
    if (played >= 0) avclock_audio(&el->clk, played);
    long long sleep_ns = 0;
    int what = avclock_frame(&el->clk, el->wait_pts, now, &sleep_ns);
    if (what == AV_WAIT) return 0;
    el->pending_wait = 0;
    if (what == AV_DROP) return 0;
    el->st.frames_shown++;
    el->frame_new = 1;
    el->ready_state = HAVE_ENOUGH_DATA;
    return 1;
}

static int mel_step_video(melem *el, long long now)
{
    if (!el->vsb) return 0;
    sbuf *sb = el->vsb;
    if (!sb->dm && !reparse(sb)) return 0;
    if (!mel_open_video(el)) return 0;

    const media_track *t = media_track_info(sb->dm, sb->vtrack);
    if (!t) return 0;

    if (el->pending_wait) return mel_retry_held(el, now);

    if (!el->hdr_sent) {
        long need = media_annexb_headers(sb->dm, sb->vtrack, 0, 0);
        if (need > 0 && nal_room(el, need)) {
            long hn = media_annexb_headers(sb->dm, sb->vtrack, el->nal, el->ncap);
            el->nlen = hn > 0 ? hn : 0;
        } else {
            el->nlen = 0;
        }
        el->noff = 0;
        el->cur_at = H264_NOPTS;        /* parameter sets are not a picture */
        el->hdr_sent = 1;
    }

    for (int guard = 0; guard < 4096; guard++) {
        int got = 0;
        long long pts = el->current_ns;

        if (el->noff < el->nlen) {
            /* KEEP FEEDING FROM THE SAME BUFFER UNTIL IT IS EMPTY. Both
             * decoders emit a picture when they see the START of the next one,
             * so the call that hands over access unit N+1 typically returns
             * picture N having consumed only a few bytes. A loop that then
             * moved on to N+2 would throw away everything after them -- it does
             * not look like a bug, it looks like a stream that plays at half
             * the frame rate with the timestamps still perfectly paced. */
            int used;
            if (el->d5) {
                h265frame f;
                used = h265_decode(el->d5, el->nal + el->noff,
                                   (int)(el->nlen - el->noff), &f, &got);
                if (used < 0) { el->err_code = 3; el->events |= MEV_ERROR; return 0; }
                nal_advance(el, used, got);
                if (got) {
                    /* h265_decode carries no opaque through, so this pairs the
                     * picture with the access unit's own stamp via a decode-order
                     * FIFO -- correct only while decode order IS display order.
                     * See the handover note in js_media.h. */
                    pts = el->vpend_n > 0 ? el->vpend[el->vpend_head] : el->current_ns;
                    if (el->vpend_n > 0) {
                        el->vpend_head = (el->vpend_head + 1) % MSE_PTSQ;
                        el->vpend_n--;
                    }
                    int what = mel_emit(el, pts, now, f.y, f.u, f.v,
                                        f.stride_y, f.stride_c, f.width, f.height);
                    if (what < 0) return 0;
                    if (what == AV_WAIT) { el->pending_wait = 1; return 0; }
                    if (what == AV_DROP) continue;
                    return 1;
                }
            } else {
                h264frame f;
                /* THE SAME STAMP ON EVERY CALL FOR THIS ACCESS UNIT, not
                 * NOPTS on the continuation. h264_decode_pts attaches the value
                 * to the picture whose data STARTS in the call -- and a picture
                 * can start part-way through the buffer, in which case NOPTS
                 * made it inherit the PREVIOUS frame's time. On this fixture
                 * that produced three pictures stamped 1.133 s at every segment
                 * boundary: the video fell a fifth of a second behind and the
                 * clock dropped frames to catch up, so the symptom was jerky
                 * playback with a -42 ms mean drift and nothing obviously
                 * wrong. */
                used = h264_decode_pts(el->d4, el->nal + el->noff,
                                       (int)(el->nlen - el->noff), (int64_t)el->cur_at,
                                       &f, &got);
                if (used < 0) { el->err_code = 3; el->events |= MEV_ERROR; return 0; }
                nal_advance(el, used, got);
                if (got) {
                    pts = (f.pts == H264_NOPTS) ? el->current_ns : (long long)f.pts;
                    int what = mel_emit(el, pts, now, f.y, f.u, f.v,
                                        f.stride_y, f.stride_c, f.width, f.height);
                    if (what < 0) return 0;
                    if (what == AV_WAIT) { el->pending_wait = 1; return 0; }
                    if (what == AV_DROP) continue;
                    return 1;
                }
            }
            continue;
        }

        /* The staging buffer is empty: take the next sample. */
        if (el->vcursor >= t->nsamples) {
            reparse(sb);
            t = media_track_info(sb->dm, sb->vtrack);
            if (!t) return 0;
        }
        if (el->vcursor >= t->nsamples) {
            if (!el->ms || el->ms->state != MSE_ENDED) {
                /* Underrun: the page has not appended enough yet. That is
                 * `waiting`, not the end -- MSE's whole point is that more is
                 * coming. */
                el->ready_state = el->have_frame ? HAVE_CURRENT_DATA : HAVE_METADATA;
                if (!el->waiting) { el->waiting = 1; el->events |= MEV_WAITING; }
                return 0;
            }
            /* endOfStream and no samples left: drain the DPB. With B frames the
             * last pictures are still inside the decoder. */
            if (el->d5) {
                h265frame f;
                if (h265_flush(el->d5, &f) != 1) { el->ended = 1; el->playing = 0;
                                                   el->events |= MEV_ENDED; return 0; }
                pts = el->vpend_n > 0 ? el->vpend[el->vpend_head] : el->current_ns;
                if (el->vpend_n > 0) { el->vpend_head = (el->vpend_head + 1) % MSE_PTSQ; el->vpend_n--; }
                int what = mel_emit(el, pts, now, f.y, f.u, f.v,
                                    f.stride_y, f.stride_c, f.width, f.height);
                if (what < 0) return 0;
                if (what == AV_WAIT) { el->pending_wait = 1; return 0; }
                if (what == AV_DROP) continue;
                return 1;
            } else {
                h264frame f;
                if (h264_flush(el->d4, &f) != 1) { el->ended = 1; el->playing = 0;
                                                   el->events |= MEV_ENDED; return 0; }
                pts = (f.pts == H264_NOPTS) ? el->current_ns : (long long)f.pts;
                int what = mel_emit(el, pts, now, f.y, f.u, f.v,
                                    f.stride_y, f.stride_c, f.width, f.height);
                if (what < 0) return 0;
                if (what == AV_WAIT) { el->pending_wait = 1; return 0; }
                if (what == AV_DROP) continue;
                return 1;
            }
        }

        media_sample s;
        if (media_get_sample(sb->dm, sb->vtrack, el->vcursor, &s) != 1) return 0;
        long long at = pres_ns(sb, sb->vtrack, el->vcursor, &s);
        el->vcursor++;
        if (el->waiting) { el->waiting = 0; el->events |= MEV_CANPLAY; }
        if (in_removed(sb, at)) continue;

        long need = media_to_annexb(sb->dm, &s, 0, 0);
        if (need < 0 || !nal_room(el, need)) { el->err_code = 3; el->events |= MEV_ERROR; return 0; }
        long n = media_to_annexb(sb->dm, &s, el->nal, el->ncap);
        if (n < 0) { el->err_code = 3; el->events |= MEV_ERROR; return 0; }
        el->nlen = n;
        el->noff = 0;
        el->cur_at = at;

        if (el->d5) {
            /* Remember the stamp for the picture this access unit will become. */
            if (el->vpend_n < MSE_PTSQ)
                el->vpend[(el->vpend_head + el->vpend_n++) % MSE_PTSQ] = at;
        } else {
            /* THE PTS GOES IN WITH THE BYTES. h264_decode_pts carries the
             * opaque through the DPB and hands it back on the picture it
             * belongs to however far out of decode order that comes -- which
             * is the whole point, because High profile means B frames and a
             * decode-order FIFO shuffles their times without losing a frame,
             * i.e. it fails in the way that is hardest to see. */
            int g2 = 0;
            h264frame f;
            int used = h264_decode_pts(el->d4, el->nal, (int)el->nlen, (int64_t)at, &f, &g2);
            if (used < 0) { el->err_code = 3; el->events |= MEV_ERROR; return 0; }
            el->noff = 0;
            nal_advance(el, used, g2);
            if (g2) {
                pts = (f.pts == H264_NOPTS) ? at : (long long)f.pts;
                int what = mel_emit(el, pts, now, f.y, f.u, f.v,
                                    f.stride_y, f.stride_c, f.width, f.height);
                if (what < 0) return 0;
                if (what == AV_WAIT) { el->pending_wait = 1; return 0; }
                if (what == AV_DROP) continue;
                return 1;
            }
        }
    }
    return 0;
}

unsigned mel_take_events(melem *el)
{
    if (!el) return 0;
    unsigned e = el->events;
    el->events = 0;
    return e;
}

melem *mel_at(int i)
{
    if (i < 0 || i >= MSE_MAX_ELEM) return 0;
    return g_el[i].used ? &g_el[i] : 0;
}

int mel_pending(void)
{
    for (int i = 0; i < MSE_MAX_ELEM; i++) {
        const melem *el = &g_el[i];
        if (!el->used) continue;
        if (el->events) return 1;
        if (el->ms && !el->paused && !el->ended) return 1;
        /* A source with buffers but no metadata yet still owes
         * `loadedmetadata`, so it has to be looked at once more. */
        if (el->ms && el->ms->nsb && el->ready_state == HAVE_NOTHING) return 1;
    }
    return 0;
}

int media_pump(void)
{
    int painted = 0;
    long long now = (long long)now_ns();
    for (int i = 0; i < MSE_MAX_ELEM; i++) {
        melem *el = &g_el[i];
        if (!el->used || !el->ms) continue;
        mel_bind_tracks(el);

        if (el->ready_state == HAVE_NOTHING && (el->vsb || el->asb)) {
            int v = mel_open_video(el);
            int a = mel_open_audio(el);
            if (v || a) {
                el->ready_state = HAVE_METADATA;
                el->events |= MEV_LOADEDMETADATA | MEV_DURATIONCHANGE;
                if (!el->clk.started) avclock_init(&el->clk, el->asb != 0);
            } else if (el->err_code) {
                el->events |= MEV_ERROR;
            }
        }
        if (el->paused || el->ended) continue;
        if (!el->playing) { el->playing = 1; el->events |= MEV_PLAYING; }
        /* NOTE: pending_wait is NOT cleared here. It says "a decoded picture is
         * being held until it is due", and clearing it every pump threw that
         * picture away and decoded the next one instead -- 54 pictures decoded,
         * ONE painted, and every counter reporting a healthy stream. */

        /* RULE ONE: audio is written ahead of the DECODED picture position and
         * never further. That cushion is what keeps the two streams together
         * when decode falls behind, which on this machine it does. Both sides
         * of the comparison are absolute presentation times -- see the note in
         * mel_pump_audio. */
        mel_pump_audio(el, el->current_ns + AV_LEAD_NS);

        long long before = el->current_ns;
        if (mel_step_video(el, now)) {
            mel_present(el);
            painted++;
        }
        if (el->current_ns != before &&
            el->current_ns - el->last_timeupdate_ns >= 250000000LL) {
            el->last_timeupdate_ns = el->current_ns;
            el->events |= MEV_TIMEUPDATE;
        }
        if (el->seeking) { el->seeking = 0; el->events |= MEV_SEEKED; }
    }
    return painted;
}
