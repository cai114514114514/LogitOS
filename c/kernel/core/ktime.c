/* ============================================================================
 * LogitOS time subsystem.
 *
 * Read time.h first: it has the model. This file has the machinery, in the
 * order a reader needs it:
 *
 *   1. cycle counters       rdtsc, CPUID, the PIT tick, and the wrap-safe
 *                           signed delta that both share
 *   2. cycles -> ns         a Q<shift> multiply done in 128 bits
 *   3. calibration          TSC measured against PIT channel 2, then
 *                           cross-checked on device against the RTC
 *   4. the clocks           monotonic (seqlock + cross-core clamp), realtime
 *   5. timers               a binary min-heap
 *   6. CPU accounting       per-pid user/sys nanoseconds
 *   7. the tick             fold, expire, account
 *   8. self-tests           what boots print, and why every one of them runs
 *                           on every boot rather than only in a test harness
 *
 * The whole file is host-compilable (-DLOGIT_TIME_HOST) with a settable fake
 * cycle counter, which is how the heap, the ns arithmetic, the counter wrap and
 * the 2x-tick negative control are unit-tested without QEMU.
 * ==========================================================================*/

#include <stdint.h>
#include "ktime.h"

#ifdef LOGIT_TIME_HOST
/* ---- host shims ---------------------------------------------------------
 * Everything the kernel provides, faked, so tests/unit/time_test.c can drive
 * the REAL code paths rather than a copy of them. */
#include <stdio.h>
typedef struct { int dummy; } spinlock_t;
#define SPINLOCK_INIT { 0 }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) { (void)l; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; }
#define kprintf printf
static uint64_t g_host_cycles = 0;
static uint64_t g_host_hz     = 1000000000ull;
static uint64_t g_host_mask   = ~0ull;
static int      g_host_rtc_s  = 0;
void time_host_set_cycles(uint64_t c) { g_host_cycles = c; }
void time_host_set_rtc_second(int s)  { g_host_rtc_s = s; }
static uint64_t host_read(void) { return g_host_cycles & g_host_mask; }
#define TIMER_HZ 100
__attribute__((unused)) static int rtc_second(void) { return g_host_rtc_s; }
__attribute__((unused)) static int64_t rtc_unix(void) { return 0; }
static int  proc_current_pid(void) { return -1; }
__attribute__((unused)) static int cpu_index(void) { return 0; }
__attribute__((unused)) static int smp_cpu_count(void) { return 1; }
static int  in_kernel_now(void) { return 0; }
#else
/* ---- kernel ------------------------------------------------------------- */
#include "spinlock.h"
#include "kprintf.h"
#include "io.h"
#include "pit.h"
#include "rtc.h"
#include "percpu.h"
#include "sched.h"
#include "smp.h"
#include "proc.h"

static int proc_current_pid(void)
{
    struct proc *p = proc_current();
    return p ? p->pid : -1;
}
static int cpu_index(void)     { struct cpu *c = this_cpu(); return c ? c->index : 0; }
static int in_kernel_now(void) { struct cpu *c = this_cpu(); return c ? c->in_kernel : 0; }
#endif

/* ============================================================================
 * 1. Cycle counters
 * ==========================================================================*/

#ifndef LOGIT_TIME_HOST
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    /* lfence, not mfence/cpuid: we need the TSC read not to float ABOVE earlier
     * loads (which would make a measured interval look shorter than it was), and
     * lfence is the documented, cheap way to get that on both vendors. A full
     * serialising cpuid here costs ~100 cycles and buys nothing -- the clock is
     * read on every syscall. */
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid"
                      : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}

/* Is the TSC usable as a CLOCK (as opposed to a cycle counter)?
 *
 * Three separate questions, and the kernel had never asked any of them:
 *   - does the CPU have a TSC at all           (CPUID.1:EDX bit 4)
 *   - does it tick at a CONSTANT rate          (CPUID.0x80000007:EDX bit 8,
 *     "invariant TSC") -- without this the counter changes speed with P-states
 *     and halts in deep C-states, so a calibrated frequency is a fiction
 *   - can we see leaf 0x80000007 at all        (max extended leaf)
 *
 * A hypervisor that does not advertise invariant TSC gets the PIT. That is a
 * real cost -- 10 ms resolution instead of ~1 ns -- and it is why the boot line
 * prints WHICH source won and why. */
static int tsc_usable(int *invariant, int *hypervisor)
{
    uint32_t r[4];
    *invariant = 0;
    *hypervisor = 0;
    cpuid(0, 0, r);
    if (r[0] < 1) return 0;
    cpuid(1, 0, r);
    if (!(r[3] & (1u << 4))) return 0;          /* no TSC */
    *hypervisor = (r[2] & (1u << 31)) != 0;     /* CPUID.1:ECX[31] */
    cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000007u) {
        uint32_t e[4];
        cpuid(0x80000007u, 0, e);
        *invariant = (e[3] & (1u << 8)) != 0;
    }
    /* THE POLICY, and it is a policy rather than "has a TSC".
     *
     * A TSC that is not invariant changes rate with P-states and stops in deep
     * C-states, so a frequency calibrated once at boot is a fiction, and the
     * clock would run fast or slow depending on how busy the machine is. That
     * is worse than 10 ms resolution: a wrong rate is a wrong duration
     * everywhere, which is the failure this whole subsystem was written after.
     *
     * The exception is a hypervisor. QEMU's TCG does not set the invariant bit
     * (verified: `-cpu max` reports invariant-tsc=no) but its TSC is derived
     * from a single stable host clock, so it is exactly the counter we want.
     * KVM and every other hypervisor here are the same. So: invariant, or
     * virtualised. Anything else takes the PIT, and the boot line says which
     * and why -- because on a machine where time is wrong, that line is the
     * whole diagnosis. */
    return (*invariant || *hypervisor);
}
#endif

/* A counter of `width` bits, read now and last folded then: how many cycles
 * passed? Masked subtraction handles the wrap; sign-extension from the counter
 * width separates "wrapped" (small positive) from "went backwards" (small
 * negative, which is what an unsynchronised second TSC looks like).
 *
 * Getting this wrong is not a rounding error. A backwards step of one
 * microsecond, read as an unsigned masked delta on a 64-bit counter, is
 * 18446744073708 microseconds -- 584 thousand years of instant uptime, and
 * every deadline in the machine expiring at once. */
