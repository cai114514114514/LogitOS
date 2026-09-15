#include "../../drivers/core/io_domain.h"
/* Start/open and detach own the device pointer across allocation or scheduler
 * publication. The IRQ/period worker still use their short existing locks. */
static struct io_domain cap_lifecycle = IO_DOMAIN_INIT;
/* The capture engine: one hardware period in, drained into one ring, once per
 * interrupt. The read-side twin of mixer.c, and worth stating up front how it
 * differs and why, because "just run mixer.c backwards" is the wrong model:
 *
 *   card ISR --> snd_capture_period_elapsed() --> sem_post(&g_cap_period)
 *                                                       |
 *                                             kcapture thread wakes
 *                                                       |
 *                    copy the period(s) the DMA engine finished WRITING
 *                    into the one open stream's byte ring (pcm_ring_write)
 *                                                       |
 *                                          wake any reader waiting for data
 *
 * WHY ONE STREAM AND NOT A TABLE OF EIGHT. Playback's SND_MAX_STREAMS exists
 * because an app-defined number of players can be open (a UI sound plus a
 * video's audio plus a synth), and the mixer's whole job is summing them.
 * There is exactly one microphone. A table of capture slots would need a
 * policy for what a SECOND open even means -- two processes reading the same
 * frames? split between them? -- that nothing in this tree has asked for, so
 * it is not built. SYS_SND_CAP_OPEN on an already-open stream returns
 * SND_E_NOMEM, the same code the playback table returns when it is full, for
 * the same reason.
 *
 * WHY NO RESAMPLING. mix_one() (mixer.c) resamples every playback stream to
 * the card's rate because the RATE IS A FACT ABOUT A FILE some decoder chose,
 * long before this kernel saw it. Capture has no file upstream -- the ADC's
 * rate is simply the rate, and a resampler here would spend cycles turning
 * one honest number into another for a caller that has never been asked for.
 * If a future consumer needs 44.1 kHz off a 48 kHz ADC, pcm_resample already
 * exists and is a stream-open-time decision to make there, not here.
 *
 * THE OVERRUN DIRECTION. A slow reader on the playback side causes an
 * UNDERRUN: the ring runs dry and the card plays silence, because nothing
 * else exists to play. A slow reader on the capture side causes an OVERRUN:
 * the ring fills, and the choice is between the DROPPING THE OLDEST captured
 * audio (this file's choice, `pcm_ring_write`'s short-write-when-full
 * behaviour) or refusing to accept the newest -- i.e. blocking the ISR-driven
 * drain, which is not legal, or dropping what the hardware just delivered
 * instead of what has been sitting in the ring longest. Dropping oldest is
 * what every real capture API does (ALSA calls it -EPIPE and expects the
 * reader to catch up from "now"), and it is the only choice that keeps
 * kcapture non-blocking.
 */
#include "snd.h"
#include "kheap.h"
#include "kprintf.h"
/* Path-qualified for the same reason mixer.c's include is: mini-libc ships a
 * userland wait.h that sorts earlier in INCDIRS, so a bare #include "wait.h"
 * from outside c/kernel/core silently picks up the wrong one. */
#include "kernel/core/wait.h"
#include "sched.h"
#include "spinlock.h"

void *memset(void *, int, size_t);

static struct snd_capdevice * _Atomic g_capdev;
static struct semaphore      g_cap_period;
static spinlock_t            g_cap_lock = SPINLOCK_INIT;
/* Public stream operations share the worker's lock. A waiting syscall must
 * drop it and re-find its handle after waking: close/reopen may recycle the
 * static slot while the task is asleep. Wait queues are initialised once. */
struct cap_guard { uint64_t f; int held; };
static struct cap_guard cap_guard_take(void)
{ return (struct cap_guard){ spin_lock_irqsave(&g_cap_lock), 1 }; }
static void cap_guard_drop(struct cap_guard *g)
{ if (g->held) { g->held = 0; spin_unlock_irqrestore(&g_cap_lock, g->f); } }
#define cap_GUARD struct cap_guard guard __attribute__((cleanup(cap_guard_drop))) = cap_guard_take()


/* The one stream. `used` and `owner`/`handle` exist (rather than a bare
 * "is-open" bool) purely so find()'s shape matches the playback side's and a
 * reader of both files sees the same pattern, not because more than one
 * could ever be `used` at once. */
struct snd_cap_stream {
    _Atomic int              used;
    void            *owner;
    _Atomic int              handle;

    struct pcm_ring  ring;
    uint8_t         *ringmem;

    uint64_t         frames_captured;
    unsigned         overruns;
    _Atomic int              closing;

    struct waitq     wq;             /* readers wait here for data */
};
static struct snd_cap_stream g_cs;
static int g_next_handle = 1;

