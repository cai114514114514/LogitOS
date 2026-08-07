/* ============================================================================
 * kprof -- the sampling profiler, the span accumulator, and the self-tests
 * that say whether either of them can be believed.
 *
 * Read kprof.h first: it has the model and the API. This file has the
 * machinery, in the order a reader needs it:
 *
 *   1. the accumulator      a lock-free open-addressed RIP histogram, written
 *                           from interrupt context on every core at once
 *   2. spans                named count/total/max, keyed by an interned slot
 *   3. the report           what `cat /dev/kprof` renders
 *   4. the sample interrupt the IDT gate, the timer that drives it, and the
 *                           IPI fan-out that makes it cover every core
 *   5. the self-tests       a workload whose answer is already known, and the
 *                           measurement of what the profiler costs
 *   6. commands             /dev/kprof's write verbs
 *
 * The accumulator half is host-compilable (-DLOGIT_KPROF_HOST) and driven by
 * tests/unit/prof_test.c from four pthreads, which is how "samples arrive on
 * four cores and the accumulator does not tear" is checked deterministically
 * instead of being hoped about on a VM.
 * ==========================================================================*/

#include <stdint.h>
#include <stddef.h>
#include "kprof.h"

#ifdef LOGIT_KPROF_HOST
/* ---- host shims ---------------------------------------------------------- */
#include <stdio.h>
#include <stdarg.h>
#define KL_PANIC 0
#define KL_ERR   1
#define KL_WARN  2
#define KL_INFO  3
#define KL_DEBUG 4
#define NS_PER_MS 1000000ull
typedef struct { int dummy; } spinlock_t;
#define SPINLOCK_INIT { 0 }
static volatile int g_host_lockword;
static uint64_t spin_lock_irqsave(spinlock_t *l)
{
    (void)l;
    while (__atomic_exchange_n(&g_host_lockword, 1, __ATOMIC_ACQUIRE))
        ;
    return 0;
}
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
    (void)l; (void)f;
    __atomic_store_n(&g_host_lockword, 0, __ATOMIC_RELEASE);
}
static int ksnprintf(char *b, int max, const char *f, ...)
{
    va_list ap; va_start(ap, f);
    int n = vsnprintf(b, (size_t)(max > 0 ? max : 0), f, ap);
    va_end(ap);
    if (max <= 0) return 0;
    return n >= max ? max - 1 : n;
}
static void klog(int level, const char *f, ...)
{
    va_list ap; va_start(ap, f);
    printf("[%d] ", level); vprintf(f, ap); printf("\n");
    va_end(ap);
}
static volatile uint64_t g_host_ns;
static uint64_t time_mono_raw_ns(void) { return __atomic_load_n(&g_host_ns, __ATOMIC_RELAXED); }
void kprof_host_advance(uint64_t ns);
void kprof_host_advance(uint64_t ns) { __atomic_add_fetch(&g_host_ns, ns, __ATOMIC_RELAXED); }
#else
/* ---- kernel -------------------------------------------------------------- */
#include "spinlock.h"
#include "kprintf.h"
#include "klog.h"
#include "ktime.h"
#include "percpu.h"
#include "smp.h"
#include "lapic.h"
#include "pit.h"

void idt_install_gate(int vec, void *handler);   /* c/kernel/cpu/idt.c */

/* Arming state. Declared here rather than next to the code that maintains it
 * (section 4) because the REPORT prints it, and the report comes first in
 * reading order. See KPROF_DEFAULT_COUNT in section 4 for how the default rate
 * was chosen -- it is a measured number, not a round one. */
static uint32_t g_lapic_count;
static volatile int g_arm_request;      /* +1 = arm, -1 = disarm, 0 = idle */
static volatile uint64_t g_arm_fired, g_arm_armed, g_arm_disarmed;
#endif

/* The two runtime flags. Deliberately plain globals: see kprof.h. */
volatile int g_kprof_sampling;
volatile int g_kprof_spans;

/* ============================================================================
 * 1. The accumulator
 *
 * WHAT HAS TO BE TRUE HERE, AND WHY THE OBVIOUS CODE IS WRONG.
 *
 * Samples arrive in INTERRUPT context, on every core, at an arbitrary
 * instruction -- including inside this function on another core. So:
 *
 *   - No lock. A sample handler that waits for a lock held by an interrupted
 *     core deadlocks the machine on the one path that is supposed to observe
 *     it. Open addressing with a CAS to claim a slot and an atomic add to bump
 *     it needs no lock at all.
 *   - No allocation. The tables are .bss, sized at compile time. A profiler
 *     that calls into the heap perturbs the very allocator this project has
 *     already found twice to be the bottleneck.
 *   - `hits++` is not enough. A non-atomic read-modify-write from two cores
 *     LOSES samples, silently, in proportion to how contended the hot address
 *     is -- that is, it under-reports exactly the code you are profiling for.
 *     -DKPROF_UNSAFE builds that version and tests/unit/prof_test.c requires it
 *     to FAIL, which is what makes the atomic version evidence and not a habit.
 *
 * Ring 0 and ring 3 go in SEPARATE tables. They are different address spaces
 * with different symbol files (build/kernel.map versus an app's ELF), and one
 * histogram holding both would resolve a browser address against the kernel map
 * and print a confident wrong name.
 * ==========================================================================*/

struct kprof_bucket {
    uint64_t rip;        /* 0 = empty. RIP 0 is not a real sample. */
    uint64_t hits;
};

static struct kprof_bucket g_kbucket[KPROF_NBUCKET];
static struct kprof_bucket g_ubucket[KPROF_UBUCKET];

static volatile uint64_t g_overflow;      /* samples the hash could not place */
static volatile uint64_t g_lost_ring;     /* samples with an unusable RIP */
static volatile uint64_t g_cpu_hits[KPROF_MAXCPU];   /* one writer per slot */
static volatile uint64_t g_cpu_user[KPROF_MAXCPU];
static volatile uint32_t g_cpu_mask;
static uint64_t g_run_t0, g_run_ns;

/* Fibonacci hashing: one multiply, and it spreads the LOW bits of an
 * instruction pointer -- which are far from uniform, since x86 instructions
 * cluster and both tables are power-of-two sized. */
static inline uint32_t rip_hash(uint64_t rip, uint32_t n)
{
    uint64_t h = rip * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 40) & (n - 1);
}

/* 1 if the sample was placed. Interrupt-context safe and lock free. */
static int accumulate(struct kprof_bucket *tab, uint32_t n, uint64_t rip)
{
    uint32_t h = rip_hash(rip, n);
    for (int probe = 0; probe < KPROF_PROBE; probe++) {
        uint32_t i = (h + (uint32_t)probe) & (n - 1);
#ifdef KPROF_UNSAFE
        /* THE NEGATIVE CONTROL: the profiler you write if you do not think
         * about two cores taking a sample at the same instant. */
        uint64_t k = tab[i].rip;
        if (k == rip) { tab[i].hits = tab[i].hits + 1; return 1; }
        if (k == 0)   { tab[i].rip = rip; tab[i].hits = tab[i].hits + 1; return 1; }
#else
        uint64_t k = __atomic_load_n(&tab[i].rip, __ATOMIC_ACQUIRE);
        if (k == rip) {
            __atomic_add_fetch(&tab[i].hits, 1, __ATOMIC_RELAXED);
            return 1;
        }
        if (k == 0) {
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&tab[i].rip, &expect, rip, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_add_fetch(&tab[i].hits, 1, __ATOMIC_RELAXED);
                return 1;
            }
            /* Lost the race. If the winner wrote OUR key we still belong here;
             * otherwise keep probing. Both cases must be handled or a sample is
             * dropped every time two cores first touch the same address. */
            if (expect == rip) {
                __atomic_add_fetch(&tab[i].hits, 1, __ATOMIC_RELAXED);
                return 1;
            }
        }
#endif
    }
    return 0;
}