static int64_t cyc_delta(uint64_t now, uint64_t last, uint64_t mask)
{
    uint64_t d = (now - last) & mask;
    uint64_t sign = (mask >> 1) + 1;            /* top bit of the counter width */
    if (d & sign) return (int64_t)(d | ~mask);  /* sign-extend: negative */
    return (int64_t)d;
}

/* ============================================================================
 * 2. cycles -> nanoseconds
 * ==========================================================================*/

struct clocksource {
    const char *name;
    uint64_t (*read)(void);     /* raw counter; monotonic within one core */
    uint64_t  mask;             /* counter width */
    uint64_t  hz;               /* calibrated frequency */
    uint32_t  mult;             /* ns = (cycles * mult) >> shift */
    uint32_t  shift;
    uint32_t  res_ns;           /* smallest step this source can report */
    int       available;
};

/* Pick the largest shift (<= 32) whose mult still fits in 32 bits. Bigger shift
 * = less rounding error per conversion; 32 bits of mult keeps the 128-bit
 * product provably below 2^96 for any 64-bit cycle count, so the multiply
 * cannot overflow no matter how stale the last fold is. */
static void calc_mult_shift(uint64_t hz, uint32_t *mult, uint32_t *shift)
{
    uint32_t sh = 32;
    if (hz == 0) hz = 1;
    for (; sh > 0; sh--) {
        unsigned __int128 m = ((unsigned __int128)NS_PER_SEC << sh) / hz;
        if (m == 0) continue;
        if (m <= 0xFFFFFFFFull) { *mult = (uint32_t)m; *shift = sh; return; }
    }
    /* hz < 1 ns of period is impossible; if we ever get here the source is
     * nonsense and 1:1 is the least harmful answer. */
    *mult = 1; *shift = 0;
}

static uint64_t ns_of_cycles(const struct clocksource *cs, uint64_t cycles)
{
    unsigned __int128 p = (unsigned __int128)cycles * cs->mult;
    p >>= cs->shift;
    if (p > (unsigned __int128)0xFFFFFFFFFFFFFFFFull) return 0xFFFFFFFFFFFFFFFFull;
    return (uint64_t)p;
}

/* ============================================================================
 * 3. Sources + calibration
 * ==========================================================================*/

#ifndef LOGIT_TIME_HOST
static uint64_t src_read_tsc(void) { return rdtsc(); }
static uint64_t src_read_pit(void) { return timer_ticks(); }
#else
static uint64_t src_read_host(void) { return host_read(); }
static uint64_t g_host_ticks = 0;
static uint64_t src_read_hpit(void) { return g_host_ticks; }
void time_host_set_ticks(uint64_t t) { g_host_ticks = t; }
#endif

static struct clocksource g_src[TIMESRC_N] = {
#ifndef LOGIT_TIME_HOST
    [TIMESRC_TSC] = { "tsc", src_read_tsc, ~0ull, 0, 1, 0, 1, 0 },
    [TIMESRC_PIT] = { "pit", src_read_pit, ~0ull, TIMER_HZ, 0, 0,
                      (uint32_t)(NS_PER_SEC / TIMER_HZ), 1 },
#else
    [TIMESRC_TSC] = { "tsc", src_read_host, ~0ull, 0, 1, 0, 1, 0 },
    [TIMESRC_PIT] = { "pit", src_read_hpit, ~0ull, TIMER_HZ, 0, 0,
                      (uint32_t)(NS_PER_SEC / TIMER_HZ), 1 },
#endif
};

static int g_cur = TIMESRC_PIT;         /* until calibration says otherwise */
static int g_tsc_invariant = 0;
static int g_tsc_hypervisor = 0;
static int g_tsc_calib_ok  = 0;
static uint64_t g_calib_samples[3];

#ifndef LOGIT_TIME_HOST
/* ---- TSC calibration against PIT channel 2 -------------------------------
 *
 * Channel 2 and not channel 0: channel 0 is the system tick and reprogramming
 * it here would be exactly the kind of change to the meaning of a tick this
 * subsystem exists to make impossible. Channel 2's gate is software-controlled
 * (port 0x61 bit 0) and its output is readable (bit 5), so it is a one-shot
 * stopwatch that needs no interrupt -- which matters, because pit_init() runs
 * before the IDT is armed and long before IF is ever set.
 *
 * The counter runs at 1193182 Hz. Three samples of 0x7FFF counts (27.5 ms
 * each), and the MEDIAN is taken -- not the mean, because one SMI or one host
 * scheduler preemption during a sample skews a mean and cannot move a median.
 *
 * The sample length is a boot-time budget: this is 82 ms of boot, measured
 * (1.63 s to LOGIT_BOOT_OK against 1.46 s with no calibration at all, at the
 * full 0xFFFF; halving it halves that). 27.5 ms is 94 million cycles at 3.4
 * GHz, so the few-hundred-cycle latency of the polling loop's exit is a
 * sub-ppm error -- the sample is limited by interference, which is what the
 * median is for, not by resolution. */
#define PIT_CAL_HZ    1193182u
#define PIT_CAL_COUNT 0x7FFFu

static uint64_t calibrate_tsc_once(void)
{
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02u) | 0x01u));   /* speaker off, gate ON */
    outb(0x43, 0xB0);                                /* ch2, lo/hi, mode 0 */
    outb(0x42, (uint8_t)(PIT_CAL_COUNT & 0xFF));
    outb(0x42, (uint8_t)(PIT_CAL_COUNT >> 8));
    /* Re-trigger: mode 0 starts counting on the gate's rising edge. */
    outb(0x61, (uint8_t)((p61 & ~0x02u) & ~0x01u));
    outb(0x61, (uint8_t)((p61 & ~0x02u) | 0x01u));

    uint64_t t0 = rdtsc();
    /* Bounded: a machine without a working channel 2 must not hang the boot. The
     * bound is in iterations rather than time because we have no clock yet --
     * that is the whole problem being solved. */
    long guard = 200000000L;
    while (!(inb(0x61) & 0x20) && --guard > 0)
        ;
    uint64_t t1 = rdtsc();
    outb(0x61, (uint8_t)(p61 & ~0x01u));             /* gate off, restore */
    if (guard <= 0) return 0;
    return t1 - t0;
}

