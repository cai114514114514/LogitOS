/* Host unit tests for the audio PCM layer -- c/kernel/audio/pcm.c compiled
 * unchanged and driven directly.
 *
 * Audio is hard to assert on once it is sound, so everything that CAN be a pure
 * function is one, and this is where those are pinned down. The three bugs this
 * file exists to catch, because each is inaudible in a unit-less "did it play"
 * check and unmistakable in a listener's ears:
 *
 *   1. A mix that wraps instead of clamping. Two loud streams summed in int16
 *      wrap to the opposite rail -- the loudest possible noise from the
 *      quietest possible mistake.
 *   2. A resampler that restarts its phase every period. Sounds fine on one
 *      buffer and clicks 47 times a second in a stream, so a single-buffer test
 *      passes it. `resample_seam` splits an input in two and requires the
 *      result to be BYTE-IDENTICAL to the one-shot conversion.
 *   3. U8 silence taken to be 0. It is 0x80; zero is full-scale negative DC, a
 *      thump on every start and a hiss forever after.
 *
 * Build: make test-audio-pcm    (host, no QEMU)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "snd.h"

static int checks = 0, fails = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

static void eqi(long got, long want, const char *what)
{
    checks++;
    if (got != want) { fails++; printf("FAIL: %s: got %ld want %ld\n", what, got, want); }
}

/* ------------------------------------------------------------- formats --- */
static void t_formats(void)
{
    eqi(snd_fmt_bytes(SND_FMT_S16), 2, "s16 is 2 bytes");
    eqi(snd_fmt_bytes(SND_FMT_U8),  1, "u8 is 1 byte");
    eqi(snd_fmt_bytes(SND_FMT_S32), 4, "s32 is 4 bytes");
    eqi(snd_fmt_bytes(SND_FMT_F32), 4, "f32 is 4 bytes");
    eqi(snd_fmt_bytes(99), 0, "unknown format has no size");

    ok(snd_fmt_ok(48000, 2, SND_FMT_S16), "48k stereo s16 accepted");
    ok(snd_fmt_ok(44100, 1, SND_FMT_F32), "44.1k mono f32 accepted");
    ok(!snd_fmt_ok(48000, 2, 99),   "unknown format rejected");
    ok(!snd_fmt_ok(48000, 0, SND_FMT_S16), "zero channels rejected");
    /* 3..8 channels are ACCEPTED, and that is deliberate -- see the comment on
     * snd_fmt_ok. A 5.1 FLAC is 6 channels; refusing it would make the file
     * unplayable rather than merely un-surrounded, so the layer takes the front
     * pair and drops the rest. This assertion used to read "3 channels
     * rejected", which was the design before that decision and made the whole
     * suite fail against the shipped code. What the drop actually DOES is
     * asserted in t_convert(); accepting the format is only half a claim. */
    ok(snd_fmt_ok(48000, 3, SND_FMT_S16), "3 channels accepted (front pair taken)");
    ok(snd_fmt_ok(48000, 6, SND_FMT_S16), "5.1 accepted rather than unplayable");
    ok(!snd_fmt_ok(48000, 9, SND_FMT_S16), "9 channels rejected (past the ceiling)");
    /* The rate bound is not fussiness: rate feeds a Q16.16 step and an
     * unbounded ratio overflows it. A rejected rate is a returned error; an
     * accepted-then-overflowing one is noise. */
    ok(!snd_fmt_ok(100, 2, SND_FMT_S16),    "4 kHz floor enforced");
    ok(!snd_fmt_ok(400000, 2, SND_FMT_S16), "192 kHz ceiling enforced");
}

