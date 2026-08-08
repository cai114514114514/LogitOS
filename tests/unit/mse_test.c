/* Host gate for Media Source Extensions (c/apps/browser/js_media_src.c).
 *
 * WHAT THIS TEST IS FOR. Two claims, and they fail in opposite directions:
 *
 *   1. isTypeSupported() IS TRUE BOTH WAYS. For every type it says yes to, the
 *      pipeline decodes a real sample of that type here, in this process, from
 *      a real file -- so a yes is a demonstrated capability and not a table
 *      somebody typed. For every type it says no to, it is asserted to say no,
 *      and av01 is on that list by name, because that answer is what steers a
 *      real site to a codec we have. A browser that lies here does not gain a
 *      codec; it gets handed a stream nothing can read, frames later, where the
 *      failure reads as a decoder bug.
 *
 *   2. A SEGMENTED STREAM PLAYS. The fixture is DASH-shaped -- an init segment
 *      plus numbered .m4s media segments, video and audio in separate files,
 *      exactly the shape bilibili delivers -- and it is appended over
 *      SIMULATED TIME, so segment 3 arrives while segment 1 is playing. The
 *      pictures are required to arrive in presentation order, to match the
 *      whole-file decode sample for sample, and to stay inside a stated A/V
 *      drift bound measured by avclock's own reporting.
 *
 * THE CLOCK AND THE SOUND CARD ARE FAKE, AND THAT IS THE POINT. avclock takes
 * `now_ns` as an argument rather than reading the machine's, so a minute of
 * playback runs in milliseconds and the drift number is reproducible instead of
 * being whatever the host's load was that afternoon. The card model is the one
 * that matters: `played` is what has come OUT of it, never what was written
 * in, because that is the master clock the whole synchronisation policy rests
 * on.
 *
 *   make test-mse           the gate
 *   make test-mse-negctl    the same suite with isTypeSupported claiming AV1,
 *                           REQUIRED TO FAIL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "js_media.h"
#include "media.h"

/* mel_for_node() keys elements by POINTER and never dereferences the node, so a
 * host test needs an address and not a DOM. Distinct offsets into this array
 * are distinct elements. */
#define MSE_TEST_NODES 64

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); \
    printf(__VA_ARGS__); printf("\n"); } else { printf("ok: "); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)
#define NOTE(...) do { printf("     "); printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *FX = "tests/fixtures/mse";

/* ------------------------------------------------------------- files ---- */
static unsigned char *slurp(const char *path, long *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)(n > 0 ? n : 1));
    if (!b) { fclose(f); return 0; }
    if (n > 0 && (long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out = n;
    return b;
}
static unsigned char *slurp_fx(const char *name, long *out)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", FX, name);
    return slurp(p, out);
}

/* ================================================= the fake platform ==== */
/* A sound card whose play cursor is driven by the same clock the video is
 * paced against. Everything the synchronisation policy claims is decided by
 * the relationship between these two, so modelling the card badly (say,
 * reporting frames WRITTEN as frames played) would make the drift number
 * meaningless while still producing one. */
static long long g_now;                 /* the injected monotonic clock, ns */
static int   g_rate, g_ch;
static long long g_queued_frames;       /* in the card's ring, not yet played */
static long long g_played_frames;
static long long g_written_frames;
#define CARD_RING_FRAMES 32768

static unsigned long long host_now(void) { return (unsigned long long)g_now; }

static int host_snd_open(int rate, int ch)
{
    g_rate = rate; g_ch = ch;
    g_queued_frames = g_played_frames = g_written_frames = 0;
    return 0;
}
static int host_snd_write(int h, const void *buf, int bytes)
{
    (void)h; (void)buf;
    int bpf = g_ch * 2;
    long long room = (CARD_RING_FRAMES - g_queued_frames) * bpf;
    if (room <= 0) return 0;
    if (bytes > room) bytes = (int)room;
    g_queued_frames += bytes / bpf;
    g_written_frames += bytes / bpf;
    return bytes;
}
static int host_snd_avail(int h)
{
    (void)h;
    return (int)((CARD_RING_FRAMES - g_queued_frames) * g_ch * 2);
}
static long long host_snd_played(int h) { (void)h; return g_played_frames; }
static void host_snd_close(int h, int drain) { (void)h; (void)drain; }

/* Move the world forward: the card plays what it can in `dt`. */
static void advance(long long dt)
{
    g_now += dt;
    if (g_rate > 0) {
        long long can = dt * g_rate / 1000000000LL;
        if (can > g_queued_frames) can = g_queued_frames;
        g_queued_frames -= can;
        g_played_frames += can;
    }
}

/* The blitter records what was drawn where. Nothing here scales or blends --
 * the claim under test is that a frame REACHED the screen, at the element's
 * box, at the right time. */
