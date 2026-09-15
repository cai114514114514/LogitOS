#ifndef LOGIT_PIT_H
#define LOGIT_PIT_H

#include <stdint.h>

/* The rate the kernel actually programs (kmain.c) and the rate timer_ms()
 * divides by. It lives here rather than in kmain.c because it is now part of an
 * answer handed to userland, not just a boot parameter. */
#define TIMER_HZ 100

/* Program the Programmable Interval Timer to fire IRQ0 at `hz` Hz. */
void pit_init(uint32_t hz);

/* Called from the IRQ0 handler. */
void timer_tick(void);

/* Monotonic tick count since pit_init. */
uint64_t timer_ticks(void);

/* The same clock in milliseconds -- what SYS_MONOTONIC_MS returns.
 *
 * The UNIT is 1 ms; the GRANULARITY is 1000/TIMER_HZ = 10 ms. Callers get a
 * number they can subtract and compare against millisecond budgets without
 * knowing the tick rate, and a later, faster PIT programming changes only the
 * step size, never the ABI. Saying "ms" is not a promise of ms resolution, and
 * every place this value surfaces says so out loud. */
uint64_t timer_ms(void);

/* The nominal duration of one tick. The tick rate is a property of this driver
 * and callers should ask rather than open-code 1000/TIMER_HZ -- which is how a
 * rate change becomes a silent, tree-wide change of meaning. */
uint64_t timer_ns_per_tick(void);

/* HOW MANY OF THE TICKS ABOVE ACTUALLY ARRIVED.
 *
 * `timer_ticks()` is an interrupt count, so a period in which IRQ0 could not be
 * taken does not make it late -- it makes it SHORTER. The PIC holds one pending
 * edge per line, so N periods spent with the interrupt undeliverable cost N-1
 * increments, permanently. Every duration built on this counter (every WM
 * animation, every network timeout, every `while (timer_ticks() - t0 < N)`)
 * then runs long by exactly that much, and no consumer can tell: they all
 * subtract two readings of the same counter, and a counter that skips agrees
 * with itself.
 *
 * So the tick has to be counted against something that cannot be lost, which
 * is the TSC-derived nanosecond clock -- a register read, not an interrupt.
 * Measured in time_tick(), which is the one place that holds both at once:
 *
 *   seen        inter-tick intervals observed
 *   missed      whole tick periods inside those intervals that produced no
 *               interrupt. Floored, so it under-reports rather than invents.
 *   gap_max_ns  the worst single interval. This is the one that names a cause:
 *               the nominal value is 10 ms, and a gap is an interrupt-disabled
 *               (or EOI-deferred) window of that length somewhere in the kernel.
 *
 * `missed / (seen + missed)` is the fraction of the machine's clock that is
 * not there. Any NULL argument is skipped.
 *
 * WHAT THIS CANNOT SEPARATE, and it matters before anyone quotes it as a
 * kernel defect: under QEMU/TCG the guest TSC advances in HOST wall time, so a
 * window in which the host descheduled the vCPU thread is indistinguishable
 * from a window in which the guest ran with IF=0. Both genuinely lose the
 * tick -- QEMU's i8254 raises IRQ0 from another thread and the PIC coalesces
 * every edge after the first -- so the number is real either way, but only one
 * of the two causes is fixable in this tree. Measured, on three boots of the
 * SAME image doing the SAME thing while the host's load differed: worst gap
 * 24.9 / 55.6 / 75.7 ms. A 3x spread with the guest held constant is the host
 * talking.
 *
 * The attribution that works anyway is WITHIN ONE BOOT, where the host is the
 * same for both windows: this machine reports lost=0 of 440 across the
 * cross-check's idle window and +65 in one second of a scroll, in the same
 * boot half a minute apart. Nothing about the host changed between those two,
 * so that difference is the guest's.
 *
 * AND THE GUEST'S PART IS NOT THIS DRIVER. The `[time] tickloss   worst gap:`
 * line prints who held the big kernel lock when the gap ended, and in every
 * attributed sample so far it is ANOTHER CORE, through the irqsave path --
 * that is, inside a kernel entry with IF=0 -- while cpu 0, the only core that
 * ticks, waited for the same lock in spin_lock_irqsave(), also with IF=0. The
 * tick that begins such a gap is NOT lost: interrupt_handler() calls
 * timer_tick() before it acquires, deliberately. Everything after it is,
 * because the interrupt cannot be taken and the controller keeps only one
 * pending edge. Programming the PIT differently cannot help that, and neither
 * can anything else in c/drivers/timer -- the length of the window is set by
 * c/kernel/cpu/irq/interrupts.c and by whatever it is holding the lock for. */
void timer_tick_loss(uint64_t *seen, uint64_t *missed, uint64_t *gap_max_ns);

#endif /* LOGIT_PIT_H */