/* The one function the interrupt handler calls. Kept tiny on purpose: every
 * instruction in here is charged to the workload being measured. */
static void record_sample(uint64_t rip, int user, int cpu)
{
    if (cpu >= 0 && cpu < KPROF_MAXCPU) {
        /* Per-CPU, single writer, so a plain increment cannot tear -- and that
         * is the point. This counter is the INDEPENDENT witness the shared
         * accumulator is checked against; if it were maintained the same way as
         * the histogram, a bug in the histogram would be invisible to it. */
        if (user) g_cpu_user[cpu]++;
        g_cpu_hits[cpu]++;
        __atomic_or_fetch(&g_cpu_mask, 1u << cpu, __ATOMIC_RELAXED);
    }
    if (!rip) { __atomic_add_fetch(&g_lost_ring, 1, __ATOMIC_RELAXED); return; }

    int ok = user ? accumulate(g_ubucket, KPROF_UBUCKET, rip)
                  : accumulate(g_kbucket, KPROF_NBUCKET, rip);
    if (!ok)
        __atomic_add_fetch(&g_overflow, 1, __ATOMIC_RELAXED);
}

#ifdef LOGIT_KPROF_HOST
void kprof_host_sample(uint64_t rip, int user, int cpu) { record_sample(rip, user, cpu); }
#endif

/* ============================================================================
 * 2. Spans
 * ==========================================================================*/

struct kprof_span {
    char     name[KPROF_NAMELEN];
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
};

static struct kprof_span g_span[KPROF_NSLOT];
static int        g_nspan;
static spinlock_t g_span_lock = SPINLOCK_INIT;
static volatile uint64_t g_span_events;

static int name_eq(const char *a, const char *b)
{
    int i = 0;
    for (; i < KPROF_NAMELEN - 1 && a[i] && a[i] == b[i]; i++)
        ;
    return i == KPROF_NAMELEN - 1 || a[i] == b[i];
}

