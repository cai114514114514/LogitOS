#ifndef LOGIT_KTIME_H
#define LOGIT_KTIME_H

#include <stdint.h>

/* ============================================================================
 * LogitOS time subsystem: a clock model, a clocksource, and timers.
 *
 * WHAT WAS HERE BEFORE
 * --------------------
 * A 100 Hz PIT incrementing a global (`timer_ticks()`), a CMOS read for the
 * wall clock, and nothing else. Every "happen later" in the tree was spelled
 * `while (timer_ticks() - t0 < N)`. That is not a stylistic complaint: it is
 * why the PIT running at exactly twice its programmed rate went unnoticed for
 * the life of the kernel (64a16c2). A duration expressed in ticks has no unit
 * a test can check, so a clock that is 2x fast agrees with itself everywhere.
 *
 * THE MODEL
 * ---------
 * Two clocks, both in NANOSECONDS, and one defined relationship between them:
 *
 *   MONOTONIC (time_mono_ns)  ns since time_init(). Never decreases -- not
 *                             across a clocksource switch, not across cores
 *                             with unsynchronised TSCs, not if someone winds
 *                             the CMOS clock backwards. This is the ONLY clock
 *                             you may subtract. Every timeout, retransmit,
 *                             animation and deadline uses it.
 *
 *   REALTIME (time_real_ns)   ns since the Unix epoch. Defined as
 *                                 real = mono + g_wall_offset
 *                             where g_wall_offset is latched ONCE from the RTC
 *                             (time_wall_sync) and re-latched only when
 *                             someone sets the wall clock. So realtime inherits
 *                             the monotonic clock's resolution -- the RTC's
 *                             one-second step is a starting POINT, not the
 *                             granularity -- and the two clocks can never drift
 *                             apart, because there is only one oscillator.
 *
 * Nanoseconds are unsigned 64-bit: 2^64 ns is ~584 years of uptime, so the
 * monotonic clock cannot wrap in any life this machine has. The CYCLE counters
 * underneath it can and do wrap (a 32-bit PIT-derived counter wraps in ~497
 * days at 100 Hz; a TSC delta multiplied by a Q32 factor overflows 64 bits in
 * about an hour at 3 GHz) -- see time.c's `ns_of_cycles` for how both are
 * handled rather than hoped about.
 *
 * WHY NOT "TICKS"
 * ---------------
 * `timer_ticks()` still exists and still means exactly what it meant before
 * this change: 100 Hz PIT interrupts since boot. Nothing that consumed it
 * changed meaning -- that was a deliberate constraint, because the last time
 * the meaning of a tick moved, every timeout in the tree silently doubled and
 * an unreachable host froze the machine (7131e34). New code should use the ns
 * clock; old code keeps working unchanged.
 * ==========================================================================*/

/* ---- clock ids ------------------------------------------------------------
 * Deliberately the POSIX numbers mini-libc's <time.h> already uses, so
 * clock_gettime(CLOCK_MONOTONIC) forwards its argument straight through. */
#define KCLOCK_REALTIME            0
#define KCLOCK_MONOTONIC           1
#define KCLOCK_PROCESS_CPUTIME_ID  2
#define KCLOCK_THREAD_CPUTIME_ID   3
#define KCLOCK_MONOTONIC_RAW       4    /* unclamped source reading; may step back */
#define KCLOCK_BOOTTIME            7    /* == MONOTONIC (we never suspend) */

#define NS_PER_SEC  1000000000ull
#define NS_PER_MS   1000000ull
#define NS_PER_US   1000ull

/* ---- the clocks ---------------------------------------------------------- */

/* Monotonic nanoseconds since time_init(). Never decreases, for any caller, on
 * any core, in any order. Safe from interrupt context and with IF either way
 * (unlike timer_ticks(), it does NOT stop advancing while interrupts are off:
 * the TSC keeps counting, so a kernel loop with IF=0 can still time out). */
uint64_t time_mono_ns(void);
uint64_t time_mono_us(void);
uint64_t time_mono_ms(void);

/* The same instant without the cross-core monotonicity clamp. Only for
 * diagnostics -- it is the number the clamp is protecting you from. */
uint64_t time_mono_raw_ns(void);

/* Wall clock, ns since the Unix epoch. */
uint64_t time_real_ns(void);
int64_t  time_real_unix(void);          /* whole seconds, the x509/http `now` */

/* Re-latch the wall offset from the RTC. Called once by time_init(); calling it
 * again is how you would implement settimeofday. Never touches the monotonic
 * clock -- that is the entire point of having two. */
void time_wall_sync(void);

/* POSIX-shaped read. Returns 0, or -1 for an unknown id. */
int time_clock_gettime(int clock_id, int64_t *sec, int64_t *nsec);
/* Resolution of `clock_id` in ns, or 0 if unknown. */
uint64_t time_clock_res_ns(int clock_id);

/* ---- clocksource --------------------------------------------------------- */

#define TIMESRC_TSC  0      /* rdtsc, calibrated; ~ns resolution */
#define TIMESRC_PIT  1      /* the 100 Hz tick counter; 10 ms resolution */
#define TIMESRC_N    2

/* Switch the clocksource at runtime. The monotonic clock is CONTINUOUS across
 * the switch (the new source is re-based onto the current mono value), so this
 * is safe on a live system and is how the PIT fallback gets exercised on real
 * hardware instead of only in theory. Returns 0, or -1 if the source is
 * unavailable. */
int         time_set_source(int src);
int         time_get_source(void);
const char *time_source_name(int src);
uint64_t    time_source_hz(int src);        /* calibrated frequency, Hz */
uint32_t    time_source_res_ns(int src);    /* smallest step it can report */

