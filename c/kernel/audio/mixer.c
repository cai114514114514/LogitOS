/* The mixer: streams in, one DMA period out, once per interrupt.
 *
 * The shape is the whole design, so it is worth stating before the code:
 *
 *   card ISR --> snd_period_elapsed() --> sem_post(&g_period)
 *                                              |
 *                                        kaudio thread wakes
 *                                              |
 *              for each open stream: peek its ring, convert to s16,
 *              resample to the card's rate, saturating-add into the period
 *                                              |
 *                                    wake any writer waiting for room
 *
 * The ISR does a counter bump and a post. It does not mix, it does not touch a
 * stream, it does not allocate and it does not use floating point -- driver.h
 * forbids all four, and an audio ISR is exactly where someone would try.
 *
 * WHY A THREAD AND NOT A POLL LOOP. Before M27 this kernel had no way to wait:
 * net_poll() was pumped from the window manager's main loop and device I/O ran
 * "interrupts on but non-preemptible". Audio is the canonical case against
 * that -- a refill is due every 21 ms forever, and a poll loop either burns a
 * core or misses the deadline. kaudio sleeps in sem_wait() and costs exactly
 * nothing between periods.
 *
 * THE ONE-PERIOD LEAD. The card plays period n while we fill period n+1. Fill
 * the period it is *playing* and you write behind the read pointer -- audible
 * as a tear at a random offset, and invisible in any test that checks only
 * that bytes were written. `g_fill` is that lead, and it is seeded in
 * snd_start_device() rather than inferred from a position register, because a
 * position register that lies is a nastier failure than a late refill.
 */
#include "snd.h"
#include "kheap.h"
#include "kprintf.h"
/* Path-qualified, and it has to be: mini-libc ships c/apps/libc/include/sys/
 * wait.h, that directory is in INCDIRS, and it sorts BEFORE c/kernel/core -- so
 * a bare #include "wait.h" from outside c/kernel/core silently picks up the
 * userland one and every waitq here becomes an incomplete type. c/kernel/core's
 * own files get away with it only because a quoted include checks the including
 * file's own directory first. */
#include "kernel/core/wait.h"
#include "sched.h"
#include "spinlock.h"

void *memset(void *, int, size_t);

#define SND_MAX_STREAMS 8

/* Enough input for one period even when resampling 4:1 down from 192 kHz. */
#define SND_MAX_RATIO   4

/* How many periods ahead of the playback pointer kaudio keeps filled.
 *
 * 1 is the minimum that is CORRECT (see kaudio_thread): it fills the period the
 * engine will play next. It is also the minimum that is correct only if kaudio
 * always runs within one period of its interrupt, which under QEMU TCG with
 * four cores and a 100 Hz tick it does not -- a 21 ms period against a 10 ms
 * tick leaves no margin at all. 3 gives 63 ms of slack out of a 170 ms buffer,
 * still leaving 4 periods of unplayed audio behind the write point, and costs
 * 63 ms of latency that nothing here is trying to be under.
 *
 * If this is ever raised to within one of `periods`, the cap in kaudio_thread
 * is what stops it wrapping onto unplayed audio -- not this constant. */
#define SND_FILL_LEAD   3

struct snd_stream {
    int              used;
    void            *owner;          /* struct proc *, or NULL for a kernel caller */
    int              handle;

    unsigned         rate;
    unsigned short   channels;
    unsigned short   format;
    unsigned         flags;

    struct pcm_ring  ring;           /* holds bytes in the APP's format */
    uint8_t         *ringmem;

    uint32_t         phase;          /* resampler state, carried across periods */
    int16_t          hist[2];

    uint64_t         frames_written;
    uint64_t         frames_consumed;
    unsigned         underruns;
    int              state;          /* SND_S_* */
    int              closing;        /* set by close(); kaudio reaps */

    struct waitq     wq;             /* writers wait here for room */
};

