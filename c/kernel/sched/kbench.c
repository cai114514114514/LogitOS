#include <stdint.h>
#include <stddef.h>
#include "kbench.h"
#include "sched.h"
#include "spinlock.h"
#include "percpu.h"
#include "pmm.h"
#include "mm.h"          /* mm_report(): the fault path's half of the trade */
#include "kheap.h"
#include "kprintf.h"
#include "pit.h"
#include "ktime.h"
/* Path-qualified, and it has to be: mini-libc ships c/apps/libc/include/sys/
 * wait.h and that directory sorts before c/kernel/core in INCDIRS, so a bare
 * #include "wait.h" from outside c/kernel/core resolves to the userland one.
 * (See the identical note in c/kernel/exec/syscall.c -- it cost a build.) */
#include "kernel/core/wait.h"   /* sched_sleep_ms */

/* --------------------------------------------------------------------------
 * The path counters. Defined here, written by spinlock.c and interrupts.c.
 * -------------------------------------------------------------------------- */
struct kb_cpu   g_kb[KB_MAXCPU];
volatile int    g_kb_stat;
uint64_t        g_kb_sys_n[KB_NSYS];
uint64_t        g_kb_sys_cyc[KB_NSYS];

void kb_stat_set(int on) { g_kb_stat = on ? 1 : 0; }

/* c/kernel/exec/file.c. A local prototype rather than a header entry, the same
 * way proc.c declares proc_fork_stats: this is a diagnostic between two files,
 * not an interface anybody else should reach for. */
void tty_wait_stats(uint64_t *wakes, uint64_t *awake_cyc);

/* --------------------------------------------------------------------------
 * cycles -> nanoseconds.
 *
 * The TSC frequency time.c calibrated is the only conversion available, and
 * under TCG it is a measurement of the HOST's clock: QEMU derives the guest TSC
 * from host wall time, so a "cycle" here is 1/3.4 GHz of real time and not a
 * unit of guest instruction work. That is exactly the unit wanted -- the
 * question is how long the machine is unavailable, not how many guest cycles
 * were retired -- but it is why every line printed below says TCG.
 * -------------------------------------------------------------------------- */
static uint64_t g_mhz = 1000;

/* Nanoseconds per operation, x1000, so a 0.4 ns operation is not reported as
 * "0". cyc * 1e6 stays under 2^63 for any cyc a benchmark here can produce. */
static uint64_t ns_x1000(uint64_t cyc, uint64_t iters)
{
    if (!iters || !g_mhz) return 0;
    return (cyc * 1000000ull) / g_mhz / iters;
}

/* ---- the repetition harness ----------------------------------------------
 * Every measurement is KB_REPS timed runs of the same loop. The kernel this
 * runs in is preemptive and the host is shared, so ONE run is noise; the three
 * numbers printed are the whole distribution: min is the cost with nothing in
 * the way, max is what a timer tick or a busy host does to it, median is what
 * to quote. A single number here would be a claim, not a measurement. */
#define KB_REPS 7

#ifdef KBENCH_MICRO
static uint64_t g_rep[KB_REPS];

static void kb_sort(void)
{
    for (int i = 1; i < KB_REPS; i++) {         /* insertion sort, 7 elements */
        uint64_t v = g_rep[i]; int j = i - 1;
        while (j >= 0 && g_rep[j] > v) { g_rep[j + 1] = g_rep[j]; j--; }
        g_rep[j + 1] = v;
    }
}

static void kb_report(const char *label, uint64_t iters)
{
    kb_sort();
    uint64_t lo = ns_x1000(g_rep[0], iters);
    uint64_t md = ns_x1000(g_rep[KB_REPS / 2], iters);
    uint64_t hi = ns_x1000(g_rep[KB_REPS - 1], iters);
    kprintf("[kbench] %s: median %d.%03d ns  (min %d.%03d, max %d.%03d, n=%d x %d) TCG\n",
            label,
            (int)(md / 1000), (int)(md % 1000),
            (int)(lo / 1000), (int)(lo % 1000),
            (int)(hi / 1000), (int)(hi % 1000),
            KB_REPS, (int)iters);
}

