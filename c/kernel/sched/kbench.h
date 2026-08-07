#ifndef LOGIT_KBENCH_H
#define LOGIT_KBENCH_H

#include <stdint.h>

/* ============================================================================
 * kbench -- what the kernel's own operations cost, in nanoseconds.
 *
 * WHY THIS IS NOT kprof
 * ---------------------
 * kprof (c/kernel/core/kprof.c) answers "where is the machine right now?" and
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
 *                    entry (c/kernel/cpu/interrupts.c). These measure the REAL
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

#define KB_MAXCPU 8          /* == PERCPU_MAXCPU */

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
    uint64_t bkl_acq;        /* BKL acquisitions by this core */
    uint64_t bkl_contended;  /* ...that had to wait for another core */
    uint64_t bkl_wait;       /* cycles spun waiting for the BKL */
    uint64_t bkl_hold;       /* cycles the BKL was held by this core */
    uint64_t bkl_t0;         /* scratch: this core's acquire timestamp */
    uint64_t n[KB_NCLASS];   /* kernel entries by class */
    uint64_t cyc[KB_NCLASS]; /* cycles from entry to exit, by class */
    uint64_t _pad[64 / 8 - 5];
} __attribute__((aligned(64)));

extern struct kb_cpu g_kb[KB_MAXCPU];

/* Per-syscall-number accounting. "1.3 million syscalls during boot" is a
 * finding; "which one" is what makes it actionable, and a class total cannot
 * say. Indexed by the raw ABI number (include/abi/logit_abi.h tops out at 94).
 * `cyc` is measured INSIDE the dispatcher, with the BKL already held, so it is
 * the handler's own cost and not the queueing in front of it -- the two are
 * separated on purpose, because only one of them is fixable by making the
 * handler faster. */
#define KB_NSYS 128
extern uint64_t g_kb_sys_n[KB_NSYS];
extern uint64_t g_kb_sys_cyc[KB_NSYS];

/* THE DISABLED PATH: one load, one predicted-not-taken branch. Nothing may be
 * added here -- the whole claim that these numbers describe the uninstrumented
 * kernel rests on the instrumented kernel being the same kernel when it is off. */
extern volatile int g_kb_stat;

static inline uint64_t kb_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void kbench_start(void);          /* spawn the benchmark thread (sched_init) */
void kb_stat_set(int on);         /* arm/disarm the path counters */
void kb_stat_report(const char *tag);

#endif /* LOGIT_KBENCH_H */