static struct snd_stream g_str[SND_MAX_STREAMS];
static spinlock_t        g_snd_lock = SPINLOCK_INIT;
static int               g_next_handle = 1;

static struct snd_device *g_dev;
static struct semaphore   g_period;
static unsigned           g_fill;        /* period index kaudio fills next */
static unsigned           g_dev_underruns;
static volatile uint64_t  g_periods_done;
static int                g_running;

/* Scratch, allocated once from the device's real geometry. Not on the stack:
 * the deep path here is 32 KiB at 192 kHz, and this kernel has already
 * overflowed a thread stack into its own page tables once (see the M11 note). */
static uint8_t  *g_in_raw;      /* app-format input */
static int16_t  *g_in_s16;      /* converted, device channel count */
static int16_t  *g_out;         /* one period, device channels, s16 */
static unsigned  g_period_frames;

/* ------------------------------------------------------------- registry -- */

int snd_register_device(struct snd_device *d)
{
    if (g_dev) {
        /* Two cards and no notion of a default sink to choose between them.
         * Saying so is better than silently picking one: on a machine with an
         * HDMI audio function as well as an analogue codec, "silently picked
         * one" is how sound comes out of a monitor nobody is looking at. */
        kprintf("[snd] %s ignored: %s is already the output device\n",
                d->name, g_dev->name);
        return -1;
    }
    /* The mixer builds periods in s16 because every card we drive consumes s16.
     * Rejecting anything else keeps the conversion in one place instead of
     * scattering a second format path through the mix loop for a case that has
     * no hardware behind it. */
    if (d->format != SND_FMT_S16) {
        kprintf("[snd] %s rejected: format %d, only s16 is wired\n", d->name, d->format);
        return -1;
    }
    if (!d->ring || !d->period_bytes || d->periods < 2) {
        kprintf("[snd] %s rejected: needs a ring of >= 2 periods\n", d->name);
        return -1;
    }
    g_dev = d;
    return 0;
}

int snd_present(void) { return g_dev != 0; }

/* Called FROM THE CARD'S INTERRUPT HANDLER. Everything it does must be legal
 * with the BKL held, in interrupt context, without blocking: a counter and a
 * post, both of which wait.h documents as interrupt-safe. */
void snd_period_elapsed(struct snd_device *d)
{
    (void)d;
    g_periods_done++;
    sem_post(&g_period);
}

/* --------------------------------------------------------------- mixing -- */

/* Mix one stream into `g_out` (already zeroed), producing exactly
 * g_period_frames device frames. Returns 1 if the stream ran dry. */
static int mix_one(struct snd_stream *s)
{
    unsigned dev_ch = g_dev->channels;
    unsigned app_bps = (unsigned)snd_fmt_bytes(s->format) * s->channels;
    unsigned want_in, have_bytes, have_frames, produced, used;

    /* Exactly how many input frames this period can need: the last output
     * frame reads source index ((phase + (N-1)*step) >> 16), so one more than
     * that is the requirement. Computed rather than guessed, then peeked (not
     * read) so the frames the resampler does NOT consume stay in the ring. */
    if (s->rate == g_dev->rate) {
        want_in = g_period_frames;
    } else {
        uint32_t step = (uint32_t)(((uint64_t)s->rate << 16) / g_dev->rate);
        want_in = ((s->phase + (g_period_frames - 1) * (uint64_t)step) >> 16) + 2;
        if (want_in > g_period_frames * SND_MAX_RATIO)
            want_in = g_period_frames * SND_MAX_RATIO;
    }

    have_bytes = pcm_ring_peek(&s->ring, g_in_raw, want_in * app_bps);
    have_frames = have_bytes / app_bps;

    if (have_frames) {
        pcm_to_s16(g_in_s16, dev_ch, g_in_raw, s->channels, s->format, have_frames);
        /* Note the resampler is handed the stream's carried phase and history
         * by pointer: that is what makes this period continuous with the last
         * one instead of restarting at a discontinuity every 21 ms. */
        produced = pcm_resample(g_out, g_period_frames, g_in_s16, have_frames,
                                dev_ch, s->rate, g_dev->rate, &s->phase, s->hist,
                                &used);
        pcm_ring_advance(&s->ring, used * app_bps);
        s->frames_consumed += used;
    } else {
        produced = 0;
    }

    /* Short of a full period means the stream ran dry. g_out was zeroed, so the
     * remainder is SILENCE -- not the previous period played again, which is
     * what a driver that simply leaves the buffer alone produces, and which
     * sounds like a stutter rather than a gap. */
    return produced < g_period_frames;
}

