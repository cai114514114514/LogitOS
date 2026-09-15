#ifndef LOGIT_KBENCH_H
#define LOGIT_KBENCH_H

#include <stdint.h>

/* ============================================================================
 * kbench -- what the kernel's own operations cost, in nanoseconds.
 *
 * WHY THIS IS NOT kprof
 * ---------------------
 * kprof (c/kernel/diag/kprof.c) answers "where is the machine right now?" and
 * "how long did ONE page load spend in TLS?". Both are questions about a
 * workload. This file answers a different one: "what does ONE syscall entry,
 * ONE context switch, ONE frame allocation cost, as a distribution, on a
 * machine that is otherwise doing its normal job?" A sampling profiler cannot
 * answer that -- at 1000 Hz a 200 ns operation is never sampled -- and a span
 * cannot either, because a span measures one occurrence and what is wanted here
 * is the median of ten thousand.
 *
 * So: kprof finds the hot function, kbench prices the primitive. Neither
 * replaces the other and this file deliberately does not sample anything.
 *
 * TWO HALVES
 * ----------
 *   MICROBENCHMARKS  a kernel thread, started from sched_init(), that runs a
 *                    fixed battery of timed loops once, ~6 s into the boot (so
 *                    the desktop is up and the numbers describe a live system
 *                    rather than an empty one), prints a table and exits.
 *                    Every line carries min / median / max over KB_REPS
 *                    repetitions, because the host running this emulator is
 *                    shared and a single sample is weather, not a measurement.
 *
 *   PATH COUNTERS    per-CPU accounting written by the two hot paths this line
 *                    owns: the BKL (c/kernel/cpu/spinlock.c) and the interrupt
 *                    entry (c/kernel/cpu/irq/interrupts.c). These measure the REAL
 *                    workload -- boot, desktop bring-up, the shell's forks --
 *                    which no synthetic loop can imitate.
 *
 * COST WHEN OFF
 * -------------
 * The path counters are behind `g_kb_stat`, and the disabled path is one load
 * of a global plus a branch the hardware predicts. Nothing else -- no rdtsc, no
 * call. The flag is turned ON at sched_init() and OFF again when the benchmark
 * thread prints, so the accounted window is exactly "boot through desktop
 * live", which is the window with the contention in it. After that the kernel
 * is back to its uninstrumented cost.
 *
 * WHY rdtsc AND NOT time_mono_ns()
 * --------------------------------
 * time_mono_ns() is itself one of the things being priced (the network stack
 * consults the clock constantly), so timing with it would fold the instrument
 * into the measurement. rdtsc is the primitive underneath it. Cycles are
 * converted to nanoseconds with the frequency time.c already calibrated, and
 * every number is labelled TCG, because under emulation a "cycle" is a unit of
 * host wall clock and not a unit of guest work.
 * ==========================================================================*/

#ifdef LOGIT_CPU_CAP_NEGCTL
#define KB_MAXCPU 8
#else
#define KB_MAXCPU 32         /* == PERCPU_MAXCPU */
#endif

/* Interrupt-entry classes. Coarse on purpose: the question is "what does
 * getting into and out of the kernel cost", not "which syscall is slow" --
 * kprof answers the second one. */
#define KB_C_SYSCALL 0       /* int 0x80 */
#define KB_C_TIMER   1       /* vector 32, the 100 Hz preemption tick */
#define KB_C_FAULT   2       /* vectors < 32, i.e. #PF and friends */
#define KB_C_IRQ     3       /* every other device IRQ */
#define KB_C_IPI     4       /* vectors 240/241, the BKL-free ones */
#define KB_NCLASS    5

/* One cache line per core: these are written on every kernel entry from four
 * cores at once, and sharing a line between them would make the instrument's
 * own coherence traffic part of what it reports. */
struct kb_cpu {
    uint64_t n[KB_NCLASS];   /* kernel entries by class */
    uint64_t cyc[KB_NCLASS]; /* cycles from entry to exit, by class */
    uint64_t _pad[64 / 8 - 5];
} __attribute__((aligned(64)));

/* Correction: five count/cycle pairs occupy two cache lines, not one.
 * Alignment still keeps adjacent CPU records on separate cache lines. */
extern struct kb_cpu g_kb[KB_MAXCPU];

/* Per-syscall-number accounting. "1.3 million syscalls during boot" is a
 * finding; "which one" is what makes it actionable, and a class total cannot
 * say. Indexed by the raw ABI number (include/abi/logit_abi.h tops out at 94).
 * `cyc` is measured INSIDE the dispatcher, with the BKL already held, so it is
 * the handler's own cost and not the queueing in front of it -- the two are
 * separated on purpose, because only one of them is fixable by making the
 * handler faster. */