static void calibrate_tsc(void)
{
    if (!tsc_usable(&g_tsc_invariant, &g_tsc_hypervisor)) return;

    for (int i = 0; i < 3; i++) {
        uint64_t c = calibrate_tsc_once();
        /* cycles -> Hz: c cycles happened in PIT_CAL_COUNT/PIT_CAL_HZ seconds. */
        g_calib_samples[i] = c ? (c * PIT_CAL_HZ) / PIT_CAL_COUNT : 0;
    }
    /* median of three */
    uint64_t a = g_calib_samples[0], b = g_calib_samples[1], c = g_calib_samples[2];
    uint64_t med = a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));

    /* Sanity band. Anything outside it is a broken channel 2 or a TSC that is
     * not a wall-clock counter, and taking it would give a clock that is wrong
     * by a factor rather than by a percent -- the 2x PIT failure mode again. */
    if (med < 10000000ull || med > 100000000000ull) return;

    g_src[TIMESRC_TSC].hz = med;
    calc_mult_shift(med, &g_src[TIMESRC_TSC].mult, &g_src[TIMESRC_TSC].shift);
    uint32_t res = (uint32_t)(NS_PER_SEC / med);
    g_src[TIMESRC_TSC].res_ns = res ? res : 1;
    g_src[TIMESRC_TSC].available = 1;
    g_tsc_calib_ok = 1;
}
#endif /* !LOGIT_TIME_HOST */

/* ============================================================================
 * 4. The clocks
 * ==========================================================================*/

static spinlock_t g_tlock = SPINLOCK_INIT;      /* guards the fold + the heap */

/* The fold point: (last_cycles, base_ns) is the last instant at which the cycle
 * counter and the nanosecond accumulator were reconciled. Written by the tick
 * (single writer) and by time_set_source(); read from every core.
 *
 * Readers use a SEQLOCK rather than the spinlock, because reading the clock
 * must not serialise four cores behind one lock, and must not be blocked by an
 * interrupt handler holding it. */
static volatile uint32_t g_seq;                 /* even = stable, odd = writing */
static uint64_t g_last_cycles;
static uint64_t g_base_ns;

static uint64_t g_wall_offset_ns;               /* real = mono + this */
static int      g_ready;

/* Cross-core monotonicity clamp. */
static volatile uint64_t g_mono_last;
static volatile uint64_t g_backsteps, g_backstep_max, g_reads;
static volatile uint32_t g_cores_seen;

static void fold_locked(void)
{
    struct clocksource *cs = &g_src[g_cur];
    uint64_t now = cs->read() & cs->mask;
    int64_t  d   = cyc_delta(now, g_last_cycles, cs->mask);
    if (d < 0) d = 0;
    g_seq++;                                    /* -> odd: writing */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_base_ns    += ns_of_cycles(cs, (uint64_t)d);
    g_last_cycles = now;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_seq++;                                    /* -> even: stable */
}

uint64_t time_mono_raw_ns(void)
{
    if (!g_ready) return 0;
    struct clocksource *cs;
    uint64_t base, last, now, ns;
    uint32_t s0, s1;
    int spins = 0;
    do {
        s0 = g_seq;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        cs   = &g_src[g_cur];
        base = g_base_ns;
        last = g_last_cycles;
        now  = cs->read() & cs->mask;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = g_seq;
        /* A writer that never finishes would spin us forever; the writer runs
         * with IF=0 and touches three words, so 1000 retries is astronomically
         * generous. Falling through with a possibly-torn read is still better
         * than hanging the machine inside a clock read. */
    } while (((s0 | s1) & 1u || s0 != s1) && ++spins < 1000);

    int64_t d = cyc_delta(now, last, cs->mask);
    if (d < 0) d = 0;
    ns = base + ns_of_cycles(cs, (uint64_t)d);
    return ns;
}

