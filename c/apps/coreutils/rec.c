/* rec -- capture audio from the machine's microphone/line-in to a PCM WAV
 * file, via SYS_SND_CAP_* (c/kernel/audio/capture.c, c/drivers/audio/hda.c).
 *
 * The cheapest real consumer of capture that exists: it exercises the whole
 * path end to end (codec widget-graph search -> input stream descriptor ->
 * DMA ring -> kcapture thread -> the byte ring a read() drains) and produces
 * an artifact -- a .wav file -- that is checkable the same way sndtest.c's
 * playback artifacts are checkable: byte for byte, not "did it not crash".
 * Preview (c/apps/gui/preview.c, via audio_sniff()+wav.c) already plays a
 * standard PCM WAV back, so this file's own output is its own round-trip
 * check with no new decoder needed on either end.
 *
 * usage: rec [seconds] [path]
 *   seconds  1..REC_SECONDS_MAX, default REC_SECONDS_DEFAULT
 *   path     default "/rec.wav"
 *
 * No malloc here -- coreutils programs use clib.h, not mini-libc's arena
 * (see clib.h's own header comment) -- so the capture buffer is a static
 * array, sized for the worst case (REC_SECONDS_MAX) and used partially for
 * anything shorter. At 48 kHz stereo s16 that is 192,000 B/s; five seconds
 * is under 1 MiB, which is unremarkable .bss next to what this tree's own
 * GUI apps already carry (see CLAUDE.md's note on the browser's own .bss).
 */
#include "clib.h"

#define RATE     48000
#define CHANNELS 2
#define BYTES_PER_FRAME (CHANNELS * 2)   /* s16 */

#define REC_SECONDS_MAX     5
#define REC_SECONDS_DEFAULT 3

static short g_buf[REC_SECONDS_MAX * RATE * CHANNELS];

/* Standard 44-byte canonical PCM WAV header (RIFF/WAVEfmt /data), s16,
 * `channels` channels at `rate` Hz. Written by hand rather than linking
 * c/lib/audio/wav.c's wav_header_s16(): that file is ring-3 but NOT wired
 * into CLIDIR's build (coreutils programs compile as exactly one .c file --
 * see CLI_RULE in the Makefile), and the format is fifteen lines of the
 * RIFF spec, not something worth a cross-directory link for. */
static int wav_header_s16(unsigned char out[44], int rate, int channels, long frames)
{
    long dbytes = frames * channels * 2;
    unsigned v;

    out[0] = 'R'; out[1] = 'I'; out[2] = 'F'; out[3] = 'F';
    v = (unsigned)(36 + dbytes);
    out[4] = (unsigned char)v; out[5] = (unsigned char)(v >> 8);
    out[6] = (unsigned char)(v >> 16); out[7] = (unsigned char)(v >> 24);
    out[8]='W'; out[9]='A'; out[10]='V'; out[11]='E';
    out[12]='f'; out[13]='m'; out[14]='t'; out[15]=' ';
    out[16] = 16; out[17] = out[18] = out[19] = 0;      /* fmt chunk size */
    out[20] = 1; out[21] = 0;                           /* WAVE_FORMAT_PCM */
    out[22] = (unsigned char)channels; out[23] = 0;
    v = (unsigned)rate;
    out[24] = (unsigned char)v; out[25] = (unsigned char)(v >> 8);
    out[26] = (unsigned char)(v >> 16); out[27] = (unsigned char)(v >> 24);
    v = (unsigned)(rate * channels * 2);                /* byte rate */
    out[28] = (unsigned char)v; out[29] = (unsigned char)(v >> 8);
    out[30] = (unsigned char)(v >> 16); out[31] = (unsigned char)(v >> 24);
    out[32] = (unsigned char)(channels * 2); out[33] = 0;   /* block align */
    out[34] = 16; out[35] = 0;                          /* bits per sample */
    out[36]='d'; out[37]='a'; out[38]='t'; out[39]='a';
    v = (unsigned)dbytes;
    out[40] = (unsigned char)v; out[41] = (unsigned char)(v >> 8);
    out[42] = (unsigned char)(v >> 16); out[43] = (unsigned char)(v >> 24);
    return 44;
}

/* i16 -> decimal string, for a crude peak-level readout -- the cheapest
 * thing that distinguishes "captured something" from "captured silence"
 * without needing a second program to inspect the file. */
