/* Host-test stub for the timer. TIMER_HZ matches the kernel's so the deadline
 * arithmetic in wait.c is exercised with the real constant; timer_ticks() is
 * driven by the host monotonic clock. */
#ifndef WAITSTUB_PIT_H
#define WAITSTUB_PIT_H

#include <stdint.h>

#define TIMER_HZ 100

uint64_t timer_ticks(void);
uint64_t timer_ms(void);

#endif