uint64_t time_mono_ns(void)
{
    uint64_t ns = time_mono_raw_ns();

    /* THE CLAMP.
     *
     * TSCs are not synchronised across sockets, and on some hardware not even
     * across cores of one socket; QEMU can be told to expose exactly that. The
     * subsystem promises a monotonic clock, so it has to make one out of what
     * the hardware gives: publish max(seen so far, my reading). A core whose
     * counter runs a few hundred nanoseconds behind sees the leader's value
     * instead of its own, which is a bounded forward error and never a backward
     * step. The alternative -- returning a smaller number than a previous
     * caller already saw -- is a negative interval, which every timeout in this
     * kernel would read as an enormous positive one.
     *
     * The regressions are COUNTED, not swallowed: a clock that needs the clamp
     * constantly is a clock whose calibration or source is wrong, and
     * time_mono_backsteps() is how the boot test finds out. */
    uint64_t prev = __atomic_load_n(&g_mono_last, __ATOMIC_RELAXED);
    for (;;) {
        if (ns <= prev) {
            uint64_t back = prev - ns;
            if (back) {
                __atomic_add_fetch(&g_backsteps, 1, __ATOMIC_RELAXED);
                uint64_t mx = __atomic_load_n(&g_backstep_max, __ATOMIC_RELAXED);
                while (back > mx &&
                       !__atomic_compare_exchange_n(&g_backstep_max, &mx, back, 0,
                                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    ;
            }
            ns = prev;
            break;
        }
        if (__atomic_compare_exchange_n(&g_mono_last, &prev, ns, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
        /* prev was reloaded by the CAS; retry against the new leader. */
    }
    __atomic_add_fetch(&g_reads, 1, __ATOMIC_RELAXED);
    return ns;
}

uint64_t time_mono_us(void) { return time_mono_ns() / NS_PER_US; }
uint64_t time_mono_ms(void) { return time_mono_ns() / NS_PER_MS; }

uint64_t time_real_ns(void)  { return time_mono_ns() + g_wall_offset_ns; }
int64_t  time_real_unix(void) { return (int64_t)(time_real_ns() / NS_PER_SEC); }

void time_wall_sync(void)
{
#ifndef LOGIT_TIME_HOST
    int64_t s = rtc_unix();
    if (s <= 0) return;
    uint64_t mono = time_mono_ns();
    uint64_t want = (uint64_t)s * NS_PER_SEC;
    /* Offset, not an absolute: this is what keeps the two clocks from drifting.
     * Setting the wall clock moves ONLY this word. */
    g_wall_offset_ns = want > mono ? want - mono : 0;
#endif
}

uint64_t time_mono_backsteps(void)       { return g_backsteps; }
uint64_t time_mono_backstep_max_ns(void) { return g_backstep_max; }
uint64_t time_mono_reads(void)           { return g_reads; }
uint32_t time_cores_seen(void)           { return g_cores_seen; }

int time_get_source(void) { return g_cur; }
const char *time_source_name(int src)
{ return (src >= 0 && src < TIMESRC_N) ? g_src[src].name : "?"; }
uint64_t time_source_hz(int src)
{ return (src >= 0 && src < TIMESRC_N) ? g_src[src].hz : 0; }
uint32_t time_source_res_ns(int src)
{ return (src >= 0 && src < TIMESRC_N) ? g_src[src].res_ns : 0; }

int time_set_source(int src)
{
    if (src < 0 || src >= TIMESRC_N || !g_src[src].available) return -1;
    uint64_t f = spin_lock_irqsave(&g_tlock);
    if (src != g_cur) {
        fold_locked();                          /* bank everything the old source owes */
        g_cur = src;
        g_seq++;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_last_cycles = g_src[src].read() & g_src[src].mask;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_seq++;
        /* g_base_ns is untouched, so the clock is CONTINUOUS: the switch changes
         * how the next nanosecond is measured, not what time it is. */
    }
    spin_unlock_irqrestore(&g_tlock, f);
    return 0;
}

int time_clock_gettime(int clock_id, int64_t *sec, int64_t *nsec)
{
    uint64_t ns;
    switch (clock_id) {
    case KCLOCK_MONOTONIC:
    case KCLOCK_BOOTTIME:          ns = time_mono_ns();     break;
    case KCLOCK_MONOTONIC_RAW:     ns = time_mono_raw_ns(); break;
    case KCLOCK_REALTIME:          ns = time_real_ns();     break;
    case KCLOCK_PROCESS_CPUTIME_ID:
    case KCLOCK_THREAD_CPUTIME_ID: {
        uint64_t u = 0, s = 0;
        if (time_cpu_ns(-1, &u, &s) < 0) { u = 0; s = 0; }
        ns = u + s;
        break;
    }
    default: return -1;
    }
    if (sec)  *sec  = (int64_t)(ns / NS_PER_SEC);
    if (nsec) *nsec = (int64_t)(ns % NS_PER_SEC);
    return 0;
}

uint64_t time_clock_res_ns(int clock_id)
{
    switch (clock_id) {
    case KCLOCK_MONOTONIC:
    case KCLOCK_MONOTONIC_RAW:
    case KCLOCK_BOOTTIME:
    case KCLOCK_REALTIME:          return g_src[g_cur].res_ns;
    /* Accounting is sampled at the tick, so its resolution is the tick -- say
     * so, rather than reporting the clock's resolution and implying a precision
     * the numbers do not have. */
    case KCLOCK_PROCESS_CPUTIME_ID:
    case KCLOCK_THREAD_CPUTIME_ID: return NS_PER_SEC / TIMER_HZ;
    default: return 0;
    }
}

/* ============================================================================
 * 5. Timers -- a binary min-heap
 *
 * WHY A HEAP AND NOT A WHEEL.
 * A timing wheel gives O(1) insert and expiry, which is the right trade when
 * you have thousands of timers, coarse buckets are acceptable, and most timers
 * FIRE. This kernel has the opposite workload: tens of timers, arbitrary
 * nanosecond deadlines, and most of them are network timeouts that get
 * CANCELLED before they fire. A wheel would have to either quantise those
 * deadlines to a bucket -- reintroducing "reason in ticks", the exact habit
 * this subsystem exists to end -- or cascade between wheel levels, which is
 * more code and more failure modes than the whole heap.
 *
 * The heap gives O(log n) insert, O(log n) cancel (each timer carries its own
 * index, so cancel is a direct removal and not a scan) and O(1) "what is due
 * next", with exact ns ordering and no cascade. n = 128 means every operation
 * is at most 7 comparisons. That is what "does not degrade as timers
 * accumulate" needs to mean here.
 *
 * Ties: many timers at the same deadline is the normal case (everything armed
 * in one pass of a poll loop), so ktimer_run drains ALL due timers per call and
 * the tests assert every one of them ran.
 * ==========================================================================*/

#define KTIMER_MAX 128

static struct ktimer *g_heap[KTIMER_MAX];
static int      g_nheap;
static uint64_t g_fired;
static uint32_t g_seqno;

static void heap_set(int i, struct ktimer *t) { g_heap[i] = t; t->heap_idx = i; }

static void sift_up(int i)
{
    struct ktimer *t = g_heap[i];
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_heap[p]->expires_ns <= t->expires_ns) break;
        heap_set(i, g_heap[p]);
        i = p;
    }
    heap_set(i, t);
}

static void sift_down(int i)
{
    struct ktimer *t = g_heap[i];
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        uint64_t best = t->expires_ns;
        if (l < g_nheap && g_heap[l]->expires_ns < best) { m = l; best = g_heap[l]->expires_ns; }
        if (r < g_nheap && g_heap[r]->expires_ns < best) { m = r; }
        if (m == i) break;
        heap_set(i, g_heap[m]);
        i = m;
    }
    heap_set(i, t);
}

static void heap_remove_locked(struct ktimer *t)
{
    int i = t->heap_idx;
    if (i < 0 || i >= g_nheap || g_heap[i] != t) { t->heap_idx = -1; return; }
    t->heap_idx = -1;
    g_nheap--;
    if (i == g_nheap) return;                   /* removed the tail */
    heap_set(i, g_heap[g_nheap]);
    /* The replacement may belong either way: it came from the end of the array,
     * not from below `i`. Both directions, or a cancel in the middle silently
     * corrupts the ordering. */
    sift_up(i);
    if (g_heap[i] && g_heap[i]->heap_idx == i) sift_down(i);
}

static int heap_insert_locked(struct ktimer *t)
{
    if (g_nheap >= KTIMER_MAX) { t->heap_idx = -1; return -1; }
    int i = g_nheap++;
    heap_set(i, t);
    sift_up(i);
    return 0;
}

int ktimer_add(struct ktimer *t, uint64_t delay_ns, uint64_t period_ns,
               ktimer_fn fn, void *arg, const char *name)
{
    if (!t || !fn) return -1;
    uint64_t f = spin_lock_irqsave(&g_tlock);
    if (t->heap_idx >= 0) heap_remove_locked(t);
    t->expires_ns = time_mono_raw_ns() + delay_ns;
    t->period_ns  = period_ns;
    t->fn         = fn;
    t->arg        = arg;
    t->name       = name;
    t->seq        = ++g_seqno;
    int rc = heap_insert_locked(t);
    spin_unlock_irqrestore(&g_tlock, f);
    return rc;
}

int ktimer_cancel(struct ktimer *t)
{
    if (!t) return 0;
    uint64_t f = spin_lock_irqsave(&g_tlock);
    int was = t->heap_idx >= 0;
    if (was) heap_remove_locked(t);
    t->period_ns = 0;           /* a periodic timer mid-callback must not re-arm */
    spin_unlock_irqrestore(&g_tlock, f);
    return was;
}

uint64_t ktimer_next_ns(void)
{
    uint64_t f = spin_lock_irqsave(&g_tlock);
    uint64_t r = g_nheap ? g_heap[0]->expires_ns : 0;
    spin_unlock_irqrestore(&g_tlock, f);
    return r;
}

int      ktimer_queued(void) { return g_nheap; }
uint64_t ktimer_fired(void)  { return g_fired; }

void ktimer_run(uint64_t now_ns)
{
    for (;;) {
        uint64_t f = spin_lock_irqsave(&g_tlock);
        if (!g_nheap || g_heap[0]->expires_ns > now_ns) {
            spin_unlock_irqrestore(&g_tlock, f);
            return;
        }
        struct ktimer *t = g_heap[0];
        heap_remove_locked(t);
        uint64_t period = t->period_ns;
        if (period) {
            /* Re-arm on the DEADLINE, not on now: a periodic timer that rebased
             * on the moment it happened to be serviced would drift by the
             * service latency every single period. If we are so far behind that
             * the next deadline is already past (a long IF=0 window), skip
             * forward rather than firing a burst to catch up. */
            t->expires_ns += period;
            if (t->expires_ns <= now_ns)
                t->expires_ns = now_ns + period;
            heap_insert_locked(t);
        }
        ktimer_fn fn = t->fn;
        g_fired++;
        /* Callback with the lock DROPPED: it may arm or cancel timers, and it
         * runs in interrupt context without the BKL (see time.h). */
        spin_unlock_irqrestore(&g_tlock, f);
        fn(t);
    }
}

/* ============================================================================
 * 6. Per-process CPU accounting
 *
 * HOW IT IS MEASURED, AND WHAT THAT COSTS IN ACCURACY.
 * The exact way is to stamp the clock in the context switch. The scheduler is
 * another line's file, so this samples instead: at every tick, whatever process
 * is current on this core gets charged the whole tick, split into ring-3 and
 * ring-0 by whether the interrupted context held the kernel lock.
 *
 * That is honest but it has two known errors, both stated here rather than
 * discovered later:
 *   - QUANTISATION. A process that runs for 3 ms between two ticks and yields
 *     is charged 0 or 10 ms, never 3. Over many ticks the error averages out;
 *     over a short-lived process it does not.
 *   - SMP UNDERCOUNT. timer_tick() runs on the BSP only (interrupts.c gates it
 *     on cpu index 0), so work done on the other cores is not sampled at all.
 *     With -smp 4 the totals are therefore roughly a quarter of reality.
 * time_account_switch() is the exact replacement, and it needs two lines in
 * schedule(). Until then the numbers are labelled `sampled` where they surface.
 * ==========================================================================*/

#define CPUACC_MAX 64

struct cpuacc { int pid; uint64_t user_ns, sys_ns; };
static struct cpuacc g_acc[CPUACC_MAX];
static uint64_t g_acc_total, g_acc_idle;
static uint64_t g_acc_last_ns;

static struct cpuacc *acc_slot(int pid, int create)
{
    int free_i = -1;
    for (int i = 0; i < CPUACC_MAX; i++) {
        if (g_acc[i].pid == pid) return &g_acc[i];
        if (free_i < 0 && g_acc[i].pid == 0) free_i = i;
    }
    if (!create || free_i < 0) return 0;
    g_acc[free_i].pid = pid;
    g_acc[free_i].user_ns = g_acc[free_i].sys_ns = 0;
    return &g_acc[free_i];
}

static void account_sample(uint64_t now_ns)
{
    uint64_t dt = now_ns > g_acc_last_ns ? now_ns - g_acc_last_ns : 0;
    g_acc_last_ns = now_ns;
    if (!dt) return;
    g_acc_total += dt;
    int pid = proc_current_pid();
    if (pid <= 0) { g_acc_idle += dt; return; }
    struct cpuacc *a = acc_slot(pid, 1);
    if (!a) { g_acc_idle += dt; return; }
    if (in_kernel_now()) a->sys_ns += dt; else a->user_ns += dt;
}

void time_account_switch(int old_pid, int new_pid)
{
    /* Exact accounting, for when the scheduler calls it: close out the outgoing
     * process at the current instant and open the incoming one. Sampling and
     * this are mutually exclusive by construction -- both charge the interval
     * since g_acc_last_ns exactly once. */
    uint64_t now = time_mono_raw_ns();
    uint64_t dt  = now > g_acc_last_ns ? now - g_acc_last_ns : 0;
    g_acc_last_ns = now;
    g_acc_total  += dt;
    struct cpuacc *a = old_pid > 0 ? acc_slot(old_pid, 1) : 0;
    if (a) { if (in_kernel_now()) a->sys_ns += dt; else a->user_ns += dt; }
    else     g_acc_idle += dt;
    (void)new_pid;
}

int time_cpu_ns(int pid, uint64_t *user_ns, uint64_t *sys_ns)
{
    if (pid < 0) pid = proc_current_pid();
    if (pid <= 0) return -1;
    struct cpuacc *a = acc_slot(pid, 0);
    if (!a) return -1;
    if (user_ns) *user_ns = a->user_ns;
    if (sys_ns)  *sys_ns  = a->sys_ns;
    return 0;
}

uint64_t time_cpu_total_ns(void) { return g_acc_total; }

/* ============================================================================
 * 7. The calibration cross-check (pure, so it can be tested from the host)
 * ==========================================================================*/

int time_xcheck(uint64_t mono_ns, uint64_t rtc_seconds, int *ok)
{
    if (ok) *ok = 0;
    if (rtc_seconds == 0) return 0;

    uint64_t want_ns = rtc_seconds * NS_PER_SEC;
    /* parts per thousand, signed */
    int64_t diff = (int64_t)mono_ns - (int64_t)want_ns;
    int64_t ppt  = (diff * 1000) / (int64_t)want_ns;

    /* THE TOLERANCE, and why it is as wide as it is.
     *
     * This is a factor-of-two detector, not a calibration verifier, and the
     * difference is worth stating because the obvious mistake is to tighten it.
     *
     * Measured on QEMU/TCG (WSL2 host, -smp 4, -accel tcg): the emulated CMOS
     * RTC advances 5 "seconds" in 4.45 s of real time -- it runs about 11% FAST,
     * identically under -rtc clock=host, clock=vm and the default. Our clock is
     * the one that is right: over an 8.5 s deadline the monotonic clock matched
     * the HOST's wall clock to +0.05% across three runs, and the TSC-derived
     * value agrees with the independent PIT interrupt count to within 1 ms.
     *
     * So the RTC is only good to ~15% here, and a tolerance tight enough to
     * catch a 5% calibration error would fail on every boot. 250 ppt keeps the
     * check honest about what it can prove: a tick at twice its rate is 1000
     * ppt, four times the bound. The measured error is PRINTED whether it
     * passes or not, so a real drift is visible to a human long before it trips
     * the assertion -- which is the part the 2x PIT bug never had.
     *
     * The added term is the sampling error of the two edge detections: the
     * caller detects second EDGES at a 10 ms poll, so the true interval is
     * bounded to +-20 ms, i.e. +-20/N ppt over an N-second sample. */
    int64_t tol = 250 + 20 / (int64_t)(rtc_seconds ? rtc_seconds : 1);
    if (ok) *ok = (ppt <= tol && ppt >= -tol);
    return (int)ppt;
}

/* ============================================================================
 * 8. Boot report, tick, self-tests
 * ==========================================================================*/

/* kprintf knows %d %u %x %s %c %p and nothing wider, so 64-bit values are
 * rendered here rather than by adding a conversion to another line's file. */
static const char *u64d(uint64_t v, char *b)
{
    char tmp[24]; int n = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return b; }
    while (v && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (n) b[j++] = tmp[--n];
    b[j] = 0;
    return b;
}
/* value scaled by 1000, printed as d.ddd -- avoids floating point in the kernel */
static const char *milli(uint64_t v_milli, char *b)
{
    char a[24], c[8];
    u64d(v_milli / 1000, a);
    uint64_t frac = v_milli % 1000;
    c[0] = (char)('0' + frac / 100); c[1] = (char)('0' + (frac / 10) % 10);
    c[2] = (char)('0' + frac % 10);  c[3] = 0;
    int j = 0;
    for (int i = 0; a[i]; i++) b[j++] = a[i];
    b[j++] = '.';
    for (int i = 0; c[i]; i++) b[j++] = c[i];
    b[j] = 0;
    return b;
}

void time_report(void)
{
    char a[32], b[32];
    struct clocksource *cs = &g_src[g_cur];
    kprintf("[time] source=%s %s MHz res=%uns  (invariant-tsc=%s hypervisor=%s calib=%s)\n",
            cs->name, milli(cs->hz / 1000, a), (unsigned)cs->res_ns,
            g_tsc_invariant ? "yes" : "no",
            g_tsc_hypervisor ? "yes" : "no",
            g_tsc_calib_ok ? "pit-ch2" : "none");
    kprintf("[time] fallback=%s %s Hz res=%uns  timers=heap/%d  mono=64-bit ns "
            "(wraps in 584 years)\n",
            g_src[TIMESRC_PIT].name, u64d(g_src[TIMESRC_PIT].hz, b),
            (unsigned)g_src[TIMESRC_PIT].res_ns, KTIMER_MAX);
    if (g_tsc_calib_ok) {
        char s0[24], s1[24], s2[24];
        kprintf("[time] calib samples(Hz) %s %s %s -> median chosen\n",
                u64d(g_calib_samples[0], s0), u64d(g_calib_samples[1], s1),
                u64d(g_calib_samples[2], s2));
    }
}

#ifndef LOGIT_TIME_HOST
/* ---- on-device self-tests -----------------------------------------------
 *
 * These run on EVERY boot, not only under a test harness, and they are timers
 * rather than loops so they cost no CPU while they wait. That is deliberate.
 * The bug this subsystem was built after -- a tick at twice its programmed rate
 * -- survived because nothing ever compared the clock to anything. A check that
 * only runs when someone remembers to run it is a check that will be wrong on
 * the machine that matters. The whole suite is four serial lines and about
 * 1.5 s of wall time, none of it before LOGIT_BOOT_OK. */

#define XCHECK_SECONDS 5
static struct ktimer t_xcheck, t_accuracy[32], t_fallback, t_smp;

/* --- (a) calibration vs the RTC ---------------------------------------- */
static int      xc_state, xc_last_sec, xc_edges;
static uint64_t xc_mono0, xc_ticks0;

static void xcheck_tick(struct ktimer *t)
{
    int s = rtc_second();
    if (s == xc_last_sec) return;
    xc_last_sec = s;
    if (xc_state == 0) {
        xc_mono0 = time_mono_ns(); xc_ticks0 = timer_ticks();
        xc_state = 1; xc_edges = 0;
        return;
    }
    if (++xc_edges < XCHECK_SECONDS) return;

    uint64_t d  = time_mono_ns() - xc_mono0;
    uint64_t dt = timer_ticks() - xc_ticks0;
    int ok = 0;
    int ppt = time_xcheck(d, (uint64_t)xc_edges, &ok);
    /* THREE sources, not two, and that is the point. The calibrated clock, the
     * PIT interrupt count and the CMOS RTC are three separate oscillators (well
     * -- two, since both PIT channels share one, which is itself worth knowing).
     * Printing all three means a disagreement says WHICH one is odd instead of
     * only that something is. A 2x tick shows up as tick=2x with mono and rtc
     * agreeing; a mis-calibrated TSC shows up as mono alone drifting. */
    char a[32], b[32];
    kprintf("[time] xcheck src=%s rtc=%ds mono=%sms tick=%sms err=%s%dppt %s\n",
            g_src[g_cur].name, xc_edges, u64d(d / NS_PER_MS, a),
            u64d(dt * (NS_PER_SEC / TIMER_HZ) / NS_PER_MS, b),
            ppt >= 0 ? "+" : "", ppt, ok ? "OK" : "FAIL");

    /* The negative control, run in the same breath and on the same code: feed
     * the checker a monotonic reading that is twice the RTC's opinion -- the
     * exact shape of the 2x PIT bug -- and require it to REJECT. An assertion
     * that has never been seen to fail is not evidence of anything. */
    int nok = 1;
    int nppt = time_xcheck(d * 2, (uint64_t)xc_edges, &nok);
    kprintf("[time] negctl 2x-tick err=%s%dppt -> %s (guard %s)\n",
            nppt >= 0 ? "+" : "", nppt, nok ? "ACCEPTED" : "REJECTED",
            nok ? "BROKEN" : "works");
    ktimer_cancel(t);
}

/* --- (b) timer accuracy ------------------------------------------------- */
static uint64_t acc_want[32], acc_err[32];
static int      acc_n, acc_done;

static void acc_fire(struct ktimer *t)
{
    int i = (int)(long)t->arg;
    uint64_t now = time_mono_ns();
    acc_err[i] = now > acc_want[i] ? now - acc_want[i] : 0;
    if (++acc_done < acc_n) return;

    uint64_t s[32];
    for (int k = 0; k < acc_n; k++) s[k] = acc_err[k];
    for (int k = 1; k < acc_n; k++) {           /* insertion sort, n = 32 */
        uint64_t v = s[k]; int j = k - 1;
        while (j >= 0 && s[j] > v) { s[j+1] = s[j]; j--; }
        s[j+1] = v;
    }
    char a[24], b[24], c[24], d[24];
    kprintf("[time] accuracy n=%d late_us min=%s p50=%s p90=%s max=%s "
            "(TCG; delivery is quantised to the %dHz tick)\n",
            acc_n, u64d(s[0] / 1000, a), u64d(s[acc_n/2] / 1000, b),
            u64d(s[(acc_n*9)/10] / 1000, c), u64d(s[acc_n-1] / 1000, d),
            TIMER_HZ);
}

/* --- (c) the PIT fallback, actually exercised --------------------------- */
static uint64_t fb_m0;
static int      fb_state;

static void fallback_step(struct ktimer *t)
{
    if (fb_state == 0) {
        /* A fallback that has never run is not a fallback. Switch the LIVE
         * clock to the PIT, leave it there for a measured interval, and switch
         * back -- if the PIT source were broken, every timeout in the machine
         * would notice immediately and so would this. */
        if (time_set_source(TIMESRC_PIT) < 0) { ktimer_cancel(t); return; }
        fb_m0 = time_mono_ns();
        fb_state = 1;
        return;
    }
    uint64_t on_pit = time_mono_ns() - fb_m0;
    uint64_t back0  = time_mono_ns();
    int rc = time_set_source(g_tsc_calib_ok ? TIMESRC_TSC : TIMESRC_PIT);
    uint64_t back1  = time_mono_ns();
    char a[24], b[24];
    kprintf("[time] fallback pit ran %sms (want ~500), switch back rc=%d "
            "continuity=%sns %s\n",
            u64d(on_pit / NS_PER_MS, a), rc,
            u64d(back1 >= back0 ? back1 - back0 : 0, b),
            back1 >= back0 ? "monotonic" : "WENT BACKWARDS");
    ktimer_cancel(t);
}

/* --- (d) SMP monotonicity ----------------------------------------------- */
#define SMP_PROBE_READS 100000
static volatile int smp_started, smp_left;
static volatile uint64_t smp_reads, smp_back0;
static volatile uint32_t smp_cores;

static void smp_probe(void)
{
    /* Read the clock hard from whatever core the scheduler put us on, and
     * record which core that was. The clamp inside time_mono_ns() is what makes
     * the answer monotonic; this measures how often it had to intervene, which
     * is the number that says whether the TSCs on this machine agree. */
    uint64_t prev = time_mono_ns();
    for (int i = 0; i < SMP_PROBE_READS; i++) {
        uint64_t now = time_mono_ns();
        if (now < prev) __atomic_add_fetch(&smp_back0, 1, __ATOMIC_RELAXED);
        prev = now;
        if ((i & 0xFF) == 0) {
            __atomic_or_fetch(&smp_cores, 1u << (cpu_index() & 31), __ATOMIC_RELAXED);
            __atomic_or_fetch(&g_cores_seen, 1u << (cpu_index() & 31), __ATOMIC_RELAXED);
            /* Yield often, on purpose. A kernel thread holds the big kernel lock
             * while it runs, so these probes cannot execute SIMULTANEOUSLY --
             * but simultaneity is not what is being tested. The property is
             * that a read on core B, happening AFTER a read on core A, never
             * returns a smaller number, and unsynchronised TSCs break exactly
             * that. Yielding is what gets the probe rescheduled onto a
             * different core, which is what makes the observation cross-core. */
            schedule();
        }
    }
    __atomic_add_fetch(&smp_reads, SMP_PROBE_READS, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&smp_left, 1, __ATOMIC_SEQ_CST) == 0) {
        /* Four DISTINCT buffers: kprintf takes all its arguments before it
         * formats any of them, so reusing one scratch buffer for two %s makes
         * both print the second value. The first version of this line reported
         * reads=0 for exactly that reason. */
        char a[24], b[24], c[24], d[24];
        kprintf("[time] smp-mono cores=%d seen=%x reads=%s observed-backsteps=%s "
                "clamped=%s max=%sns\n",
                smp_cpu_count(), (unsigned)smp_cores, u64d(smp_reads, a),
                u64d(smp_back0, b), u64d(g_backsteps, c),
                u64d(g_backstep_max, d));
    }
    thread_exit();
}

