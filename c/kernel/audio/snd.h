#ifndef LOGIT_SND_H
#define LOGIT_SND_H
/* ===========================================================================
 * LogitOS audio -- the PCM/mixing layer, and the contract a sound card driver
 * implements.
 *
 * WHY THE MIXER IS IN THE KERNEL
 *   This project's direction is to push policy to ring 3: M17 moved the entire
 *   HTML/CSS/layout/paint pipeline into browser.aex, and the H.264 decoder is
 *   deliberately ring-3 too. So the default answer to "where does this go" is
 *   userspace, and the burden of proof is on keeping anything in ring 0. Here
 *   is the proof, and its limits.
 *
 *   The thing in this directory is not policy. A sound card has ONE DMA ring
 *   running at a fixed rate, and something must put the next period of samples
 *   into it before the hardware plays that memory -- a hard deadline of a few
 *   milliseconds, driven by an interrupt. That is the same class of object as
 *   the block layer, not the same class as CSS. The parts that ARE policy --
 *   what a file's bytes mean, how to turn them into PCM -- stay in ring 3
 *   exactly like the video decoder, and this layer never sees a codec.
 *
 *   A userspace sound server was the alternative, and it does not fit this
 *   kernel yet, for three concrete reasons rather than a preference:
 *     - No shared memory. Processes have private PML4s and there is no mmap or
 *       shm, so every period would be copied kernel->pipe->server->kernel. At
 *       48 kHz stereo s16 that is 192 KB/s of pure copying to accomplish a
 *       saturating add.
 *     - No priority. The scheduler is round-robin. A server that must run
 *       within one 21 ms period would be behind whatever else is runnable, and
 *       an underrun is audible in a way a late frame is not.
 *     - The interrupt cannot reach it. Waking a ring-3 process from the audio
 *       ISR needs the very wakeup path this layer would be built on anyway.
 *   Change any of those three -- shm, priorities -- and moving the mixer out is
 *   a contained change: the syscalls in snd.c would become the server's IPC and
 *   everything below `struct snd_device` stays put. It is not written to be
 *   permanent, it is written to be replaceable.
 *
 * THE LAYERS
 *   c/drivers/audio/hda.c    the card: DMA ring, interrupts, codec setup.
 *                            Knows nothing about streams, formats or mixing.
 *     -> snd_register_device()  hands the mixer a ring and a way to start it
 *     <- snd_period_elapsed()   called FROM THE ISR when a period completed
 *
 *   c/kernel/audio/mixer.c   the kaudio thread: sleeps on a semaphore the ISR
 *                            posts, converts+resamples+sums every open stream
 *                            into the period the card is about to play.
 *
 *   c/kernel/audio/pcm.c     pure functions: format conversion, resampling,
 *                            mixing, the ring. No kernel dependencies at all,
 *                            so tests/unit/audio_pcm_test.c drives the REAL
 *                            code on the host rather than a copy of it.
 *
 *   c/kernel/audio/snd.c     the syscalls, the handle table, the boot report.
 * =========================================================================== */

#include <stdint.h>
#include <stddef.h>
#include "logit_abi.h"

/* --------------------------------------------------------------- formats -- */
/* Bytes per sample for a SND_FMT_*; 0 for an unknown format. */
int snd_fmt_bytes(int fmt);
/* Is this a format/rate/channel triple we can serve? 1 = yes. */
int snd_fmt_ok(unsigned rate, unsigned channels, int fmt);

/* ------------------------------------------------------- the card driver -- */
/* What a sound card driver registers. The mixer owns `ring`: it writes whole
 * periods into it and never reads it back. The driver owns everything else.
 *
 * The ring is `periods * period_bytes` of DMA-able memory the driver allocated
 * (physically contiguous, identity-mapped -- see hda.c), laid out as the
 * hardware plays it: period 0 first. The mixer fills period (n+1) while the
 * card plays period n, which is the entire reason there is more than one. */