#define KB_RUN(label, iters, body)                                   \
    do {                                                             \
        for (int _r = 0; _r < KB_REPS; _r++) {                       \
            uint64_t _t0 = kb_rdtsc();                               \
            for (long _i = 0; _i < (long)(iters); _i++) { body; }    \
            g_rep[_r] = kb_rdtsc() - _t0;                            \
        }                                                            \
        kb_report(label, (iters));                                   \
    } while (0)

/* A sink the optimiser cannot fold away. volatile, because -O2 will otherwise
 * delete a loop whose result nobody reads, and a benchmark of a deleted loop
 * reports 0.000 ns and looks like a triumph. */
static volatile uint64_t g_sink;

/* ========================= THE MICROBENCHMARK BATTERY ======================
 * Compiled only when KBENCH_MICRO is defined (make KBENCH=1). See the note at
 * its call site in kbench_thread() for why it is not in the default build.
 * ========================================================================= */

/* -------------------------------------------------------------- primitives */

static spinlock_t kb_lock = SPINLOCK_INIT;

static void bench_primitives(void)
{
    /* this_cpu() -- the per-core lookup. Called at least four times on every
     * kernel entry (interrupt_handler twice, spin_lock's BKL-owner update, and
     * again on the exit path), so its cost is multiplied by every syscall,
     * every fault and every timer tick on every core. Split into the
     * instruction and the table walk, because "make this_cpu() faster" means
     * two completely different changes depending on which half it is. */
    KB_RUN("this_cpu()", 20000, g_sink = (uint64_t)(long)this_cpu()->index);
    {
        struct gdt_ptr gp;
        KB_RUN("  ...of which: bare sgdt", 20000,
               { __asm__ volatile ("sgdt %0" : "=m"(gp)); g_sink = gp.base; });
    }

    /* An uncontended private spinlock, irqsave/irqrestore included -- the shape
     * every fine-grained lock in the kernel is taken in. */
    KB_RUN("spin_lock_irqsave + irqrestore (uncontended)", 20000,
           { uint64_t f = spin_lock_irqsave(&kb_lock);
             spin_unlock_irqrestore(&kb_lock, f); });

    /* rdtsc itself, so every other number here can be read net of the
     * instrument. Two rdtsc calls bracket each loop, not each iteration, so
     * this is a correction of order 1/iters -- but it is what makes the
     * sub-nanosecond numbers below defensible rather than assumed. */
    KB_RUN("rdtsc", 20000, g_sink = kb_rdtsc());

    /* The M28 calibrated clock, and the tick counter it replaced. The network
     * stack consults timer_ticks()/the ns clock on every poll, so the
     * difference between these two lines is a real per-packet cost. */
    KB_RUN("time_mono_ns()", 20000, g_sink = time_mono_ns());
    KB_RUN("timer_ticks()", 20000, g_sink = timer_ticks());

    /* FXSAVE + FXRSTOR: M15 put this pair around every C interrupt handler
     * (boot/isr.asm isr_common), so it is paid TWICE per kernel entry --
     * syscall, fault and timer tick alike. Measured here because that file
     * belongs to another line and this is the only way to price its decision
     * without touching it. */
    {
        static uint8_t fxarea[512] __attribute__((aligned(16)));
        KB_RUN("fxsave + fxrstor (2x per interrupt)", 20000,
               { __asm__ volatile ("fxsave (%0); fxrstor (%0)" :: "r"(fxarea) : "memory"); });
    }
}

/* ------------------------------------------------------------ the allocator */

/* Frame allocation, at each poison level, with everything else held equal.
 *
 * Poison level 1 is ON BY DEFAULT and writes PMM_POISON_HEAD (64 B, one cache
 * line) into every freed frame and compares it on every allocation. Whether
 * that is affordable is not a matter of opinion: level 0 / 1 / 2 are three runs
 * of the same loop, and the differences are the price. Level 2 is included
 * because it is the debug setting and somebody will eventually leave it on. */