static void smp_kick(struct ktimer *t)
{
    g_time_deferred = 1;            /* time_safepoint() does the thread_create */
    ktimer_cancel(t);
}

void time_safepoint(void)
{
    if (__builtin_expect(!g_time_deferred || smp_started, 1)) return;
    /* thread_create takes the scheduler lock and kmallocs. Only do it from a
     * genuine THREAD context: interrupts on (so we are not inside an interrupt
     * gate) and the big kernel lock already held (so the lock order is the same
     * BKL -> sched one every other spawn site uses). timer_ticks() is called
     * from both contexts; this is the gate that tells them apart. */
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0" : "=r"(fl));
    if (!(fl & 0x200) || !in_kernel_now()) return;
    if (__atomic_exchange_n(&smp_started, 1, __ATOMIC_SEQ_CST)) return;

    g_time_deferred = 0;
    int n = smp_cpu_count();
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    smp_left = n;
    for (int i = 0; i < n; i++)
        thread_create(smp_probe, "timeprobe");
}

static void selftests_arm(void)
{
    /* Nothing here starts before ~2 s: boot is the busiest the machine ever is,
     * and a measurement taken during it measures the boot, not the clock. */
    ktimer_add(&t_xcheck, 2 * NS_PER_SEC, 10 * NS_PER_MS, xcheck_tick, 0, "xcheck");

    acc_n = 32; acc_done = 0;
    uint64_t now = time_mono_raw_ns();
    for (int i = 0; i < acc_n; i++) {
        /* Spread the requested delays across two decades so the histogram says
         * something about scheduling latency rather than about one delay. */
        uint64_t d = (uint64_t)(2 + i) * 3 * NS_PER_MS + 2 * NS_PER_SEC;
        acc_want[i] = now + d;
        ktimer_add(&t_accuracy[i], d, 0, acc_fire, (void *)(long)i, "acc");
    }
    ktimer_add(&t_fallback, 8 * NS_PER_SEC, 500 * NS_PER_MS, fallback_step, 0, "fallback");
    ktimer_add(&t_smp, 10 * NS_PER_SEC, 0, smp_kick, 0, "smpkick");
}
#else
void time_safepoint(void) {}
__attribute__((unused)) static void selftests_arm(void) {}
#endif /* !LOGIT_TIME_HOST */