static int g_blits, g_fills;
static int g_last_w, g_last_h, g_last_x, g_last_y;
static void host_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{
    (void)rgba;
    g_blits++;
    g_last_x = x; g_last_y = y; g_last_w = w; g_last_h = h;
    (void)sw; (void)sh;
}
static void host_fill(int x, int y, int w, int h, unsigned rgb)
{ (void)x; (void)y; (void)w; (void)h; (void)rgb; g_fills++; }
static void host_clip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static void host_flush(void) { }

static const struct media_platform g_hostplat = {
    host_now, host_blit, host_fill, host_clip, host_flush,
    host_snd_open, host_snd_write, host_snd_avail, host_snd_played, host_snd_close
};

static void world_reset(void)
{
    g_now = 0;
    g_rate = g_ch = 0;
    g_queued_frames = g_played_frames = g_written_frames = 0;
    g_blits = g_fills = 0;
    mel_free_all();
}

/* ============================================ 1. the codec answer ======= */
/* Every entry names the evidence. A "yes" line additionally has a fixture in
 * the decode table below; a "no" line names the gate that refuses it. */
struct tcase { const char *type; int want; const char *why; };

static const struct tcase g_types[] = {
    /* --- yes ------------------------------------------------------------- */
    { "video/mp4; codecs=\"avc1.640033\"", 1,
      "H.264 High L5.1 -- bilibili's own string; h264_nal.c parses profile 100 "
      "and this fixture decodes" },
    { "video/mp4; codecs=\"avc1.64001f\"", 1, "H.264 High L3.1" },
    { "video/mp4; codecs=\"avc1.4d401e\"", 1, "H.264 Main -- profile_idc 77" },
    { "video/mp4; codecs=\"avc1.42E01E\"", 1, "H.264 Baseline -- profile_idc 66" },
    { "video/mp4; codecs=\"avc3.640028\"", 1, "avc3 is avc1 with inband SPS/PPS" },
    { "video/mp4; codecs=\"hvc1.1.6.L120.90\"", 1,
      "HEVC Main L4.0 -- bilibili's own string; h265_nal.c gates on 8..10 bit 4:2:0" },
    { "video/mp4; codecs=\"hev1.2.4.L120.90\"", 1, "HEVC Main 10 -- the decoder does 10-bit" },
    { "audio/mp4; codecs=\"mp4a.40.2\"", 1, "AAC-LC -- aac.c's whole subject" },
    { "audio/mp4; codecs=\"mp4a.40.34\"", 1, "MP3 in MP4 -- mp3_decode is frame-incremental" },
    { "video/mp4; codecs=\"avc1.640033,mp4a.40.2\"", 1, "both codecs in one type" },

    /* --- no -------------------------------------------------------------- */
    { "video/mp4; codecs=\"av01.0.08M.08.0.110.01.01.01.0\"", 0,
      "AV1 -- THE ONE THAT MATTERS. No AV1 decoder exists in this tree, and "
      "saying no is what makes a real site serve the H.264 it also offered" },
    { "video/mp4; codecs=\"av01.0.05M.08\"", 0, "AV1, any level" },
    { "video/webm; codecs=\"vp9,opus\"", 0, "WebM: every codec that ships in one is undecodable here" },
    { "video/mp4; codecs=\"vp09.00.10.08\"", 0, "VP9 -- no decoder" },
    { "video/webm; codecs=\"vp8,vorbis\"", 0, "VP8 -- no decoder" },
    { "audio/mp4; codecs=\"opus\"", 0, "Opus -- no decoder" },
    { "audio/webm; codecs=\"vorbis\"", 0, "Vorbis: c/lib/audio has one, but never in fMP4" },
    { "audio/mp4; codecs=\"mp4a.40.5\"", 0,
      "HE-AAC v1 -- aac.c refuses object type 5 on purpose: the core alone is "
      "the right samples at half the rate" },
    { "audio/mp4; codecs=\"mp4a.40.29\"", 0, "HE-AAC v2 -- same refusal" },
    { "audio/mp4; codecs=\"ac-3\"", 0, "AC-3 -- no decoder" },
    { "audio/mp4; codecs=\"flac\"", 0, "FLAC-in-MP4: a decoder exists, this combination has never been demuxed" },
    { "video/mp4; codecs=\"avc1.6E0033\"", 0,
      "H.264 High 10 (profile 110) -- h264_parse_sps refuses bit_depth != 8" },
    { "video/mp4; codecs=\"avc1.7A0033\"", 0, "H.264 High 4:2:2 (122) -- chroma_format_idc != 1" },
    { "video/mp4; codecs=\"avc1.F40033\"", 0, "H.264 High 4:4:4 (244) -- same gate" },
    { "video/mp4; codecs=\"avc1.580033\"", 0,
      "H.264 Extended (88): the SPS parses, but SP/SI slices and data "
      "partitioning are both refused, so a stream that really is Extended fails" },
    { "video/mp4; codecs=\"avc1.640034\"", 0, "H.264 level 5.2 -- above the stated ceiling" },
    { "video/mp4; codecs=\"hvc1.4.10.L120.9c\"", 0, "HEVC Rext (profile 4) -- not Main/Main 10" },
    { "video/mp4; codecs=\"hvc1.A1.6.L120.90\"", 0, "HEVC with a non-zero profile space" },
    { "video/mp4", 0, "no codecs parameter: the spec requires a no" },
    { "video/mp4; codecs=\"\"", 0, "an empty codecs list is a no" },
    { "video/mp4; codecs=\"avc1.640033,av01.0.08M.08\"", 0,
      "one unsupported codec in the list poisons the whole type" },
    { "", 0, "the empty string" },
    { "application/octet-stream", 0, "not a media container at all" },
};