static void bench_pmm(void)
{
    int saved = pmm_poison_level();
    static const int levels[3] = { 0, 1, 2 };
    static const char *names[3] = {
        "pmm_alloc + pmm_free, poison OFF (level 0)",
        "pmm_alloc + pmm_free, poison HEAD 64B (level 1, the default)",
        "pmm_alloc + pmm_free, poison WHOLE 4 KiB (level 2)",
    };

    for (int k = 0; k < 3; k++) {
        pmm_set_poison(levels[k]);
        /* Warm: the first pass through a level pays for frames that were freed
         * under the PREVIOUS level, whose poison state does not match. */
        for (int w = 0; w < 64; w++) { uint64_t f = pmm_alloc(); if (f) pmm_free(f); }
        KB_RUN(names[k], 2000,
               { uint64_t f = pmm_alloc(); if (f) pmm_free(f); else g_sink++; });
    }
    pmm_set_poison(saved);

    /* The other half of the fork/exec path: many frames live at once, so the
     * allocator's bitmap scan cannot keep handing back the same frame and
     * alloc_hint has to walk. 256 frames is exactly one execve's CLI stack.
     * Run at both poison settings, because the single-frame loop above reuses
     * ONE hot frame and therefore measures poison against a cache that is
     * always warm -- which is not what 256 cold frames look like. */
    {
        static uint64_t held[256];
        pmm_set_poison(0);
        KB_RUN("pmm_alloc x256 + free x256 (one execve stack), poison OFF", 8,
               { for (int j = 0; j < 256; j++) held[j] = pmm_alloc();
                 for (int j = 0; j < 256; j++) if (held[j]) pmm_free(held[j]); });
        pmm_set_poison(1);
        KB_RUN("pmm_alloc x256 + free x256 (one execve stack), poison level 1", 8,
               { for (int j = 0; j < 256; j++) held[j] = pmm_alloc();
                 for (int j = 0; j < 256; j++) if (held[j]) pmm_free(held[j]); });
        pmm_set_poison(saved);
    }

    /* THE REFCOUNT TABLE'S TOUCH PATTERN. 131072 frames x 2 bytes = 256 KiB,
     * which does not fit any cache this emulator models generously. Read
     * sequentially it is 32 counts per cache line and effectively free; read at
     * a stride that skips a line every time it is one miss per access. fork,
     * exit and every copy-on-write fault index it by frame number, and frame
     * numbers from the allocator are near-sequential -- so which of these two
     * lines describes reality is the whole question. */
    {
        uint64_t nframes = pmm_total_frames();
        uint64_t base = nframes > 8192 ? (nframes - 8192) & ~(uint64_t)0xFFF : 0;
        KB_RUN("pmm_refcount(), sequential frames", 4096,
               g_sink = pmm_refcount((base + (uint64_t)_i) * 4096));
        /* 4096 frames apart = 8 KiB apart in the table: a different line, and a
         * different page, every time. */
        KB_RUN("pmm_refcount(), 8 KiB-strided frames", 4096,
               g_sink = pmm_refcount(((uint64_t)(_i * 61) % nframes) * 4096));
    }

    KB_RUN("kmalloc(64) + kfree", 4000,
           { void *p = kmalloc(64); if (p) kfree(p); else g_sink++; });
}

/* ------------------------------------------------------------- the scheduler */

static volatile int  g_partner_go;
static volatile long g_partner_spins;

/* A second runnable kernel thread, so schedule() has somewhere to go. Without
 * it the pick loop finds nothing on an idle desktop, returns without switching,
 * and the "context switch" measurement is a measurement of not switching. */
static void kb_partner(void)
{
    while (g_partner_go) { g_partner_spins++; schedule(); }
}

static void bench_sched(void)
{
    /* schedule() with a partner: each call is one switch out and (eventually)
     * one back, so the interesting number is derived from the scheduler's own
     * switch counter rather than assumed to be two. */
    g_partner_go = 1;
    g_partner_spins = 0;
    thread_create(kb_partner, "kbpartner");
    sched_sleep_ms(20);                    /* let it reach the run ring */

    for (int r = 0; r < KB_REPS; r++) {
        uint64_t s0 = sched_switches();
        uint64_t t0 = kb_rdtsc();
        for (int i = 0; i < 200; i++) schedule();
        uint64_t dt = kb_rdtsc() - t0;
        uint64_t ds = sched_switches() - s0;
        g_rep[r] = ds ? dt * 200 / ds : dt;   /* normalise to "per switch" x200 */
    }
    kb_report("context switch (schedule(), 2 runnable threads)", 200);

    g_partner_go = 0;
    sched_sleep_ms(20);
    kprintf("[kbench] partner thread ran %d times while the switch bench ran\n",
            (int)g_partner_spins);
}

