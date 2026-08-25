/* /bin/msecheck -- the MSE engine, on the machine.
 *
 * tests/unit/mse_test.c proves the same C on the host, where the clock and the
 * sound card are simulations. This runs it on LogitOS: mini-libc's arena
 * allocator, -ffreestanding -msse2, a 32 KiB stack, a real monotonic clock and
 * a real sound card, with the DASH segments read one at a time off LogitFS and
 * appended exactly as a page's fetch loop would. That is what turns "Media
 * Source Extensions works" from a claim about a clang build on Linux into a
 * claim about this machine.
 *
 * It drives js_media_src.c THROUGH ITS PUBLIC API -- the same calls js_media.c
 * makes for MediaSource.appendBuffer, video.play() and the pump -- so the thing
 * measured here is the thing the browser runs. What it does not exercise is
 * QuickJS and the DOM; tests/boot/run-mse-test.sh drives those through the
 * browser itself.
 *
 * Everything it prints is a number a harness reads back. No screenshots.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logit.h"
#include "js_media.h"

/* c/lib/video's mjpeg.c decodes each frame through c/lib/image's img_decode(),
 * which allocates with the kernel heap's names. In ring 3 those names are
 * mini-libc's -- the same two-line shim preview.c:64, browser_rt.c:44 and
 * vidcheck.c carry. VID_OBJ is a wildcard over c/lib/video/*.c, so every
 * consumer of it inherited this the day MJPEG landed; the link lines did not
 * follow, and the break was invisible because the stale binaries on disk were
 * newer than the new objects. See the Makefile note beside this file's rule. */
#include <stdlib.h>
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

/* ---- the platform, wired to the syscalls ---- */
static unsigned long long p_now(void) { return monotonic_ns(); }
static void p_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{ gui_blit(x, y, w, h, rgba, sw, sh); }
static void p_fill(int x, int y, int w, int h, unsigned rgb) { gui_rect(x, y, w, h, rgb); }
static void p_clip(int x, int y, int w, int h) { gui_clip(x, y, w, h); }
static void p_flush(void) { }
static int  p_snd_open(int rate, int ch)
{
    struct logit_sndinfo si;
    if (snd_info(&si) != 1) return -1;
    /* NON-BLOCKING, and that is not a preference. A blocking snd_write PARKS
     * the thread when the ring is full (500 ms backstop, see snd_stream_write
     * in c/kernel/audio/mixer.c) -- and this thread is the one that decodes the
     * next picture, runs the page's timers and answers the mouse. A player that
     * can be parked by its own sound card is a browser that freezes while a
     * video plays. The pump asks snd_avail() how much room there is and writes
     * that much; a short write is a normal answer here, not an error. */
    struct logit_sndfmt f;
    f.rate = (unsigned)rate;
    f.channels = (unsigned short)ch;
    f.format = SND_FMT_S16;
    f.buffer_ms = 0;
    f.flags = SND_F_NONBLOCK;
    return snd_open(&f);
}
static int  p_snd_write(int h, const void *b, int n) { return snd_write(h, b, n); }
static int  p_snd_avail(int h) { return snd_avail(h); }
static long long p_snd_played(int h)
{
    struct logit_sndstate st;
    if (snd_state(h, &st) != 0) return -1;
    return (long long)st.frames_played;
}
static void p_snd_close(int h, int drain) { snd_close(h, drain); }

static const struct media_platform g_plat = {
    p_now, p_blit, p_fill, p_clip, p_flush,
    p_snd_open, p_snd_write, p_snd_avail, p_snd_played, p_snd_close
};

/* This program has no window, so a blit would go nowhere. Counting the calls
 * instead is the honest measurement: what is being checked here is that a frame
 * REACHED the presentation step at the right time, and the browser is what
 * proves it lands on the screen. */
static long g_blits;
static void c_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{ (void)x; (void)y; (void)w; (void)h; (void)rgba; (void)sw; (void)sh; g_blits++; }
static void c_fill(int x, int y, int w, int h, unsigned rgb)
{ (void)x; (void)y; (void)w; (void)h; (void)rgb; }
static void c_clip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