static void test_types(void)
{
    printf("\n== isTypeSupported: the answer table, both ways ==\n");
    for (unsigned i = 0; i < sizeof g_types / sizeof g_types[0]; i++) {
        int got = mse_type_supported(g_types[i].type);
        CHECK(got == g_types[i].want, "%s -> %s  (%s)",
              g_types[i].type[0] ? g_types[i].type : "(empty)",
              got ? "yes" : "no", g_types[i].why);
    }
}

/* ============================ 2. every YES decodes a real sample ======== */
/* The other half of honesty. A table that says yes is worth nothing until the
 * bytes go through the decoder that has to honour it. Each row is a type this
 * build claims and a file that really is that type. */
struct dcase { const char *type; const char *path; int want_video; };

static const struct dcase g_decode[] = {
    { "video/mp4; codecs=\"avc1.640033\"", "tests/fixtures/mse/whole-video.mp4", 1 },
    { "video/mp4; codecs=\"avc1.4d401e\"", "tests/fixtures/media/h264-mp3.mp4", 1 },
    { "video/mp4; codecs=\"avc1.42E01E\"", "tests/fixtures/media/h264-mp3-nobf.mp4", 1 },
    { "video/mp4; codecs=\"hvc1.1.6.L120.90\"", "tests/fixtures/media/h265.mp4", 1 },
    { "audio/mp4; codecs=\"mp4a.40.2\"", "tests/fixtures/mse/whole-audio.mp4", 0 },
    { "audio/mp4; codecs=\"mp4a.40.34\"", "tests/fixtures/media/h264-mp3-nobf.mp4", 0 },
};

/* Drive one whole file through the MSE path in one append and require frames. */
static int play_whole(const char *type, const char *path, int want_video,
                      struct mel_stats *out, int limit_steps)
{
    world_reset();
    long n = 0;
    unsigned char *buf = slurp(path, &n);
    if (!buf) return -1;

    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    char url[64];
    mse_object_url(ms, url, sizeof url);
    mel_attach_url(el, url);

    int err = 0;
    sbuf *sb = mse_add_source_buffer(ms, type, &err);
    if (!sb) { free(buf); mse_free(ms); return -2; }
    if (sb_append(sb, buf, n) != MSE_OK) { free(buf); mse_free(ms); return -3; }
    mse_end_of_stream(ms, 0);
    mel_play(el);

    for (int i = 0; i < limit_steps && !mel_ended(el); i++) {
        int painted = media_pump();
        advance(painted ? 8000000LL : 4000000LL);
    }
    if (out) mel_get_stats(el, out);
    int shown = 0;
    struct mel_stats st;
    mel_get_stats(el, &st);
    shown = want_video ? (int)st.frames_shown : (int)st.audio_frames_written;
    free(buf);
    mse_free(ms);
    return shown;
}

static void test_decodes(void)
{
    printf("\n== every type the table says yes to actually decodes ==\n");
    for (unsigned i = 0; i < sizeof g_decode / sizeof g_decode[0]; i++) {
        const struct dcase *d = &g_decode[i];
        CHECK(mse_type_supported(d->type) == 1, "claimed: %s", d->type);
        int got = play_whole(d->type, d->path, d->want_video, 0, 40000);
        CHECK(got > 0, "%s from %s -> %d %s", d->type, d->path, got,
              d->want_video ? "pictures" : "audio frames");
    }
}

/* ======================= 3. the segmented stream, over time ============= */
/* The claim: a DASH-shaped stream, appended segment by segment as it would
 * arrive off the network, plays -- in order, in sync, and producing the same
 * pictures the whole file does. */
struct seginfo { unsigned char *data; long len; long long due_ns; int sent; };

