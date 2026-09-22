/* sndtest -- the ring-3 instrument for the audio stack.
 *
 * Sound is hard to assert on, so this program's job is to make it easy: it
 * generates signals whose CORRECTNESS IS RECOVERABLE FROM THE CAPTURED SAMPLES
 * ALONE. QEMU's `wav` audiodev writes what the guest actually played to a file,
 * and tests/boot/run-audio-wav-test.sh checks that file sample by sample. So
 * "did it play" becomes an artefact, and every claim below is a property of
 * that artefact rather than of a log line.
 *
 * Modes:
 *
 *   info      Print the device the kernel found and exit 0. Exits 0 WITH NO
 *             CARD TOO -- that is the point: a machine with no sound hardware
 *             must degrade to silence, not to an error.
 *
 *   ramp [ms] A 200 Hz sawtooth whose every sample encodes its own frame index:
 *             left = (i % 240) * 256 - 30720, right = -left. The step of 256 is
 *             enormous compared to any rounding the host mixer can introduce, so
 *             the checker recovers i exactly and requires the recovered indices
 *             to advance by ONE, forever. That single property catches a
 *             dropped period, a repeated period, a swapped pair of periods and a
 *             wrong sample rate -- all of which are invisible to a test that
 *             only asks whether bytes were written. Right = -left additionally
 *             pins the channel mapping.
 *
 *   mix       Two streams at once: A holds +8000 for the whole run, B holds
 *             -3000 and STARTS AND STOPS while A is still playing. The capture
 *             must read 8000, then 5000, then 8000. A mixer that overwrites
 *             instead of summing gives -3000 in the middle; one that wraps
 *             instead of clamping is caught by the host test.
 *
 *   underrun  Deliberately run the ring dry: tone, then STOP WRITING for longer
 *             than the whole hardware buffer, then tone again. The gap must be
 *             SILENCE and the tone must come back. A driver that leaves the
 *             stale buffer alone repeats the last period forever, which sounds
 *             like a stutter and passes any "is it still playing" check.
 *
 * Every mode prints a single SNDTEST_* line the harness greps for, so a failure
 * that happens before any audio is produced is still diagnosable.
 */
#include "clib.h"

#define RATE      48000
#define CHANNELS  2
#define RAMP_MOD  240          /* 48000/240 = a 200 Hz sawtooth */
#define RAMP_STEP 256
#define RAMP_BASE (-30720)

/* One chunk of frames per write. 1024 frames == the driver's period, so the
 * write loop runs at the same granularity the hardware consumes. */
#define CHUNK_FRAMES 1024
static short buf[CHUNK_FRAMES * CHANNELS];

static void print_info(struct logit_sndinfo *si)
{
    outs("SNDTEST_INFO driver="); outs(si->driver[0] ? si->driver : "none");
    outs(" codec="); outs(si->codec[0] ? si->codec : "-");
    outs(" rate="); outn((long)si->rate);
    outs(" ch="); outn((long)si->channels);
    outs(" period="); outn((long)si->period_bytes);
    outs(" periods="); outn((long)si->periods);
    outs(" streams_max="); outn((long)si->streams_max);
    outs(" irq="); outn((long)si->irq_mode);
    outs("\n");
}

/* Fill `frames` of the shared buffer with the index-encoded sawtooth, starting
 * at absolute frame `base`. */
static void fill_ramp(long base, int frames)
{
    for (int i = 0; i < frames; i++) {
        short v = (short)((int)((base + i) % RAMP_MOD) * RAMP_STEP + RAMP_BASE);
        buf[i * 2 + 0] = v;
        buf[i * 2 + 1] = (short)-v;
    }
}

static void fill_const(int frames, short v)
{
    for (int i = 0; i < frames; i++) { buf[i * 2] = v; buf[i * 2 + 1] = v; }
}

/* Write one buffer completely. snd_write_all handles the short writes; a
 * negative return means the device went away, which is fatal for a test. */
static int push(int h, int frames)
{
    int rc = snd_write_all(h, buf, frames * CHANNELS * (int)sizeof(short));
    return rc < 0 ? rc : 0;
}

static int mode_ramp(int ms)
{
    long total = (long)RATE * ms / 1000, done = 0;
    int h = snd_open_s16(RATE, CHANNELS);
    if (h < 0) { outs("SNDTEST_FAIL open="); outn(h); outs("\n"); return 1; }

    while (done < total) {
        int n = (int)(total - done);
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
        fill_ramp(done, n);
        if (push(h, n) < 0) { outs("SNDTEST_FAIL write\n"); return 1; }
        done += n;
    }
    snd_close(h, 1);                       /* drain: play out what is queued */

    outs("SNDTEST_RAMP_DONE frames="); outn(done);
    outs(" mod="); outn(RAMP_MOD);
    outs(" step="); outn(RAMP_STEP);
    outs(" base="); outn(RAMP_BASE);
    outs("\n");
    return 0;
}

