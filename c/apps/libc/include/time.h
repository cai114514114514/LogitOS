#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>

typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
struct timespec { time_t tv_sec; long tv_nsec; };

time_t time(time_t *);
clock_t clock(void);
int clock_gettime(int, struct timespec *);
/* CLOCK_REALTIME is the RTC wall clock (whole seconds, settable).
 * CLOCK_MONOTONIC is SYS_MONOTONIC_MS: milliseconds since boot, never
 * backwards -- the one to measure intervals with. Its granularity is 10 ms (the
 * kernel's 100 Hz tick), so tv_nsec always lands on a 10 ms boundary; the
 * nanosecond field is POSIX's unit, not a precision claim.
 * _RAW is the same clock: Logit has no NTP slewing for it to differ from. */
#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1
#define CLOCK_MONOTONIC_RAW 4

struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *);
struct tm *localtime_r(const time_t *, struct tm *);
time_t mktime(struct tm *);
time_t timegm(struct tm *);

#endif