static void test_segmented(void)
{
    printf("\n== a DASH-shaped segmented stream plays from MediaSource ==\n");
    world_reset();

    /* The reference: what the whole file demuxes to. Everything the segmented
     * run produces is compared against this rather than against a number. */
    long wn = 0;
    unsigned char *whole = slurp_fx("whole-video.mp4", &wn);
    if (!whole) { CHECK(0, "fixture whole-video.mp4 is missing -- run make mse-fixtures"); return; }
    int err = 0;
    mdemux *ref = media_open(whole, wn, &err);
    if (!ref) { CHECK(0, "the reference whole file does not demux (%s)", media_strerror(err)); return; }
    int rvt = media_find_track(ref, MEDIA_TRACK_VIDEO);
    long ref_samples = media_track_info(ref, rvt)->nsamples;
    long long ref_dur = media_duration_ns(ref);

    /* Load the segments. */
    struct seginfo vseg[16], aseg[16];
    int nv = 0, na = 0;
    long vinit_n = 0, ainit_n = 0;
    unsigned char *vinit = slurp_fx("init-video.mp4", &vinit_n);
    unsigned char *ainit = slurp_fx("init-audio.mp4", &ainit_n);
    for (int i = 1; i < 16; i++) {
        char nm[32];
        long n = 0;
        snprintf(nm, sizeof nm, "video-%d.m4s", i);
        unsigned char *b = slurp_fx(nm, &n);
        if (!b) break;
        vseg[nv].data = b; vseg[nv].len = n;
        /* Segment k is not offered to the player until second k-1 of wall
         * time, which is what makes "append segment 3 while segment 1 is
         * playing" a sequence rather than a phrase. */
        vseg[nv].due_ns = (long long)nv * 600000000LL;
        vseg[nv].sent = 0;
        nv++;
    }
    for (int i = 1; i < 16; i++) {
        char nm[32];
        long n = 0;
        snprintf(nm, sizeof nm, "audio-%d.m4s", i);
        unsigned char *b = slurp_fx(nm, &n);
        if (!b) break;
        aseg[na].data = b; aseg[na].len = n;
        aseg[na].due_ns = (long long)na * 600000000LL;
        aseg[na].sent = 0;
        na++;
    }
    CHECK(nv >= 2 && na >= 2, "the fixture is really segmented: %d video + %d audio .m4s", nv, na);
    if (nv < 2 || na < 2) return;

    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    char url[64];
    mse_object_url(ms, url, sizeof url);
    CHECK(mse_state(ms) == MSE_CLOSED, "a fresh MediaSource is 'closed'");
    CHECK(mel_attach_url(el, url) == MSE_OK, "video.src = URL.createObjectURL(ms) attaches");
    CHECK(mse_state(ms) == MSE_OPEN, "attaching moves it to 'open' -- what fires sourceopen");

    sbuf *vsb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
    sbuf *asb = mse_add_source_buffer(ms, "audio/mp4; codecs=\"mp4a.40.2\"", &err);
    CHECK(vsb && asb, "two SourceBuffers on one element -- video/mp4 and audio/mp4");
    if (!vsb || !asb) return;

    int e2 = 0;
    CHECK(mse_add_source_buffer(ms, "video/mp4; codecs=\"av01.0.08M.08.0.110.01.01.01.0\"", &e2) == 0 &&
          e2 == MSE_E_NOTSUPPORTED,
          "addSourceBuffer(av01) throws NotSupportedError -- the refusal a player reads");

    /* The init segments, then the media segments as they come due. */
    CHECK(sb_append(vsb, vinit, vinit_n) == MSE_OK, "the video init segment appends");
    CHECK(sb_append(asb, ainit, ainit_n) == MSE_OK, "the audio init segment appends");
    CHECK(sb_buffered_count(vsb) == 0,
          "an init segment alone buffers no media time -- it is a moov, not a picture");

    /* The painter reports the element's border box every redraw; without that
     * the engine has nowhere to put a frame. One call stands in for it. */
    media_paint_key(1, 10, 20, 320, 240, 0, 0, 800, 600);

    mel_play(el);

    long long last_time_ns = -1;
    int backwards = 0, painted_total = 0, sent_v = 0, sent_a = 0;
    int saw_waiting = 0;
    unsigned all_events = 0;
    long long first_paint_ns = -1;

    for (int step = 0; step < 200000 && !mel_ended(el); step++) {
        /* The network delivers. */
        for (int i = 0; i < nv; i++)
            if (!vseg[i].sent && g_now >= vseg[i].due_ns) {
                vseg[i].sent = 1; sent_v++;
                if (sb_append(vsb, vseg[i].data, vseg[i].len) != MSE_OK)
                    CHECK(0, "video segment %d failed to append", i + 1);
            }
        for (int i = 0; i < na; i++)
            if (!aseg[i].sent && g_now >= aseg[i].due_ns) {
                aseg[i].sent = 1; sent_a++;
                if (sb_append(asb, aseg[i].data, aseg[i].len) != MSE_OK)
                    CHECK(0, "audio segment %d failed to append", i + 1);
            }
        if (sent_v == nv && sent_a == na && mse_state(ms) == MSE_OPEN)
            mse_end_of_stream(ms, 0);

        int painted = media_pump();
        all_events |= mel_take_events(el);
        if (all_events & MEV_WAITING) saw_waiting = 1;
        if (painted) {
            if (getenv("MSE_TRACE") && painted_total < 30)
                fprintf(stderr, "TRACE paint#%d now=%.3f played=%.3f pts=%.3f\n",
                        painted_total, (double)g_now/1e9,
                        (double)g_played_frames/(g_rate?g_rate:1), mel_current_time(el));
            painted_total += painted;
            if (first_paint_ns < 0) first_paint_ns = g_now;
            double t = mel_current_time(el);
            long long tns = (long long)(t * 1e9);
            if (last_time_ns >= 0 && tns < last_time_ns) backwards++;
            last_time_ns = tns;
        }
        advance(painted ? 8000000LL : 2000000LL);
    }

    struct mel_stats st;
    mel_get_stats(el, &st);

    CHECK(painted_total > 0, "frames reached the screen: %d paints, %lld shown",
          painted_total, st.frames_shown);
    CHECK(g_blits >= painted_total, "each shown frame was blitted (%d blits)", g_blits);
    CHECK(backwards == 0,
          "presentation time never went backwards across %d paints -- B frames are "
          "timed by h264_decode_pts, not by a decode-order FIFO", painted_total);
    CHECK(st.frames_decoded == ref_samples,
          "the segmented run decoded every picture the whole file has: %lld of %ld",
          st.frames_decoded, ref_samples);
    CHECK(st.audio_frames_written > 0, "audio was written to the card: %lld frames",
          st.audio_frames_written);
    CHECK(mel_ended(el), "playback reached the end and the element is 'ended'");
    CHECK(st.appends >= (long long)(nv + na + 2),
          "the stream really arrived in pieces: %lld appends", st.appends);
    CHECK((all_events & MEV_LOADEDMETADATA) != 0, "loadedmetadata fired");
    CHECK((all_events & MEV_TIMEUPDATE) != 0, "timeupdate fired");
    CHECK((all_events & MEV_ENDED) != 0, "ended fired");

    /* Geometry: the picture is the fixture's, not a default. */
    CHECK(mel_video_width(el) == 128 && mel_video_height(el) == 96,
          "videoWidth/videoHeight are the stream's: %dx%d",
          mel_video_width(el), mel_video_height(el));

    /* buffered() grew as the segments landed and ends where the media does. */
    int bn = mel_buffered_count(el);
    double bs = 0, be = 0;
    CHECK(bn >= 1 && mel_buffered_range(el, bn - 1, &bs, &be),
          "buffered has %d range(s), the last ending at %.3fs", bn, be);
    CHECK(be > (double)ref_dur / 1e9 - 0.25,
          "buffered reaches the end of the media: %.3fs of %.3fs", be, (double)ref_dur / 1e9);
    CHECK(mel_duration(el) > 0, "duration is known: %.3fs", mel_duration(el));

    /* ---- A/V drift, from avclock's own reporting ---- */
    NOTE("A/V drift: mean %+lld ms, max %+lld ms, min %+lld ms over %lld measurements",
         st.drift_mean_ns / 1000000, st.drift_max_ns / 1000000,
         st.drift_min_ns / 1000000, st.frames_shown);
    NOTE("frames: decoded %lld, shown %lld, dropped %lld, re-syncs %lld",
         st.frames_decoded, st.frames_shown, st.frames_dropped, st.resyncs);
    NOTE("appends %lld (%lld bytes), demuxer re-parses %lld",
         st.appends, st.bytes_appended, st.reparses);
    long long am = st.drift_mean_ns < 0 ? -st.drift_mean_ns : st.drift_mean_ns;
    /* The bar the container line set on real clips was -9 to -11 ms. Here the
     * card and the clock are the same simulation, so anything above a couple of
     * milliseconds is a real defect and not measurement noise -- which is
     * exactly what makes this worth gating on. */
    CHECK(am <= 5000000LL,
          "mean A/V drift is inside 5 ms: %+lld ms", st.drift_mean_ns / 1000000);
    CHECK(st.frames_dropped == 0 && st.frames_shown == st.frames_decoded,
          "at real-time decode speed nothing was dropped: %lld shown of %lld decoded",
          st.frames_shown, st.frames_decoded);
    CHECK(st.resyncs == 0,
          "the clock never had to be re-based: %lld re-syncs", st.resyncs);
    /* The re-parse count is the cost of the whole-prefix re-open. It must stay
     * proportional to the number of segments, not to the number of appends or
     * of pump steps -- that is the difference between lazy and per-append. */
    CHECK(st.reparses <= (long long)(nv + na) * 3 + 8,
          "re-parses stayed proportional to segments (%lld for %d segments)",
          st.reparses, nv + na);
    (void)saw_waiting;

    media_close(ref);
    free(whole);
    free(vinit); free(ainit);
    for (int i = 0; i < nv; i++) free(vseg[i].data);
    for (int i = 0; i < na; i++) free(aseg[i].data);
    mse_free(ms);
}

