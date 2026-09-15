#ifndef LOGIT_KTIME_H
#define LOGIT_KTIME_H
#include <stdint.h>
uint64_t time_mono_raw_ns(void);
int time_ready(void);
#endif
