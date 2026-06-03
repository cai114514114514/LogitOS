#ifndef AQUA_PIT_H
#define AQUA_PIT_H

#include <stdint.h>

/* Program the Programmable Interval Timer to fire IRQ0 at `hz` Hz. */
void pit_init(uint32_t hz);

/* Called from the IRQ0 handler. */
void timer_tick(void);

/* Monotonic tick count since pit_init. */
uint64_t timer_ticks(void);

#endif /* AQUA_PIT_H */