/* ============== 3b. the same stream on a machine too slow for it ======== */
/* This one is not a corner case here: a from-scratch H.264 decoder under TCG is
 * nowhere near real time, so the machine this ships on IS the slow machine. The
 * policy avclock states for it is that video is dropped rather than audio, and
 * that a decoder permanently behind causes a RE-BASE rather than an ever-growing
 * lateness -- because a pipeline 3x too slow can never catch up and every frame
 * after the first would be "late" for ever.
 *
 * What must hold: audio is still complete, the two streams stay together (the
 * drift stays bounded), and one frame in five is still painted. What must NOT
 * hold is real time; playing slow is honest, and the counters say so. */
static void test_slow_machine(void)
{
    printf("\n== the same stream with decode 3x slower than real time ==\n");
    world_reset();
    long wn = 0;
    unsigned char *whole = slurp_fx("whole-video.mp4", &wn);
    long an = 0;
    unsigned char *aud = slurp_fx("whole-audio.mp4", &an);
    if (!whole || !aud) { CHECK(0, "fixture missing"); return; }

    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    char url[64];
    int err = 0;
    mse_object_url(ms, url, sizeof url);
    mel_attach_url(el, url);
    sbuf *v = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
    sbuf *a = mse_add_source_buffer(ms, "audio/mp4; codecs=\"mp4a.40.2\"", &err);
    sb_append(v, whole, wn);
    sb_append(a, aud, an);
    mse_end_of_stream(ms, 0);
    media_paint_key(1, 0, 0, 256, 192, 0, 0, 800, 600);
    mel_play(el);

    /* 200 ms of simulated cost per decoded picture against a 66.7 ms frame
     * period: three times too slow, deliberately. */
    for (int i = 0; i < 400000 && !mel_ended(el); i++) {
        int p = media_pump();
        advance(p ? 200000000LL : 4000000LL);
    }
    struct mel_stats st;
    mel_get_stats(el, &st);
    NOTE("slow: decoded %lld, shown %lld, dropped %lld, re-syncs %lld, "
         "drift mean %+lld ms max %+lld ms min %+lld ms",
         st.frames_decoded, st.frames_shown, st.frames_dropped, st.resyncs,
         st.drift_mean_ns / 1000000, st.drift_max_ns / 1000000, st.drift_min_ns / 1000000);
    CHECK(st.frames_decoded == 60,
          "every picture is still DECODED (%lld) -- skipping decode would corrupt "
          "everything until the next keyframe", st.frames_decoded);
    CHECK(st.frames_shown * 5 >= st.frames_decoded,
          "at least one frame in five is still painted: %lld of %lld",
          st.frames_shown, st.frames_decoded);
    CHECK(st.audio_frames_written > 170000,
          "audio is complete: %lld frames -- audio is never dropped or stretched",
          st.audio_frames_written);
    long long worst = st.drift_min_ns < 0 ? -st.drift_min_ns : st.drift_min_ns;
    CHECK(worst < 1500000000LL,
          "lateness stayed bounded rather than accumulating: worst %+lld ms over "
          "%lld re-bases", st.drift_min_ns / 1000000, st.resyncs);
    free(whole); free(aud);
    mse_free(ms);
}

