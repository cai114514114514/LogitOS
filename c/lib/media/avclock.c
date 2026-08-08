/* c/lib/media/avclock.c -- the A/V clock and the falling-behind policy.
 *
 * A container hands out two streams of timestamps that were written by an
 * encoder on some other machine and mean nothing until something here decides
 * what "now" is. That decision is the whole of A/V synchronisation, and on
 * THIS machine it is not academic: a from-scratch H.264 decoder under QEMU's
 * TCG is nowhere near real time, so the interesting case -- decode falls
 * behind -- is the normal case and has to be decided deliberately.
 *
 * WHY AUDIO IS THE MASTER.
 * Because the sound card is a real clock. It consumes exactly `rate` frames a
 * second whatever the rest of the machine is doing, so the number of frames it
 * has PLAYED is a measurement rather than an estimate. Nothing else here is:
 * the monotonic clock is real too, but it says how much time passed, not how
 * much media came out, and those differ the moment the pipeline stalls.
 *
 * There is a second reason, which is about people rather than clocks. A
 * dropped video frame is invisible. A gap in audio is a click that everybody
 * hears, and a resampled or stretched one is worse. So audio is never dropped,
 * never stretched, and never waited for; video is what gives way.
 *
 * AND WHY THAT DEGRADES CORRECTLY WHEN THE MACHINE IS TOO SLOW.
 * The player writes audio AHEAD OF THE DECODED VIDEO POSITION BY A BOUNDED
 * LEAD, and no further. That one rule is what makes the whole thing degrade
 * the right way, and it is worth being exact about, because the alternative is
 * tempting and wrong.
 *
 * The alternative is to write audio as fast as the ring accepts it. Do that on
 * a machine whose video decode runs at a third of real time and the sound card
 * -- which consumes at real time whatever else happens -- races ahead: the
 * audio finishes minutes before the picture, and the two are not merely out of
 * sync, they are no longer playing the same part of the film.
 *
 * Gating the writes on video progress means that when decode falls behind, the
 * player stops topping the ring up, the card drains, and frames_played STOPS
 * ADVANCING. The master clock therefore stalls exactly as much as the pipeline
 * does, and video stays in step with the audio that actually came out of the
 * speaker -- which is what a viewer perceives as being in sync. The cost is
 * real and audible: gaps. That is the honest failure, it is reported as an
 * underrun count rather than hidden, and it is the one that keeps A and V
 * together. make test-avsync measures both alternatives over a simulated
 * minute at four decode speeds.
 *
 * WHAT HAPPENS TO A LATE FRAME.
 * It is still DECODED. It has to be: a P frame is the reference for the next
 * one, so skipping a decode corrupts every frame until the next keyframe --
 * far more visible than the frame that was late. What is skipped is the colour
 * conversion and the blit, which is where this player's per-frame time
 * actually goes (a 640x360 YUV->RGBA pass plus a scaled blit through the
 * window system, per frame, versus the decode itself).
 *
 * RE-BASING. If video is behind by more than resync_ns, chasing is pointless:
 * a decoder that is permanently 3x too slow will never catch up and every
 * frame from then on is "late", so every frame gets dropped and nothing is
 * displayed at all. Past that threshold the clock is re-anchored, the count of
 * re-bases is reported, and playback runs slow. Slow and visible beats
 * in-time and blank -- and the resync count is what keeps "it played" and "it
 * played in real time" from being confused for each other.
 *
 * The clock takes `now_ns` as an argument and never reads the machine's, so
 * the whole policy is exercised on the host by tests/unit/avsync_test.c over a
 * simulated minute at several decode speeds. A sync policy that is only
 * observable by watching a video is not a measurement.
 */
#include "media.h"

