/* Host unit tests for the time subsystem: the timer heap, the nanosecond
 * arithmetic, counter wrap, the monotonic clamp, and the calibration
 * cross-check including a NEGATIVE CONTROL that fails without the guard.
 *
 * These drive c/kernel/core/ktime.c itself, compiled with -DLOGIT_TIME_HOST so
 * the cycle counter is settable, rather than a reimplementation of it. A test
 * of a copy of the code proves things about the copy.
 *
 *   cc -DLOGIT_TIME_HOST -o time_test tests/unit/time_test.c c/kernel/core/ktime.c
 *
 * make test-time-host
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ktime.h"

static int checks, fails;

#define CHECK(cond, ...) do {                                              \
    checks++;                                                              \
    if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);  \
                   printf(__VA_ARGS__); printf("\n"); }                    \
} while (0)

/* ---------------------------------------------------------------- helpers */

static int fired[256];
static int fire_order[256], nfired;
static void on_fire(struct ktimer *t)
{
    int i = (int)(long)t->arg;
    fired[i]++;
    if (nfired < 256) fire_order[nfired++] = i;
}

static void advance_ns(uint64_t ns)
{
    /* 1 GHz fake counter: one cycle is one nanosecond, so the tests can talk in
     * nanoseconds without a conversion of their own getting in the way. */
    static uint64_t cyc;
    cyc += ns;
    time_host_set_cycles(cyc);
}

static void reset(void)
{
    time_host_reset(1000000000ull, ~0ull);
    memset(fired, 0, sizeof fired);
    nfired = 0;
    advance_ns(0);
}

/* ------------------------------------------------------------ 1. the heap */

static void test_heap_order(void)
{
    reset();
    static struct ktimer t[8];
    /* Armed out of order on purpose. */
    int delays[8] = { 500, 100, 800, 300, 200, 700, 400, 600 };
    for (int i = 0; i < 8; i++)
        CHECK(ktimer_add(&t[i], (uint64_t)delays[i], 0, on_fire, (void *)(long)i, "t") == 0,
              "arm %d failed", i);
    CHECK(ktimer_queued() == 8, "queued=%d want 8", ktimer_queued());
    CHECK(time_host_heap_ok(), "heap invariant broken after 8 inserts");
    CHECK(ktimer_next_ns() == 100, "next=%llu want 100",
          (unsigned long long)ktimer_next_ns());

    ktimer_run(1000);
    CHECK(nfired == 8, "fired %d want 8", nfired);
    /* Deadline order, not insertion order. */
    int want[8] = { 1, 4, 3, 6, 0, 7, 5, 2 };
    for (int i = 0; i < 8 && i < nfired; i++)
        CHECK(fire_order[i] == want[i], "slot %d fired %d want %d", i, fire_order[i], want[i]);
    CHECK(ktimer_queued() == 0, "queue not drained: %d", ktimer_queued());
}

static void test_heap_same_deadline(void)
{
    /* Many timers on ONE deadline is the normal case (everything armed in one
     * pass of a poll loop), and the failure it hides is a run loop that expires
     * only the head. */
    reset();
    static struct ktimer t[64];
    for (int i = 0; i < 64; i++)
        ktimer_add(&t[i], 5000, 0, on_fire, (void *)(long)i, "same");
    CHECK(ktimer_queued() == 64, "queued=%d", ktimer_queued());

    ktimer_run(4999);
    CHECK(nfired == 0, "fired %d before the deadline", nfired);
    ktimer_run(5000);
    CHECK(nfired == 64, "fired %d of 64 at the deadline", nfired);
    for (int i = 0; i < 64; i++) CHECK(fired[i] == 1, "timer %d fired %d times", i, fired[i]);
    CHECK(ktimer_queued() == 0, "queue not drained: %d", ktimer_queued());
}