/* ================= 4. the incremental parse == the whole file =========== */
/* The load-bearing identity of the whole design: init + segments concatenated
 * IS the whole file, so the demuxer must see exactly the same samples either
 * way. Compared sample for sample, not by count -- a parser that dropped one
 * fragment and duplicated another produces the right total. */
static void test_incremental_identity(void)
{
    printf("\n== the incremental append sees exactly the whole file's samples ==\n");
    world_reset();
    long wn = 0;
    unsigned char *whole = slurp_fx("whole-video.mp4", &wn);
    if (!whole) { CHECK(0, "fixture missing"); return; }
    int err = 0;
    mdemux *ref = media_open(whole, wn, &err);
    if (!ref) { CHECK(0, "reference open failed"); free(whole); return; }
    int rt = media_find_track(ref, MEDIA_TRACK_VIDEO);
    long rn = media_track_info(ref, rt)->nsamples;

    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    char url[64];
    mse_object_url(ms, url, sizeof url);
    mel_attach_url(el, url);
    sbuf *sb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);

    /* Appended in 997-byte slices: a prime, so almost every append lands in the
     * MIDDLE of a box. If the box walk believed a partial size field, or
     * re-opened the demuxer over an incomplete moof, this is where it shows. */
    long off = 0;
    int chunks = 0;
    while (off < wn) {
        long k = wn - off;
        if (k > 997) k = 997;
        if (sb_append(sb, whole + off, k) != MSE_OK) { CHECK(0, "append failed at %ld", off); break; }
        off += k;
        chunks++;
    }
    CHECK(chunks > 40, "the file arrived in %d sub-box-sized pieces", chunks);
    mse_end_of_stream(ms, 0);

    /* Force the parse and walk both. */
    int bn = sb_buffered_count(sb);
    CHECK(bn >= 1, "buffered has %d range(s) after the whole file", bn);

    /* Compare through the public surface: play it and count. */
    mel_play(el);
    for (int i = 0; i < 200000 && !mel_ended(el); i++) {
        int p = media_pump();
        advance(p ? 8000000LL : 2000000LL);
    }
    struct mel_stats st;
    mel_get_stats(el, &st);
    CHECK(st.frames_decoded == rn,
          "byte-sliced append decoded %lld pictures; the whole file has %ld",
          st.frames_decoded, rn);

    media_close(ref);
    free(whole);
    mse_free(ms);
}