static void out_peak(const short *buf, long frames)
{
    int peak = 0;
    for (long i = 0; i < frames * CHANNELS; i++) {
        int v = buf[i]; if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    outs("REC_PEAK "); outn(peak); outs(" / 32767\n");
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? c_atoi(argv[1]) : REC_SECONDS_DEFAULT;
    const char *path = argc > 2 ? argv[2] : "/rec.wav";
    struct logit_sndinfo si;
    struct logit_sndfmt f;
    int h, got;
    long want_frames, want_bytes;
    unsigned char hdr[44];

    if (secs < 1) secs = 1;
    if (secs > REC_SECONDS_MAX) secs = REC_SECONDS_MAX;

    /* snd_info() first, same discipline sndtest.c uses for playback: a
     * machine with no card (or, here, no capture path on the card it has)
     * degrades to a clear message and exit 1, not a hang inside cap_open. */
    snd_info(&si);
    outs("REC_INFO driver="); outs(si.driver[0] ? si.driver : "none");
    outs(" codec="); outs(si.codec[0] ? si.codec : "-");
    outs("\n");

    f.rate = 0; f.channels = 0; f.format = 0; f.buffer_ms = 0; f.flags = 0;
    h = snd_cap_open(&f);
    if (h < 0) {
        outs("REC_FAIL open="); outn(h); outs("\n");
        return 1;
    }
    outs("REC_OPEN rate="); outn((long)f.rate);
    outs(" ch="); outn((long)f.channels);
    outs(" fmt="); outn((long)f.format);
    outs("\n");

    if ((int)f.channels != CHANNELS || (int)f.format != SND_FMT_S16) {
        /* This driver only ever registers a 48 kHz/stereo/s16 capture
         * device (see codec_setup_capture in hda.c), so this branch is
         * unreachable today -- kept because a refusal that assumes its own
         * caller's shape is exactly the kind of silent breakage this tree
         * has been bitten by before when a fact that was true only "for
         * now" got baked into a fixed-size buffer. */
        outs("REC_FAIL unexpected format\n");
        snd_cap_close(h);
        return 1;
    }

    want_frames = (long)secs * f.rate;
    if (want_frames > REC_SECONDS_MAX * RATE) want_frames = REC_SECONDS_MAX * RATE;
    want_bytes = want_frames * BYTES_PER_FRAME;

    got = snd_cap_read_all(h, g_buf, (int)want_bytes);
    snd_cap_close(h);

    if (got < 0) {
        outs("REC_FAIL read="); outn(got); outs("\n");
        return 1;
    }
    if (got < want_bytes) {
        /* A short read here means the hardware period stopped arriving --
         * SYS_SND_CAP_READ's 500 ms park timed out (see capture.c) rather
         * than the machine actually delivering less than asked. Reported,
         * not hidden: the file this writes below will be exactly `got`
         * bytes of real audio, not `want_bytes` of partly-uninitialised
         * buffer padded with silence pretending to be a full recording. */
        outs("REC_SHORT got="); outn(got); outs(" want="); outn(want_bytes); outs("\n");
        want_bytes = got;
        want_frames = got / BYTES_PER_FRAME;
    }

    out_peak(g_buf, want_frames);

    wav_header_s16(hdr, (int)f.rate, (int)f.channels, want_frames);
    {
        /* write_file() takes one buffer, not a header then a body -- so the
         * header and the samples are assembled into g_buf's own byte range
         * via a small on-stack shuffle: write the header, then the PCM,
         * into a second static buffer sized for the header plus the worst
         * case capture. Simpler than a second syscall this ABI does not
         * have (no append-write here -- see write_file's one-shot contract
         * in logit.h). */
        static unsigned char out[44 + REC_SECONDS_MAX * RATE * CHANNELS * 2];
        long i;
        for (i = 0; i < 44; i++) out[i] = hdr[i];
        for (i = 0; i < want_bytes; i++) out[44 + i] = ((unsigned char *)g_buf)[i];
        if (write_file(path, out, (int)(44 + want_bytes)) < 0) {
            outs("REC_FAIL write_file\n");
            return 1;
        }
    }

    outs("REC_DONE frames="); outn(want_frames);
    outs(" bytes="); outn(want_bytes);
    outs(" path="); outs(path);
    outs("\n");
    return 0;
}
