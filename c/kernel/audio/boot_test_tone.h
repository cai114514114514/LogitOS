#ifndef BOOT_TEST_TONE_H
#define BOOT_TEST_TONE_H

/* A one-second 440 Hz test tone, played once per boot a few seconds after the
 * desktop appears, in HWHUD=1 builds only.
 *
 * WHY. On the X79-VG4 nobody can tell whether silence means "no sound path",
 * "wrong jack" or "volume down". A tone that plays by itself, goes through the
 * same stream/mixer/ring/DMA path every application uses, and leaves a log line
 * plus a panel entry saying how far the hardware position advanced, turns that
 * guess into a measurement. Release builds do not carry it: a kernel must not
 * make noise at boot unasked.
 *
 * Silence it by putting a boot module named NOSOUND on the boot stick (a file
 * NOSOUND in \LOGIT\FW\ listed in INDEX.TXT; the loader hands those files to
 * the kernel as boot modules, c/kernel/init/bootmod.h). */

#include <stdint.h>

/* The tone itself. Fixed numbers, because "moderate volume" is a safety
 * property, not a preference: -12 dBFS cannot hurt speakers or ears. */
#define BOOT_TEST_TONE_SAMPLE_RATE_HZ       48000u
#define BOOT_TEST_TONE_FREQUENCY_HZ         440u
#define BOOT_TEST_TONE_DURATION_MS          1000u
#define BOOT_TEST_TONE_PEAK_AMPLITUDE       8192
#define BOOT_TEST_TONE_FADE_MS              10u
#define BOOT_TEST_TONE_FRAME_COUNT \
    (BOOT_TEST_TONE_SAMPLE_RATE_HZ * BOOT_TEST_TONE_DURATION_MS / 1000u)

/* Fill `frame_count` interleaved stereo s16 frames of the tone, starting at
 * frame `first_frame_index` of the whole tone. Pure: no clock, no state, so the
 * host gate can check the pitch, peak and fades sample by sample. Frames at or
 * beyond BOOT_TEST_TONE_FRAME_COUNT are written as silence. */
void boot_test_tone_generate(int16_t *stereo_frames, uint32_t first_frame_index,
                             uint32_t frame_count);

/* Where the tone is in its life, for the log and the panel. */
enum boot_test_tone_state {
    BOOT_TEST_TONE_WAITING = 0,
    BOOT_TEST_TONE_PLAYING = 1,
    BOOT_TEST_TONE_DONE = 2,
    BOOT_TEST_TONE_SKIPPED_NOSOUND = 3,
    BOOT_TEST_TONE_NO_DEVICE = 4,
    BOOT_TEST_TONE_FAILED = 5
};

/* Called from the panel's once-a-second tick (WM thread). Starts the tone the
 * first time at least five seconds have passed since the first call. Never
 * blocks: the playing happens on its own kernel thread. */
void boot_test_tone_poll(uint64_t uptime_milliseconds);

/* "waiting", "playing", "done", "skipped", "no-device" or "failed". */
const char *boot_test_tone_state_name(void);

#endif