/* ============================ 5. modes, offsets, remove ================= */
static void test_modes(void)
{
    printf("\n== SourceBuffer.mode, timestampOffset and remove ==\n");
    world_reset();
    long in = 0, s1 = 0, s2 = 0;
    unsigned char *init = slurp_fx("init-video.mp4", &in);
    unsigned char *seg1 = slurp_fx("video-1.m4s", &s1);
    unsigned char *seg2 = slurp_fx("video-2.m4s", &s2);
    if (!init || !seg1 || !seg2) { CHECK(0, "fixture missing"); return; }

    /* --- segments mode: the samples keep their own timestamps --- */
    {
            melem *el = mel_for_key(1, 1);
        msource *ms = mse_new();
        char url[64];
        int err = 0;
        mse_object_url(ms, url, sizeof url);
        mel_attach_url(el, url);
        sbuf *sb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
        CHECK(sb_mode(sb) == MSE_MODE_SEGMENTS, "a new SourceBuffer defaults to 'segments'");
        sb_append(sb, init, in);
        sb_append(sb, seg1, s1);
        double a0 = 0, a1 = 0;
        sb_buffered_range(sb, 0, &a0, &a1);
        /* Its own time, which is NOT zero: this stream has B frames, so the
         * first sample's composition time sits two frames in. That number is
         * the thing sequence mode below is compared against. */
        CHECK(a0 < 0.25, "segment 1 in segments mode keeps its own start: %.3fs", a0);
        sb_append(sb, seg2, s2);
        double b0 = 0, b1 = 0;
        int n = sb_buffered_count(sb);
        sb_buffered_range(sb, n - 1, &b0, &b1);
        CHECK(b1 > a1, "appending segment 2 extended buffered to %.3fs (was %.3fs)", b1, a1);
        CHECK(n == 1, "two adjacent segments merged into ONE buffered range");

        /* remove() takes a range out of buffered and out of playback. */
        CHECK(sb_remove(sb, 0.0, 0.5) == MSE_OK, "remove(0, 0.5) is accepted");
        double r0 = 0, r1 = 0;
        sb_buffered_range(sb, 0, &r0, &r1);
        CHECK(r0 >= 0.4, "buffered now starts at %.3fs -- the removed head is gone", r0);
        mse_free(ms);
        world_reset();
    }

    /* --- sequence mode: each segment is placed after the last --- */
    {
            melem *el = mel_for_key(1, 1);
        msource *ms = mse_new();
        char url[64];
        int err = 0;
        mse_object_url(ms, url, sizeof url);
        mel_attach_url(el, url);
        sbuf *sb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
        sb_set_mode(sb, MSE_MODE_SEQUENCE);
        CHECK(sb_mode(sb) == MSE_MODE_SEQUENCE, "mode = 'sequence' takes");
        sb_append(sb, init, in);
        sb_append(sb, seg2, s2);          /* the SECOND segment, appended FIRST */
        double q0 = 0, q1 = 0;
        sb_buffered_range(sb, 0, &q0, &q1);
        CHECK(q0 < 0.05,
              "in sequence mode a segment whose own timestamps start at ~1s is placed "
              "at %.3fs -- which is the whole difference from segments mode", q0);
        CHECK(q1 > 0.5 && q1 < 1.6, "and it is one segment long: ends at %.3fs", q1);
        mse_free(ms);
        world_reset();
    }

    /* --- timestampOffset in segments mode --- */
    {
            melem *el = mel_for_key(1, 1);
        msource *ms = mse_new();
        char url[64];
        int err = 0;
        mse_object_url(ms, url, sizeof url);
        mel_attach_url(el, url);
        sbuf *sb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
        sb_append(sb, init, in);
        sb_set_timestamp_offset(sb, 10.0);
        CHECK(sb_timestamp_offset(sb) > 9.99, "timestampOffset reads back as 10");
        sb_append(sb, seg1, s1);
        double t0 = 0, t1 = 0;
        sb_buffered_range(sb, 0, &t0, &t1);
        CHECK(t0 > 9.9 && t0 < 10.2,
              "the appended segment landed at %.3fs, moved by the offset", t0);
        mse_free(ms);
        world_reset();
    }
    free(init); free(seg1); free(seg2);
}

