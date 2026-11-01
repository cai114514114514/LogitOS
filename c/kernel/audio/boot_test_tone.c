#include "boot_test_tone.h"

/* See boot_test_tone.h. Compiled only into HWHUD=1 kernels (LOGIT_HWHUD), or
 * into the host gate (BOOT_TEST_TONE_HOST_TEST), which takes just the pure
 * generator. Model and QEMU evidence only: not verified on X79-VG4 hardware. */
#if defined(LOGIT_HWHUD) || defined(BOOT_TEST_TONE_HOST_TEST)

/* A phase is a fraction of a turn in 1/65536ths, so one 16-bit value is exactly
 * one period of the sine and wraps for free. */
#define PHASE_TURN                      65536u
#define PHASE_HALF_TURN                 32768u

/* Bhaskara I's sine approximation (7th century; maximum error 0.0016):
 *   sin(pi*x) ~= 16 x (1 - x) / (5 - 4 x (1 - x))   for x in [0, 1].
 * With x = half / PHASE_HALF_TURN and t = half * (PHASE_HALF_TURN - half) the
 * pi cancels and the ratio becomes 16 t / (5 H^2 - 4 t), H = PHASE_HALF_TURN.
 * Integer-only on purpose, like pcm.c: the same arithmetic gives the same
 * samples on the host gate and in the kernel, with no libm or FPU state. */
#define BHASKARA_NUMERATOR_FACTOR       16
#define BHASKARA_DENOMINATOR_BASE       5
#define BHASKARA_DENOMINATOR_SLOPE      4

#define FADE_FRAME_COUNT \
    (BOOT_TEST_TONE_SAMPLE_RATE_HZ * BOOT_TEST_TONE_FADE_MS / 1000u)

/* sin(2 * pi * phase / PHASE_TURN) * amplitude, rounded toward zero. The sine
 * is evaluated on the first half turn and mirrored for the second, which is
 * what keeps the approximation's formula valid for the whole circle. */
static int32_t scaled_sine(uint32_t phase, int32_t amplitude)
{
    uint64_t position_in_half_turn = phase & (PHASE_HALF_TURN - 1u);
    uint64_t rise = position_in_half_turn * (PHASE_HALF_TURN - position_in_half_turn);
    uint64_t denominator = (uint64_t)BHASKARA_DENOMINATOR_BASE * PHASE_HALF_TURN * PHASE_HALF_TURN -
                           (uint64_t)BHASKARA_DENOMINATOR_SLOPE * rise;
    int64_t magnitude = (int64_t)amplitude * BHASKARA_NUMERATOR_FACTOR * (int64_t)rise /
                        (int64_t)denominator;

    return (int32_t)((phase & PHASE_HALF_TURN) ? -magnitude : magnitude);
}

/* Linear fade in over the first FADE_FRAME_COUNT frames and out over the last,
 * as a numerator over FADE_FRAME_COUNT. A tone that starts at full amplitude
 * clicks, and a click at boot is exactly the noise that makes people think the
 * speakers are broken. */
static uint32_t fade_numerator(uint32_t frame_index)
{
    if (frame_index < FADE_FRAME_COUNT) {
        return frame_index;
    }
    if (frame_index >= BOOT_TEST_TONE_FRAME_COUNT - FADE_FRAME_COUNT) {
        return BOOT_TEST_TONE_FRAME_COUNT - frame_index;
    }
    return FADE_FRAME_COUNT;
}

void boot_test_tone_generate(int16_t *stereo_frames, uint32_t first_frame_index,
                             uint32_t frame_count)
{
    for (uint32_t offset = 0; offset < frame_count; offset++) {
        uint32_t frame_index = first_frame_index + offset;
        int16_t sample = 0;

        if (frame_index < BOOT_TEST_TONE_FRAME_COUNT) {
            uint64_t turns = (uint64_t)frame_index * BOOT_TEST_TONE_FREQUENCY_HZ * PHASE_TURN /
                             BOOT_TEST_TONE_SAMPLE_RATE_HZ;
            int32_t unfaded = scaled_sine((uint32_t)(turns % PHASE_TURN),
                                          BOOT_TEST_TONE_PEAK_AMPLITUDE);

            sample = (int16_t)(unfaded * (int32_t)fade_numerator(frame_index) /
                               (int32_t)FADE_FRAME_COUNT);
        }
        stereo_frames[2u * offset] = sample;
        stereo_frames[2u * offset + 1u] = sample;
    }
}

#ifndef BOOT_TEST_TONE_HOST_TEST
#include "snd.h"
#include "kprintf.h"
#include "sched.h"
#include "kernel/sync/wait.h"
#include "bootmod.h"
#include "../../../include/weaksym.h"