/* Zero for all but a few microseconds of the machine's life; see
 * timer_ticks() in c/drivers/timer/pit.c for why it is polled from there. */
volatile int g_time_deferred;

/* ---- init + tick --------------------------------------------------------- */

int time_ready(void) { return g_ready; }

void time_init(void)
{
    if (g_ready) return;
#ifndef LOGIT_TIME_HOST
    calc_mult_shift(TIMER_HZ, &g_src[TIMESRC_PIT].mult, &g_src[TIMESRC_PIT].shift);
    calibrate_tsc();
    g_cur = g_tsc_calib_ok ? TIMESRC_TSC : TIMESRC_PIT;
#else
    calc_mult_shift(g_host_hz, &g_src[TIMESRC_TSC].mult, &g_src[TIMESRC_TSC].shift);
    g_src[TIMESRC_TSC].hz = g_host_hz;
    g_src[TIMESRC_TSC].mask = g_host_mask;
    g_src[TIMESRC_TSC].available = 1;
    calc_mult_shift(TIMER_HZ, &g_src[TIMESRC_PIT].mult, &g_src[TIMESRC_PIT].shift);
    g_cur = TIMESRC_TSC;
#endif
    g_last_cycles = g_src[g_cur].read() & g_src[g_cur].mask;
    g_base_ns = 0;
    g_acc_last_ns = 0;
    g_ready = 1;
#ifndef LOGIT_TIME_HOST
    time_wall_sync();
    time_report();
    selftests_arm();
#endif
}