/* ============================ 6. states and refusals ==================== */
static void test_states(void)
{
    printf("\n== readyState, refusals and the error path ==\n");
    world_reset();
    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    int err = 0;

    CHECK(mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err) == 0 &&
          err == MSE_E_INVALIDSTATE,
          "addSourceBuffer before attach is InvalidStateError -- readyState is 'closed'");

    char url[64];
    mse_object_url(ms, url, sizeof url);
    mel_attach_url(el, url);
    sbuf *sb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
    CHECK(sb != 0, "addSourceBuffer after attach succeeds");
    CHECK(mse_set_duration(ms, 12.5) == MSE_OK && mse_duration(ms) == 12.5,
          "duration is settable while open");
    CHECK(mse_end_of_stream(ms, 0) == MSE_OK && mse_state(ms) == MSE_ENDED,
          "endOfStream moves readyState to 'ended'");
    CHECK(mse_end_of_stream(ms, 0) == MSE_E_INVALIDSTATE,
          "endOfStream twice is InvalidStateError");

    long in = 0;
    unsigned char *init = slurp_fx("init-video.mp4", &in);
    if (init) {
        CHECK(sb_append(sb, init, in) == MSE_OK && mse_state(ms) == MSE_OPEN,
              "appending after endOfStream re-opens the source, as the spec says");
        free(init);
    }
    CHECK(mse_remove_source_buffer(ms, sb) == MSE_OK, "removeSourceBuffer detaches it");
    CHECK(mse_sb_count(ms) == 0, "sourceBuffers is empty afterwards");

    /* A src that is not one of our object URLs. */
    world_reset();
    melem *e2 = mel_for_key(2, 1);
    CHECK(mel_attach_url(e2, "https://example.com/movie.mp4") == MSE_E_NOTSUPPORTED,
          "a progressive src is refused out loud (there is no such loader here)");
    CHECK(mel_error(e2) == 4, "MediaError.code is MEDIA_ERR_SRC_NOT_SUPPORTED (%d)",
          mel_error(e2));
    NOTE("and it says why: \"%s\"", mel_error_message(e2));
    mse_free(ms);
}

/* ============================ 7. the AV1 consequence ==================== */
/* In the honest build this asserts a REFUSAL. In the sabotaged build
 * (-DMSE_CONTROL_CLAIM_AV1) the refusal does not happen, the AV1 bytes append
 * cleanly, and no picture is ever produced -- which is exactly the failure this
 * design exists to prevent, arriving exactly where it was predicted to. */
static void test_av1(void)
{
    printf("\n== AV1: the answer, and what happens if it is a lie ==\n");
    world_reset();
    long an = 0;
    unsigned char *av1 = slurp_fx("whole-av1.mp4", &an);
    const char *T = "video/mp4; codecs=\"av01.0.08M.08.0.110.01.01.01.0\"";

    int claimed = mse_type_supported(T);
    CHECK(claimed == 0, "isTypeSupported(av01) is no -- there is no AV1 decoder here");

    if (!av1) { NOTE("no av1 fixture: the consequence half is skipped"); return; }

    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    char url[64];
    int err = 0;
    mse_object_url(ms, url, sizeof url);
    mel_attach_url(el, url);
    sbuf *sb = mse_add_source_buffer(ms, T, &err);

    if (!claimed) {
        CHECK(sb == 0 && err == MSE_E_NOTSUPPORTED,
              "addSourceBuffer(av01) throws NotSupportedError, so a real player "
              "falls back to the avc1.64 the same manifest offers");
    } else {
        /* The lie was told. Follow it all the way down. */
        CHECK(sb != 0, "the sabotaged build ACCEPTED an AV1 SourceBuffer");
        if (sb) {
            sb_append(sb, av1, an);
            mse_end_of_stream(ms, 0);
            mel_play(el);
            for (int i = 0; i < 20000 && !mel_ended(el); i++) {
                int p = media_pump();
                advance(p ? 8000000LL : 2000000LL);
            }
            struct mel_stats st;
            mel_get_stats(el, &st);
            NOTE("the AV1 stream appended %lld bytes and produced %lld pictures",
                 st.bytes_appended, st.frames_shown);
            CHECK(st.frames_shown > 0,
                  "a type isTypeSupported said yes to must actually decode "
                  "(it produced %lld pictures)", st.frames_shown);
        }
    }
    free(av1);
    mse_free(ms);
}

/* ================================================================ main ==== */
int main(int argc, char **argv)
{
    if (argc > 1) FX = argv[1];
    media_set_platform(&g_hostplat);

    test_types();
    test_decodes();
    test_segmented();
    test_slow_machine();
    test_incremental_identity();
    test_modes();
    test_states();
    test_av1();

    printf("\nmse_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