/* The HDA driver suspends auto-mute while the tone plays (hda.c). Weak, as the
 * other cross-layer hooks are: the host gates link this file without a driver,
 * and a kernel whose only sound device is not HDA has nothing to suspend. */
void hda_automute_suspend(int suspended) LOGIT_WEAK;
LOGIT_WEAK_STUB(hda_automute_suspend);

/* Ask for every jack to drive (or return to auto-mute) when the driver exists. */
static void tone_automute_suspend(int suspended)
{
    if (LOGIT_HAVE(hda_automute_suspend)) {
        hda_automute_suspend(suspended);
    }
}

/* How long after the first panel tick (the desktop is running by then) the
 * tone starts: long enough for the boot chatter to finish and for a person to
 * be looking at the screen and listening. */
#define TONE_START_DELAY_MS             5000u

/* The flag that silences the tone: a boot module of this name. */
#define TONE_SILENCE_MODULE_NAME        "NOSOUND"

/* Frames written to the stream per call: one HDA period (1024 frames), so each
 * write is about one refill and the thread never holds more than 4 KiB. */
#define TONE_WRITE_CHUNK_FRAMES         1024u
#define TONE_BYTES_PER_FRAME            4u

/* snd_stream_write() returns 0 after waiting 500 ms for ring room without
 * getting any (the card has stopped consuming). Give up after this many
 * consecutive such stalls instead of holding a thread forever. */
#define TONE_MAX_CONSECUTIVE_STALLS     4u

/* After the last write the tone is still in two queues: the stream's own ring
 * (snd_stream_close with drain waits for the mixer to empty it) and the
 * hardware ring (the DMA engine plays it out, about 170 ms for the HDA
 * driver). Closing returns when the first is empty; the thread then polls the
 * engine position every TONE_POLL_MS until it covers the whole tone, so the log
 * reports a position that really includes the tail. The limit is a generous
 * multiple of the hardware ring: it exists only so a stuck card cannot hold
 * the thread forever. */
#define TONE_POLL_MS                    10u
/* Longer than one jack poll interval (250 ms, hda.c), so the poll that
 * applies the auto-mute suspension has run before the tone starts. */
#define TONE_AUTOMUTE_SETTLE_MS         300u
#define TONE_PLAY_OUT_TIMEOUT_MS        1000u

static int g_state = BOOT_TEST_TONE_WAITING;
static uint64_t g_first_poll_milliseconds;
static int g_first_poll_seen;
static int g_thread_started;

/* The state is written by the tone thread and read by the panel on the window
 * manager thread, so both go through atomics. */
static void set_state(enum boot_test_tone_state state)
{
    __atomic_store_n(&g_state, (int)state, __ATOMIC_RELEASE);
}

static enum boot_test_tone_state get_state(void)
{
    return (enum boot_test_tone_state)__atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
}

/* The tone thread's chunk, static rather than on the stack: kernel thread
 * stacks are small (see the overflow notes in mixer.c) and only one tone ever
 * plays. */
static int16_t g_chunk[TONE_WRITE_CHUNK_FRAMES * 2u];

/* Play the whole tone through the ordinary application path: open a stream,
 * write the samples, close with drain. Logs the hardware position before and
 * after, because "the stream position advanced" is the evidence that bytes
 * actually left through the sound card and did not merely vanish into the
 * mixer. Runs on its own thread so its blocking writes never stall the window
 * manager. */
