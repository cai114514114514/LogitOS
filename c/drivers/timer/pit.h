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

#endif /* LOGIT_PIT_H */