static _Atomic uint64_t g_cap_periods_done;   /* periods the DMA has FINISHED writing */
static unsigned          g_cap_drain;          /* next period index to copy out */
static unsigned          g_cap_dev_overruns;    /* periods dropped with NO stream open */
static _Atomic int       g_cap_running;
static int               g_cap_engine_up;

/* ------------------------------------------------------------- registry -- */

int snd_register_capture_device(struct snd_capdevice *d)
{
    if (g_capdev) {
        kprintf("[snd] %s (capture) ignored: %s is already the input device\n",
                d->name, g_capdev->name);
        return -1;
    }
    /* Same reasoning as snd_register_device(): one hardware format wired,
     * because s16 is the only one anything downstream (WAV writers, the s16
     * ring type itself) has been asked to consume. */
    if (d->format != SND_FMT_S16) {
        kprintf("[snd] %s (capture) rejected: format %d, only s16 is wired\n",
                d->name, d->format);
        return -1;
    }
    if (!d->ring || !d->period_bytes || d->periods < 2) {
        kprintf("[snd] %s (capture) rejected: needs a ring of >= 2 periods\n", d->name);
        return -1;
    }
    g_capdev = d;
    return 0;
}

int snd_capture_present(void) { return g_capdev != 0; }

/* Called FROM THE CARD'S INTERRUPT HANDLER -- see the identical contract on
 * snd_period_elapsed() in snd.h. A counter bump and a post, nothing else. */
void snd_capture_period_elapsed(struct snd_capdevice *d)
{
    (void)d;
    if (!g_cap_running) return;
    g_cap_periods_done++;
    sem_post(&g_cap_period);
}

/* -------------------------------------------------------------- draining -- */

static int reap_cap_closed(void)
{
    if (!g_cs.used || !g_cs.closing) return 0;
    int stopped = g_cap_running;
    /* The ABI currently permits one capture consumer. Stop only when that
     * consumer is reaped, after the drain loop has relinquished its ring. The
     * kcapture thread survives; a later open restarts the same DMA buffers. */
    if (g_cap_running) {
        g_cap_running = 0;
        if (g_capdev && g_capdev->stop) g_capdev->stop(g_capdev);
    }
    g_cs.used = 0;
    g_cs.closing = 0;
    waitq_wake_all(&g_cs.wq);
    if (g_cs.ringmem) { kfree(g_cs.ringmem); g_cs.ringmem = 0; }
    return stopped;
}

static void kcapture_thread(void)
{
    for (;;) {
        sem_wait(&g_cap_period);

        uint64_t fl = spin_lock_irqsave(&g_cap_lock);
        int stopped = reap_cap_closed();

        if (g_capdev && g_cap_running) {
            uint64_t done = g_cap_periods_done;

            /* Fell behind by more than the ring holds: the periods between
             * g_cap_drain and (done - periods + 1) have already been
             * overwritten by the DMA engine -- there is nothing left to copy
             * out of them. Same recovery shape as mixer.c's g_fill clamp,
             * mirrored: there it protects against writing into what the
             * engine is currently playing, here it protects against reading
             * what the engine has already overwritten. */
            uint64_t oldest_live = (done > g_capdev->periods)
                                  ? done - g_capdev->periods : 0;
            if (g_cap_drain < oldest_live) {
                unsigned skipped = (unsigned)(oldest_live - g_cap_drain);
                g_cap_dev_overruns += skipped;
                if (g_cs.used) g_cs.overruns += skipped;
                g_cap_drain = (unsigned)oldest_live;
            }

            while (g_cap_drain < done) {
                uint8_t *period = g_capdev->ring +
                                  (g_cap_drain % g_capdev->periods) * g_capdev->period_bytes;
                if (g_cs.used) {
                    unsigned n = pcm_ring_write(&g_cs.ring, period, g_capdev->period_bytes);
                    g_cs.frames_captured += n /
                        ((unsigned)snd_fmt_bytes(g_capdev->format) * g_capdev->channels);
                    if (n < g_capdev->period_bytes) {
                        /* The ring itself was already full: this period is
                         * PARTIALLY or not at all delivered. Count the whole
                         * period as one overrun (matching the "fell behind"
                         * branch above, which counts in whole periods) rather
                         * than a byte-granular number nothing downstream
                         * would use differently. */
                        g_cs.overruns++;
                    }
                } else {
                    g_cap_dev_overruns++;   /* nobody is listening */
                }
                g_cap_drain++;
            }
        }
        spin_unlock_irqrestore(&g_cap_lock, fl);

#ifdef HDA_DMA_CAPTURE_CANARY
        if (stopped) {
            extern void hda_capture_stop_check(void);
            hda_capture_stop_check(); /* sleeps only AFTER releasing the lock */
        }
#else
        (void)stopped;
#endif
        if (g_cs.used) waitq_wake_all(&g_cs.wq);
    }
}