/* Correction (2026-09-10): the ABI already reaches 189, and no BKL remains.
 * Each CPU owns a separate 64-byte-aligned 4096-byte histogram. Record at EXIT
 * after disabling IRQs and re-reading this_cpu(): blocking handlers can migrate.
 * With one writer per CPU, relaxed atomic loads/stores need no locked RMW;
 * atomic readers can sample the live table without a C data race. */
#define KB_NSYS 256
struct kb_sys_cpu {
    uint64_t n[KB_NSYS];
    uint64_t cyc[KB_NSYS];
} __attribute__((aligned(64)));
extern struct kb_sys_cpu g_kb_sys[KB_MAXCPU];
struct kb_sys_snapshot { uint64_t n[KB_NSYS], cyc[KB_NSYS]; };
_Static_assert(sizeof(struct kb_sys_cpu) % 64 == 0, "CPU histograms must not share a cache line");
_Static_assert(sizeof(struct kb_sys_snapshot) == 4096, "report snapshot must stay one page");

/* Caller: IRQs disabled on this CPU, no scheduling inside either record helper.
 * NMI handlers do not record here. No other CPU may write this CPU's shard. */
static inline void kb_sys_record(unsigned cpu, uint64_t nr, uint64_t cycles)
{
    if (cpu >= KB_MAXCPU || nr >= KB_NSYS) return;
    struct kb_sys_cpu *s = &g_kb_sys[cpu];
    __atomic_store_n(&s->n[nr], __atomic_load_n(&s->n[nr], __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->cyc[nr], __atomic_load_n(&s->cyc[nr], __ATOMIC_RELAXED) + cycles, __ATOMIC_RELAXED);
}
static inline void kb_entry_record(unsigned cpu, unsigned cls, uint64_t cycles)
{
    if (cpu >= KB_MAXCPU || cls >= KB_NCLASS) return;
    struct kb_cpu *s = &g_kb[cpu];
    __atomic_store_n(&s->n[cls], __atomic_load_n(&s->n[cls], __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->cyc[cls], __atomic_load_n(&s->cyc[cls], __ATOMIC_RELAXED) + cycles, __ATOMIC_RELAXED);
}

/* Cumulative, non-destructive snapshot. While writers are active, individual
 * scalar reads may belong to adjacent instants; disarming prevents new timed
 * entries but does not wait for old blocking calls to return. No claim of a
 * globally simultaneous sample is made. Sorting may change only this copy. */
static inline void kb_sys_snapshot_read(struct kb_sys_snapshot *out)
{
    for (unsigned n = 0; n < KB_NSYS; n++) {
        uint64_t count = 0, cycles = 0;
        for (unsigned c = 0; c < KB_MAXCPU; c++) {
            count += __atomic_load_n(&g_kb_sys[c].n[n], __ATOMIC_RELAXED);
            cycles += __atomic_load_n(&g_kb_sys[c].cyc[n], __ATOMIC_RELAXED);
        }
        out->n[n] = count; out->cyc[n] = cycles;
    }
}

/* THE DISABLED PATH: one load, one predicted-not-taken branch. Nothing may be
 * added here -- the whole claim that these numbers describe the uninstrumented
 * kernel rests on the instrumented kernel being the same kernel when it is off. */
extern int g_kb_stat;
static inline int kb_stat_enabled(void)
{ return __atomic_load_n(&g_kb_stat, __ATOMIC_RELAXED) != 0; }

static inline uint64_t kb_rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    /* HOST BUILDS ONLY (arm64 macOS is where this tree's gates run). The real
     * kernel is x86-64 and takes the branch above; this arm exists so a host
     * test can compile a TU that includes this header -- file.c, for
     * tests/unit/storage_test.c -- without the rdtsc constraint killing the
     * build. On the host this is a nanosecond clock, not cycles: nothing host-
     * side reads the value as anything but "later than before" (file.c's tty
     * wait accounting, which the host test never drives), so a different unit
     * changes no measured claim. Added by the storage wave with the host gate
     * in the same commit, per the same-commit rule. */
#include <time.h>              /* host only; the kernel branch needs nothing */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void kbench_start(void);          /* spawn the benchmark thread (sched_init) */
void kb_stat_set(int on);         /* arm/disarm the path counters */
void kb_stat_report(const char *tag);

/* Sample who holds the BKL. Called from the timer tick, which runs BEFORE
 * the interrupt entry takes the lock -- so it observes the holder from
 * outside instead of becoming one. No-op unless the accounting is armed. */
/* BKL holder sampling removed with the global lock. */

#endif /* LOGIT_KBENCH_H */