static void mix_period(int16_t *dst)
{
    unsigned dev_ch = g_dev->channels;
    unsigned samples = g_period_frames * dev_ch;
    int i, any = 0;

    memset(dst, 0, samples * sizeof(int16_t));

    for (i = 0; i < SND_MAX_STREAMS; i++) {
        struct snd_stream *s = &g_str[i];
        if (!s->used || s->state == SND_S_CLOSED) continue;

        memset(g_out, 0, samples * sizeof(int16_t));
        if (mix_one(s)) {
            /* A draining stream reaching the end is not an underrun; it is the
             * stream finishing. Counting it as one would make every clean
             * close look like a fault. */
            if (s->state == SND_S_DRAINING) {
                if (pcm_ring_used(&s->ring) == 0) s->closing = 1;
            } else {
                s->underruns++;
            }
        }
        pcm_mix_add(dst, g_out, samples);
        any = 1;
    }

    if (!any) g_dev_underruns++;   /* nothing playing: the period goes out silent */
}

/* Reap streams that close() marked. Done HERE, in the only thread that reads a
 * stream, so a ring can never be freed while the mixer is walking it. */
static void reap_closed(void)
{
    int i;
    for (i = 0; i < SND_MAX_STREAMS; i++) {
        struct snd_stream *s = &g_str[i];
        if (!s->used || !s->closing) continue;
        s->state = SND_S_CLOSED;
        s->used = 0;
        s->closing = 0;
        waitq_wake_all(&s->wq);      /* release anyone still parked on it */
        if (s->ringmem) { kfree(s->ringmem); s->ringmem = 0; }
    }
}

