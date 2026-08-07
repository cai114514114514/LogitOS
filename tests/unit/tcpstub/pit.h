#ifndef LOGIT_PIT_H
#define LOGIT_PIT_H
#include <stdint.h>
/* The kernel programs 100 Hz (c/drivers/timer/pit.h). tcp.c derives every
 * protocol timeout from this rather than open-coding tick counts. */
#define TIMER_HZ 100

uint64_t timer_ticks(void);
#endif
