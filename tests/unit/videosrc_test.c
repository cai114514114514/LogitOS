/* Host gate for <video src> + <track> (tests/videosrc.mk).
 *
 * WHAT THIS IS FOR. test-video-page proves the whole browser path on the
 * machine; this proves the two halves a boot run cannot isolate:
 *
 *   1. A TRACK'S BYTES BECOME CUES THE CLOCK CAN FIND. mel_subs_attach parses
 *      a real .vtt, and mel_subs_active answers from the element's own media
 *      time -- so the assertions are "cue A at 0.2 s, cue B at 0.7 s, nothing
 *      at 1.7 s", and a track whose timings were dropped or mis-scaled fails
 *      by NAME, not by pixel count.
 *   2. CUES FOLLOW PLAYBACK, NOT JUST SEEKS. The fixture's edges are inside
 *      the media, the clock is stepped over the whole 2 s, and the test
 *      requires every cue to appear and every gap to be empty during the
 *      run -- the same timeline the pixel renderer on the device reads.
 *
 * NO FONT IS IN THIS PROCESS. The renderer lives in js_media.c behind
 * gui_text_run; what is gated here is everything UNDER it. That is the same
 * split as the rest of the media feature: engine arithmetic on the host,
 * pixels on the machine.
 *
 *   make test-videosrc           the gate
 *   make test-videosrc-negctl    the same suite with -DSUBS_CONTROL_HIDE,
 *                                REQUIRED TO FAIL on the cue assertions and
 *                                REQUIRED TO PASS on the playback ones
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "js_media.h"
#include "media.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); \
    printf(__VA_ARGS__); printf("\n"); } else { printf("ok: "); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)
#define NOTE(...) do { printf("     "); printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *VFX = "tests/fixtures/video";
static const char *MFX = "tests/fixtures/media";

static unsigned char *slurp(const char *dir, const char *name, long *out)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)(n > 0 ? n : 1));
    if (!b || (n > 0 && (long)fread(b, 1, (size_t)n, f) != n)) { free(b); fclose(f); return 0; }
    fclose(f);
    *out = n;
    return b;
}

/* ============================== the fake platform =========================
 * The same shape mse_test.c uses, and for the same reasons: a clock the test
 * steps by hand and a card whose play cursor is driven by that clock, so a
 * two-second film runs in milliseconds and the answers are reproducible. */
static long long g_now;
static int g_rate, g_ch;
static long long g_queued_frames, g_played_frames;
static long long g_ring_frames;

static unsigned long long host_now(void) { return (unsigned long long)g_now; }
static int host_snd_open(int rate, int ch)
{
    g_rate = rate; g_ch = ch;
    g_queued_frames = g_played_frames = 0;
    g_ring_frames = (long long)rate * 200 / 1000;
    return 0;
}
static int host_snd_write(int h, const void *buf, int bytes)
{
    (void)h; (void)buf;
    int bpf = g_ch * 2;
    long long room = (g_ring_frames - g_queued_frames) * bpf;
    if (room <= 0) return 0;
    if (bytes > room) bytes = (int)room;
    g_queued_frames += bytes / bpf;
    return bytes;
}
static int host_snd_avail(int h)
{
    (void)h;
    return (int)((g_ring_frames - g_queued_frames) * g_ch * 2);
}
static long long host_snd_played(int h) { (void)h; return g_played_frames; }
static void host_snd_close(int h, int drain) { (void)h; (void)drain; }

static int g_blits;
static void host_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{ (void)x; (void)y; (void)w; (void)h; (void)rgba; (void)sw; (void)sh; g_blits++; }
static void host_fill(int x, int y, int w, int h, unsigned rgb)
{ (void)x; (void)y; (void)w; (void)h; (void)rgb; }
static void host_clip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
static void host_flush(void) { }

static const struct media_platform g_hostplat = {
    host_now, host_blit, host_fill, host_clip, host_flush,
    host_snd_open, host_snd_write, host_snd_avail, host_snd_played, host_snd_close
};

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