static void kaudio_thread(void)
{
    for (;;) {
        /* The only wait in the audio path. Not a poll, not a spin, not a
         * "non-preemptible with interrupts on" -- the core is genuinely idle
         * between periods and every other thread runs. */
        sem_wait(&g_period);

        uint64_t fl = spin_lock_irqsave(&g_snd_lock);
        reap_closed();
        if (g_dev && g_running) {
            /* WHERE THE ENGINE IS, derived from the interrupt count rather
             * than from a position register. The IOC for period p fires when
             * the engine has FINISHED p, at which instant g_periods_done
             * becomes p+1 and the engine is already playing period p+1. So the
             * earliest index it is safe to write is p+2 == g_periods_done + 1.
             *
             * The original code seeded g_fill = 1 and filled exactly one period
             * per interrupt, which made the fill index equal g_periods_done --
             * THE PERIOD CURRENTLY UNDER THE PLAYBACK POINTER. Every refill
             * raced the engine, and whatever part of the period the engine had
             * already passed went out as the silence that was there before.
             * That is the 2631-frames-in-47637 of dropped ramp indices in the
             * captured WAV: not a decoder fault, not a lost interrupt, an
             * off-by-one in the lead. */
            uint64_t done = g_periods_done;
            unsigned earliest = (unsigned)done + 1;

            /* Fell behind (a late wake under TCG, or several IOCs coalesced).
             * The periods between g_fill and `earliest` are already history --
             * writing them now would put samples into memory the engine has
             * passed, so they are SKIPPED and counted, and the stream carries
             * on from the present. Recovering rather than trying to catch up
             * from behind is what stops a late refill becoming a permanent lag.
             */
            if (g_fill < earliest) {
                g_dev_underruns += earliest - g_fill;
                g_fill = earliest;
            }

            /* Fill FORWARD to the lead, which may be several periods on the
             * first interrupt or after a stall. Bounded by the ring: never
             * write past g_periods_done + periods, which would clobber a
             * period the engine has not played yet. */
            unsigned limit = (unsigned)done + SND_FILL_LEAD;
            unsigned cap   = (unsigned)done + g_dev->periods - 1;
            if (limit > cap) limit = cap;

            while (g_fill <= limit) {
                int16_t *period = (int16_t *)(g_dev->ring +
                                  (g_fill % g_dev->periods) * g_dev->period_bytes);
                mix_period(period);
                g_fill++;
            }

            /* THE SILENCE GUARANTEE. One period past the lead is zeroed, so a
             * kaudio that never gets to run again plays silence rather than
             * whatever was written into that slot `periods` ago. Without this,
             * a stall produces the last few periods on repeat -- which sounds
             * like a stutter, is not silence, and passes any "is it still
             * playing" check. The engine is g_fill - done >= SND_FILL_LEAD
             * periods away from this slot, so zeroing it races nothing. */
            memset(g_dev->ring + (g_fill % g_dev->periods) * g_dev->period_bytes,
                   0, g_dev->period_bytes);
        }
        spin_unlock_irqrestore(&g_snd_lock, fl);

        /* Woken outside the lock: a writer that wakes takes the lock itself,
         * and waking inside would make it spin on the lock we still hold. */
        for (int i = 0; i < SND_MAX_STREAMS; i++)
            if (g_str[i].used) waitq_wake_all(&g_str[i].wq);
    }
}

/* ----------------------------------------------------------------- init -- */

/* Start the DMA engine and the kaudio thread. SEPARATE FROM snd_init(), and
 * that separation is the whole fix for a boot that did not complete.
 *
 * snd_init() is called from a driver's probe(), and hda.c used to carry a
 * comment claiming "dev_probe_all() runs late enough in boot that
 * thread_create() is available". It is not. kmain.c calls dev_probe_all() and
 * only afterwards wm_run(), and it is wm_run() that calls sched_init() -- so at
 * probe time the run ring (g_ring in sched.c) is still NULL. thread_create()
 * then read g_ring->next through a NULL g_ring, which does NOT fault, because
 * the low 1 GiB is identity-mapped with huge pages: it quietly returned the
 * garbage sitting at physical address 8 (the real-mode interrupt vector table)
 * and the next store went to a non-canonical address. That is the
 * "#GP vector 13, error=0, cr2=0, cur=0x0" immediately after a perfectly
 * correct [snd] boot line, and it killed the boot every time a card was
 * present -- which is why the card enumerated, the report line was exactly
 * right, and the capture was silence.
 *
 * Nothing here needs to happen at probe time. Deferring costs nothing and buys
 * the property that a machine nobody plays audio on never creates the thread
 * or runs the DMA engine at all.
 *
 * Idempotent, and called with g_snd_lock NOT held (thread_create takes
 * g_sched_lock, and the ordering the rest of the kernel uses is
 * g_sched_lock inside nothing). */
static int g_engine_up;