/* ------------------------------------------------------------- the log ring */

static void bench_klog(void)
{
    char buf[96];
    /* Formatting alone -- ksnprintf deliberately does NOT feed the ring. */
    KB_RUN("ksnprintf (format only, no ring, no serial)", 4000,
           g_sink = (uint64_t)ksnprintf(buf, sizeof buf,
                                        "[kbench] a representative log line %d %p\n",
                                        (int)_i, (void *)&g_sink));
    /* One whole kprintf: format + VGA + the 16550 + the per-CPU line buffer and
     * the ring's lock. 64 lines, because this one really does print. */
    {
        for (int r = 0; r < KB_REPS; r++) {
            uint64_t t0 = kb_rdtsc();
            for (int i = 0; i < 8; i++)
                kprintf("[kbench] log-cost probe %d\n", i);
            g_rep[r] = kb_rdtsc() - t0;
        }
        kb_report("kprintf one line (format + VGA + serial + ring)", 8);
    }
}

#endif /* KBENCH_MICRO */

/* --------------------------------------------------------------- the report */

/* ==========================================================================
 * WHO IS HOLDING THE BKL, sampled.
 *
 * The lock's own counters say how long it was held and how often it was
 * waited for. They do not say BY WHAT -- and after SYS_WAIT_EVENT deleted
 * 98% of the traffic, "by what" is the only question left: 6,549 ms of
 * waiting remained against 2,500 ms of holding, so the hold is concentrated
 * somewhere and widening the BKL-free syscall list is a guess until that
 * somewhere has a name.
 *
 * The sample is taken in the timer tick, which runs BEFORE the interrupt
 * entry acquires the lock -- so this observes the holder from outside,
 * without becoming a holder itself and without perturbing what it measures.
 * spinlock_t already records the acquiring caller's return address; this only
 * has to count them.
 *
 * Direct-mapped, 64 slots, no eviction policy: a hold that matters shows up
 * at hundreds of samples, and one that collides away was not the answer. The
 * addresses are printed raw and symbolised by the harness with nm, because a
 * kernel that could resolve its own symbols would need the table in memory
 * for the sake of a diagnostic.
 * ========================================================================== */
#define KB_BKLPROF_N 64
static struct { unsigned long ra; unsigned int hits; } g_bklprof[KB_BKLPROF_N];
static unsigned int g_bklprof_samples, g_bklprof_held, g_bklprof_lost;

void kb_bkl_sample(void)
{
    if (!g_kb_stat) return;
    g_bklprof_samples++;
    if (g_bkl_owner < 0) return;                 /* nobody: an idle machine */
    unsigned long ra = g_bkl.owner_ra;
    if (!ra) return;
    g_bklprof_held++;
    unsigned int i = (unsigned int)((ra >> 4) & (KB_BKLPROF_N - 1));
    for (unsigned int n = 0; n < 8; n++) {       /* short linear probe */
        unsigned int k = (i + n) & (KB_BKLPROF_N - 1);
        if (g_bklprof[k].ra == ra) { g_bklprof[k].hits++; return; }
        if (!g_bklprof[k].ra) { g_bklprof[k].ra = ra; g_bklprof[k].hits = 1; return; }
    }
    g_bklprof_lost++;                            /* said out loud, not dropped */
}

static void bklprof_report(void)
{
    if (!g_bklprof_samples) return;
    kprintf("[kbench] BKL holders: %u samples, held in %u (%u%%), %u lost to collision\n",
            g_bklprof_samples, g_bklprof_held,
            g_bklprof_samples ? g_bklprof_held * 100 / g_bklprof_samples : 0,
            g_bklprof_lost);
    for (int rank = 0; rank < 8; rank++) {
        int best = -1;
        for (int k = 0; k < KB_BKLPROF_N; k++)
            if (g_bklprof[k].hits && (best < 0 || g_bklprof[k].hits > g_bklprof[best].hits))
                best = k;
        if (best < 0) break;
        kprintf("[kbench]   bkl 0x%lx: %u samples (%u%% of held)\n",
                g_bklprof[best].ra, g_bklprof[best].hits,
                g_bklprof_held ? g_bklprof[best].hits * 100 / g_bklprof_held : 0);
        g_bklprof[best].hits = 0;
    }
}

