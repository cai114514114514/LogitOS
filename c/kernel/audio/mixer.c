#include "../../drivers/core/io_domain.h"
/* Start/open and detach own the device pointer across allocation or scheduler
 * publication. The IRQ/period worker still use their short existing locks. */
static struct io_domain snd_lifecycle = IO_DOMAIN_INIT;
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
#include "kernel/sync/wait.h"
#include "sched.h"
#include "spinlock.h"

#ifndef memset
void *memset(void *, int, size_t);
#endif

#define SND_MAX_STREAMS 8
#define SND_MAX_APP_CHANNELS 8u
#define SND_MAX_SAMPLE_BYTES 4u
#define SND_MAX_PERIOD_BYTES (256u * 1024u)
#define SND_MAX_RING_BYTES (16u * 1024u * 1024u)
#define SND_MAX_PERIODS 256u

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
    _Atomic int              used;
    void            *owner;          /* struct proc *, or NULL for a kernel caller */
    _Atomic int              handle;

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
    _Atomic int              closing;        /* set by close(); kaudio reaps */

    struct waitq     wq;             /* writers wait here for room */
};

static struct snd_stream g_str[SND_MAX_STREAMS];
static spinlock_t        g_snd_lock = SPINLOCK_INIT;
/* Public stream operations share the worker's lock. A waiting syscall must
 * drop it and re-find its handle after waking: close/reopen may recycle the
 * static slot while the task is asleep. Wait queues are initialised once. */
struct snd_guard { uint64_t f; int held; };
static struct snd_guard snd_guard_take(void)
{ return (struct snd_guard){ spin_lock_irqsave(&g_snd_lock), 1 }; }
static void snd_guard_drop(struct snd_guard *g)
{ if (g->held) { g->held = 0; spin_unlock_irqrestore(&g_snd_lock, g->f); } }
#define snd_GUARD struct snd_guard guard __attribute__((cleanup(snd_guard_drop))) = snd_guard_take()

static int               g_next_handle = 1;

static struct snd_device * _Atomic g_dev;
static struct semaphore   g_period;
static uint64_t           g_fill;        /* period index kaudio fills next */
static unsigned           g_dev_underruns;
static _Atomic uint64_t   g_periods_done;
static _Atomic int        g_running;

/* Separate from the mixer lock: driver start holds the mixer lock and then
 * the card gate; the card ISR already owns that gate. Taking the mixer lock
 * from the ISR would invert that order. Event state uses only this short lock,
 * and no code holding it enters a driver or acquires the mixer lock. */
static spinlock_t g_event_lock = SPINLOCK_INIT;
static int g_initialized;
static int g_worker_created;
static int g_start_failed;

/* Scratch, allocated for each registered device's validated geometry. Not on the stack:
 * the deep path here is 32 KiB at 192 kHz, and this kernel has already
 * overflowed a thread stack into its own page tables once (see the M11 note). */
static uint8_t  *g_in_raw;      /* app-format input */
static int16_t  *g_in_s16;      /* converted, device channel count */
static int16_t  *g_out;         /* one period, device channels, s16 */
static unsigned  g_period_frames;

/* ------------------------------------------------------------- registry -- */

/* Lifecycle domain held by the caller; wait queues must outlive any stream
 * generation. Reinitializing them on each probe used to orphan sleeping
 * writers and reset the live semaphore, while leaking the previous scratch. */
static void initialize_service(void)
{
    if (g_initialized) {
        return;
    }
    semaphore_init(&g_period, 0);
    for (unsigned index = 0; index < SND_MAX_STREAMS; ++index) {
        waitq_init(&g_str[index].wq);
    }
    g_initialized = 1;
}

static int valid_device_geometry(const struct snd_device *device)
{
    if (!device || !device->name || !device->ring || !device->start || !device->stop ||
        device->format != SND_FMT_S16 || device->channels < 1 || device->channels > 2 ||
        !snd_fmt_ok(device->rate, device->channels, device->format)) {
        return 0;
    }
    unsigned frame_bytes = device->channels * sizeof(int16_t);
    if (!device->period_bytes || device->period_bytes % frame_bytes ||
        device->period_bytes > SND_MAX_PERIOD_BYTES || device->periods < 2 ||
        device->periods > SND_MAX_PERIODS ||
        device->periods > SND_MAX_RING_BYTES / device->period_bytes ||
        (uintptr_t)device->ring % _Alignof(int16_t)) {
        return 0;
    }
    size_t ring_bytes = (size_t)device->period_bytes * device->periods;
    /* pcm_resample carries its phase in Q16.16. Even at the allowed 4:1
     * input ratio, one call must stay below its 16-bit integer frame range. */
    unsigned period_frames = device->period_bytes / frame_bytes;
    if (period_frames * SND_MAX_RATIO + 4u > UINT16_MAX) {
        return 0;
    }
    return (uintptr_t)device->ring <= UINTPTR_MAX - (ring_bytes - 1u);
}