static void test_heap_cancel(void)
{
    reset();
    static struct ktimer t[32];
    for (int i = 0; i < 32; i++)
        ktimer_add(&t[i], (uint64_t)(i + 1) * 100, 0, on_fire, (void *)(long)i, "c");

    /* Cancel from the MIDDLE of the heap, not the head or the tail: the
     * replacement element comes from the end of the array and may have to move
     * either up or down. A sift in one direction only leaves the heap subtly
     * mis-ordered, and the symptom is a timer that fires late by an unbounded
     * amount, which is not a symptom anyone traces back to a sift. */
    for (int i = 0; i < 32; i += 2)
        CHECK(ktimer_cancel(&t[i]) == 1, "cancel %d reported not-queued", i);
    CHECK(time_host_heap_ok(), "heap invariant broken after 16 middle cancels");
    CHECK(ktimer_queued() == 16, "queued=%d want 16", ktimer_queued());

    /* Cancelling twice is a no-op, not a corruption. */
    CHECK(ktimer_cancel(&t[0]) == 0, "second cancel claimed it was queued");

    ktimer_run(100000);
    CHECK(nfired == 16, "fired %d want 16", nfired);
    for (int i = 0; i < 32; i++)
        CHECK(fired[i] == (i % 2), "timer %d fired %d (parity)", i, fired[i]);
    /* The survivors must still have come out in deadline order. */
    for (int i = 1; i < nfired; i++)
        CHECK(fire_order[i] > fire_order[i-1], "order broken at %d", i);
}

static void test_heap_full(void)
{
    reset();
    static struct ktimer t[200];
    int ok = 0, refused = 0;
    for (int i = 0; i < 200; i++) {
        if (ktimer_add(&t[i], 1000, 0, on_fire, (void *)(long)i, "f") == 0) ok++;
        else refused++;
    }
    CHECK(ok == 128, "accepted %d, want the documented 128", ok);
    CHECK(refused == 72, "refused %d, want 72", refused);
    CHECK(time_host_heap_ok(), "heap invariant broken when full");
    /* Refusal must be reported, not silently dropped: a timer that was never
     * armed and whose caller was not told is a deadline that never happens. */
    ktimer_run(2000);
    CHECK(nfired == 128, "fired %d want 128", nfired);
}

static void test_periodic(void)
{
    reset();
    static struct ktimer p;
    ktimer_add(&p, 100, 100, on_fire, (void *)(long)0, "per");
    for (uint64_t now = 100; now <= 1000; now += 100) ktimer_run(now);
    CHECK(fired[0] == 10, "periodic fired %d want 10", fired[0]);

    /* Catch-up policy: a long interrupts-off window must not produce a BURST of
     * back-dated firings. One late firing, then back on cadence. */
    reset();
    ktimer_add(&p, 100, 100, on_fire, (void *)(long)0, "per");
    ktimer_run(100000);
    CHECK(fired[0] == 1, "a 100us gap produced %d firings, want 1", fired[0]);
    CHECK(ktimer_next_ns() == 100100, "re-armed to %llu, want now+period",
          (unsigned long long)ktimer_next_ns());

    CHECK(ktimer_cancel(&p) == 1, "periodic cancel failed");
    ktimer_run(1000000);
    CHECK(fired[0] == 1, "cancelled periodic fired again (%d)", fired[0]);
}

static struct ktimer re_t;
static int re_count;
static void rearm_cb(struct ktimer *t)
{
    /* A callback arming a timer -- including itself -- must not deadlock on the
     * timer lock. ktimer_run drops it before the call for exactly this. */
    if (++re_count < 5) ktimer_add(t, 10, 0, rearm_cb, 0, "re");
}
static void test_callback_rearm(void)
{
    reset();
    re_count = 0;
    ktimer_add(&re_t, 10, 0, rearm_cb, 0, "re");
    for (uint64_t now = 10; now <= 100; now += 10) ktimer_run(now);
    CHECK(re_count == 5, "self-rearming callback ran %d times, want 5", re_count);
}

/* --------------------------------------------- 2. nanosecond arithmetic */