/* ----------------------------------------------------------------- init -- */

/* Start the capture DMA engine and the kcapture thread. Separate from
 * snd_cap_init() for the EXACT reason snd_engine_start() is separate from
 * snd_init() in mixer.c: at probe() time (dev_probe_all(), before wm_run()
 * calls sched_init()) thread_create() reads a NULL run ring and corrupts
 * memory rather than faulting cleanly -- see that comment for the full
 * "#GP vector 13" story, which this driver would reproduce verbatim if it
 * called thread_create() from here at probe time. Deferred to first open. */
static int snd_cap_engine_start(void)
{
    if (g_cap_running) return 1;
    if (!g_capdev) return 0;
    if (!sched_current_thread()) {
        kprintf("[snd] capture engine start refused: the scheduler is not up yet\n");
        return 0;
    }

    memset(g_capdev->ring, 0, (size_t)g_capdev->periods * g_capdev->period_bytes);

    g_cap_periods_done = 0;
    g_cap_drain = 0;
    g_cap_running = 1;
    if (g_capdev->start && g_capdev->start(g_capdev) != 0) {
        g_cap_running = 0;
        kprintf("[snd] %s: capture DMA engine would not start -- capture disabled\n",
                g_capdev->name);
        return 0;
    }
    kprintf("[snd] capture engine running: kcapture started, DMA at %u Hz\n",
            g_capdev->rate);
    return 1;
}

void snd_cap_init(void)
{
    waitq_init(&g_cs.wq);
    semaphore_init(&g_cap_period, 0);
    /* Nothing else to allocate: unlike the mixer's scratch buffers (one
     * resampler needs several), a capture period is copied byte-for-byte with
     * no conversion, so there is no per-format scratch sized here. */
}

/* ------------------------------------------------------------- streams --- */

static struct snd_cap_stream *cap_find(void *owner, int h)
{
    if (g_cs.used && g_cs.handle == h && g_cs.owner == owner && !g_cs.closing)
        return &g_cs;
    return 0;
}

int snd_cap_open(void *owner, struct logit_sndfmt *f)
{
    IO_DOMAIN_GUARD(&cap_lifecycle);
    unsigned bytes, ms;
    uint64_t fl;

    if (!g_capdev) return SND_E_NODEV;

    /* rate==0 is the "native" sentinel -- see the ABI note. Anything else
     * must match the hardware EXACTLY: no resampler exists on this path (see
     * the file header), so a caller naming a rate/channels/format the device
     * does not deliver gets refused rather than silently handed the wrong
     * one, which is a worse failure than an error a caller can check for. */
    if (f->rate == 0 && f->channels == 0 && f->format == 0) {
        f->rate = g_capdev->rate;
        f->channels = g_capdev->channels;
        f->format = g_capdev->format;
    } else if (f->rate != g_capdev->rate || f->channels != g_capdev->channels ||
               f->format != g_capdev->format) {
        return SND_E_FORMAT;
    }

    ms = f->buffer_ms ? f->buffer_ms : 200;
    if (ms < 20)   ms = 20;
    if (ms > 2000) ms = 2000;
    bytes = (f->rate * ms / 1000) * (unsigned)snd_fmt_bytes(f->format) * f->channels;
    if (bytes < 4096) bytes = 4096;

    uint8_t *ringmem = (uint8_t *)kmalloc(bytes);
    if (!ringmem) return SND_E_NOMEM;
    fl = spin_lock_irqsave(&g_cap_lock);
    if (g_cs.used) {
        spin_unlock_irqrestore(&g_cap_lock, fl);
        kfree(ringmem);
        return SND_E_NOMEM;
    }
    /* Publish one fully initialised stream with start/stop serialised by the
     * capture lock. No IRQ or worker can observe a half-initialised byte ring. */
    g_cs.ringmem = ringmem;
    g_cs.owner = owner;
    g_cs.frames_captured = 0;
    g_cs.overruns = 0;
    g_cs.closing = 0;
    pcm_ring_init(&g_cs.ring, g_cs.ringmem, bytes);
    g_cs.used = 1;
    g_cs.handle = g_next_handle++;
    spin_unlock_irqrestore(&g_cap_lock, fl);
    /* Scheduler publication cannot run under an audio spinlock. The claimed
     * stream is already initialised, and g_cap_running keeps the worker idle. */
    if (!g_cap_engine_up) {
        thread_create(kcapture_thread, "kcapture");
        g_cap_engine_up = 1;
    }
    fl = spin_lock_irqsave(&g_cap_lock);
    if (!g_capdev || g_cs.closing || !snd_cap_engine_start()) {
        g_cs.used = 0;
        g_cs.ringmem = 0;
        spin_unlock_irqrestore(&g_cap_lock, fl);
        kfree(ringmem);
        return SND_E_NODEV;
    }
    spin_unlock_irqrestore(&g_cap_lock, fl);

    return g_cs.handle;
}

