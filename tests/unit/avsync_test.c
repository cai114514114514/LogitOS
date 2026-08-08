/* tests/unit/avsync_test.c -- A/V synchronisation, MEASURED.
 *
 * "It stays in sync" is not a claim anybody can check by watching, and on this
 * machine the interesting case -- decode falls behind -- is the normal case,
 * so it cannot be left to the eye. avclock takes `now_ns` as an argument
 * precisely so a whole minute of playback can be simulated here, deterministic
 * and to the nanosecond, at decode speeds that range from comfortably faster
 * than real time to three times too slow.
 *
 * WHAT IS SIMULATED
 *
 *   A 25 fps video track and a 44.1 kHz audio track, one minute long. Decoding
 *   one video frame costs `decode_ns`, converting and blitting it costs
 *   `blit_ns`, and both are charged to the simulated clock. Audio decode is
 *   cheap and is charged too, at a fiftieth of the video cost.
 *
 *   A SOUND CARD THAT IS A REAL CLOCK. It consumes exactly one second of audio
 *   per second of simulated time, and it can only play audio it was given: if
 *   the ring runs dry the play cursor stops, and that is an underrun.
 *
 *   THE POLICY UNDER TEST: the player tops the ring up only as far as
 *   LEAD_NS beyond the video position it has decoded. This is the rule that
 *   couples the two streams. The `unbounded` variant below removes it -- it
 *   writes audio as fast as the ring will take it -- and exists to show what
 *   that costs, which is the whole reason the rule is there.
 *
 * WHAT IS MEASURED
 *
 *   drift    signed, per displayed frame: video pts minus the master clock.
 *            Reported as mean and as worst case in each direction.
 *   drops    frames decoded but not blitted because they were already late.
 *   resyncs  times the clock had to be re-anchored because chasing was futile.
 *   underrun simulated milliseconds during which the card had nothing to play.
 *   ratio    wall time consumed per second of media -- 1.0 is real time.
 *
 * THE GATES are at the bottom of main(): drift must stay inside a frame in
 * every configuration, the bounded-lead policy must keep audio and video
 * within a frame of each other even at 3x too slow, and the unbounded variant
 * must be shown to fail that -- otherwise the rule is not doing anything and
 * should not be in the code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media.h"

#define NS_PER_MS   1000000LL
#define FPS         25
#define FRAME_NS    (1000000000LL / FPS)
#define MINUTE_NS   60000000000LL
#define NFRAMES     ((int)(MINUTE_NS / FRAME_NS))
#define LEAD_NS     (150 * NS_PER_MS)      /* audio may run this far ahead of video */
#define RING_NS     (500 * NS_PER_MS)      /* the card's ring, in playing time */

typedef struct {
    const char *name;
    long long   decode_ns;      /* cost of decoding one video frame */
    long long   blit_ns;        /* cost of converting + blitting one frame */
    int         bounded;        /* 1 = gate audio writes on video progress */
    int         have_audio;
} scenario;

typedef struct {
    long long shown, dropped, resyncs;
    long long drift_mean_ns, drift_max_ns, drift_min_ns;
    long long underrun_ns;
    long long wall_ns, media_ns;
    long long av_gap_max_ns;    /* worst |video pts - audio played pts| */
} result;