void avclock_init(avclock *c, int have_audio)
{
    c->start_ns = 0;
    c->anchor_pts_ns = 0;
    c->audio_pts_ns = -1;
    c->offset_ns = 0;
    c->max_sleep_ns = 250000000LL;   /* never park on one absurd timestamp */
    c->resync_ns = 1000000000LL;     /* a second behind: stop chasing */
    c->drop_ns = 50000000LL;         /* two frames at 25 fps, and about the
                                      * threshold at which lip-sync error
                                      * becomes visible rather than felt */
    /* AND A FLOOR UNDER THE DROPPING. Without this the policy has a failure
     * mode that looks like success: when the master clock stalls WITH the
     * pipeline -- which is exactly what the bounded audio lead makes it do --
     * every frame is a fixed amount late, every frame is therefore dropped,
     * and the screen stays black for the whole film while the counters report
     * a small, steady drift. Dropping only helps when it lets us catch up, and
     * a frozen master cannot be caught up with. So at most four frames in a
     * row are skipped: one in five is always painted. Playback is slow and
     * visibly jerky, which is the truth about the machine. */
    c->max_drop_run = 4;
    c->drop_run = 0;
    c->frames_shown = c->frames_dropped = c->resyncs = 0;
    c->drift_sum_ns = c->drift_n = 0;
    c->drift_max_ns = -0x7FFFFFFFFFFFFFFFLL;
    c->drift_min_ns = 0x7FFFFFFFFFFFFFFFLL;
    c->last_drift_ns = 0;
    c->started = 0;
    c->have_audio = have_audio ? 1 : 0;
}

void avclock_audio(avclock *c, long long played_pts_ns)
{
    /* Monotone by construction: the card cannot un-play a frame, and a
     * backwards master would make video jump. A caller that seeks calls
     * avclock_init again. */
    if (played_pts_ns > c->audio_pts_ns) c->audio_pts_ns = played_pts_ns;
}

/* Where the master says we are, in the file's presentation timeline. */
static long long master_ns(const avclock *c, long long now_ns)
{
    if (c->have_audio && c->audio_pts_ns >= 0)
        return c->audio_pts_ns + c->offset_ns;
    return c->anchor_pts_ns + (now_ns - c->start_ns);
}

static void record(avclock *c, long long drift)
{
    c->last_drift_ns = drift;
    c->drift_sum_ns += drift;
    c->drift_n++;
    if (drift > c->drift_max_ns) c->drift_max_ns = drift;
    if (drift < c->drift_min_ns) c->drift_min_ns = drift;
}

int avclock_frame(avclock *c, long long pts_ns, long long now_ns, long long *sleep_ns)
{
    if (sleep_ns) *sleep_ns = 0;

    if (!c->started) {
        c->started = 1;
        c->start_ns = now_ns;
        c->anchor_pts_ns = pts_ns;
        /* With an audio master the offset makes the first video frame agree
         * with wherever audio has got to, which is not necessarily zero: the
         * player may have queued audio before the first frame decoded. */
        if (c->have_audio && c->audio_pts_ns >= 0)
            c->offset_ns = pts_ns - c->audio_pts_ns;
        record(c, 0);
        c->frames_shown++;
        return AV_SHOW;
    }

    long long m = master_ns(c, now_ns);
    long long drift = pts_ns - m;          /* + = video ahead, - = video behind */

    if (drift > 0) {
        /* Early. Wait -- but bounded, so one wild timestamp in a corrupt file
         * cannot park the player for an hour. */
        long long s = drift;
        if (s > c->max_sleep_ns) s = c->max_sleep_ns;
        if (sleep_ns) *sleep_ns = s;
        return AV_WAIT;
    }

    if (-drift > c->resync_ns) {
        /* Permanently behind. Re-anchor instead of dropping for ever. */
        c->resyncs++;
        if (c->have_audio && c->audio_pts_ns >= 0)
            c->offset_ns = pts_ns - c->audio_pts_ns;
        else {
            c->start_ns = now_ns;
            c->anchor_pts_ns = pts_ns;
        }
        record(c, drift);
        c->drop_run = 0;
        c->frames_shown++;
        return AV_SHOW;
    }

    record(c, drift);
    if (-drift > c->drop_ns && c->drop_run < c->max_drop_run) {
        c->drop_run++;
        c->frames_dropped++;
        return AV_DROP;
    }
    c->drop_run = 0;
    c->frames_shown++;
    return AV_SHOW;
}

long long avclock_drift_ns(const avclock *c) { return c->last_drift_ns; }