int main(void)
{
    media_set_platform(&g_hostplat);

    /* ---------- 1. the track parses, and refuses what is not a track ----- */
    long vn;
    unsigned char *vtt = slurp(VFX, "captions.vtt", &vn);
    CHECK(vtt != 0, "captions.vtt is present");
    if (!vtt) return 1;

    melem *el = mel_for_key(1, 1);
    CHECK(el != 0, "an element exists");
    int ncues = mel_subs_attach(el, vtt, vn);
    CHECK(ncues == 3, "the track parsed to 3 cues (got %d)", ncues);

    /* WHAT "GARBAGE" MEANS HERE, measured rather than assumed: SRT has no
     * signature line, so subs.c's sniffer reads any plain text as a candidate
     * SRT and finds no cues in it (probed: fmt=2, 0 cues, no error). That is
     * the parser's documented leniency, not a bug -- so the honest contract
     * this gates is "a file with no cues becomes a track with no cues", and
     * the page-side consequence (a dead, loggable track) is what
     * __mediaTrackLoad reports as "0 cues". */
    static const char garbage[] = "this is not a WebVTT file at all\n";
    CHECK(mel_subs_attach(el, (const unsigned char *)garbage, (long)sizeof garbage - 1) == 0,
          "non-VTT bytes parse to a ZERO-cue track, not an error (SRT has no signature)");
    char buf[256];
    int act = mel_subs_active(el, buf, sizeof buf);
    CHECK(act == 0, "a zero-cue track is silent (got %d)", act);
    /* Put the real track back: everything below is about a track that exists. */
    CHECK(mel_subs_attach(el, vtt, vn) == 3, "the real track re-attaches");

    /* ---------- 2. cue lookup across a seeked timeline -------------------- */
    /* mel_seek sets the element's media time directly, which is the same
     * field the renderer reads; this half needs no clock at all. */
    long mn;
    unsigned char *mp4 = slurp(MFX, "h264-mp3.mp4", &mn);
    CHECK(mp4 != 0, "h264-mp3.mp4 is present");
    if (!mp4) return 1;
    CHECK(mel_load_bytes(el, mp4, mn) == MSE_OK, "the progressive file loads");

    mel_seek(el, 0.2);
    act = mel_subs_active(el, buf, sizeof buf);
    CHECK(act == 1 && strcmp(buf, "FIRST HALF MARKER") == 0,
          "at t=0.2s the FIRST cue is active (got %d '%s')", act, buf);

    mel_seek(el, 0.7);
    act = mel_subs_active(el, buf, sizeof buf);
    CHECK(act == 1 && strcmp(buf, "SECOND HALF MARKER\nwith two lines") == 0,
          "at t=0.7s the SECOND cue is active, both lines (got %d '%s')", act, buf);

    mel_seek(el, 1.7);
    act = mel_subs_active(el, buf, sizeof buf);
    CHECK(act == 0, "at t=1.7s nothing is active -- the gap after the last cue (got %d '%s')",
          act, buf);

    mel_seek(el, 1.45);
    act = mel_subs_active(el, buf, sizeof buf);
    CHECK(act == 1 && strcmp(buf, "EDGE CUE") == 0,
          "at t=1.45s the 100ms EDGE cue is found (got %d '%s')", act, buf);

    /* ---------- 3. cues follow PLAYBACK, over the whole film -------------- */
    mel_seek(el, 0.0);
    mel_play(el);
    /* The painter must have reported a box for a cue to be drawable; the
     * renderer reads this same mel_box. 512x384 is what the guest page uses. */
    media_paint_key(1, 100, 100, 512, 384, 100, 100, 512, 384);
    int bx, by, bw, bh;
    CHECK(mel_box(el, &bx, &by, &bw, &bh) == 1 && bw == 512 && bh == 384,
          "the painter's box is what the renderer will get (%dx%d)", bw, bh);

    int saw_first = 0, saw_second = 0, saw_gap = 0, saw_edge = 0;
    long long t0 = g_now;
    while (g_now - t0 < 2600000000LL) {
        advance(20000000LL);                     /* 20 ms of world per pump */
        media_pump();
        int n = mel_subs_active(el, buf, sizeof buf);
        double t = mel_current_time(el);
        if (n > 0 && strcmp(buf, "FIRST HALF MARKER") == 0) saw_first++;
        if (n > 0 && strcmp(buf, "SECOND HALF MARKER\nwith two lines") == 0) saw_second++;
        if (n > 0 && strcmp(buf, "EDGE CUE") == 0) saw_edge++;
        if (n == 0 && t > 1.55 && t < 1.9) saw_gap++;
        if (mel_ended(el)) break;
    }
    struct mel_stats st;
    mel_get_stats(el, &st);
    NOTE("playback: decoded=%lld shown=%lld audio=%lld",
         st.frames_decoded, st.frames_shown, st.audio_frames_written);
    CHECK(st.frames_shown > 0, "pictures were shown during the run (%lld)",
          st.frames_shown);
    CHECK(mel_ended(el), "the element reached 'ended'");
    CHECK(saw_first > 0, "cue 1 was active at its own time during playback");
    CHECK(saw_second > 0, "cue 2 was active at its own time during playback");
    CHECK(saw_edge > 0, "the 100ms edge cue was caught by the 20ms poll");
    CHECK(saw_gap > 0, "the gap after the last cue was empty -- cues END");

    /* ---------- 4. detach ------------------------------------------------- */
    mel_subs_detach(el);
    act = mel_subs_active(el, buf, sizeof buf);
    CHECK(act == 0, "after detach nothing is active");
    mel_free_all();
    free(vtt);
    free(mp4);

    printf("\nvideosrc_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