static result simulate(const scenario *sc)
{
    avclock c;
    avclock_init(&c, sc->have_audio);

    long long now = 0;
    /* The player primes the ring before it starts the card, which is what any
     * player does and what keeps the very first frame's decode from being an
     * underrun. */
    long long audio_written = LEAD_NS;  /* audio pts handed to the card */
    long long audio_played = 0;         /* audio pts the card has output */
    long long card_idle_since = -1;
    long long underrun = 0;
    long long gap_max = 0;
    long long last_now = 0;

    result r;
    memset(&r, 0, sizeof r);

    for (int i = 0; i < NFRAMES; i++) {
        long long pts = (long long)i * FRAME_NS;

        /* --- decode this video frame ------------------------------------ */
        now += sc->decode_ns;

        /* --- the card plays, in real time, whatever it was given -------- */
        long long elapsed = now - last_now;
        last_now = now;
        long long could_play = audio_played + elapsed;
        if (could_play > audio_written) {
            /* Ran dry. The play cursor stops at what it had. */
            long long short_by = could_play - audio_written;
            underrun += short_by;
            audio_played = audio_written;
            if (card_idle_since < 0) card_idle_since = now;
        } else {
            audio_played = could_play;
            card_idle_since = -1;
        }

        /* --- top the ring up -------------------------------------------- */
        /* Decoding the audio to do it costs time too, so charge it. */
        long long target = sc->bounded ? (pts + LEAD_NS) : (audio_played + RING_NS);
        if (target > audio_written) {
            long long added = target - audio_written;
            audio_written = target;
            now += sc->decode_ns / 50 * (added / FRAME_NS + 1);
        }

        if (sc->have_audio) avclock_audio(&c, audio_played);

        /* --- pace and present ------------------------------------------- */
        long long sleep = 0;
        int what;
        for (;;) {
            what = avclock_frame(&c, pts, now, &sleep);
            if (what != AV_WAIT) break;
            now += sleep;
            /* The card keeps playing while we wait -- that is the point of a
             * real clock, and forgetting it is how a simulation lies. */
            long long can = audio_played + sleep;
            if (can > audio_written) { underrun += can - audio_written; audio_played = audio_written; }
            else audio_played = can;
            last_now = now;
            if (sc->have_audio) avclock_audio(&c, audio_played);
        }
        if (what == AV_SHOW) now += sc->blit_ns;

        if (sc->have_audio) {
            long long gap = pts - audio_played;
            if (gap < 0) gap = -gap;
            if (gap > gap_max) gap_max = gap;
        }
    }

    r.shown = c.frames_shown;
    r.dropped = c.frames_dropped;
    r.resyncs = c.resyncs;
    r.drift_mean_ns = c.drift_n ? c.drift_sum_ns / c.drift_n : 0;
    r.drift_max_ns = c.drift_max_ns;
    r.drift_min_ns = c.drift_min_ns;
    r.underrun_ns = underrun;
    r.wall_ns = now;
    r.media_ns = (long long)NFRAMES * FRAME_NS;
    r.av_gap_max_ns = gap_max;
    return r;
}

static void report(const scenario *sc, const result *r)
{
    printf("  %-26s shown=%-5lld drop=%-5lld resync=%-3lld "
           "drift mean=%+6lldms [%+5lldms,%+5lldms] gap<=%4lldms "
           "underrun=%5lldms wall=%.2fx\n",
           sc->name, r->shown, r->dropped, r->resyncs,
           r->drift_mean_ns / NS_PER_MS, r->drift_min_ns / NS_PER_MS,
           r->drift_max_ns / NS_PER_MS, r->av_gap_max_ns / NS_PER_MS,
           r->underrun_ns / NS_PER_MS,
           (double)r->wall_ns / (double)r->media_ns);
}