static int snd_engine_start(int quiet)
{
    if (g_engine_up) return 1;
    if (!g_dev) return 0;

    /* The guard that makes this safe wherever it is called from. Before
     * sched_init() there is no current thread on any CPU, and creating one is
     * the thing that faults. At probe time this is the expected path and says
     * so; anywhere else it is a diagnosis rather than a silent no-sound. */
    if (!sched_current_thread()) {
        if (!quiet)
            kprintf("[snd] engine start refused: the scheduler is not up yet\n");
        return 0;
    }

    /* Fill the WHOLE ring with silence before the engine is allowed to run, so
     * a card that starts playing the instant it is told to plays silence rather
     * than whatever was in that RAM. */
    memset(g_dev->ring, 0, (size_t)g_dev->periods * g_dev->period_bytes);

    if (g_dev->start && g_dev->start(g_dev) != 0) {
        kprintf("[snd] %s: DMA engine would not start -- audio disabled\n", g_dev->name);
        g_dev = 0;
        return 0;
    }
    /* The engine is about to start at period 0 and the whole ring is silent.
     * kaudio's first wake resyncs this to the real playback position anyway
     * (g_periods_done + 1), so the only thing that matters here is that it is
     * not AHEAD of the engine -- 1 is the earliest safe value at start. */
    g_fill = 1;
    g_running = 1;
    g_engine_up = 1;

    thread_create(kaudio_thread, "kaudio");
    kprintf("[snd] engine running: kaudio started, DMA at %u Hz\n", g_dev->rate);
    return 1;
}

/* Called from the first operation that actually needs sound to be coming out.
 * Declared in snd.h so snd.c's syscall entry can use it too. */
int snd_engine_ensure(void)
{
    return snd_engine_start(0);
}

void snd_init(void)
{
    unsigned dev_ch, in_frames;

    semaphore_init(&g_period, 0);
    for (int i = 0; i < SND_MAX_STREAMS; i++) waitq_init(&g_str[i].wq);

    /* _once, not snd_report(): this call is what marks the report as done, so
     * the first audio syscall's snd_report_once() stays quiet. Calling the bare
     * form here printed the whole boot line twice on every machine that had a
     * card -- once at probe, once at the first SYS_SND_*. */
    snd_report_once();
    if (!g_dev) return;      /* no card: nothing allocated, no thread, no cost */

    dev_ch = g_dev->channels;
    g_period_frames = g_dev->period_bytes / (dev_ch * (unsigned)snd_fmt_bytes(g_dev->format));
    in_frames = g_period_frames * SND_MAX_RATIO + 4;

    g_in_raw = (uint8_t *)kmalloc(in_frames * 2 * 4);          /* worst case: 2ch f32 */
    g_in_s16 = (int16_t *)kmalloc(in_frames * dev_ch * 2);
    g_out    = (int16_t *)kmalloc(g_period_frames * dev_ch * 2);
    if (!g_in_raw || !g_in_s16 || !g_out) {
        kprintf("[snd] no memory for mix buffers -- audio disabled\n");
        g_dev = 0;
        return;
    }

    /* Everything above is allocation and is safe at probe time. The engine and
     * its thread are not; they start on first use. If some future boot order
     * probes after sched_init(), this call succeeds here and the deferral never
     * happens -- so the behaviour is "as early as is safe", not "always late". */
    snd_engine_start(1);   /* quiet: at probe time deferral is the design */

    /* Capture's own init (capture.c), called from here rather than from
     * hda.c a second time: "adding a driver requires editing no other file"
     * (see the comment at hda_probe's snd_init() call) applies just as much
     * to capture as to playback, and snd_init() is already the ONE place
     * every card driver calls unconditionally. Safe when no capture device
     * registered -- snd_cap_init() only sets up a waitq and a semaphore, no
     * g_capdev check needed because it does not touch g_capdev at all. */
    snd_cap_init();
}