static struct media_platform g_counting;

static unsigned char *slurp(const char *path, long *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *b = malloc((unsigned long)n);
    if (!b) { fclose(f); return 0; }
    if ((long)fread(b, 1, (unsigned long)n, f) != n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out = n;
    return b;
}

static int have_segment(const char *dir, const char *kind, int n)
{
    char p[192];
    snprintf(p, sizeof p, "%s/%s-%d.m4s", dir, kind, n);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ---- the answer table, printed so the harness can diff it against the host's --
 * The table is the browser's promise about itself; printing it here is how the
 * device build is shown to be making the SAME promise, rather than a build
 * where a #define wandered. */
static const char *g_types[] = {
    "video/mp4; codecs=\"avc1.640033\"",
    "video/mp4; codecs=\"avc1.4d401e\"",
    "video/mp4; codecs=\"avc1.42E01E\"",
    "video/mp4; codecs=\"hvc1.1.6.L120.90\"",
    "audio/mp4; codecs=\"mp4a.40.2\"",
    "audio/mp4; codecs=\"mp4a.40.34\"",
    "video/mp4; codecs=\"av01.0.08M.08.0.110.01.01.01.0\"",
    "video/mp4; codecs=\"vp09.00.10.08\"",
    "video/webm; codecs=\"vp9,opus\"",
    "audio/mp4; codecs=\"opus\"",
    "audio/mp4; codecs=\"mp4a.40.5\"",
    "video/mp4; codecs=\"avc1.6E0033\"",
    "video/mp4",
    0
};

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/media/mse";

    for (int i = 0; g_types[i]; i++)
        printf("MSE-TYPE %s %s\n", mse_type_supported(g_types[i]) ? "yes" : "no", g_types[i]);
    fflush(stdout);

    g_counting = g_plat;
    g_counting.blit = c_blit;
    g_counting.fill = c_fill;
    g_counting.clip = c_clip;
    media_set_platform(&g_counting);

    char path[192];
    long vn = 0, an = 0;
    snprintf(path, sizeof path, "%s/init-video.mp4", dir);
    unsigned char *vinit = slurp(path, &vn);
    snprintf(path, sizeof path, "%s/init-audio.mp4", dir);
    unsigned char *ainit = slurp(path, &an);
    if (!vinit || !ainit) { printf("MSE-FAIL no init segments under %s\n", dir); return 1; }

    melem *el = mel_for_key(1, 1);
    msource *ms = mse_new();
    char url[64];
    mse_object_url(ms, url, sizeof url);
    if (mel_attach_url(el, url) != MSE_OK) { printf("MSE-FAIL attach\n"); return 1; }
    printf("MSE-ATTACH %s state=%d\n", url, mse_state(ms));

    int err = 0;
    sbuf *vsb = mse_add_source_buffer(ms, "video/mp4; codecs=\"avc1.640033\"", &err);
    sbuf *asb = mse_add_source_buffer(ms, "audio/mp4; codecs=\"mp4a.40.2\"", &err);
    int e2 = 0;
    sbuf *bad = mse_add_source_buffer(ms, "video/mp4; codecs=\"av01.0.08M.08.0.110.01.01.01.0\"", &e2);
    printf("MSE-SB video=%d audio=%d av1=%d av1err=%d\n",
           vsb != 0, asb != 0, bad != 0, e2);
    if (!vsb || !asb) { printf("MSE-FAIL addSourceBuffer\n"); return 1; }

    sb_append(vsb, vinit, vn);
    sb_append(asb, ainit, an);

    /* The element is 320x240 in a notional page; media_paint_key is what
     * browser_paint.c calls, so the presentation path here is the real one. */
    media_paint_key(1, 0, 0, 320, 240, 0, 0, 640, 480);
    mel_play(el);

    /* The segments, appended as they would arrive: one per 400 ms of wall time,
     * so later ones really do land while earlier ones are playing. */
    int nv = 0, na = 0;
    unsigned long long t0 = monotonic_ns();
    long long last_ns = -1;
    int backwards = 0, drained = 0;

    for (int step = 0; step < 2000000 && !mel_ended(el); step++) {
        unsigned long long el_ns = monotonic_ns() - t0;
        int want = (int)(el_ns / 400000000ULL) + 1;
        while (nv < want && nv < 16) {
            long n = 0;
            snprintf(path, sizeof path, "%s/video-%d.m4s", dir, nv + 1);
            unsigned char *b = slurp(path, &n);
            if (!b) break;
            if (sb_append(vsb, b, n) != MSE_OK) printf("MSE-FAIL video append %d\n", nv + 1);
            free(b);
            nv++;
        }
        while (na < want && na < 16) {
            long n = 0;
            snprintf(path, sizeof path, "%s/audio-%d.m4s", dir, na + 1);
            unsigned char *b = slurp(path, &n);
            if (!b) break;
            if (sb_append(asb, b, n) != MSE_OK) printf("MSE-FAIL audio append %d\n", na + 1);
            free(b);
            na++;
        }
        /* Once neither track has another segment on the disk, the "download" is
         * complete: endOfStream, which is exactly what a DASH player does when
         * it reaches the last entry in the manifest. Asked ONCE, not per pump:
         * a file open costs 2.8 ms on this machine (see the kbench line for
         * SYS 84) and two failing probes per iteration is most of the loop. */
        if (!drained && nv > 0 && na > 0 &&
            !have_segment(dir, "video", nv + 1) && !have_segment(dir, "audio", na + 1)) {
            drained = 1;
            if (mse_state(ms) == MSE_OPEN) mse_end_of_stream(ms, 0);
        }

        int painted = media_pump();
        if (painted) {
            long long t = (long long)(mel_current_time(el) * 1e9);
            if (last_ns >= 0 && t < last_ns) backwards++;
            last_ns = t;
        } else {
            /* Sleep, do not spin. Waiting for the sound card's play cursor is
             * the normal state of a player -- the clock decides when the next
             * picture is due -- and a yield loop would hold the big lock for
             * the whole clip on a machine where a syscall costs 36 us. */
            sys_sleep_ms(2);
        }
        if ((step % 200) == 0) {
            struct mel_stats ps;
            mel_get_stats(el, &ps);
            printf("MSE-PROGRESS step=%d t_ms=%d decoded=%lld shown=%lld segs=%d+%d\n",
                   step, (int)(mel_current_time(el) * 1000), ps.frames_decoded,
                   ps.frames_shown, nv, na);
            fflush(stdout);
        }
    }

    struct mel_stats st;
    mel_get_stats(el, &st);
    int bn = mel_buffered_count(el);
    double bs = 0, be = 0;
    if (bn > 0) mel_buffered_range(el, bn - 1, &bs, &be);

    printf("MSE-PLAY segments=%d+%d decoded=%lld shown=%lld dropped=%lld resyncs=%lld "
           "backwards=%d blits=%ld\n",
           nv, na, st.frames_decoded, st.frames_shown, st.frames_dropped,
           st.resyncs, backwards, g_blits);
    printf("MSE-SYNC drift_mean_ms=%lld drift_max_ms=%lld drift_min_ms=%lld "
           "audio_frames=%lld\n",
           st.drift_mean_ns / 1000000, st.drift_max_ns / 1000000,
           st.drift_min_ns / 1000000, st.audio_frames_written);
    printf("MSE-BUF ranges=%d end=%d.%03ds duration=%d.%03ds size=%dx%d\n",
           bn, (int)be, (int)((be - (int)be) * 1000),
           (int)mel_duration(el), (int)((mel_duration(el) - (int)mel_duration(el)) * 1000),
           mel_video_width(el), mel_video_height(el));
    printf("MSE-APPEND appends=%lld bytes=%lld reparses=%lld ended=%d\n",
           st.appends, st.bytes_appended, st.reparses, mel_ended(el));
    printf("MSE-DONE\n");
    fflush(stdout);
    return 0;
}