static void test_ns_conversion(void)
{
    reset();
    advance_ns(1234567);
    time_tick();
    CHECK(time_mono_ns() == 1234567, "1 GHz counter: %llu want 1234567",
          (unsigned long long)time_mono_ns());

    /* A frequency that does NOT divide a nanosecond evenly is where a naive
     * cycles*1000000000/hz either overflows or rounds to nothing. */
    time_host_reset(3417622909ull, ~0ull);
    time_host_set_cycles(3417622909ull);        /* exactly one second of cycles */
    time_tick();
    uint64_t ns = time_mono_ns();
    CHECK(ns > 999900000ull && ns < 1000100000ull,
          "3.417 GHz: one second of cycles read as %lluns", (unsigned long long)ns);

    /* One hour at 3.4 GHz is 1.2e13 cycles. cycles*mult in 64 bits overflows at
     * about 1.3e13, which is where the obvious implementation silently starts
     * returning garbage; the 128-bit product does not. */
    time_host_reset(3417622909ull, ~0ull);
    time_host_set_cycles(3417622909ull * 3600ull);
    time_tick();
    ns = time_mono_ns();
    CHECK(ns > 3599900000000ull && ns < 3600100000000ull,
          "one hour of cycles read as %lluns (want ~3.6e12)", (unsigned long long)ns);

    /* And a full day, which is past 2^63 nanoseconds of nothing but still well
     * inside the range the ABI promises. */
    time_host_reset(3417622909ull, ~0ull);
    time_host_set_cycles(3417622909ull * 86400ull);
    time_tick();
    ns = time_mono_ns();
    CHECK(ns > 86390000000000ull && ns < 86410000000000ull,
          "one day of cycles read as %lluns", (unsigned long long)ns);
}

static void test_counter_wrap(void)
{
    /* THE 32-BIT CASE. 64-bit nanoseconds wrap in 584 years and can be ignored;
     * a 32-bit cycle counter at 1 GHz wraps every 4.29 SECONDS, and the whole
     * point of the masked, sign-extended delta is that the fold does not care.
     *
     * The failure being excluded: an unsigned subtraction across the wrap gives
     * ~2^32 cycles instead of a small number, i.e. the clock jumps four seconds
     * forward every four seconds, and every deadline in the machine expires. */
    time_host_reset(1000000000ull, 0xFFFFFFFFull);
    uint64_t total = 0;
    for (int i = 0; i < 100; i++) {           /* 100 x 100ms = 10 s, i.e. 2+ wraps */
        total += 100000000ull;
        time_host_set_cycles(total);
        time_tick();
    }
    uint64_t ns = time_mono_ns();
    CHECK(ns > 9990000000ull && ns < 10010000000ull,
          "10s across two 32-bit counter wraps read as %lluns", (unsigned long long)ns);

    /* A 16-bit counter wraps every 65 us; folding at a 10 ms tick would be far
     * too slow and the delta becomes ambiguous. Verified explicitly so the
     * limit is a known one rather than a surprise: fold faster than half the
     * counter's period, or do not use the counter. */
    time_host_reset(1000000000ull, 0xFFFFull);
    total = 0;
    for (int i = 0; i < 1000; i++) {          /* fold every 30 us: inside the limit */
        total += 30000ull;
        time_host_set_cycles(total);
        time_tick();
    }
    ns = time_mono_ns();
    CHECK(ns > 29900000ull && ns < 30100000ull,
          "16-bit counter folded every 30us read as %lluns want ~30ms",
          (unsigned long long)ns);
}

static void test_source_steps_backwards(void)
{
    /* MECHANISM ONE: the signed delta. A counter that reads lower than the last
     * fold produces a NEGATIVE delta, which is clamped to zero.
     *
     * The bug this excludes is the one that makes the whole subsystem
     * unusable: an unsigned masked subtraction across a 1 ms backward step
     * yields 2^64 - 10^6 cycles, and the clock reports 584 thousand years of
     * uptime in one read. Every deadline in the machine expires at once. */
    time_host_reset(1000000000ull, ~0ull);
    time_host_set_cycles(1000000000ull);
    time_tick();
    uint64_t base = time_mono_ns();

    time_host_set_cycles(999000000ull);          /* the source loses 1 ms */
    uint64_t raw = time_mono_raw_ns();
    CHECK(raw == base, "backward source read as %lluns, want the last fold %lluns",
          (unsigned long long)raw, (unsigned long long)base);
    CHECK(raw < 2000000000ull, "backward source EXPLODED to %lluns",
          (unsigned long long)raw);
}

