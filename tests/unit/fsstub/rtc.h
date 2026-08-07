#ifndef LOGIT_RTC_H
#define LOGIT_RTC_H
/* Host stub: a settable clock, so a test can assert an exact mtime. */
#include <stdint.h>
extern int64_t fsstub_clock;
static inline int64_t rtc_unix(void) { return fsstub_clock; }
#endif