void kb_stat_report(const char *tag)
{
    uint64_t acq = 0, cont = 0, wait = 0, hold = 0;
    uint64_t n[KB_NCLASS] = { 0 }, cyc[KB_NCLASS] = { 0 };
    int cores = 0;

    for (int c = 0; c < KB_MAXCPU; c++) {
        if (!g_kb[c].bkl_acq && !g_kb[c].n[KB_C_TIMER]) continue;
        cores++;
        acq  += g_kb[c].bkl_acq;
        cont += g_kb[c].bkl_contended;
        wait += g_kb[c].bkl_wait;
        hold += g_kb[c].bkl_hold;
        for (int k = 0; k < KB_NCLASS; k++) {
            n[k]   += g_kb[c].n[k];
            cyc[k] += g_kb[c].cyc[k];
        }
    }

    static const char *cn[KB_NCLASS] = { "syscall", "timer", "fault", "irq", "ipi" };

    kprintf("[kbench] --- kernel entry cost, %s (%d cores, TCG) ---\n", tag, cores);
    for (int k = 0; k < KB_NCLASS; k++) {
        if (!n[k]) continue;
        uint64_t per = ns_x1000(cyc[k], n[k]);
        kprintf("[kbench] entry %s: %d entries, mean %d.%03d ns each, %d ms total\n",
                cn[k], (int)n[k], (int)(per / 1000), (int)(per % 1000),
                (int)(cyc[k] / g_mhz / 1000));
    }

    /* THE ANSWER TO "file_read IS 25% OF AN IDLE MACHINE".
     *
     * The sampler cannot tell a halted core from a spinning one (it is an
     * ordinary interrupt; a halted core wakes to take it and reports the RIP
     * after the `hlt`). So the console wait counts its own awake cycles, and
     * this line prints them against the uptime: if the tty loop were burning a
     * quarter of a core, `awake` would be a quarter of the elapsed time. */
    {
        uint64_t wakes = 0, awake = 0;
        tty_wait_stats(&wakes, &awake);
        uint64_t up_ms = time_mono_ns() / NS_PER_MS;
        uint64_t awake_ms = awake / g_mhz / 1000;
        uint64_t per = wakes ? ns_x1000(awake, wakes) : 0;
        kprintf("[kbench] tty console wait: %d wakes, %d us awake in total "
                "(%d.%03d us each), %d ms uptime -- %d/1000 of ONE core\n",
                (int)wakes, (int)(awake / g_mhz), (int)(per / 1000000),
                (int)((per / 1000) % 1000), (int)up_ms,
                (int)(up_ms ? awake_ms * 1000 / up_ms : 0));
    }

    kprintf("[kbench] scheduler: %d context switches, %d threads parked now, "
            "%d poll passes through bkl_hlt_wait\n",
            (int)sched_switches(), (int)sched_blocked_count(),
            (int)sched_hlt_waits());

    /* The syscall histogram, biggest first. Printed as a top-N rather than in
     * full: 128 lines of mostly zeroes on a serial console at ~200 us a line is
     * four seconds of boot spent describing syscalls nobody made.
     *
     * SIXTEEN, not eight. run-kbench.sh reads SYS_GET_TIME's mean out of this
     * list, which makes the metric depend on the syscall's RANK -- a harness
     * asserting something about the histogram's shape that it did not mean to.
     * Widening the list does not fix that gate (SYS 10 is absent because it is
     * not called in the window at all, which is a separate and older problem)
     * but it removes rank from the question, and eight more lines cost 1.6 ms
     * of boot. */
    {
        uint64_t total_n = 0, total_c = 0;
        for (int s = 0; s < KB_NSYS; s++) { total_n += g_kb_sys_n[s]; total_c += g_kb_sys_cyc[s]; }
        bklprof_report();
    kprintf("[kbench] syscalls: %d calls, %d ms inside the dispatcher "
                "(BKL already held; blocking calls include time descheduled)\n",
                (int)total_n, (int)(total_c / g_mhz / 1000));
        for (int rank = 0; rank < 16; rank++) {
            int best = -1;
            for (int s = 0; s < KB_NSYS; s++)
                if (g_kb_sys_n[s] && (best < 0 || g_kb_sys_n[s] > g_kb_sys_n[best])) best = s;
            if (best < 0) break;
            uint64_t per = ns_x1000(g_kb_sys_cyc[best], g_kb_sys_n[best]);
            kprintf("[kbench]   SYS %d: %d calls (%d%%), mean %d.%03d ns in handler, %d ms total\n",
                    best, (int)g_kb_sys_n[best],
                    (int)(total_n ? g_kb_sys_n[best] * 100 / total_n : 0),
                    (int)(per / 1000), (int)(per % 1000),
                    (int)(g_kb_sys_cyc[best] / g_mhz / 1000));
            g_kb_sys_n[best] = 0;          /* consumed: this report runs once */
        }
    }

    /* The BKL. Two numbers decide whether it is the dominant cost: how much of
     * the wall clock the cores spent WAITING for it, and how much they spent
     * HOLDING it. Waiting is pure loss. Holding is not loss by itself -- it is
     * only loss to the extent another core wanted it -- so `contended` is what
     * turns the second number into an argument. */
    if (acq) {
        uint64_t wper = ns_x1000(wait, acq);
        uint64_t hper = ns_x1000(hold, acq);
        kprintf("[kbench] BKL: %d acquisitions, %d contended (%d%%); "
                "wait mean %d.%03d ns, hold mean %d.%03d ns\n",
                (int)acq, (int)cont, (int)(acq ? cont * 100 / acq : 0),
                (int)(wper / 1000), (int)(wper % 1000),
                (int)(hper / 1000), (int)(hper % 1000));
        kprintf("[kbench] BKL: %d ms spent waiting, %d ms spent holding, "
                "across %d cores\n",
                (int)(wait / g_mhz / 1000), (int)(hold / g_mhz / 1000), cores);
    }
}