static void test_monotonic_clamp(void)
{
    /* MECHANISM TWO: the cross-core clamp, and this is the case the signed
     * delta cannot fix. Core A's TSC runs 400 ms ahead of core B's. A reads,
     * publishing 2.0 s. B then reads its own (slower) counter against the same
     * fold point and computes 1.6 s -- a perfectly well-formed positive delta,
     * and 400 ms in the past. Subtracting those two in either order gives a
     * caller a negative interval. */
    time_host_reset(1000000000ull, ~0ull);
    time_host_set_cycles(1500000000ull);
    time_tick();                                  /* fold at 1.5 s */

    time_host_set_cycles(2000000000ull);          /* "core A", fast */
    uint64_t fast = time_mono_ns();
    CHECK(fast > 1990000000ull && fast < 2010000000ull,
          "core A read %lluns, want ~2s", (unsigned long long)fast);
    CHECK(time_mono_backsteps() == 0, "clamped before anything went backwards");

    time_host_set_cycles(1600000000ull);          /* "core B", 400 ms behind */
    uint64_t slow = time_mono_ns();
    CHECK(slow == fast, "core B saw %lluns after core A published %lluns "
                        "-- the clock went backwards",
          (unsigned long long)slow, (unsigned long long)fast);
    CHECK(time_mono_backsteps() >= 1, "the clamp did not COUNT the regression");
    CHECK(time_mono_backstep_max_ns() > 390000000ull &&
          time_mono_backstep_max_ns() < 410000000ull,
          "backstep recorded as %lluns, want ~400ms",
          (unsigned long long)time_mono_backstep_max_ns());

    /* RAW is allowed to be smaller -- that is what makes it the diagnostic. */
    CHECK(time_mono_raw_ns() < slow, "raw clock was clamped too");

    /* And it recovers: once the source passes the clamped value the clock
     * advances again rather than sticking at the high-water mark forever. */
    time_host_set_cycles(3000000000ull);
    CHECK(time_mono_ns() > fast + 900000000ull, "clock stuck after a clamp");
}

static void test_wall_vs_mono(void)
{
    /* The defined relationship: real - mono is a constant. Reading both
     * across an interval must move them by the same amount. */
    time_host_reset(1000000000ull, ~0ull);
    time_host_set_cycles(1000000000ull);
    time_tick();
    uint64_t m0 = time_mono_ns(), r0 = time_real_ns();
    time_host_set_cycles(3000000000ull);
    time_tick();
    uint64_t m1 = time_mono_ns(), r1 = time_real_ns();
    CHECK(m1 - m0 == r1 - r0, "wall and mono drifted apart: %llu vs %llu",
          (unsigned long long)(m1 - m0), (unsigned long long)(r1 - r0));
}

static void test_clock_ids(void)
{
    time_host_reset(1000000000ull, ~0ull);
    time_host_set_cycles(2500000000ull);
    time_tick();
    int64_t s = -1, ns = -1;
    CHECK(time_clock_gettime(KCLOCK_MONOTONIC, &s, &ns) == 0, "MONOTONIC rejected");
    CHECK(s == 2, "MONOTONIC sec=%lld want 2", (long long)s);
    CHECK(ns > 400000000 && ns < 600000000, "MONOTONIC nsec=%lld want ~5e8", (long long)ns);
    CHECK(ns < 1000000000, "nsec field must be < 1e9, got %lld", (long long)ns);
    CHECK(time_clock_gettime(999, &s, &ns) == -1, "an unknown clock id was accepted");
    CHECK(time_clock_res_ns(KCLOCK_MONOTONIC) == 1,
          "1 GHz source reports %lluns resolution",
          (unsigned long long)time_clock_res_ns(KCLOCK_MONOTONIC));
}

static void test_source_switch(void)
{
    /* Switching the clocksource must not move the clock. If it did, the PIT
     * fallback could never be exercised on a live machine, which is how a
     * fallback ends up never having run. */
    time_host_reset(1000000000ull, ~0ull);
    time_host_set_cycles(5000000000ull);
    time_tick();
    uint64_t before = time_mono_ns();
    CHECK(time_set_source(TIMESRC_PIT) == 0, "switch to the PIT source refused");
    uint64_t after = time_mono_ns();
    CHECK(after >= before, "the clock went backwards across a source switch");
    CHECK(after - before < 1000000ull, "the clock JUMPED %lluns across a switch",
          (unsigned long long)(after - before));
    CHECK(time_get_source() == TIMESRC_PIT, "source did not change");
    CHECK(strcmp(time_source_name(TIMESRC_PIT), "pit") == 0, "source name wrong");
    CHECK(time_set_source(TIMESRC_TSC) == 0, "switch back refused");
    CHECK(time_mono_ns() >= after, "the clock went backwards switching back");
}

