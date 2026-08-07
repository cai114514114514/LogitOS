#ifndef LOGIT_PIT_H
#define LOGIT_PIT_H
#include <stdint.h>
#define TIMER_HZ 100
void     pit_init(uint32_t hz);
void     timer_tick(void);
uint64_t timer_ticks(void);
uint64_t timer_ms(void);
#endif