/* One line, at boot, naming the source, its calibrated frequency and its
 * resolution -- the "130 roots, 0 skipped" of the clock. When time is wrong on
 * unfamiliar hardware this line is the whole diagnosis. */
void time_report(void);

/* ---- timers -------------------------------------------------------------- */

struct ktimer;
typedef void (*ktimer_fn)(struct ktimer *t);

struct ktimer {
    uint64_t  expires_ns;    /* monotonic deadline */
    uint64_t  period_ns;     /* 0 = one-shot; else re-armed to expires+period */
    ktimer_fn fn;            /* runs in INTERRUPT context -- see the note below */
    void     *arg;
    int       heap_idx;      /* position in the heap, -1 = not queued */
    uint32_t  seq;           /* bumped on every arm; lets a stale cancel no-op */
    const char *name;
};

/* Arm `t` to fire `delay_ns` from now, repeating every `period_ns` (0 = one
 * shot). Re-arming an already-queued timer moves it. Returns 0, or -1 if the
 * heap is full.
 *
 * THE CALLBACK RUNS IN INTERRUPT CONTEXT, from the timer tick, with the timer
 * lock DROPPED (so a callback may arm/cancel timers, including its own) and
 * WITHOUT the big kernel lock -- exactly like timer_tick() itself, and for the
 * same reason: a deadline must never queue behind a lock held by a thread that
 * is waiting for that deadline. A callback must not block, must not take the
 * BKL, and must not touch the heap. Wake a thread and get out. */
int ktimer_add(struct ktimer *t, uint64_t delay_ns, uint64_t period_ns,
               ktimer_fn fn, void *arg, const char *name);
/* Remove `t`. Returns 1 if it was queued (so it definitely will not fire
 * again), 0 if it had already fired or was never armed. */
int ktimer_cancel(struct ktimer *t);
/* Deadline of the earliest queued timer, or 0 if none. What a tickless idle
 * would program the next one-shot to. */
uint64_t ktimer_next_ns(void);
int      ktimer_queued(void);           /* timers currently armed */
uint64_t ktimer_fired(void);            /* callbacks run since boot */

/* Expire everything due at `now_ns`. Called from timer_tick(); exposed for the
 * host tests, which drive it with a fake clock. */
void ktimer_run(uint64_t now_ns);

/* ---- per-process / per-thread CPU time ----------------------------------- */

/* Nanoseconds of CPU observed for `pid` (-1 = the running process), split into
 * ring-3 and ring-0 time. Any pointer may be NULL. Returns 0, or -1 if the pid
 * has no accounting record. */
int time_cpu_ns(int pid, uint64_t *user_ns, uint64_t *sys_ns);
/* Total CPU time attributed to threads, and how much fell on the idle path. */
uint64_t time_cpu_total_ns(void);

/* The exact hook the scheduler should call from its context switch, which is
 * the only place that can attribute CPU time without sampling error. Until it
 * is wired, accounting is tick-sampled (see time.c). Passing the outgoing and
 * incoming pids; either may be -1 for "not a process". */
void time_account_switch(int old_pid, int new_pid);

/* ---- init + tick --------------------------------------------------------- */

/* Calibrates the clocksource and prints time_report(). Called from pit_init()
 * -- NOT from kmain.c, so that owning the timer driver is enough to own the
 * clock's initialisation order. Idempotent. */
void time_init(void);
int  time_ready(void);

/* Called from timer_tick() on every PIT interrupt: folds the cycle counter into
 * the ns accumulator, runs due timers, and samples CPU accounting. */
void time_tick(void);

/* A safe point in THREAD context (interrupts on, no timer lock held) at which
 * deferred work -- the on-device self-tests -- may start kernel threads.
 * Called from timer_ticks() only when g_time_deferred says there is something
 * to do, so the hot path costs one predicted-not-taken branch on a global. */
extern volatile int g_time_deferred;
void time_safepoint(void);

/* ---- the calibration cross-check ----------------------------------------- *
 *
 * The negative control for the 2x PIT bug, factored out as a PURE function so
 * the host tests can feed it a tick that runs at twice its programmed rate and
 * assert that it is rejected. `mono_ns` is what our clock says elapsed;
 * `rtc_seconds` is what an INDEPENDENT oscillator (the CMOS RTC) says elapsed.
 * Returns the signed error in parts-per-thousand, and sets *ok to 0 if that
 * error is outside the tolerance for the given number of whole RTC seconds.
 *
 * The RTC reports whole seconds, so N observed seconds means the true interval
 * is in (N-1, N+1): the tolerance has to widen as the sample shortens, which is
 * why this takes the second count rather than a fixed percentage. */
int time_xcheck(uint64_t mono_ns, uint64_t rtc_seconds, int *ok);

/* Diagnostics the boot tests read back. */
uint64_t time_mono_backsteps(void);     /* times the clamp caught a backward source */
uint64_t time_mono_backstep_max_ns(void);
uint64_t time_mono_reads(void);
uint32_t time_cores_seen(void);         /* bitmap of core indices that read the clock */

#ifdef LOGIT_TIME_HOST
/* Host-test knobs. The unit tests drive the REAL implementation with a fake
 * cycle counter -- including a deliberately narrow one, so the counter-wrap
 * path is executed rather than reasoned about. */
void     time_host_reset(uint64_t hz, uint64_t mask);
void     time_host_set_cycles(uint64_t c);
void     time_host_set_ticks(uint64_t t);
void     time_host_set_rtc_second(int s);
uint64_t time_host_base_ns(void);
int      time_host_heap_ok(void);
#endif

#endif /* LOGIT_KTIME_H */