void time_tick(void)
{
    if (!g_ready) return;
    /* Fold under the lock (the seqlock writer), then run timers and account
     * OUTSIDE it: a callback may arm timers, and account_sample calls into the
     * process table. */
    uint64_t f = spin_lock_irqsave(&g_tlock);
    fold_locked();
    uint64_t now = g_base_ns;
    spin_unlock_irqrestore(&g_tlock, f);

    account_sample(now);
    ktimer_run(now);
}

#ifdef LOGIT_TIME_HOST
/* Host-only knobs, declared in time.h under the same guard. */
void time_host_reset(uint64_t hz, uint64_t mask)
{
    g_host_hz = hz; g_host_mask = mask;
    g_host_cycles = 0; g_host_ticks = 0; g_host_rtc_s = 0;
    g_ready = 0; g_seq = 0; g_base_ns = 0; g_last_cycles = 0;
    g_mono_last = 0; g_backsteps = 0; g_backstep_max = 0; g_reads = 0;
    g_nheap = 0; g_fired = 0; g_seqno = 0;
    g_acc_total = 0; g_acc_idle = 0; g_acc_last_ns = 0;
    g_wall_offset_ns = 0;
    for (int i = 0; i < CPUACC_MAX; i++) g_acc[i].pid = 0;
    time_init();
}
uint64_t time_host_base_ns(void) { return g_base_ns; }
int      time_host_heap_ok(void)
{
    /* Structural invariant: every parent <= both children, and every element's
     * heap_idx agrees with where it actually sits. */
    for (int i = 0; i < g_nheap; i++) {
        if (g_heap[i]->heap_idx != i) return 0;
        int l = 2*i+1, r = l+1;
        if (l < g_nheap && g_heap[l]->expires_ns < g_heap[i]->expires_ns) return 0;
        if (r < g_nheap && g_heap[r]->expires_ns < g_heap[i]->expires_ns) return 0;
    }
    return 1;
}
#endif