/* ---------------------------------------------------------- conversion --- */
static void t_convert(void)
{
    int16_t out[8];

    /* s16 stereo -> s16 stereo is the identity, exactly. */
    {
        int16_t in[4] = { -32768, 32767, 1234, -1 };
        pcm_to_s16(out, 2, in, 2, SND_FMT_S16, 2);
        ok(memcmp(out, in, sizeof in) == 0, "s16->s16 stereo is bit-identical");
    }

    /* U8: 0x80 is silence, 0x00 is full negative, 0xFF is near full positive. */
    {
        uint8_t in[2] = { 0x80, 0x00 };
        pcm_to_s16(out, 1, in, 1, SND_FMT_U8, 2);
        eqi(out[0], 0, "u8 0x80 converts to exact silence");
        eqi(out[1], -32768, "u8 0x00 converts to full-scale negative");
    }

    /* S32 keeps the top 16 bits. */
    {
        int32_t in[2] = { 0x12340000, -0x08000000 };
        pcm_to_s16(out, 1, in, 1, SND_FMT_S32, 2);
        eqi(out[0], 0x1234, "s32 keeps the high 16 bits");
        eqi(out[1], (int16_t)0xF800, "s32 negative keeps sign");
    }

    /* F32: in range, and both rails, and the values a decoder emits when it is
     * having a bad day. A NaN cast straight to int is undefined; it must become
     * a sample, not a trap. */
    {
        float in[5];
        in[0] = 0.0f; in[1] = 1.0f; in[2] = -1.0f; in[3] = 4.0f;
        in[4] = (float)NAN;
        pcm_to_s16(out, 1, in, 1, SND_FMT_F32, 5);
        eqi(out[0], 0, "f32 0.0 is silence");
        eqi(out[1], 32767, "f32 +1.0 is full scale");
        eqi(out[2], -32767, "f32 -1.0 is full scale negative");
        eqi(out[3], 32767, "f32 above +1.0 clamps, does not wrap");
        ok(out[4] >= -32768 && out[4] <= 32767, "f32 NaN produces a finite sample");
    }

    /* Mono fans out to both speakers. A mono decoder heard only on the left is
     * the classic version of this bug. */
    {
        int16_t in[2] = { 1000, -2000 };
        pcm_to_s16(out, 2, in, 1, SND_FMT_S16, 2);
        eqi(out[0], 1000, "mono->stereo fills left");
        eqi(out[1], 1000, "mono->stereo fills right with the SAME sample");
        eqi(out[2], -2000, "mono->stereo frame 2 left");
        eqi(out[3], -2000, "mono->stereo frame 2 right");
    }

    /* More than two source channels: the front pair is TAKEN, the rest are
     * dropped. This path had no test at all, which is how a stale "3 channels
     * are rejected" assertion survived next to code that accepts up to 8.
     *
     * Channel order in WAV/FLAC is FL, FR, FC, LFE, BL, BR -- so taking [0] and
     * [1] is a real stereo signal. The bug this guards against is reading the
     * frame with the WRONG STRIDE: at src_ch=6 the second frame starts at index
     * 6, and a decoder that used dst_ch or a hardcoded 2 would return FC and
     * LFE as if they were the next frame's L/R. That is inaudible on a sine and
     * unmistakable on real content, so it is asserted on frame 2, not frame 1. */
    {
        int16_t in[12] = { 100, 200, 300, 400, 500, 600,       /* frame 0 */
                           111, 222, 333, 444, 555, 666 };     /* frame 1 */
        memset(out, 0x7F, sizeof out);
        pcm_to_s16(out, 2, in, 6, SND_FMT_S16, 2);
        eqi(out[0], 100, "5.1 frame 0 -> front left");
        eqi(out[1], 200, "5.1 frame 0 -> front right");
        eqi(out[2], 111, "5.1 frame 1 -> front left (stride is src_ch, not 2)");
        eqi(out[3], 222, "5.1 frame 1 -> front right");
    }

    /* And the same source folded to a single output channel must average the
     * front pair -- not average all six, and not take FL alone. */
    {
        int16_t in[6] = { 1000, 2000, -30000, -30000, -30000, -30000 };
        pcm_to_s16(out, 1, in, 6, SND_FMT_S16, 1);
        eqi(out[0], 1500, "5.1 -> mono averages the FRONT PAIR only");
    }

    /* A 3-channel source with dst_ch=2: the third channel is dropped, and the
     * output is not left holding whatever was in the buffer before. */
    {
        int16_t in[3] = { -500, 500, 32767 };
        memset(out, 0x7F, sizeof out);
        pcm_to_s16(out, 2, in, 3, SND_FMT_S16, 1);
        eqi(out[0], -500, "3ch -> left");
        eqi(out[1], 500,  "3ch -> right, centre channel dropped");
    }

    /* Stereo folded to mono averages rather than dropping a channel. */
    {
        int16_t in[2] = { 1000, 2000 };
        pcm_to_s16(out, 1, in, 2, SND_FMT_S16, 1);
        eqi(out[0], 1500, "stereo->mono averages");
    }
}

/* ---------------------------------------------------------- resampling --- */

/* The identity path must be exact: a 48 kHz decoder into a 48 kHz card should
 * get its own bytes back, not a linear interpolation of them. */