struct snd_device {
    const char *name;                 /* "hda", "ac97" -- appears in the boot line */
    char        codec[32];            /* what the card said about itself */

    unsigned    rate;                 /* Hz the DMA engine actually runs at */
    unsigned short channels;          /* interleaved */
    unsigned short format;            /* SND_FMT_* the hardware consumes */

    unsigned    period_bytes;         /* one refill */
    unsigned    periods;              /* periods in `ring` */
    uint8_t    *ring;                 /* periods * period_bytes, DMA-able */

    /* Start/stop the DMA engine. start() returns 0 on success. The mixer calls
     * start() only after it has filled the whole ring with silence, so a card
     * that begins playing immediately plays silence, not uninitialised RAM. */
    int  (*start)(struct snd_device *d);
    void (*stop)(struct snd_device *d);
    /* Frames the engine has consumed since start(), monotonically. Used only
     * for reporting and for the drain deadline -- never for pacing, because a
     * position register that lies is a nastier failure than a late refill. */
    uint64_t (*position)(struct snd_device *d);

    unsigned irq_mode;                /* DEV_IRQ_* as wired, 0 = polled */
    void    *priv;
};

/* Called by a card driver from its probe(). The first device registered wins;
 * a second one is logged and ignored (we have no notion of a default sink to
 * choose between them yet, and silently picking one would be worse). */
int  snd_register_device(struct snd_device *d);

/* Called by the card driver FROM ITS INTERRUPT HANDLER when the engine has
 * finished playing a period. Interrupt-safe and non-blocking by construction:
 * it does a counter bump and a sem_post, nothing else. */
void snd_period_elapsed(struct snd_device *d);

/* Is there a card? Everything else degrades to SND_E_NODEV when this is 0. */
int  snd_present(void);

/* ------------------------------------------------------------- the mixer -- */
/* Start the kaudio thread. Called once from kmain after dev_probe_all(); a
 * no-op when no card registered, so a machine with no sound hardware spends
 * nothing on it. */
void snd_init(void);

/* Start the DMA engine and the kaudio thread if they are not already running.
 * Returns 1 if sound can now come out, 0 if there is no card or the scheduler
 * is not up yet.
 *
 * This is separate from snd_init() because snd_init() runs from a driver's
 * probe(), and dev_probe_all() happens BEFORE sched_init() -- so thread_create()
 * is not available there, and calling it anyway killed the boot on every
 * machine that had a sound card. Idempotent; called from the first stream open.
 * See the long comment on snd_engine_start() in mixer.c. */
int  snd_engine_ensure(void);

/* One line at boot naming the driver, the codec, the rate/format, and the
 * buffering -- the "130 roots, 0 skipped" of audio. On unfamiliar hardware
 * this line is the whole diagnosis, so it prints on the no-card path too. */
void snd_report(void);
/* snd_report(), but at most once per boot. Called from snd_init() on the
 * card-present path and from the first audio syscall otherwise. */
void snd_report_once(void);

/* The SYS_SND_* entry point. c/kernel/exec/syscall.c forwards to this rather
 * than carrying six cases of audio-specific pointer validation. */
long snd_syscall(long num, long a, long b, long c);

/* ------------------------------------------------------- the capture card -- */
/* What a card driver registers for the INPUT direction. Mirrors struct
 * snd_device but is deliberately a separate type rather than a `dir` flag on
 * the same one: the two differ in who owns `ring` (the mixer WRITES a
 * playback ring and NEVER reads it; the kernel READS a capture ring and NEVER
 * writes it), and folding both into one struct would need every field's
 * comment to say "for playback" or "for capture" instead of the type doing
 * it. See c/kernel/audio/capture.c for the layer this feeds -- the read-side
 * twin of mixer.c, single-consumer instead of N-stream-summed because there
 * is exactly one microphone, not an app-defined number of players. */
