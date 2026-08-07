#include <stdint.h>
#include "pit.h"
#include "io.h"
#include "ktime.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQ     1193182u     /* base oscillator frequency in Hz */

static volatile uint64_t ticks = 0;

void pit_init(uint32_t hz)
{
    if (hz == 0) hz = 100;                  /* avoid div-by-zero; fall back to a sane tick */
    uint32_t divisor = PIT_FREQ / hz;
    if (divisor == 0) divisor = 1;          /* hz > PIT_FREQ: clamp to the fastest rate */
    /* Channel 0, lo/hi byte, MODE 2 (rate generator) -- one output pulse per
     * `divisor` cycles, i.e. exactly one interrupt per period.
     *
     * This was mode 3 (0x36, square wave), and the tick ran at exactly TWICE the
     * programmed rate: mode 3's output is a square wave that transitions twice
     * per period, and the interrupt path counted both, so `pit_init(100)` gave a
     * 200 Hz tick. Measured, not deduced -- boot, read the counter, sleep 15 s of
     * host time, read again: mode 3 reported 30.40 s, mode 2 reports 15.20 s.
     *
     * Nothing noticed for the same reason nothing ever could: every consumer of
     * timer_ticks() was a timeout or an animation whose only spec was a comment,
     * so a "5 second" network timeout that actually expired in 2.5 s just looked
     * like the number someone chose. SYS_MONOTONIC_MS is the first consumer that
     * makes a checkable claim in a unit the caller supplies, and a clock that is
     * 2x fast is not a millisecond clock -- setTimeout(1000) would fire in half a
     * second. Fixing the tick rather than dividing by two somewhere makes every
     * duration in the tree mean what its comment already said it meant. Mode 2
     * is also what a periodic tick wants regardless; the square wave was never
     * buying anything here. */
    outb(PIT_COMMAND, 0x34);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    /* Bring the clock up HERE, not in kmain.c.
     *
     * The tick and the clock are one thing -- the clock calibrates against PIT
     * channel 2 while channel 0 is being programmed, and the ordering between
     * them is a property of this driver, not of the boot sequence. Putting the
     * call in kmain would mean the timer driver could be initialised into a
     * state where the clock did not exist, and the only symptom would be
     * time_mono_ns() returning 0 forever. Idempotent, so a second pit_init() is
     * harmless. Needs no interrupts, no heap and no filesystem: it is port I/O
     * and CPUID. */
    time_init();
}

void timer_tick(void)
{
    ticks++;
    /* The nanosecond clock folds its cycle counter here, expires due timers and
     * samples CPU accounting. Runs in the SAME pre-BKL window as the tick itself
     * (see interrupts.c): a deadline must never queue behind a lock held by a
     * thread that is waiting for that deadline. */
    time_tick();
}

uint64_t timer_ticks(void)
{
    /* Unchanged: 100 Hz PIT interrupts since boot, exactly as before. Every
     * timeout in the network stack subtracts these, and the last time the
     * meaning of a tick moved, every one of them silently doubled (64a16c2 ->
     * 7131e34). The nanosecond clock is a NEW surface beside this one, not a
     * reinterpretation of it.
     *
     * The one addition is a deferred-work poll, which is a single predicted
     * branch on a global that is zero for all but a few microseconds of the
     * machine's life. It is here because timer_ticks() is the one function the
     * time subsystem owns that gets called from ordinary thread context with
     * interrupts on -- see time_safepoint(). */
    if (__builtin_expect(g_time_deferred != 0, 0))
        time_safepoint();
    return ticks;
}

uint64_t timer_ms(void)
{
    /* One aligned 64-bit load on x86_64, so it cannot tear against the IRQ0
     * writer -- no lock, and correct to call with interrupts either way. (With
     * IF=0 the answer is simply frozen: nothing is incrementing `ticks`. A
     * kernel loop that waits for this to change must have re-enabled IF first.)
     *
     * ticks * 10 in 64 bits does not overflow before the heat death of anything
     * that matters, so there is no wraparound case to handle here or in the
     * callers -- which is the whole reason the counter is 64-bit rather than the
     * 32-bit ms the obvious implementation would give.
     *
     * DELIBERATELY still derived from the tick and not from time_mono_ns(),
     * even though the ns clock is finer and available. SYS_MONOTONIC_MS's ABI
     * documents a 10 ms step and tests/boot/run-clock-test.sh asserts it; making
     * this value finer would change what an existing, published number means,
     * which is the one thing this subsystem exists to stop happening silently.
     * New code wants time_mono_ns() / SYS_CLOCK_GETTIME. */
    return ticks * (1000u / TIMER_HZ);
}

uint64_t timer_ns_per_tick(void)
{
    return 1000000000ull / TIMER_HZ;
}