static void t_resample_identity(void)
{
    int16_t in[64], out[64], hist[2] = { 0, 0 };
    uint32_t ph = 0;
    unsigned used = 0, n;

    for (int i = 0; i < 64; i++) in[i] = (int16_t)(i * 511 - 16000);
    n = pcm_resample(out, 32, in, 32, 2, 48000, 48000, &ph, hist, &used);
    eqi(n, 32, "identity resample returns every frame");
    eqi(used, 32, "identity resample consumes every frame");
    ok(memcmp(in, out, 32 * 2 * sizeof(int16_t)) == 0,
       "identity resample is bit-identical, not interpolated");
}

static void t_resample_rates(void)
{
    static int16_t in[4096], out[8192];
    int16_t hist[2] = { 0, 0 };
    uint32_t ph = 0;
    unsigned used = 0, n;

    for (int i = 0; i < 4096; i++) in[i] = 1000;    /* DC */

    /* Upsampling 24k -> 48k: twice as many frames out as in, and DC in must be
     * DC out. An interpolator that gets its endpoints wrong shows here as a
     * sawtooth riding on the DC. */
    n = pcm_resample(out, 4096, in, 2048, 1, 24000, 48000, &ph, hist, &used);
    ok(n > 4000 && n <= 4096, "24k->48k roughly doubles the frame count");
    {
        int bad = 0;
        /* Skip the ramp-in. `hist` starts at silence, so the output climbs from
         * 0 to the DC level over exactly the frames whose source index is still
         * 0 -- dst_rate/src_rate of them when upsampling, 2 here. That is a
         * stream START, not an error: a stream that jumped straight to full
         * amplitude on its first sample would click. Everything after must be
         * the input's DC exactly. */
        for (unsigned i = 48000 / 24000; i < n; i++) if (out[i] != 1000) bad++;
        eqi(bad, 0, "DC survives upsampling unchanged after the ramp-in");
        eqi(out[0], 0, "an upsampled stream ramps in from silence rather than clicking");
    }

    /* Downsampling 48k -> 24k: half the frames, and it must consume its input
     * rather than looping on it. */
    ph = 0; hist[0] = hist[1] = 0;
    n = pcm_resample(out, 4096, in, 2048, 1, 48000, 24000, &ph, hist, &used);
    ok(n >= 1023 && n <= 1025, "48k->24k roughly halves the frame count");
    ok(used > 2040, "downsampling consumes nearly all of its input");

    /* The rate pair this actually ships against: a 44.1 kHz file into a 48 kHz
     * card. Assert the ratio to within a frame, because a wrong step here is a
     * few percent of pitch error -- perfectly listenable and completely wrong. */
    ph = 0; hist[0] = hist[1] = 0;
    n = pcm_resample(out, 8192, in, 4096, 1, 44100, 48000, &ph, hist, &used);
    {
        unsigned want = (unsigned)((uint64_t)4096 * 48000 / 44100);
        ok(n + 2 >= want && n <= want + 2, "44.1k->48k ratio is right to +/-2 frames");
    }
}

/* THE seam test. A resampler that resets its phase and history each call sounds
 * perfect on one buffer and clicks at every period boundary in a real stream --
 * 47 times a second at our period size. Splitting the input and requiring a
 * byte-identical result is the only way a unit test sees it. */
static void t_resample_seam(void)
{
    static int16_t in[2048], one[4096], split[4096];
    int16_t hist[2];
    uint32_t ph;
    unsigned used = 0, n1, na, nb;

    for (int i = 0; i < 2048; i++) in[i] = (int16_t)(3000.0 * sin(i * 0.05));

    ph = 0; hist[0] = hist[1] = 0;
    n1 = pcm_resample(one, 4096, in, 2048, 1, 44100, 48000, &ph, hist, &used);

    /* Same input, same carried state, but handed over in two pieces -- exactly
     * what happens across two DMA periods. */
    ph = 0; hist[0] = hist[1] = 0;
    na = pcm_resample(split, 4096, in, 1000, 1, 44100, 48000, &ph, hist, &used);
    nb = pcm_resample(split + na, 4096 - na, in + used, 2048 - used, 1,
                      44100, 48000, &ph, hist, &used);

    eqi((long)(na + nb), (long)n1, "split resample yields the same frame count");
    ok(memcmp(one, split, n1 * sizeof(int16_t)) == 0,
       "split resample is BYTE-IDENTICAL to one-shot (no per-period click)");
}