int snd_register_device(struct snd_device *device)
{
    IO_DOMAIN_GUARD(&snd_lifecycle);
    if (!valid_device_geometry(device)) {
        return -1;
    }
    if (g_dev) {
        kprintf("[snd] %s ignored: %s is already the output device\n",
                device->name, g_dev->name);
        return -1;
    }
    initialize_service();
    unsigned period_frames = device->period_bytes / (device->channels * sizeof(int16_t));
    unsigned input_frames = period_frames * SND_MAX_RATIO + 4u;
    /* snd_fmt_ok accepts eight app channels. The old two-channel raw allocation
     * overflowed when an eight-channel F32 stream filled one period. Conversion
     * scratch and resampler history still follow the one/two-channel device. */
    uint8_t *input_raw = kmalloc((size_t)input_frames * SND_MAX_APP_CHANNELS *
                                SND_MAX_SAMPLE_BYTES);
    int16_t *input_s16 = kmalloc((size_t)input_frames * device->channels * sizeof(int16_t));
    int16_t *output = kmalloc(device->period_bytes);
    if (!input_raw || !input_s16 || !output) {
        kfree(input_raw);
        kfree(input_s16);
        kfree(output);
        return -1;
    }
    snd_GUARD;
    g_in_raw = input_raw;
    g_in_s16 = input_s16;
    g_out = output;
    g_period_frames = period_frames;
    g_fill = 1;
    g_dev_underruns = 0;
    g_start_failed = 0;
    uint64_t event_flags = spin_lock_irqsave(&g_event_lock);
    g_periods_done = 0;
    g_running = 0;
    g_dev = device;
    spin_unlock_irqrestore(&g_event_lock, event_flags);
    return 0;
}

int snd_present(void) { return g_dev != 0; }

/* The card ISR may hold its own gate. Only event_lock is taken here: its
 * identity/running test and counter update cannot straddle detach/rebind. */
void snd_period_elapsed(struct snd_device *device)
{
    uint64_t flags = spin_lock_irqsave(&g_event_lock);
    if (device == g_dev && g_running) {
        ++g_periods_done;
        sem_post(&g_period);
    }
    spin_unlock_irqrestore(&g_event_lock, flags);
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
            uint64_t earliest = done + 1;

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
            uint64_t limit = done + SND_FILL_LEAD;
            uint64_t cap = done + g_dev->periods - 1;
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
            /* Correction: the old unconditional zero wraps onto the active
             * DMA slot with a two/three/four-period ring. There is no spare
             * silence slot when the lead already fills every future period. */
            if (g_fill - done < g_dev->periods) {
                memset(g_dev->ring + (g_fill % g_dev->periods) * g_dev->period_bytes,
                       0, g_dev->period_bytes);
            }
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
static int snd_engine_start(int quiet)
{
    IO_DOMAIN_GUARD(&snd_lifecycle);
    if (!g_dev || g_start_failed) {
        return 0;
    }
    if (g_running) {
        return 1;
    }
    if (!sched_current_thread()) {
        if (!quiet) {
            kprintf("[snd] engine start refused: the scheduler is not up yet\n");
        }
        return 0;
    }
    /* One worker survives device detach. The old g_engine_up conflated this
     * lifetime with DMA: a second card was reported started without start()
     * ever being called. Scheduler publication must occur outside snd_lock.
     * thread_create currently returns void; its allocation failure cannot be
     * detected through this interface, so no reliable OOM retry is claimed. */
    if (!g_worker_created) {
        thread_create(kaudio_thread, "kaudio");
        g_worker_created = 1;
    }
    snd_GUARD;
    memset(g_dev->ring, 0, (size_t)g_dev->periods * g_dev->period_bytes);
    g_fill = 1;
    uint64_t event_flags = spin_lock_irqsave(&g_event_lock);
    g_periods_done = 0;
    g_running = 1;
    spin_unlock_irqrestore(&g_event_lock, event_flags);
    /* Holding snd_lock keeps the worker off the ring until start returns.
     * IRQs use event_lock and may arrive synchronously from a driver callback. */
    if (g_dev->start(g_dev) != 0) {
        event_flags = spin_lock_irqsave(&g_event_lock);
        g_running = 0;
        spin_unlock_irqrestore(&g_event_lock, event_flags);
        g_start_failed = 1;
        kprintf("[snd] %s: DMA start failed; binding disabled until removal\n", g_dev->name);
        /* Do not retry or zero the DMA ring again: the void stop contract
         * cannot prove that failed hardware stopped. Driver removal owns the
         * final hardware quiescence and DMA allocation lifetime. */
        return 0;
    }
    kprintf("[snd] engine running: kaudio ready, DMA at %u Hz\n", g_dev->rate);
    return 1;
}

int snd_engine_ensure(void)
{
    return snd_engine_start(0);
}

void snd_init(void)
{
    IO_DOMAIN_GUARD(&snd_lifecycle);
    initialize_service();
    snd_report_once();
    snd_cap_init();
    /* io_domain recursion is task-owned, so open/ensure/init can all share
     * this domain. None of these calls holds snd_lock while entering it. */
    (void)snd_engine_start(1);
}

void snd_report(void)
{
    snd_GUARD;
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
            (unsigned)((uint64_t)(g_dev->period_bytes / (g_dev->channels * 2u)) * 1000u * g_dev->periods / g_dev->rate),
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
    IO_DOMAIN_GUARD(&snd_lifecycle);
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

    uint8_t *ringmem = (uint8_t *)kmalloc(bytes);
    if (!ringmem) return SND_E_NOMEM;
    fl = spin_lock_irqsave(&g_snd_lock);
    if (!g_dev) { spin_unlock_irqrestore(&g_snd_lock, fl); kfree(ringmem); return SND_E_NODEV; }
    for (int i = 0; i < SND_MAX_STREAMS; i++)
        if (!g_str[i].used) { s = &g_str[i]; break; }
    if (!s) { spin_unlock_irqrestore(&g_snd_lock, fl); kfree(ringmem); return SND_E_NOMEM; }
    s->handle = g_next_handle++;
    s->ringmem = ringmem;

    s->owner = owner;
    s->rate = f->rate; s->channels = f->channels; s->format = f->format;
    s->flags = f->flags;
    s->phase = 0; s->hist[0] = s->hist[1] = 0;
    s->frames_written = 0; s->frames_consumed = 0;
    s->underruns = 0;
    s->state = SND_S_RUNNING;
    s->closing = 0;
    pcm_ring_init(&s->ring, s->ringmem, bytes);
    s->used = 1;
    int handle = s->handle;
    spin_unlock_irqrestore(&g_snd_lock, fl);
    return handle;
}

int snd_stream_avail(void *owner, int h)
{
    snd_GUARD;
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
    snd_GUARD;
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
        snd_guard_drop(&guard);
        wait_event_timeout(&s->wq, pcm_ring_free(&s->ring) > 0 || !s->used || s->closing || s->handle != h, 500,
                           waited_ok);
        guard = snd_guard_take();
        s = find(owner, h);
        if (!waited_ok || !s || !g_dev) return 0;
    }

    n = pcm_ring_write(&s->ring, buf, (unsigned)bytes);
    s->frames_written += n / bps;
    return (int)n;
}