struct snd_capdevice {
    const char *name;                 /* "hda-in" -- appears in the boot line */
    char        codec[48];            /* what the codec/path was, adc=/pin= */

    unsigned    rate;
    unsigned short channels;
    unsigned short format;            /* SND_FMT_* the hardware DELIVERS */

    unsigned    period_bytes;         /* one DMA period, i.e. one IOC interrupt */
    unsigned    periods;              /* periods in `ring` */
    uint8_t    *ring;                 /* periods * period_bytes, DMA-able, the
                                        * DEVICE writes here -- the kernel only
                                        * ever reads it, the mirror image of
                                        * struct snd_device's `ring`. */

    int  (*start)(struct snd_capdevice *d);   /* 0 on success */
    void (*stop)(struct snd_capdevice *d);

    unsigned irq_mode;
    void    *priv;
};

/* Called by a card driver from its probe(). Same "first one wins" policy as
 * snd_register_device(), same reason: no notion yet of which of two capture
 * devices should be the default. */
int  snd_register_capture_device(struct snd_capdevice *d);

/* Called by the card driver FROM ITS INTERRUPT HANDLER when the engine has
 * finished WRITING a period -- the read-side twin of snd_period_elapsed().
 * Same interrupt-safety contract: a counter and a sem_post, nothing else. */
void snd_capture_period_elapsed(struct snd_capdevice *d);

int  snd_capture_present(void);

/* Idempotent; safe to call whether or not a capture device is registered.
 * Called once from snd_init() (mixer.c) so a machine with no capture path
 * pays nothing extra beyond the check. Allocates the single stream's waitq;
 * does NOT start the DMA engine or spawn kcapture -- see snd_cap_engine_ensure,
 * same boot-order landmine snd_engine_ensure documents for playback
 * (thread_create() before sched_init() faults). */
void snd_cap_init(void);

/* One line, printed from snd_report() (mixer.c) right after the playback
 * line -- "no line" and "the line says none" need to stay distinguishable on
 * the input side exactly as much as they do on the output side (see the
 * comment on snd_report() itself), so this prints on the no-capture-device
 * path too. */
void snd_cap_report(void);

/* --------------------------------------------------------- stream handles -- */
/* These are what the SYS_SND_* syscalls call. They are the same functions the
 * on-device self-test uses, so the test drives production paths.
 * `owner` is an opaque process token (struct proc *); handles are per-owner so
 * one process cannot write to another's stream. NULL owner = kernel caller. */
int  snd_stream_open(void *owner, const struct logit_sndfmt *f);
int  snd_stream_write(void *owner, int h, const void *buf, int bytes);
int  snd_stream_avail(void *owner, int h);
int  snd_stream_close(void *owner, int h, int drain);
int  snd_stream_state(void *owner, int h, struct logit_sndstate *st);
void snd_info_fill(struct logit_sndinfo *si);
/* Close every stream belonging to `owner`. Called when a process exits, so a
 * player that faults mid-tone stops making noise. */
void snd_owner_release(void *owner);

/* The capture-side twins, in c/kernel/audio/capture.c. Exactly one stream can
 * be open at a time (see the ABI note in logit_abi.h); a second SYS_SND_CAP_OPEN
 * gets SND_E_NOMEM, same code the playback side returns for the same reason
 * (no free slot) rather than a new error the caller has to special-case.
 *
 * `f` is filled in place with the format actually delivered -- see the ABI
 * note on rate==0. */
int  snd_cap_open(void *owner, struct logit_sndfmt *f);
int  snd_cap_read(void *owner, int h, void *buf, int bytes);
int  snd_cap_avail(void *owner, int h);
int  snd_cap_close(void *owner, int h);
int  snd_cap_state(void *owner, int h, struct logit_sndstate *st);
/* Close the capture stream if `owner` holds it. Same shape as
 * snd_owner_release() and the same caveat applies: see the report on whether
 * anything actually calls either of them from proc_exit(). */