static int mode_mix(void)
{
    /* A runs the whole time; B lives only in the middle third. The harness
     * asserts three plateaus, so the transitions are what is being tested, not
     * merely that two handles could be opened. */
    const int seg = RATE * 400 / 1000;     /* 400 ms per segment, in frames */
    int a = snd_open_s16(RATE, CHANNELS);
    int b;
    if (a < 0) { outs("SNDTEST_FAIL open_a="); outn(a); outs("\n"); return 1; }

    for (int i = 0; i < seg; i += CHUNK_FRAMES) {
        int n = seg - i < CHUNK_FRAMES ? seg - i : CHUNK_FRAMES;
        fill_const(n, 8000);
        if (push(a, n) < 0) return 1;
    }

    b = snd_open_s16(RATE, CHANNELS);
    if (b < 0) { outs("SNDTEST_FAIL open_b="); outn(b); outs("\n"); return 1; }
    outs("SNDTEST_MIX_B_OPEN\n");

    /* Both streams fed over the same span. Written A-chunk then B-chunk so
     * neither can get arbitrarily far ahead of the other -- the mix is only
     * meaningful where both rings hold data for the same period. */
    for (int i = 0; i < seg; i += CHUNK_FRAMES) {
        int n = seg - i < CHUNK_FRAMES ? seg - i : CHUNK_FRAMES;
        fill_const(n, 8000);  if (push(a, n) < 0) return 1;
        fill_const(n, -3000); if (push(b, n) < 0) return 1;
    }

    snd_close(b, 1);
    outs("SNDTEST_MIX_B_CLOSED\n");

    for (int i = 0; i < seg; i += CHUNK_FRAMES) {
        int n = seg - i < CHUNK_FRAMES ? seg - i : CHUNK_FRAMES;
        fill_const(n, 8000);
        if (push(a, n) < 0) return 1;
    }
    snd_close(a, 1);

    outs("SNDTEST_MIX_DONE seg_frames="); outn(seg);
    outs(" a=8000 b=-3000 sum=5000\n");
    return 0;
}

static int mode_underrun(void)
{
    struct logit_sndstate st;
    const int seg = RATE * 300 / 1000;     /* 300 ms of tone */
    int h = snd_open_s16(RATE, CHANNELS);
    if (h < 0) { outs("SNDTEST_FAIL open="); outn(h); outs("\n"); return 1; }

    for (int i = 0; i < seg; i += CHUNK_FRAMES) {
        int n = seg - i < CHUNK_FRAMES ? seg - i : CHUNK_FRAMES;
        fill_const(n, 12000);
        if (push(h, n) < 0) return 1;
    }

    /* Stop writing for longer than the ENTIRE hardware buffer plus the stream
     * ring, so the engine is guaranteed to run out of real data. Sleeping (not
     * spinning) so the rest of the machine is demonstrably fine meanwhile. */
    outs("SNDTEST_UNDERRUN_GAP_START\n");
    sys_sleep_ms(600);
    outs("SNDTEST_UNDERRUN_GAP_END\n");

    for (int i = 0; i < seg; i += CHUNK_FRAMES) {
        int n = seg - i < CHUNK_FRAMES ? seg - i : CHUNK_FRAMES;
        fill_const(n, 12000);
        if (push(h, n) < 0) return 1;
    }

    if (snd_state(h, &st) == 0) {
        /* The stream must have NOTICED it ran dry. A driver that silently
         * papers over an underrun is harder to debug than one that glitches. */
        outs("SNDTEST_UNDERRUN_DONE underruns="); outn((long)st.underruns);
        outs(" written="); outn((long)st.frames_written);
        outs(" played="); outn((long)st.frames_played);
        outs("\n");
    } else {
        outs("SNDTEST_FAIL state\n");
        return 1;
    }
    snd_close(h, 1);
    return 0;
}

int main(int argc, char **argv)
{
    struct logit_sndinfo si;
    const char *mode = argc > 1 ? argv[1] : "info";
    int have = snd_info(&si);

    print_info(&si);

    if (!have) {
        /* THE NO-CARD PATH, and it exits 0 on purpose. A machine with no sound
         * hardware is not a broken machine; it is a silent one. Anything that
         * treats SND_E_NODEV as fatal would make sound a boot requirement. */
        int h = snd_open_s16(RATE, CHANNELS);
        outs("SNDTEST_NODEV open_returned="); outn(h);
        outs(" (want "); outn(SND_E_NODEV); outs(")\n");
        outs(h == SND_E_NODEV ? "SNDTEST_NODEV_OK\n" : "SNDTEST_NODEV_BAD\n");
        return 0;
    }

    if (c_streq(mode, "info"))     return 0;
    if (c_streq(mode, "ramp"))     return mode_ramp(argc > 2 ? c_atoi(argv[2]) : 1000);
    if (c_streq(mode, "mix"))      return mode_mix();
    if (c_streq(mode, "underrun")) return mode_underrun();

    outs("SNDTEST_FAIL unknown mode\n");
    return 1;
}