/* ----------------------------------------- 3. the 2x negative control */

static void test_xcheck_negative_control(void)
{
    int ok;

    /* A clock that is right is accepted. */
    ok = 0;
    int ppt = time_xcheck(5ull * 1000000000ull, 5, &ok);
    CHECK(ok == 1, "an exact 5s/5s cross-check was REJECTED (err=%dppt)", ppt);
    CHECK(ppt == 0, "an exact match reported %dppt", ppt);

    /* THE NEGATIVE CONTROL. This is the assertion that fails without the
     * change: a tick running at exactly twice its programmed rate reports twice
     * as much elapsed time as the RTC saw, and the guard must reject it. Before
     * this subsystem existed there was no code that could even be handed these
     * two numbers -- which is precisely why the real 2x PIT bug survived for the
     * life of the kernel (commit 64a16c2). */
    ok = 1;
    ppt = time_xcheck(10ull * 1000000000ull, 5, &ok);
    CHECK(ok == 0, "a 2x-FAST clock was ACCEPTED -- the guard does not work");
    CHECK(ppt >= 900 && ppt <= 1100, "2x clock reported %dppt, want ~+1000", ppt);

    /* And the other direction: a clock running at HALF rate, which is what the
     * same bug looks like from the other side of the fix. */
    ok = 1;
    ppt = time_xcheck(25ull * 100000000ull, 5, &ok);
    CHECK(ok == 0, "a 2x-SLOW clock was ACCEPTED");
    CHECK(ppt <= -400, "0.5x clock reported %dppt, want ~-500", ppt);

    /* The tolerance is deliberately wide enough for QEMU's emulated RTC, which
     * was MEASURED running ~11% fast under TCG. That much error must still
     * pass, or the check fails on every boot and gets deleted. */
    ok = 0;
    ppt = time_xcheck(4450ull * 1000000ull, 5, &ok);
    CHECK(ok == 1, "the measured TCG RTC bias (%dppt) is rejected -- too tight", ppt);

    /* A 30% error must NOT pass: the band has to stay a band. */
    ok = 1;
    time_xcheck(6500ull * 1000000ull, 5, &ok);
    CHECK(ok == 0, "a 30%% fast clock was accepted -- the band is too wide");
}

/* -------------------------------------------------------------- 4. misc */

static void test_cpu_accounting(void)
{
    /* There is no process table on the host, so proc_current_pid() answers -1
     * and every sampled tick is charged to idle. The property under test is
     * that the total is CONSERVED -- accounting that loses or double-counts
     * time is worse than none, because it looks authoritative. */
    time_host_reset(1000000000ull, ~0ull);
    for (int i = 1; i <= 10; i++) {
        time_host_set_cycles((uint64_t)i * 10000000ull);       /* 10 ms steps */
        time_tick();
    }
    uint64_t total = time_cpu_total_ns();
    CHECK(total > 99000000ull && total < 101000000ull,
          "10 ticks of 10ms accounted as %lluns, want ~100ms",
          (unsigned long long)total);
    uint64_t u = 0, s = 0;
    CHECK(time_cpu_ns(4242, &u, &s) == -1, "an unknown pid returned accounting");
}

int main(void)
{
    printf("time_test: timer heap, ns arithmetic, wrap, clamp, cross-check\n");
    test_heap_order();
    test_heap_same_deadline();
    test_heap_cancel();
    test_heap_full();
    test_periodic();
    test_callback_rearm();
    test_ns_conversion();
    test_counter_wrap();
    test_source_steps_backwards();
    test_monotonic_clamp();
    test_wall_vs_mono();
    test_clock_ids();
    test_source_switch();
    test_xcheck_negative_control();
    test_cpu_accounting();
    printf("time_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