static void boot_test_tone_thread(void)
{
    struct logit_sndfmt format = {
        .rate = BOOT_TEST_TONE_SAMPLE_RATE_HZ,
        .channels = 2,
        .format = SND_FMT_S16,
        .buffer_ms = 0,
        .flags = 0
    };
    uint64_t position_before;
    uint64_t position_after;
    uint32_t frames_written = 0;
    unsigned consecutive_stalls = 0;
    int start_announced = 0;
    struct logit_sndstate stream_state = {0};
    int stream;

    /* Every jack sounds for the tone, whatever the jack sense says, then the
     * normal auto-mute policy returns. The wait lets the next jack poll
     * (every HDA_JACK_POLL_INTERVAL_MS, 250 ms) apply it before playback. */
    if (LOGIT_HAVE(hda_automute_suspend)) {
        tone_automute_suspend(1);
        sched_sleep_ms(TONE_AUTOMUTE_SETTLE_MS);
    }
    stream = snd_stream_open(NULL, &format);

    if (stream < 0) {
        tone_automute_suspend(0);
        kprintf("[hda] test tone skipped: no sound stream could be opened (%d)\n", stream);
        set_state(BOOT_TEST_TONE_NO_DEVICE);
        return;
    }
    position_before = snd_device_position_frames();
    set_state(BOOT_TEST_TONE_PLAYING);
    while (frames_written < BOOT_TEST_TONE_FRAME_COUNT && consecutive_stalls < TONE_MAX_CONSECUTIVE_STALLS) {
        uint32_t chunk_frames = BOOT_TEST_TONE_FRAME_COUNT - frames_written;
        int accepted;

        if (chunk_frames > TONE_WRITE_CHUNK_FRAMES) {
            chunk_frames = TONE_WRITE_CHUNK_FRAMES;
        }
        boot_test_tone_generate(g_chunk, frames_written, chunk_frames);
        accepted = snd_stream_write(NULL, stream, g_chunk, (int)(chunk_frames * TONE_BYTES_PER_FRAME));
        if (accepted < 0) {
            break;
        }
        if (accepted == 0) {
            consecutive_stalls++;
            continue;
        }
        consecutive_stalls = 0;
        frames_written += (uint32_t)accepted / TONE_BYTES_PER_FRAME;
        /* Announced after the first chunk is queued, not before: the card's
         * engine is already running, and a serial print ahead of the first
         * write would let the mixer find an empty stream and count an underrun
         * that the tone itself caused. */
        if (!start_announced) {
            start_announced = 1;
            kprintf("[hda] test tone start: %u Hz, %u ms, peak %d of 32768, hardware position %llu frames\n",
                    BOOT_TEST_TONE_FREQUENCY_HZ, BOOT_TEST_TONE_DURATION_MS,
                    BOOT_TEST_TONE_PEAK_AMPLITUDE, (unsigned long long)position_before);
        }
    }
    /* The stream's own underrun count, read while the stream is still open and
     * still holds data: once the ring runs dry the mixer counts that end of
     * stream as one more underrun, which would make every clean tone look like
     * a fault. */
    (void)snd_stream_state(NULL, stream, &stream_state);
    (void)snd_stream_close(NULL, stream, 1);
    position_after = snd_device_position_frames();
    for (uint32_t waited_ms = 0; waited_ms < TONE_PLAY_OUT_TIMEOUT_MS &&
         frames_written == BOOT_TEST_TONE_FRAME_COUNT &&
         position_after - position_before < BOOT_TEST_TONE_FRAME_COUNT;
         waited_ms += TONE_POLL_MS) {
        sched_sleep_ms(TONE_POLL_MS);
        position_after = snd_device_position_frames();
    }
    tone_automute_suspend(0);
    if (frames_written < BOOT_TEST_TONE_FRAME_COUNT) {
        kprintf("[hda] test tone FAILED: wrote %u of %u frames\n", frames_written,
                BOOT_TEST_TONE_FRAME_COUNT);
        set_state(BOOT_TEST_TONE_FAILED);
        return;
    }
    kprintf("[hda] test tone done: %u frames written, hardware position "
            "%llu -> %llu frames (advanced %llu), underruns %u\n",
            frames_written, (unsigned long long)position_before,
            (unsigned long long)position_after,
            (unsigned long long)(position_after - position_before), stream_state.underruns);
    set_state(BOOT_TEST_TONE_DONE);
}

void boot_test_tone_poll(uint64_t uptime_milliseconds)
{
    if (get_state() != BOOT_TEST_TONE_WAITING || g_thread_started) {
        return;
    }
    if (!g_first_poll_seen) {
        g_first_poll_seen = 1;
        g_first_poll_milliseconds = uptime_milliseconds;
        if (bootmod_find(TONE_SILENCE_MODULE_NAME, NULL)) {
            kprintf("[hda] test tone skipped: boot module " TONE_SILENCE_MODULE_NAME " present\n");
            set_state(BOOT_TEST_TONE_SKIPPED_NOSOUND);
            return;
        }
        kprintf("[hda] test tone armed: starts %u ms after the desktop\n", TONE_START_DELAY_MS);
    }
    if (uptime_milliseconds - g_first_poll_milliseconds < TONE_START_DELAY_MS) {
        return;
    }
    g_thread_started = 1;
    if (thread_create(boot_test_tone_thread, "hdatone") < 0) {
        kprintf("[hda] test tone FAILED: no thread\n");
        set_state(BOOT_TEST_TONE_FAILED);
    }
}

const char *boot_test_tone_state_name(void)
{
    switch (get_state()) {
    case BOOT_TEST_TONE_PLAYING:
        return "playing";
    case BOOT_TEST_TONE_DONE:
        return "done";
    case BOOT_TEST_TONE_SKIPPED_NOSOUND:
        return "skipped";
    case BOOT_TEST_TONE_NO_DEVICE:
        return "no-device";
    case BOOT_TEST_TONE_FAILED:
        return "failed";
    case BOOT_TEST_TONE_WAITING:
        break;
    }
    return "waiting";
}
#endif /* !BOOT_TEST_TONE_HOST_TEST */

#endif /* LOGIT_HWHUD || BOOT_TEST_TONE_HOST_TEST */