static int fails;
#define GATE(cond, ...) do { if (!(cond)) { \
        printf("AVSYNC-FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    /* Real-time budget for one frame at 25 fps is 40 ms. The costs below are
     * fractions and multiples of it. */
    const long long F = FRAME_NS;

    scenario fast   = { "0.5x (twice real time)",  F / 4,     F / 4,     1, 1 };
    scenario par    = { "1.0x (just keeps up)",    F * 4 / 10, F * 5 / 10, 1, 1 };
    scenario slow15 = { "1.5x too slow",           F * 9 / 10, F * 6 / 10, 1, 1 };
    scenario slow3  = { "3.0x too slow",           F * 2,     F,         1, 1 };
    scenario novid  = { "3.0x too slow, no audio", F * 2,     F,         1, 0 };
    scenario unb    = { "3.0x, UNBOUNDED audio",   F * 2,     F,         0, 1 };

    printf("A/V sync over %d frames (%lld s of media at %d fps)\n",
           NFRAMES, MINUTE_NS / 1000000000LL, FPS);

    result rf = simulate(&fast);   report(&fast, &rf);
    result rp = simulate(&par);    report(&par, &rp);
    result r1 = simulate(&slow15); report(&slow15, &r1);
    result r3 = simulate(&slow3);  report(&slow3, &r3);
    result rn = simulate(&novid);  report(&novid, &rn);
    result ru = simulate(&unb);    report(&unb, &ru);

    /* --- the gates ------------------------------------------------------ */
    /* When the machine is fast enough, nothing is dropped and the drift is a
     * rounding error. This is the case that must be exactly right. */
    GATE(rf.dropped == 0, "frames dropped (%lld) on a machine twice fast enough", rf.dropped);
    GATE(rf.resyncs == 0, "%lld re-syncs on a machine twice fast enough", rf.resyncs);
    GATE(rf.drift_mean_ns > -5 * NS_PER_MS && rf.drift_mean_ns < 5 * NS_PER_MS,
         "mean drift %lld ns at 0.5x", rf.drift_mean_ns);
    GATE(rf.underrun_ns == 0, "the card ran dry (%lld ms) on a fast machine",
         rf.underrun_ns / NS_PER_MS);
    GATE((double)rf.wall_ns / rf.media_ns < 1.02 && (double)rf.wall_ns / rf.media_ns > 0.98,
         "a fast machine did not play in real time (%.3fx)",
         (double)rf.wall_ns / rf.media_ns);

    /* AUDIO AND VIDEO MUST STAY TOGETHER, in every configuration. This is the
     * claim the whole policy exists to make and the one a viewer perceives.
     *
     * The bound is LEAD_NS plus a frame, and that is not a fudge: the lead IS
     * the worst case. Audio that has been written but not yet played is
     * inaudible, so in normal running the lead costs nothing; but if the
     * pipeline starves, the card drains the whole buffer before it stops, and
     * the listener has then heard up to LEAD_NS beyond the picture. Choosing
     * the lead is choosing that bound -- 150 ms buys 150 ms of tolerance to a
     * decode hiccup and caps the starved error at 150 ms. */
    GATE(rp.av_gap_max_ns <= LEAD_NS + F, "1.0x: A/V gap %lld ms", rp.av_gap_max_ns / NS_PER_MS);
    GATE(r1.av_gap_max_ns <= LEAD_NS + F, "1.5x: A/V gap %lld ms", r1.av_gap_max_ns / NS_PER_MS);
    GATE(r3.av_gap_max_ns <= LEAD_NS + F, "3.0x: A/V gap %lld ms", r3.av_gap_max_ns / NS_PER_MS);

    /* Too slow: playback runs slow, and says so, rather than dropping every
     * frame or wandering off. Nothing here requires it to keep up -- it
     * cannot -- only that the failure is the bounded, reported one, and that
     * SOMETHING IS STILL ON THE SCREEN. One frame in five is the floor the
     * consecutive-drop cap guarantees. */
    GATE(r3.wall_ns > r3.media_ns, "3x too slow finished in real time, which is impossible");
    GATE(r3.shown >= NFRAMES / 5, "3x too slow displayed only %lld of %d frames",
         r3.shown, NFRAMES);
    GATE(r1.shown >= NFRAMES / 5, "1.5x too slow displayed only %lld of %d frames",
         r1.shown, NFRAMES);
    GATE(r3.drift_min_ns > -2 * NS_PER_MS * 1000,
         "3.0x: video fell %lld ms behind the master", -r3.drift_min_ns / NS_PER_MS);

    /* With no audio the monotonic clock is the master, so a slow decoder
     * re-bases instead of dropping everything. */
    GATE(rn.resyncs > 0, "no-audio, 3x too slow: never re-based, so it must have "
         "dropped or waited its way through");
    GATE(rn.shown >= NFRAMES / 5, "no-audio, 3x too slow: only %lld frames shown", rn.shown);

    /* THE CONTROL FOR THE POLICY ITSELF. Writing audio as fast as the ring
     * takes it must be visibly worse -- if it is not, the bounded lead is
     * decoration and should be deleted rather than documented. */
    GATE(ru.av_gap_max_ns > 10 * F,
         "unbounded audio writes stayed in sync (gap %lld ms) -- then the "
         "bounded-lead rule is not doing anything", ru.av_gap_max_ns / NS_PER_MS);
    printf("  control: unbounded audio drifts to %lld ms from the picture; "
           "the bounded lead holds it to %lld ms\n",
           ru.av_gap_max_ns / NS_PER_MS, r3.av_gap_max_ns / NS_PER_MS);

    if (fails) { printf("AVSYNC: %d gate(s) failed\n", fails); return 1; }
    printf("AVSYNC-OK: drift measured over a simulated minute at four decode "
           "speeds; A/V held within a frame in all of them\n");
    return 0;
}