int kprof_slot(const char *name)
{
    if (!name || !name[0]) return -1;
    /* Read side first, unlocked: g_nspan is published with a release store
     * AFTER the name is written, so a reader that sees the count sees the name.
     * This is the path every span after the first takes. */
    int n = __atomic_load_n(&g_nspan, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (name_eq(g_span[i].name, name))
            return i;

    uint64_t f = spin_lock_irqsave(&g_span_lock);
    n = g_nspan;
    for (int i = 0; i < n; i++) {
        if (name_eq(g_span[i].name, name)) {
            spin_unlock_irqrestore(&g_span_lock, f);
            return i;
        }
    }
    int slot = -1;
    if (n < KPROF_NSLOT) {
        int i = 0;
        for (; i < KPROF_NAMELEN - 1 && name[i]; i++)
            g_span[n].name[i] = name[i];
        g_span[n].name[i] = 0;
        g_span[n].count = g_span[n].total_ns = g_span[n].max_ns = 0;
        __atomic_store_n(&g_nspan, n + 1, __ATOMIC_RELEASE);
        slot = n;
    }
    spin_unlock_irqrestore(&g_span_lock, f);
    return slot;
}

void kprof_span_add(int slot, uint64_t ns)
{
    if (slot < 0 || slot >= KPROF_NSLOT) return;
    __atomic_add_fetch(&g_span[slot].count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_span[slot].total_ns, ns, __ATOMIC_RELAXED);
    uint64_t mx = __atomic_load_n(&g_span[slot].max_ns, __ATOMIC_RELAXED);
    while (ns > mx &&
           !__atomic_compare_exchange_n(&g_span[slot].max_ns, &mx, ns, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    __atomic_add_fetch(&g_span_events, 1, __ATOMIC_RELAXED);
}

uint64_t kprof_span_begin_slow(int *slot, const char *name)
{
    if (*slot < 0)
        *slot = kprof_slot(name);
    /* RAW, not the clamped monotonic clock, and that is a deliberate trade.
     * time_mono_ns() publishes max(seen, mine) with a compare-exchange on ONE
     * global word -- correct, but it makes every span on every core hit one
     * cache line, which is a measurement instrument perturbing the thing it
     * measures precisely when the machine is busiest. The raw clock can step
     * back by the inter-core TSC skew (bounded, and zero under QEMU/TCG, which
     * derives every core's TSC from one host clock); a negative delta is
     * clamped to zero at END. For spans measuring microseconds and up that is
     * noise, and the alternative is noise of a worse kind. */
    /* Biased by one so that 0 can be the unambiguous "spans were off" sentinel
     * without a clock reading of 0 being mistaken for it -- and without the
     * cheaper `return t ? t : 1`, which silently measures a span starting at
     * monotonic time 0 as one nanosecond short. That is an irrelevant error on
     * the machine and a wrong answer in a test, and a measurement instrument
     * does not get to have those in different places. */
    return time_mono_raw_ns() + 1;
}

void kprof_span_end_slow(int slot, uint64_t t0)
{
    uint64_t t1 = time_mono_raw_ns();
    uint64_t s  = t0 - 1;
    kprof_span_add(slot, t1 > s ? t1 - s : 0);
}

int kprof_span_get(const char *name, uint64_t *count, uint64_t *total_ns,
                   uint64_t *max_ns)
{
    int n = __atomic_load_n(&g_nspan, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        if (!name_eq(g_span[i].name, name))
            continue;
        if (count)    *count    = g_span[i].count;
        if (total_ns) *total_ns = g_span[i].total_ns;
        if (max_ns)   *max_ns   = g_span[i].max_ns;
        return 0;
    }
    return -1;
}

/* ============================================================================
 * 3. Statistics + the report
 * ==========================================================================*/

void kprof_get_stats(struct kprof_stats *o)
{
    if (!o) return;
    uint64_t taken = 0, user = 0, rec = 0;
    uint32_t distinct = 0;
    for (int i = 0; i < KPROF_MAXCPU; i++) { taken += g_cpu_hits[i]; user += g_cpu_user[i]; }
    for (int i = 0; i < KPROF_NBUCKET; i++)
        if (g_kbucket[i].rip) { rec += g_kbucket[i].hits; distinct++; }
    for (int i = 0; i < KPROF_UBUCKET; i++)
        if (g_ubucket[i].rip) { rec += g_ubucket[i].hits; distinct++; }
    o->samples        = taken;
    o->user_samples   = user;
    o->kernel_samples = taken - user;
    o->recorded       = rec;
    o->overflow       = g_overflow;
    o->lost_ring      = g_lost_ring;
    o->cores          = g_cpu_mask;
    o->distinct       = distinct;
    o->spans          = g_span_events;
    if (g_kprof_sampling && g_run_t0) {
        uint64_t now = time_mono_raw_ns();
        o->run_ns = now > g_run_t0 ? now - g_run_t0 : 0;
    } else {
        o->run_ns = g_run_ns;
    }
}

uint64_t kprof_hits_in(uint64_t lo, uint64_t hi)
{
    uint64_t n = 0;
    for (int i = 0; i < KPROF_NBUCKET; i++)
        if (g_kbucket[i].rip >= lo && g_kbucket[i].rip < hi)
            n += g_kbucket[i].hits;
    return n;
}

void kprof_reset(void)
{
    for (int i = 0; i < KPROF_NBUCKET; i++) { g_kbucket[i].rip = 0; g_kbucket[i].hits = 0; }
    for (int i = 0; i < KPROF_UBUCKET; i++) { g_ubucket[i].rip = 0; g_ubucket[i].hits = 0; }
    for (int i = 0; i < KPROF_MAXCPU; i++)  { g_cpu_hits[i] = 0; g_cpu_user[i] = 0; }
    g_overflow = g_lost_ring = 0;
    g_cpu_mask = 0;
    g_run_ns = 0;
    g_run_t0 = 0;
    uint64_t f = spin_lock_irqsave(&g_span_lock);
    for (int i = 0; i < KPROF_NSLOT; i++)
        g_span[i].count = g_span[i].total_ns = g_span[i].max_ns = 0;
    g_span_events = 0;
    spin_unlock_irqrestore(&g_span_lock, f);
    /* g_nspan and the names are KEPT: a slot number already cached in a
     * KPROF_BEGIN static must stay valid across a reset. */
}

int kprof_active(void) { return g_kprof_sampling; }

/* Top-N by repeated maximum over the key (hits descending, rip ascending).
 * Non-destructive -- a `cat` must not change what the next `cat` reports. N is
 * 48 against 8192 buckets, so this is ~400k comparisons, once, off the hot
 * path, with no allocation: a qsort here would need a scratch array as large as
 * the table and the kernel has nowhere free to put one. */
static int render_top(char *buf, int max, const struct kprof_bucket *tab, int n,
                      const char *ring, int want, uint64_t total)
{
    int w = 0, first = 1;
    uint64_t ch = 0, cr = 0;             /* ceiling key (hits, rip) */
    for (int k = 0; k < want && w < max - 96; k++) {
        int bi = -1;
        uint64_t bh = 0, br = 0;
        for (int i = 0; i < n; i++) {
            uint64_t r = tab[i].rip;
            if (!r) continue;
            uint64_t h = tab[i].hits;
            /* strictly below the previous key in (hits desc, rip asc) order */
            if (!first && !(h < ch || (h == ch && r > cr))) continue;
            if (bi < 0 || h > bh || (h == bh && r < br)) { bi = i; bh = h; br = r; }
        }
        if (bi < 0) break;
        /* Permille, integer: the kernel prints no floats, and a percentage with
         * no fraction is useless once a line is below 1%. */
        unsigned per = total ? (unsigned)((bh * 1000ull) / total) : 0;
        w += ksnprintf(buf + w, max - w, "%016llx %8llu %3u.%u%% %s\n",
                       (unsigned long long)br, (unsigned long long)bh,
                       per / 10, per % 10, ring);
        ch = bh; cr = br; first = 0;
    }
    return w;
}

int kprof_report(char *buf, int max)
{
    int n = 0;
    n += ksnprintf(buf + n, max - n, "kprof v1\n");
#ifdef KPROF_DISABLE
    n += ksnprintf(buf + n, max - n, "built              no (-DKPROF_DISABLE)\n");
    n += ksnprintf(buf + n, max - n, "KPROF_INTEGRITY n/a taken=0 accounted=0\n");
    return n;
#else
    struct kprof_stats st;
    kprof_get_stats(&st);

    n += ksnprintf(buf + n, max - n, "built              yes\n");
    n += ksnprintf(buf + n, max - n, "state              %s\n",
                   g_kprof_sampling ? "sampling" : "stopped");
    n += ksnprintf(buf + n, max - n, "span_recording     %s\n",
                   g_kprof_spans ? "on" : "off");
    n += ksnprintf(buf + n, max - n, "run_ns             %llu\n",
                   (unsigned long long)st.run_ns);
    n += ksnprintf(buf + n, max - n, "samples            %llu\n",
                   (unsigned long long)st.samples);
    n += ksnprintf(buf + n, max - n, "samples_kernel     %llu\n",
                   (unsigned long long)st.kernel_samples);
    n += ksnprintf(buf + n, max - n, "samples_user       %llu\n",
                   (unsigned long long)st.user_samples);
    n += ksnprintf(buf + n, max - n, "recorded           %llu\n",
                   (unsigned long long)st.recorded);
    n += ksnprintf(buf + n, max - n, "overflow           %llu\n",
                   (unsigned long long)st.overflow);
    n += ksnprintf(buf + n, max - n, "distinct_rips      %u\n", st.distinct);
    n += ksnprintf(buf + n, max - n, "cores_sampled      %x\n", st.cores);
    n += ksnprintf(buf + n, max - n, "span_events        %llu\n",
                   (unsigned long long)st.spans);
    /* MEASURED, not requested. Nothing in this kernel knows the LAPIC timer's
     * input frequency a priori, so the honest thing to publish is samples over
     * the interval they actually arrived in. */
    n += ksnprintf(buf + n, max - n, "sample_hz_measured %u\n",
                   st.run_ns ? (unsigned)((st.samples * 1000000000ull) / st.run_ns) : 0);
#ifndef LOGIT_KPROF_HOST
    /* The arming state. `start` returning 0 while the timer was never actually
     * programmed is the failure this profiler has really had (see
     * arm_request()), and it is invisible without these. */
    n += ksnprintf(buf + n, max - n,
                   "timer_arm          count=%u armed=%llu disarmed=%llu "
                   "deferred_fired=%llu pending=%d\n",
                   (unsigned)g_lapic_count, (unsigned long long)g_arm_armed,
                   (unsigned long long)g_arm_disarmed,
                   (unsigned long long)g_arm_fired, g_arm_request);
#endif
    /* THE INTEGRITY LINE. Every sample the interrupt handler counted must show
     * up in the histogram or in a loss counter. A mismatch means the
     * accumulator lost or tore a write, and every distribution below it is
     * wrong by an unknown amount -- so it is printed at the top rather than
     * buried, and the harness greps exactly this. */
    unsigned long long acc = (unsigned long long)(st.recorded + st.overflow + st.lost_ring);
    n += ksnprintf(buf + n, max - n, "KPROF_INTEGRITY %s taken=%llu accounted=%llu\n",
                   acc == (unsigned long long)st.samples ? "ok" : "TORN",
                   (unsigned long long)st.samples, acc);

    int ns = __atomic_load_n(&g_nspan, __ATOMIC_ACQUIRE);
    int any = 0;
    for (int i = 0; i < ns; i++) if (g_span[i].count) { any = 1; break; }
    if (any) {
        n += ksnprintf(buf + n, max - n,
                       "\n# span                     count         total_ns"
                       "           max_ns           avg_ns\n");
        for (int i = 0; i < ns && n < max - 128; i++) {
            if (!g_span[i].count) continue;
            n += ksnprintf(buf + n, max - n, "span %-20s %8llu %16llu %16llu %16llu\n",
                           g_span[i].name,
                           (unsigned long long)g_span[i].count,
                           (unsigned long long)g_span[i].total_ns,
                           (unsigned long long)g_span[i].max_ns,
                           (unsigned long long)(g_span[i].total_ns / g_span[i].count));
        }
    }

    if (st.recorded) {
        n += ksnprintf(buf + n, max - n,
                       "\n# rip                 hits   share ring\n");
        n += render_top(buf + n, max - n, g_kbucket, KPROF_NBUCKET, "k", 48, st.recorded);
        n += render_top(buf + n, max - n, g_ubucket, KPROF_UBUCKET, "u", 16, st.recorded);
    }
    return n;
#endif /* KPROF_DISABLE */
}

int kprof_summary(char *buf, int max)
{
    struct kprof_stats st;
    kprof_get_stats(&st);
    return ksnprintf(buf, max,
                     "kprof              %s samples=%llu spans=%llu overflow=%llu\n",
                     g_kprof_sampling ? "sampling" : "stopped",
                     (unsigned long long)st.samples,
                     (unsigned long long)st.spans,
                     (unsigned long long)st.overflow);
}

#ifndef LOGIT_KPROF_HOST
/* ============================================================================
 * 4. The sample interrupt
 *
 * WHICH TIMER, AND WHY NOT A FASTER ONE.
 *
 * The candidates, and what each one costs:
 *
 *   PIT channel 0 (the 100 Hz system tick). Reprogramming it changes the
 *     meaning of a tick, which is the exact failure ktime.h was written after:
 *     a PIT running at twice its programmed rate went unnoticed for the life of
 *     the kernel because a duration expressed in ticks has no unit a test can
 *     check. Rejected outright.
 *
 *   ktimer (c/kernel/core/ktime.c). The right facility, but ktimer_run() is
 *     called from time_tick(), which is called from the 100 Hz PIT interrupt --
 *     so a ktimer cannot fire faster than 100 Hz whatever period it is given,
 *     and ktime.c deliberately SKIPS a periodic timer forward rather than
 *     firing a catch-up burst, so asking for 1 ms yields 100 Hz rather than ten
 *     bunched samples per tick. 100 Hz over a 3-second profile is 300 samples;
 *     the standard error on a 70% share at n=300 is 2.6 percentage points,
 *     which cannot distinguish 70/20/10 from 65/25/10.
 *
 *   The LAPIC timer. Per-core, and on the APs it is ALREADY the preemption
 *     tick -- c/kernel/cpu/smp.c arms it at vector 32, staggered per core, and
 *     the scheduler depends on it. Reprogramming an AP's LAPIC timer would stop
 *     that core being preempted, which is disturbing the scheduler. But the
 *     BSP's LAPIC timer is armed by NOTHING: the BSP is preempted by the PIT
 *     through the legacy PIC. So the BSP has a free high-resolution per-core
 *     timer, and that is what this uses.
 *
 * THE ARMING IS DONE FROM A ktimer CALLBACK, not from the caller of
 * kprof_start(). A write to /dev/kprof is a syscall, and a syscall runs on
 * whichever core the scheduler put the writing process on; arming the LAPIC
 * timer from there would, one time in four, reprogram an AP's preemption timer,
 * and the machine would keep running just long enough for the damage to look
 * like something else. time_tick() is called only from the BSP's tick
 * (interrupts.c gates it on cpu index 0), so a ktimer callback is on the BSP by
 * construction and there is no core to get wrong.
 *
 * COVERING THE OTHER CORES. The BSP's sample interrupt fans an IPI at the same
 * vector to every other online core, so all cores are sampled at the SAME rate.
 * Uniformity matters more than the rate: a profile that samples one core ten
 * times as often as the others reports where the SAMPLER was, not where the
 * machine was.
 *
 * WHAT THIS CANNOT SEE, stated because a profiler's blind spot is the one thing
 * its user must know: the vector is an ordinary interrupt, not an NMI, so a
 * region running with IF=0 defers its sample until interrupts are re-enabled.
 * Long IF=0 windows are therefore under-attributed and the code just after them
 * over-attributed. Fixing that needs an NMI sampler that is safe against every
 * IF=0 critical section in the tree -- a much larger change, and one that
 * should be made for a measured reason rather than for tidiness.
 * ==========================================================================*/

/* Default LAPIC initial count (divide-by-16). NOT a frequency: nothing in this
 * kernel knows the LAPIC's input clock, so the report publishes the rate that
 * was actually ACHIEVED rather than one derived from an assumption, and this
 * number is chosen from measurements rather than from a round figure.
 *
 * Measured on QEMU/TCG (`echo overhead > /dev/kprof`), cost as a percentage of
 * the same 400 ms integer workload:
 *
 *     count     rate/core     -smp 1      -smp 4
 *      20000      ~3000 Hz      8.1%       47.5%
 *      60000      ~1000 Hz      3.1%       ~16%
 *     200000       ~300 Hz      0.1%        ~5%
 *
 * The -smp 4 column is dominated by the IPI fan-out, not by the sample itself:
 * lapic_send_ipi() polls the delivery-status bit, and three of those per sample
 * costs about 75 us under TCG against about 25 us for the interrupt, the
 * fxsave and the accumulator together.
 *
 * 60000 is the default because 1 kHz per core is already far more resolution
 * than any question being asked here needs -- a four-second profile is 4000
 * samples on a busy core, and the standard error on a 70% share at n=4000 is
 * 0.7 percentage points -- while 3% is an overhead you can leave on. Ask for
 * more with `echo "start 20000" > /dev/kprof`; the report always states the
 * rate it actually got. */
#define KPROF_DEFAULT_COUNT 60000u

static struct ktimer g_arm_timer;
static int g_gate_installed;

/* Called from the naked stub below with a pointer to the CPU's interrupt frame:
 * frame[0] = RIP, [1] = CS, [2] = RFLAGS, [3] = RSP, [4] = SS. */
void kprof_sample_isr(uint64_t *frame);
void kprof_sample_isr(uint64_t *frame)
{
    struct cpu *c = this_cpu();
    int cpu = c ? c->index : 0;
    uint64_t rip = frame[0];
    /* Ring is the low two bits of the SAVED CS: ring 3 addresses belong to
     * whichever process was running, ring 0 to the kernel image. */
    int user = (frame[1] & 3u) == 3u;

    if (g_kprof_sampling)
        record_sample(rip, user, cpu);

    /* Fan out from the BSP only. Every core IPI-ing every other core would be
     * quadratic and, worse, each core's samples would arrive in bursts
     * correlated with its neighbours'. */
    if (cpu == 0 && g_kprof_sampling && lapic_ready()) {
        int n = smp_cpu_count();
        if (n > KPROF_MAXCPU) n = KPROF_MAXCPU;
        for (int i = 1; i < n; i++)
            lapic_send_ipi((uint8_t)g_cpus[i].lapic_id, KPROF_VEC);
    }
    lapic_eoi();
}

/* Raw interrupt entry, for the same reason kdiag's log probe has one: the
 * boot/isr.asm path takes the big kernel lock, and a sampler that waits for the
 * BKL can only ever observe cores that are NOT holding it -- precisely the code
 * you least want to be blind to.
 *
 * Alignment: the CPU aligns rsp to 16 before pushing the 40-byte interrupt
 * frame, so rsp = 8 (mod 16) here; 15 pushes (120 bytes) bring it to 0, and the
 * 528-byte fxsave area keeps it there -- which is both what fxsave requires and
 * what the SysV ABI wants at a call. The fxsave is NOT optional: the kernel is
 * built with -msse2, this gate bypasses isr_common's FXSAVE, and clobbering the
 * XMM state of a ring-3 app mid-computation would be a corruption introduced BY
 * the measurement. It is also the single biggest item in the per-sample cost,
 * which is why that cost is measured (`echo overhead > /dev/kprof`) instead of
 * assumed to be small. */
__attribute__((naked)) static void kprof_isr(void)
{
    __asm__ volatile (
        "push %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %rsi\n\tpush %rdi\n\t"
        "push %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\t"
        "push %rbx\n\tpush %rbp\n\tpush %r12\n\tpush %r13\n\tpush %r14\n\tpush %r15\n\t"
        "subq $528, %rsp\n\t"
        "fxsave (%rsp)\n\t"
        "leaq 648(%rsp), %rdi\n\t"      /* 528 fxsave + 120 pushes -> iret frame */
        "call kprof_sample_isr\n\t"
        "fxrstor (%rsp)\n\t"
        "addq $528, %rsp\n\t"
        "pop %r15\n\tpop %r14\n\tpop %r13\n\tpop %r12\n\tpop %rbp\n\tpop %rbx\n\t"
        "pop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\t"
        "pop %rdi\n\tpop %rsi\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\t"
        "iretq\n\t");
}

/* Runs in interrupt context on the BSP, and ONLY while a start or stop is
 * pending -- it cancels itself as soon as it has served the request, so a
 * machine that is not profiling runs no kprof code at all. */

/* Program (or stop) THIS core's LAPIC timer. Only ever called once the caller
 * has established that this core is the BSP -- an AP's LAPIC timer is the
 * scheduler's preemption tick and is not ours to touch. A write of 0 to the
 * initial count is what stops it (SDM vol 3, "Local APIC Timer"). */
static void arm_here(int req)
{
    if (req > 0)      { lapic_timer_init(KPROF_VEC, g_lapic_count); g_arm_armed++; }
    else if (req < 0) { lapic_timer_init(KPROF_VEC, 0); g_arm_disarmed++; }
}

static void arm_tick(struct ktimer *t)
{
    int req = g_arm_request;
    g_arm_request = 0;
    g_arm_fired++;
    arm_here(req);
    ktimer_cancel(t);
}

/* Arm now if we are on the BSP; otherwise leave a request for the BSP tick.
 *
 * The direct path is not an optimisation, it is a correctness fix found by
 * measurement, and the reason is a property of this kernel that every user of
 * this profiler has to know:
 *
 *   A write to /dev/kprof is a syscall, and a syscall holds the big kernel
 *   lock. If it lands on an application processor and runs for a while, the BSP
 *   ends up spinning for the BKL inside interrupt_handler -- and that spin is
 *   spin_lock_irqsave, i.e. IF=0. A core with IF=0 takes no interrupts at all,
 *   so the PIT tick stops, ktimers stop, and the arming request is never
 *   served. Observed directly: a four-second profile with fired=0 and not one
 *   sample, on the runs where the shell happened to be on an AP.
 *
 * Arming here fixes the request never being served when we ARE the BSP. It does
 * not -- and from these files cannot -- fix the deeper consequence: while one
 * core holds the BKL for a long time, NO core is sampling, because none of them
 * is taking interrupts. tests/boot/run-prof-test.sh is structured around that
 * rather than pretending it away. */
static void arm_request(int req)
{
    struct cpu *c = this_cpu();
    if (c && c->index == 0) {
        g_arm_request = 0;
        arm_here(req);
        return;
    }
    g_arm_request = req;
    ktimer_add(&g_arm_timer, 5 * NS_PER_MS, 5 * NS_PER_MS,
               arm_tick, 0, "kprof-arm");
}

void kprof_init(void)
{
    if (g_gate_installed) return;
    g_lapic_count = KPROF_DEFAULT_COUNT;
    idt_install_gate(KPROF_VEC, (void *)kprof_isr);
    g_gate_installed = 1;
}

int kprof_start(uint32_t lapic_count)
{
    if (!g_gate_installed) return -1;
    if (!lapic_ready())   return -2;
    kprof_reset();
    if (lapic_count) g_lapic_count = lapic_count;
    g_run_t0 = time_mono_raw_ns();
    g_kprof_spans = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_kprof_sampling = 1;
    arm_request(1);
    return 0;
}

void kprof_stop(void)
{
    uint64_t now = time_mono_raw_ns();
    g_run_ns = (g_run_t0 && now > g_run_t0) ? now - g_run_t0 : 0;
    g_kprof_sampling = 0;
    g_kprof_spans = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Samples that arrive before the BSP tick gets round to masking the timer
     * are dropped by the g_kprof_sampling test in the ISR, so they cannot
     * corrupt the profile -- the disarm is about cost, not correctness. */
    arm_request(-1);
}

/* ============================================================================
 * 5. The self-tests
 *
 * A profiler that has never been checked against a known answer is a random
 * number generator with good formatting. So there is a workload here whose
 * distribution is fixed by construction, measured independently by the span
 * clock, and required to come back out of the sampler.
 * ==========================================================================*/

static volatile uint64_t g_burn_sink;

/* Three functions with identical bodies and different constants: identical cost
 * per iteration, so the TIME split is exactly the iteration split. They must not
 * be inlined -- the whole point is that the profiler has to tell them apart by
 * address -- and the extents are marked with global labels rather than inferred
 * from the function symbols. clang emits functions in source order today, but
 * nothing guarantees it, and "the three symbols are contiguous" is exactly the
 * kind of assumption that turns into a wrong measurement nobody questions. */
#define BURN_BODY(lbl, seed)                                                  \
    __asm__ volatile (".globl " lbl "_lo\n" lbl "_lo:" ::: "memory");         \
    uint64_t x = (seed);                                                      \
    for (uint64_t i = 0; i < iters; i++)                                      \
        x = x * 6364136223846793005ull + 1442695040888963407ull;              \
    g_burn_sink += x;                                                         \
    __asm__ volatile (".globl " lbl "_hi\n" lbl "_hi:" ::: "memory")

__attribute__((noinline)) static void burn_a(uint64_t iters)
{ BURN_BODY("kprof_burn_a", 0x1111111111111111ull); }
__attribute__((noinline)) static void burn_b(uint64_t iters)
{ BURN_BODY("kprof_burn_b", 0x2222222222222222ull); }
__attribute__((noinline)) static void burn_c(uint64_t iters)
{ BURN_BODY("kprof_burn_c", 0x3333333333333333ull); }

extern char kprof_burn_a_lo[], kprof_burn_a_hi[];
extern char kprof_burn_b_lo[], kprof_burn_b_hi[];
extern char kprof_burn_c_lo[], kprof_burn_c_hi[];

/* d.ddd from a value scaled by 1000: this kernel prints no floats. */
static void milli(uint64_t v, char *b)
{
    uint64_t w = v / 1000, f = v % 1000;
    char t[24]; int n = 0, j = 0;
    if (!w) t[n++] = '0';
    while (w) { t[n++] = (char)('0' + w % 10); w /= 10; }
    while (n) b[j++] = t[--n];
    b[j++] = '.';
    b[j++] = (char)('0' + f / 100);
    b[j++] = (char)('0' + (f / 10) % 10);
    b[j++] = (char)('0' + f % 10);
    b[j] = 0;
}

/* How many iterations of burn_a take about `want_ns`? MEASURED, because the
 * answer differs by orders of magnitude between TCG and hardware and a
 * hardcoded count makes the self-test either meaningless or endless. */
static uint64_t calibrate_iters(uint64_t want_ns)
{
    uint64_t iters = 20000;
    for (int i = 0; i < 16; i++) {
        uint64_t t0 = time_mono_raw_ns();
        burn_a(iters);
        uint64_t dt = time_mono_raw_ns() - t0;
        if (dt >= want_ns / 4 && dt <= want_ns * 4)
            return dt ? (iters * want_ns) / dt : iters;
        if (!dt) { iters *= 8; continue; }
        /* Growth is bounded PER STEP rather than absolutely. An absolute cap is
         * the obvious thing and it is wrong: the first version capped at 2e8,
         * clang unrolls this loop eight ways into a single fused multiply-add,
         * and 2e8 turned out to be 23 ms -- so a request for 700 ms silently
         * measured 23 ms instead, and the overhead figure was computed over a
         * workload thirty times shorter than the one it named. The ceiling here
         * exists only so a broken clock cannot ask for an unbounded loop. */
        uint64_t next = (iters * want_ns) / dt;
        if (next > iters * 64) next = iters * 64;
        if (next < 1000) next = 1000;
        if (next > 40000000000ull) next = 40000000000ull;
        iters = next;
    }
    return iters;
}

/* Wait `ns` with IF=1 so interrupts can actually be delivered. */
static void spin_ns(uint64_t ns)
{
    uint64_t t0 = time_mono_raw_ns();
    while (time_mono_raw_ns() - t0 < ns)
        __asm__ volatile ("pause");
}

/* The known-answer test. Nominal split 70/20/10 by iteration count; the ground
 * truth is the span clock, because iteration counts prove nothing about elapsed
 * time on a preemptible machine. Sampling must reproduce the ground truth. */
static void do_selftest(long ms)
{
    if (ms <= 0)    ms = 4000;
    if (ms > 30000) ms = 30000;

    /* Slice the work finely and interleave the three phases. Three long phases
     * would let a periodic sampler ALIAS with the workload's period and return
     * a confident wrong split; ~1 ms slices decorrelate them. */
    uint64_t unit = calibrate_iters(NS_PER_MS);
    if (unit < 100) unit = 100;
    /* WHICH CORE this ran on is not a debug aid, it is part of the result. A
     * long-running syscall holds the BKL, and on any core but the BSP that
     * starves every other core of interrupts -- so a selftest that lands on an
     * AP cannot be sampled at all, and the report has to say so rather than
     * look like a broken sampler. See arm_request() above. */
    struct cpu *dc = this_cpu();
    int run_cpu = dc ? dc->index : -1;

    if (kprof_start(0) < 0) {
        klog(KL_ERR, "KPROF_SELFTEST FAIL cannot start (LAPIC not ready?)");
        return;
    }

    /* IF=1 is mandatory: int 0x80 is an interrupt gate, and neither the LAPIC
     * timer nor an IPI can be delivered to a core with interrupts masked. Safe
     * for the same reason SYS_HTTP_GET's sti window and kdiag's irqstorm are:
     * interrupts.c keys re-entrancy off g_bkl_owner, so a nested IRQ on a core
     * already holding the BKL neither re-acquires it nor re-enters the
     * scheduler. */
    __asm__ volatile ("sti");
    /* Settle: arming is immediate on the BSP, but off it the request has to
     * wait for a BSP tick, and starting the workload before the timer is live
     * measures the split over less work than it appears to. */
    spin_ns(20 * NS_PER_MS);

    uint64_t t_end = time_mono_raw_ns() + (uint64_t)ms * NS_PER_MS;
    long rounds = 0;
    while (time_mono_raw_ns() < t_end) {
        { KPROF_BEGIN(burn70); burn_a(unit * 7); KPROF_END(burn70); }
        { KPROF_BEGIN(burn20); burn_b(unit * 2); KPROF_END(burn20); }
        { KPROF_BEGIN(burn10); burn_c(unit * 1); KPROF_END(burn10); }
        rounds++;
    }
    kprof_stop();
    __asm__ volatile ("cli");

    uint64_t sa = kprof_hits_in((uint64_t)(uintptr_t)kprof_burn_a_lo,
                                (uint64_t)(uintptr_t)kprof_burn_a_hi);
    uint64_t sb = kprof_hits_in((uint64_t)(uintptr_t)kprof_burn_b_lo,
                                (uint64_t)(uintptr_t)kprof_burn_b_hi);
    uint64_t sc = kprof_hits_in((uint64_t)(uintptr_t)kprof_burn_c_lo,
                                (uint64_t)(uintptr_t)kprof_burn_c_hi);
    uint64_t stot = sa + sb + sc;

    uint64_t na = 0, nb = 0, nc = 0, dummy = 0;
    kprof_span_get("burn70", &dummy, &na, &dummy);
    kprof_span_get("burn20", &dummy, &nb, &dummy);
    kprof_span_get("burn10", &dummy, &nc, &dummy);
    uint64_t ntot = na + nb + nc;

    struct kprof_stats st;
    kprof_get_stats(&st);

    if (!stot || !ntot) {
        klog(KL_ERR, "KPROF_SELFTEST FAIL no samples landed in the workload "
             "(samples=%llu span_ns=%llu rounds=%ld cpu=%d armed=%llu %s)",
             (unsigned long long)st.samples, (unsigned long long)ntot, rounds,
             run_cpu, (unsigned long long)g_arm_armed,
             run_cpu == 0 ? "on the BSP -- this is a real failure"
                          : "on an AP: the BKL starved every core of "
                            "interrupts, so nothing could be sampled");
        return;
    }

    unsigned pa = (unsigned)((sa * 1000ull) / stot), qa = (unsigned)((na * 1000ull) / ntot);
    unsigned pb = (unsigned)((sb * 1000ull) / stot), qb = (unsigned)((nb * 1000ull) / ntot);
    unsigned pc = (unsigned)((sc * 1000ull) / stot), qc = (unsigned)((nc * 1000ull) / ntot);

    long ea = (long)pa - (long)qa, eb = (long)pb - (long)qb, ec = (long)pc - (long)qc;
    if (ea < 0) ea = -ea;
    if (eb < 0) eb = -eb;
    if (ec < 0) ec = -ec;
    long worst = ea > eb ? ea : eb;
    if (ec > worst) worst = ec;

    /* THE TOLERANCE, and why it is this wide rather than tighter.
     *
     * A sampling profiler is a binomial estimator: with n samples inside the
     * workload the standard error on a share p is sqrt(p(1-p)/n). At n = 2000
     * and p = 0.7 that is 1.0 percentage point, so three sigma is 3 points. The
     * bound below is 60 permille = 6 points, three sigma at n = 500 -- the
     * count a 4-second profile yields if the achieved rate is only a few
     * hundred Hz. The sample count is printed alongside, so a run that passes
     * with n = 300 is visibly weaker evidence than one that passes with
     * n = 4000, rather than both simply saying "ok". */
    long tol = 60;
    int ok = worst <= tol &&
             st.recorded + st.overflow + st.lost_ring == st.samples;

    char a1[24], a2[24], a3[24], b1[24], b2[24], b3[24];
    milli(pa, a1); milli(pb, a2); milli(pc, a3);
    milli(qa, b1); milli(qb, b2); milli(qc, b3);
    klog(ok ? KL_INFO : KL_ERR,
         "KPROF_SELFTEST %s sampled=%s/%s/%s%% clock=%s/%s/%s%% "
         "worst_err=%ld.%ldpp tol=%ld.%ldpp",
         ok ? "ok" : "FAIL", a1, a2, a3, b1, b2, b3,
         worst / 10, worst % 10, tol / 10, tol % 10);
    klog(KL_INFO, "KPROF_SELFTEST_N hits=%llu/%llu/%llu inwork=%llu samples=%llu "
         "rounds=%ld unit=%llu hz=%u cores=%x cpu=%d",
         (unsigned long long)sa, (unsigned long long)sb, (unsigned long long)sc,
         (unsigned long long)stot, (unsigned long long)st.samples, rounds,
         (unsigned long long)unit,
         st.run_ns ? (unsigned)((st.samples * 1000000000ull) / st.run_ns) : 0,
         st.cores, run_cpu);

    /* The nominal split, checked separately: if the workload itself did not come
     * out 70/20/10 on the clock, agreement between sampler and clock says
     * nothing about the sampler being right about a KNOWN distribution. */
    long dn = (long)qa - 700; if (dn < 0) dn = -dn;
    long dm = (long)qb - 200; if (dm < 0) dm = -dm;
    long dk = (long)qc - 100; if (dk < 0) dk = -dk;
    long wn = dn > dm ? dn : dm; if (dk > wn) wn = dk;
    klog(wn <= 50 ? KL_INFO : KL_ERR,
         "KPROF_NOMINAL %s clock split vs the constructed 70/20/10: "
         "worst_err=%ld.%ldpp", wn <= 50 ? "ok" : "FAIL", wn / 10, wn % 10);
}

/* --- what the profiler costs ---------------------------------------------
 * The same benchmark with profiling off and on. This number bounds every result
 * the profiler will ever produce, so it is measured on the machine, on every
 * run of the harness, and printed whether it flatters the profiler or not. */
static void do_overhead(long ms)
{
    if (ms <= 0)   ms = 700;
    if (ms > 5000) ms = 5000;

    __asm__ volatile ("sti");
    uint64_t iters = calibrate_iters((uint64_t)ms * NS_PER_MS);

    kprof_stop();
    kprof_reset();
    g_kprof_spans = 0;
    g_kprof_sampling = 0;
    spin_ns(20 * NS_PER_MS);            /* let the disarm land */

    /* (1) OFF. A short warm pass first, discarded: charging cold caches to
     * "profiling off" would flatter the on/off ratio. */
    burn_a(iters / 8);
    uint64_t t0 = time_mono_raw_ns();
    burn_a(iters);
    uint64_t off_ns = time_mono_raw_ns() - t0;

    /* (2) SAMPLING ON, identical work. The sample count is taken across the
     * BURN ONLY -- bracketing the whole start/stop would include the samples
     * that arrived during the 40 ms arming wait and divide the extra time by
     * roughly twice as many samples as caused it. */
    struct kprof_stats st, pre;
    kprof_start(0);
    spin_ns(20 * NS_PER_MS);
    kprof_get_stats(&pre);
    uint64_t s0 = time_mono_raw_ns();
    burn_a(iters);
    uint64_t on_ns = time_mono_raw_ns() - s0;
    kprof_get_stats(&st);
    st.samples -= pre.samples;
    kprof_stop();
    spin_ns(20 * NS_PER_MS);

    /* (3) SPANS: enabled and disabled, the same loop and the same count, plus a
     * bare loop so the disabled figure can be stated as a DIFFERENCE rather
     * than as the cost of the loop that carries it. */
    const long NSPAN = 200000;
    g_kprof_spans = 0;
    uint64_t d0 = time_mono_raw_ns();
    for (long i = 0; i < NSPAN; i++) {
        KPROF_BEGIN(ovh);
        g_burn_sink += (uint64_t)i;
        KPROF_END(ovh);
    }
    uint64_t span_off = time_mono_raw_ns() - d0;

    g_kprof_spans = 1;
    uint64_t e0 = time_mono_raw_ns();
    for (long i = 0; i < NSPAN; i++) {
        KPROF_BEGIN(ovh);
        g_burn_sink += (uint64_t)i;
        KPROF_END(ovh);
    }
    uint64_t span_on = time_mono_raw_ns() - e0;
    g_kprof_spans = 0;

    uint64_t f0 = time_mono_raw_ns();
    for (long i = 0; i < NSPAN; i++)
        g_burn_sink += (uint64_t)i;
    uint64_t bare = time_mono_raw_ns() - f0;

    __asm__ volatile ("cli");

    uint64_t ppt = off_ns ? ((on_ns > off_ns ? on_ns - off_ns : 0) * 1000ull) / off_ns : 0;
    uint64_t per_sample = st.samples ? (on_ns > off_ns ? (on_ns - off_ns) / st.samples : 0) : 0;
    long dis = (long)((span_off > bare ? span_off - bare : 0) / (uint64_t)NSPAN);
    long ena = (long)((span_on  > bare ? span_on  - bare : 0) / (uint64_t)NSPAN);

    /* ppt is parts per thousand of the OFF time, so the percentage is ppt/10.
     * Printed as a percentage AND as the raw ppt, because "0.556" read as a
     * percentage when it meant 55.6% is exactly the class of mistake this
     * whole facility exists to stop people making. */
    klog(KL_INFO, "KPROF_OVERHEAD sampling off=%lluus on=%lluus cost=%llu.%llu%% "
         "(%lluppt) samples=%llu per_sample=%lluns hz=%u",
         (unsigned long long)(off_ns / 1000), (unsigned long long)(on_ns / 1000),
         (unsigned long long)(ppt / 10), (unsigned long long)(ppt % 10),
         (unsigned long long)ppt,
         (unsigned long long)st.samples, (unsigned long long)per_sample,
         st.run_ns ? (unsigned)((st.samples * 1000000000ull) / st.run_ns) : 0);
    /* The raw totals are printed as well as the per-pair figures, because
     * "+0 ns/pair" on its own is indistinguishable from "we did not measure
     * anything": with the totals visible, a reader can see that the disabled
     * difference is below the resolution of a loop this long, which is the
     * actual claim. */
    klog(KL_INFO, "KPROF_SPANCOST n=%ld bare=%lluus off=%lluus on=%lluus "
         "-> disabled=+%ldns/pair enabled=+%ldns/pair",
         NSPAN, (unsigned long long)(bare / 1000),
         (unsigned long long)(span_off / 1000), (unsigned long long)(span_on / 1000),
         dis, ena);
    kprof_reset();
}

/* --- the SMP integrity verdict --------------------------------------------
 * Samples arrive in interrupt context on several cores at once, and the shared
 * accumulator must not tear. The witness is the per-CPU counter the handler
 * keeps for itself: exactly one writer, so it cannot be wrong, and it must
 * equal what comes back out of the shared histogram.
 *
 * This verb only JUDGES; it does not generate the load, and that is a finding
 * rather than laziness. The first version ran a busy loop here, inside the
 * write syscall -- which holds the big kernel lock, so every other core was
 * parked in spin_lock_irqsave() with IF=0 and could not take the sample IPI at
 * all. It reported cores=1 on a four-core machine and looked like a bug in the
 * fan-out. The load that actually exercises four cores is the SYSTEM running
 * normally, so the harness does:
 *
 *     echo start > /dev/kprof ; sleep 5 ; echo stop > /dev/kprof
 *     echo smpcheck > /dev/kprof
 *
 * (The IF=0 blindness is real and permanent for a non-NMI sampler; see the note
 * at the top of section 4. It is why this verdict reports the core MASK: a
 * profile that only ever saw one core is a profile you should not trust to be
 * about the machine.) */
static void do_smpcheck(long unused)
{
    (void)unused;
    struct kprof_stats st;
    kprof_get_stats(&st);
    uint64_t acc = st.recorded + st.overflow + st.lost_ring;
    int ncores = 0;
    for (unsigned m = st.cores; m; m >>= 1) ncores += (int)(m & 1u);
    int ok = (acc == st.samples) && st.samples > 0;
    klog(ok ? KL_INFO : KL_ERR,
         "KPROF_SMP %s cores=%d mask=%x taken=%llu recorded=%llu overflow=%llu "
         "accounted=%llu distinct=%u",
         ok ? "ok" : "FAIL", ncores, st.cores,
         (unsigned long long)st.samples, (unsigned long long)st.recorded,
         (unsigned long long)st.overflow, (unsigned long long)acc, st.distinct);
}
#endif /* !LOGIT_KPROF_HOST */

/* ============================================================================
 * 6. /dev/kprof's write verbs
 * ==========================================================================*/

static long parse_long(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    long v = 0; int any = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    *pp = p;
    return any ? v : -1;
}

static int word_is(const char *b, const char *e, const char *w, const char **rest)
{
    const char *p = b;
    while (*w && p < e && *p == *w) { p++; w++; }
    if (*w) return 0;
    if (p < e && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\0') return 0;
    *rest = p;
    return 1;
}

int kprof_command(const char *b, const char *e)
{
    const char *rest = b;
    while (b < e && (*b == ' ' || *b == '\n' || *b == '\t')) b++;

    if (word_is(b, e, "start", &rest)) {
        long c = parse_long(&rest, e);
        int rc = kprof_start(c > 0 ? (uint32_t)c : 0);
        klog(KL_INFO, "KPROF_START rc=%d count=%ld", rc, c);
    } else if (word_is(b, e, "stop", &rest)) {
        kprof_stop();
        struct kprof_stats st;
        kprof_get_stats(&st);
        klog(KL_INFO, "KPROF_STOP samples=%llu recorded=%llu run_ms=%llu",
             (unsigned long long)st.samples, (unsigned long long)st.recorded,
             (unsigned long long)(st.run_ns / NS_PER_MS));
    } else if (word_is(b, e, "reset", &rest)) {
        kprof_reset();
        klog(KL_INFO, "KPROF_RESET done");
    } else if (word_is(b, e, "spans", &rest)) {
        long v = parse_long(&rest, e);
        g_kprof_spans = v > 0;
        klog(KL_INFO, "KPROF_SPANS %d", g_kprof_spans);
    } else if (word_is(b, e, "span", &rest)) {
        /* The ring-3 entry point: an app times a phase with clock_gettime and
         * posts the duration. One write per span, which at page-load
         * granularity (tens of spans) costs nothing worth measuring -- and it
         * needs no new syscall number, for the reason kdiag.h gives about
         * include/abi/logit_abi.h being the most contended header in this tree. */
        while (rest < e && (*rest == ' ' || *rest == '\t')) rest++;
        char nm[KPROF_NAMELEN];
        int i = 0;
        while (rest < e && *rest != ' ' && *rest != '\t' && *rest != '\n' &&
               i < KPROF_NAMELEN - 1)
            nm[i++] = *rest++;
        nm[i] = 0;
        long ns = parse_long(&rest, e);
        if (!i || ns < 0)
            klog(KL_WARN, "KPROF_SPAN usage: span <name> <nanoseconds>");
        else
            kprof_span_add(kprof_slot(nm), (uint64_t)ns);
#ifndef LOGIT_KPROF_HOST
    } else if (word_is(b, e, "selftest", &rest)) {
        do_selftest(parse_long(&rest, e));
    } else if (word_is(b, e, "overhead", &rest)) {
        do_overhead(parse_long(&rest, e));
    } else if (word_is(b, e, "smpcheck", &rest)) {
        do_smpcheck(parse_long(&rest, e));
#endif
    } else {
        klog(KL_WARN, "KPROF unknown command (start [count]|stop|reset|"
             "spans 0|1|span NAME NS|selftest [ms]|overhead [ms]|smpcheck [ms])");
    }
    return 0;
}

#ifdef LOGIT_KPROF_HOST
/* Host-only: also drops the interned NAMES, which kprof_reset() deliberately
 * keeps (a slot number cached in a KPROF_BEGIN static must survive a reset on
 * the machine). Tests want each case to start from nothing. */
void kprof_host_reset(void)
{
    kprof_reset();
    uint64_t f = spin_lock_irqsave(&g_span_lock);
    for (int i = 0; i < KPROF_NSLOT; i++) g_span[i].name[0] = 0;
    __atomic_store_n(&g_nspan, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_span_lock, f);
}
void kprof_init(void) {}
int  kprof_start(uint32_t c) { (void)c; g_kprof_sampling = 1; g_kprof_spans = 1; return 0; }
void kprof_stop(void) { g_kprof_sampling = 0; g_kprof_spans = 0; }
#endif