void snd_report(void)
{
    if (!g_dev) {
        /* Printed on the no-card path too, and deliberately: on unfamiliar
         * hardware "there is no line" and "the line says none" are completely
         * different diagnoses, and only the second one is evidence. */
        kprintf("[snd] no audio device found -- output is silent\n");
        snd_cap_report();      /* symmetric: no controller means no input either */
        return;
    }
    kprintf("[snd] %s: %s, %u Hz %u ch s16, period %u B (%u frames, %u ms) x %u = %u ms buffer, irq=%s\n",
            g_dev->name, g_dev->codec, g_dev->rate, (unsigned)g_dev->channels,
            g_dev->period_bytes,
            g_dev->period_bytes / (g_dev->channels * 2u),
            (g_dev->period_bytes / (g_dev->channels * 2u)) * 1000u / g_dev->rate,
            g_dev->periods,
            (g_dev->period_bytes / (g_dev->channels * 2u)) * 1000u * g_dev->periods / g_dev->rate,
            g_dev->irq_mode == 3 ? "msix" : g_dev->irq_mode == 2 ? "msi"
          : g_dev->irq_mode == 1 ? "intx" : "polled");
    snd_cap_report();
}

/* ------------------------------------------------------------- streams --- */

static struct snd_stream *find(void *owner, int h)
{
    for (int i = 0; i < SND_MAX_STREAMS; i++)
        if (g_str[i].used && g_str[i].handle == h && g_str[i].owner == owner
            && !g_str[i].closing)
            return &g_str[i];
    return 0;
}

int snd_stream_open(void *owner, const struct logit_sndfmt *f)
{
    struct snd_stream *s = 0;
    unsigned bytes, ms;
    uint64_t fl;

    if (!g_dev) return SND_E_NODEV;
    /* First stream on this boot is what starts the DMA engine and kaudio; see
     * snd_engine_start(). Before the format checks, so a caller that opens with
     * a bad format does not leave the engine half-started. */
    if (!snd_engine_ensure()) return SND_E_NODEV;
    if (!snd_fmt_ok(f->rate, f->channels, f->format)) return SND_E_FORMAT;
    /* An app rate more than SND_MAX_RATIO from the card's would need more input
     * per period than the scratch holds. Refuse it rather than truncate, which
     * would play at the wrong speed. */
    if (f->rate > g_dev->rate * SND_MAX_RATIO) return SND_E_FORMAT;

    ms = f->buffer_ms ? f->buffer_ms : 200;
    if (ms < 20)   ms = 20;
    if (ms > 2000) ms = 2000;
    bytes = (f->rate * ms / 1000) * (unsigned)snd_fmt_bytes(f->format) * f->channels;
    if (bytes < 4096) bytes = 4096;

    fl = spin_lock_irqsave(&g_snd_lock);
    for (int i = 0; i < SND_MAX_STREAMS; i++)
        if (!g_str[i].used) { s = &g_str[i]; break; }
    if (!s) { spin_unlock_irqrestore(&g_snd_lock, fl); return SND_E_NOMEM; }
    /* Claim the slot before dropping the lock to allocate. */
    s->used = 1;
    s->handle = g_next_handle++;
    spin_unlock_irqrestore(&g_snd_lock, fl);

    s->ringmem = (uint8_t *)kmalloc(bytes);
    if (!s->ringmem) { s->used = 0; return SND_E_NOMEM; }

    s->owner = owner;
    s->rate = f->rate; s->channels = f->channels; s->format = f->format;
    s->flags = f->flags;
    s->phase = 0; s->hist[0] = s->hist[1] = 0;
    s->frames_written = 0; s->frames_consumed = 0;
    s->underruns = 0;
    s->state = SND_S_RUNNING;
    s->closing = 0;
    waitq_init(&s->wq);
    pcm_ring_init(&s->ring, s->ringmem, bytes);

    return s->handle;
}

int snd_stream_avail(void *owner, int h)
{
    struct snd_stream *s;
    if (!g_dev) return SND_E_NODEV;
    s = find(owner, h);
    if (!s) return SND_E_BADH;
    return (int)pcm_ring_free(&s->ring);
}

/* Copy `bytes` into the stream's ring, taking what fits. `buf` is already a
 * kernel-accessible pointer -- the syscall layer does the user_range_ok and the
 * copy, so this function is equally usable by the on-device self-test. */