int snd_cap_avail(void *owner, int h)
{
    cap_GUARD;
    struct snd_cap_stream *s;
    if (!g_capdev) return SND_E_NODEV;
    s = cap_find(owner, h);
    if (!s) return SND_E_BADH;
    return (int)pcm_ring_used(&s->ring);
}

int snd_cap_read(void *owner, int h, void *buf, int bytes)
{
    cap_GUARD;
    struct snd_cap_stream *s;
    unsigned n;
    int waited_ok;

    if (!g_capdev) return SND_E_NODEV;
    if (bytes <= 0) return 0;
    s = cap_find(owner, h);
    if (!s) return SND_E_BADH;

    if (pcm_ring_used(&s->ring) == 0) {
        /* PARK, not spin -- same reasoning as snd_stream_write's mirror-image
         * wait: a busy loop here would hold the BKL against the very thread
         * (kcapture) that produces the data being waited for. The timeout is
         * a backstop for hardware that has gone quiet, not the mechanism. */
        cap_guard_drop(&guard);
        wait_event_timeout(&s->wq, pcm_ring_used(&s->ring) > 0 || !s->used || s->closing || s->handle != h, 500,
                           waited_ok);
        guard = cap_guard_take();
        s = cap_find(owner, h);
        if (!waited_ok || !s || !g_capdev) return 0;
    }

    n = pcm_ring_read(&s->ring, buf, (unsigned)bytes);
    return (int)n;
}

int snd_cap_close(void *owner, int h)
{
    cap_GUARD;
    struct snd_cap_stream *s = cap_find(owner, h);
    if (!s) return SND_E_BADH;
    /* Nothing to drain on the input side -- there is no "play out what is
     * queued" for a microphone. Mark closing and let kcapture (the only
     * thread that touches s->ring) free it on its next wake, same as
     * reap_closed() on the playback side and for the same reason: the ring
     * must never be freed while the drain loop is walking it. */
    s->closing = 1;
    sem_post(&g_cap_period);
    return 0;
}

int snd_cap_state(void *owner, int h, struct logit_sndstate *st)
{
    cap_GUARD;
    struct snd_cap_stream *s;
    if (!g_capdev) return SND_E_NODEV;
    s = cap_find(owner, h);
    if (!s) return SND_E_BADH;
    st->frames_written = 0;                     /* meaningless on this side */
    st->frames_played  = s->frames_captured;     /* repurposed: frames CAPTURED */
    st->avail_bytes    = pcm_ring_used(&s->ring);/* repurposed: bytes WAITING, not room */
    st->ring_bytes     = s->ring.size;
    st->underruns      = s->overruns;            /* repurposed: OVERRUNS */
    st->state          = s->closing ? SND_S_DRAINING : SND_S_RUNNING;
    return 0;
}

void snd_cap_report(void)
{
    cap_GUARD;
    if (!g_capdev) {
        kprintf("[snd] no capture device found -- input is silent\n");
        return;
    }
    kprintf("[snd] %s: %s, %u Hz %u ch s16, period %u B (%u frames, %u ms) x %u = %u ms buffer, irq=%s\n",
            g_capdev->name, g_capdev->codec, g_capdev->rate, (unsigned)g_capdev->channels,
            g_capdev->period_bytes,
            g_capdev->period_bytes / (g_capdev->channels * 2u),
            (g_capdev->period_bytes / (g_capdev->channels * 2u)) * 1000u / g_capdev->rate,
            g_capdev->periods,
            (g_capdev->period_bytes / (g_capdev->channels * 2u)) * 1000u * g_capdev->periods / g_capdev->rate,
            g_capdev->irq_mode == 3 ? "msix" : g_capdev->irq_mode == 2 ? "msi"
          : g_capdev->irq_mode == 1 ? "intx" : "polled");
}

void snd_cap_owner_release(void *owner)
{
    cap_GUARD;
    if (g_cs.used && g_cs.owner == owner && owner) {
        g_cs.closing = 1;
        if (g_capdev) sem_post(&g_cap_period);
    }
}

void snd_unregister_capture_device(struct snd_capdevice *d)
{
    IO_DOMAIN_GUARD(&cap_lifecycle);
    uint64_t fl = spin_lock_irqsave(&g_cap_lock);
    if (g_capdev != d) { spin_unlock_irqrestore(&g_cap_lock, fl); return; }
    g_cap_running = 0;
    g_capdev = NULL;
    if (g_cs.used) {
        g_cs.closing = 1;
        waitq_wake_all(&g_cs.wq);
    }
    spin_unlock_irqrestore(&g_cap_lock, fl);
    if (g_cap_engine_up) sem_post(&g_cap_period);
}