/* ----------------------------------------------------------------- the thread */

static void kbench_thread(void)
{
    /* Late enough that the desktop is up and the shell has forked at least
     * once, so the path counters below describe a system doing its job. Early
     * enough to be finished before any boot harness gives up. */
    sched_sleep_ms(6000);

    g_mhz = time_source_hz(TIMESRC_TSC) / 1000000ull;
    if (!g_mhz) g_mhz = 1000;

#ifdef KBENCH_MICRO
    /* NOT IN THE DEFAULT BUILD, and the reason is a measured one. The battery
     * below is CPU-bound kernel code and it runs holding the big kernel lock,
     * so for its duration -- 270 ms, timed -- the desktop does not composite and
     * no other core enters the kernel. That is a 270 ms stall injected into
     * every boot of a tree that twenty other lines are running boot harnesses
     * against, to produce numbers only this line reads. An instrument that
     * changes the machine it measures is worth exactly what it costs everybody
     * else, so it is opt-in: `make KBENCH=1` (tests/kbench.mk), which is what
     * `make test-kbench-micro` does.
     *
     * The ACCOUNTING below this block stays in every build. It is the part that
     * costs nothing to collect (the counters are already there) and everything
     * to lose (nobody can re-derive what boot cost after boot has happened). */
    kprintf("[kbench] --- primitive cost, TSC %d MHz, %d reps each, TCG ---\n",
            (int)g_mhz, KB_REPS);

    bench_primitives();
    bench_pmm();
    bench_sched();
    bench_klog();
#endif

    /* Stop accounting and print what boot cost. Reading first and disarming
     * after would race the other three cores; disarming first means the last
     * few entries are missing, which is the harmless direction. */
    kb_stat_set(0);
    kb_stat_report("boot through desktop live");

    /* The fault path's own accounting, printed here because this is the one
     * place in the boot that asks what the whole kernel cost. Demand paging and
     * copy-on-write moved work off fork and onto these faults; this is the
     * other end of that trade. */
    mm_report("kbench");

    kprintf("KBENCH_DONE\n");
}

void kbench_start(void)
{
    kb_stat_set(1);                 /* account from the very first kernel entry */
    thread_create(kbench_thread, "kbench");
}