int snd_stream_write(void *owner, int h, const void *buf, int bytes)
{
    struct snd_stream *s;
    unsigned n, bps;
    int waited_ok;

    if (!g_dev) return SND_E_NODEV;
    if (bytes <= 0) return 0;
    s = find(owner, h);
    if (!s) return SND_E_BADH;

    bps = (unsigned)snd_fmt_bytes(s->format) * s->channels;

    if (pcm_ring_free(&s->ring) == 0) {
        if (s->flags & SND_F_NONBLOCK) return 0;
        /* PARK, do not spin. A busy-wait here would hold the BKL against the
         * very thread that drains the ring -- the exact shape of the freeze
         * documented above bkl_hlt_wait() in sched.c. The timeout is a
         * backstop: if the card stopped producing interrupts we return a short
         * write and let the caller decide, rather than hanging its thread
         * forever on hardware that has gone quiet. */
        wait_event_timeout(&s->wq, pcm_ring_free(&s->ring) > 0 || s->closing, 500,
                           waited_ok);
        if (!waited_ok || s->closing) return 0;
    }

    n = pcm_ring_write(&s->ring, buf, (unsigned)bytes);
    s->frames_written += n / bps;
    return (int)n;
}

int snd_stream_close(void *owner, int h, int drain)
{
    struct snd_stream *s = find(owner, h);
    if (!s) return SND_E_BADH;

    if (drain && g_running) {
        /* Play out what is queued. Bounded: a stream whose ring cannot drain
         * because the card has stopped must not wedge the closing thread. */
        int ok;
        s->state = SND_S_DRAINING;
        wait_event_timeout(&s->wq, pcm_ring_used(&s->ring) == 0, 3000, ok);
    }
    s->state = SND_S_DRAINING;
    s->closing = 1;
    /* The actual free happens in kaudio (reap_closed), which is the only thread
     * that reads the ring -- so the memory cannot go away under the mixer. */
    sem_post(&g_period);
    return 0;
}

int snd_stream_state(void *owner, int h, struct logit_sndstate *st)
{
    struct snd_stream *s;
    if (!g_dev) return SND_E_NODEV;
    s = find(owner, h);
    if (!s) return SND_E_BADH;
    st->frames_written = s->frames_written;
    st->frames_played  = s->frames_consumed;
    st->avail_bytes    = pcm_ring_free(&s->ring);
    st->ring_bytes     = s->ring.size;
    st->underruns      = s->underruns;
    st->state          = (unsigned)s->state;
    return 0;
}

void snd_owner_release(void *owner)
{
    /* A player that faults mid-tone must stop making noise. Without this the
     * ring keeps its last samples and the mixer keeps playing them until the
     * ring drains, which is a dead process still audible. */
    for (int i = 0; i < SND_MAX_STREAMS; i++)
        if (g_str[i].used && g_str[i].owner == owner && owner) {
            g_str[i].state = SND_S_DRAINING;
            g_str[i].closing = 1;
        }
    if (g_dev) sem_post(&g_period);
}

void snd_info_fill(struct logit_sndinfo *si)
{
    int i, open = 0;
    memset(si, 0, sizeof *si);
    if (!g_dev) return;

    for (i = 0; (unsigned)i < sizeof si->driver - 1 && g_dev->name[i]; i++)
        si->driver[i] = g_dev->name[i];
    for (i = 0; (unsigned)i < sizeof si->codec - 1 && g_dev->codec[i]; i++)
        si->codec[i] = g_dev->codec[i];

    si->rate         = g_dev->rate;
    si->channels     = g_dev->channels;
    si->format       = g_dev->format;
    si->period_bytes = g_dev->period_bytes;
    si->periods      = g_dev->periods;
    si->streams_max  = SND_MAX_STREAMS;
    si->underruns    = g_dev_underruns;
    si->irq_mode     = g_dev->irq_mode;

    for (i = 0; i < SND_MAX_STREAMS; i++) if (g_str[i].used) open++;
    si->streams_open = (unsigned)open;
}
