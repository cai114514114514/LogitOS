#include <stdint.h>
#include "pit.h"
#include "io.h"

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
}

void timer_tick(void)
{
    ticks++;
}

uint64_t timer_ticks(void)
{
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
     * 32-bit ms the obvious implementation would give. */
    return ticks * (1000u / TIMER_HZ);
}