/* --------------------------------------------------------------- mixing --- */
static void t_mix(void)
{
    int16_t a[4], b[4];

    a[0] = 100;   b[0] = 250;
    a[1] = -100;  b[1] = -250;
    a[2] = 30000; b[2] = 30000;     /* would wrap to negative in int16 */
    a[3] = -30000; b[3] = -30000;
    pcm_mix_add(a, b, 4);

    eqi(a[0], 350, "mix sums");
    eqi(a[1], -350, "mix sums negatives");
    eqi(a[2], 32767, "mix CLAMPS at the positive rail, does not wrap");
    eqi(a[3], -32768, "mix clamps at the negative rail");

    /* Adding silence must be exactly transparent -- a stream that has stopped
     * writing must not attenuate the ones still playing. */
    {
        int16_t x[3] = { 1234, -4321, 0 }, z[3] = { 0, 0, 0 };
        pcm_mix_add(x, z, 3);
        ok(x[0] == 1234 && x[1] == -4321 && x[2] == 0, "mixing silence changes nothing");
    }
}

static void t_silence(void)
{
    uint8_t u[4]; int16_t s[4];
    pcm_silence(u, SND_FMT_U8, 4);
    ok(u[0] == 0x80 && u[3] == 0x80, "u8 silence is 0x80, NOT zero");
    pcm_silence(s, SND_FMT_S16, 4);
    ok(s[0] == 0 && s[3] == 0, "s16 silence is zero");
}

/* ----------------------------------------------------------------- ring --- */
static void t_ring(void)
{
    uint8_t mem[16], tmp[32];
    struct pcm_ring r;

    pcm_ring_init(&r, mem, 16);
    eqi(pcm_ring_used(&r), 0, "fresh ring is empty");
    eqi(pcm_ring_free(&r), 16, "fresh ring is all free");

    eqi(pcm_ring_write(&r, "0123456789", 10), 10, "write fits");
    eqi(pcm_ring_used(&r), 10, "used tracks the write");
    eqi(pcm_ring_free(&r), 6, "free tracks the write");

    /* Short write when it does not fit -- the property SYS_SND_WRITE's whole
     * contract rests on. Silently dropping the overflow is the alternative and
     * it loses audio without telling anyone. */
    eqi(pcm_ring_write(&r, "ABCDEFGH", 8), 6, "write is SHORT when the ring fills");
    eqi(pcm_ring_free(&r), 0, "ring is now full");
    eqi(pcm_ring_write(&r, "X", 1), 0, "write to a full ring takes nothing");

    eqi(pcm_ring_read(&r, tmp, 4), 4, "read returns what was asked");
    ok(memcmp(tmp, "0123", 4) == 0, "read is FIFO");

    /* Wrap: 4 bytes of room at the physical end, then the write must continue
     * at offset 0. Getting this wrong reverses a fragment of every buffer. */
    eqi(pcm_ring_write(&r, "wxyz", 4), 4, "write wraps into the freed space");
    eqi(pcm_ring_read(&r, tmp, 16), 16, "drain the whole ring");
    ok(memcmp(tmp, "456789ABCDEFwxyz", 16) == 0, "wrapped data comes back in order");

    eqi(pcm_ring_used(&r), 0, "ring empty after the drain");
    eqi(pcm_ring_read(&r, tmp, 4), 0, "read from an empty ring returns nothing");

    /* A long randomized round trip, so the wrap is exercised at every offset
     * rather than the one this test happened to choose. */
    {
        static uint8_t src[4096], got[4096];
        unsigned wi = 0, ri = 0, i;
        pcm_ring_init(&r, mem, 16);
        for (i = 0; i < 4096; i++) src[i] = (uint8_t)(i * 31 + 7);
        srand(1234);
        while (ri < 4096) {
            unsigned n = (unsigned)(rand() % 9);
            if (wi < 4096) wi += pcm_ring_write(&r, src + wi, (n < 4096 - wi) ? n : 4096 - wi);
            n = (unsigned)(rand() % 9);
            ri += pcm_ring_read(&r, got + ri, n);
            if (wi == 4096 && pcm_ring_used(&r) == 0) break;
        }
        eqi((long)ri, 4096, "randomized round trip moved every byte");
        ok(memcmp(src, got, 4096) == 0, "randomized round trip preserved byte order");
    }
}

int main(void)
{
    t_formats();
    t_convert();
    t_resample_identity();
    t_resample_rates();
    t_resample_seam();
    t_mix();
    t_silence();
    t_ring();

    printf("%s: audio pcm: %d checks, %d failures\n",
           fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