void snd_cap_owner_release(void *owner);

/* ============================ pcm.c: pure functions ======================= */
/* Everything below is free of kernel dependencies on purpose -- it is compiled
 * unchanged into tests/unit/audio_pcm_test.c. */

/* Convert `frames` of `src_ch`-channel `src_fmt` into interleaved s16 with
 * `dst_ch` channels. Mono -> N fans the sample to every channel; N -> mono
 * averages; otherwise channels are copied and any surplus is zeroed. Returns
 * frames written (== frames). */
unsigned pcm_to_s16(int16_t *dst, unsigned dst_ch,
                    const void *src, unsigned src_ch, int src_fmt,
                    unsigned frames);

/* Linear-interpolating resampler over a Q16.16 phase accumulator.
 *
 * Integer-only, though the kernel has SSE since M15: a resampler is exactly the
 * place where a rounding difference between host tests and target is invisible
 * until it is a click, and integers make the unit test bit-exact on both.
 *
 * `*phase` and `*hist` carry the fractional position and the last input frame
 * ACROSS CALLS -- that is what makes period boundaries seamless. Zero them once
 * at stream open and never again. Returns frames written to `dst` (at most
 * dst_max). Consumes from `src` and reports how many input frames were used in
 * *used, so the caller can advance its ring. */
unsigned pcm_resample(int16_t *dst, unsigned dst_max,
                      const int16_t *src, unsigned src_frames,
                      unsigned ch, unsigned src_rate, unsigned dst_rate,
                      uint32_t *phase, int16_t *hist, unsigned *used);

/* dst += src, saturating at the s16 limits. This is the whole of mixing; it is
 * separate so the clamp has a test of its own -- two loud streams that wrap
 * instead of clamping is the single most audible bug in this file. */
void pcm_mix_add(int16_t *dst, const int16_t *src, unsigned samples);

/* Exact silence for a format. Not always zero (SND_FMT_U8's silence is 0x80),
 * which is exactly the kind of thing that is silent-until-it-is-a-buzz. */
void pcm_silence(void *dst, int fmt, unsigned samples);

/* --- the byte ring a stream's writes land in ------------------------------ */
/* Single producer (the writing thread), single consumer (kaudio). The counters
 * are free-running so `head - tail` is the fill level with no ambiguity at
 * full-vs-empty, and wraparound of the 64-bit counters is not a thing that can
 * happen in any uptime this machine has. */
struct pcm_ring {
    uint8_t  *buf;
    unsigned  size;        /* bytes; not required to be a power of two */
    uint64_t  head;        /* bytes ever written */
    uint64_t  tail;        /* bytes ever read */
};
void     pcm_ring_init(struct pcm_ring *r, uint8_t *buf, unsigned size);
unsigned pcm_ring_used(const struct pcm_ring *r);
unsigned pcm_ring_free(const struct pcm_ring *r);
/* Returns bytes actually taken (short when the ring fills). */
unsigned pcm_ring_write(struct pcm_ring *r, const void *src, unsigned bytes);
/* Returns bytes actually read (short when the ring runs dry). */
unsigned pcm_ring_read(struct pcm_ring *r, void *dst, unsigned bytes);

/* Read WITHOUT consuming, and consume separately.
 *
 * The mixer needs this pair because the resampler decides how much input it
 * actually used only after it has run: to fill one period it must be handed a
 * frame or two more than it will consume, and a destructive read would throw
 * those away. Dropping one input frame per period is a 0.1% pitch error and an
 * audible tick at the period rate -- the kind of bug that survives listening
 * tests. So: peek what the resampler might need, then advance by what it says
 * it took. */
unsigned pcm_ring_peek(const struct pcm_ring *r, void *dst, unsigned bytes);
void     pcm_ring_advance(struct pcm_ring *r, unsigned bytes);

#endif /* LOGIT_SND_H */