int snd_stream_close(void *owner, int h, int drain)
{
    snd_GUARD;
    struct snd_stream *s = find(owner, h);
    if (!s) return SND_E_BADH;

    if (drain && g_running) {
        /* Play out what is queued. Bounded: a stream whose ring cannot drain
         * because the card has stopped must not wedge the closing thread. */
        int ok;
        s->state = SND_S_DRAINING;
        snd_guard_drop(&guard);
        wait_event_timeout(&s->wq, pcm_ring_used(&s->ring) == 0 || !s->used || s->closing || s->handle != h, 3000, ok);
        guard = snd_guard_take();
        s = find(owner, h);
        if (!s) return 0;
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
    snd_GUARD;
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
    snd_GUARD;
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
    snd_GUARD;
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

void snd_unregister_device(struct snd_device *device)
{
    IO_DOMAIN_GUARD(&snd_lifecycle);
    uint64_t flags = spin_lock_irqsave(&g_snd_lock);
    if (!device || g_dev != device) {
        spin_unlock_irqrestore(&g_snd_lock, flags);
        return;
    }
    uint64_t event_flags = spin_lock_irqsave(&g_event_lock);
    g_running = 0;
    g_dev = NULL;
    g_periods_done = 0;
    spin_unlock_irqrestore(&g_event_lock, event_flags);
    for (unsigned index = 0; index < SND_MAX_STREAMS; ++index) {
        if (g_str[index].used) {
            g_str[index].closing = 1;
            g_str[index].state = SND_S_DRAINING;
        }
    }
    /* We own the same lock as kaudio, so no reader can still use these rings.
     * Reap before another device registers: old samples/resampler state must
     * never enter the new sink, even if no worker wake occurs in between. */
    reap_closed();
    uint8_t *input_raw = g_in_raw;
    int16_t *input_s16 = g_in_s16;
    int16_t *output = g_out;
    g_in_raw = NULL;
    g_in_s16 = NULL;
    g_out = NULL;
    g_period_frames = 0;
    g_start_failed = 0;
    spin_unlock_irqrestore(&g_snd_lock, flags);
    kfree(input_raw);
    kfree(input_s16);
    kfree(output);
    /* Queued old semaphore tokens are harmless: kaudio reads current device
     * state under snd_lock. Never reset a semaphore containing live waiters. */
}
