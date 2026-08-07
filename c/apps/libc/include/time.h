#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>

typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1

#ifndef __LIBC_WCHAR_T_DEFINED
#define __LIBC_WCHAR_T_DEFINED
typedef __WCHAR_TYPE__ wchar_t;
#endif

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
struct timespec { time_t tv_sec; long tv_nsec; };

time_t time(time_t *);
clock_t clock(void);
double difftime(time_t, time_t);
int clock_gettime(int, struct timespec *);
int clock_getres(int, struct timespec *);
int timespec_get(struct timespec *, int);
int nanosleep(const struct timespec *, struct timespec *);

/* CLOCK_REALTIME is the RTC wall clock (whole seconds, settable).
 * CLOCK_MONOTONIC is SYS_MONOTONIC_MS: milliseconds since boot, never
 * backwards -- the one to measure intervals with. Its granularity is 10 ms (the
 * kernel's 100 Hz tick), so tv_nsec always lands on a 10 ms boundary; the
 * nanosecond field is POSIX's unit, not a precision claim.
 * _RAW is the same clock: Logit has no NTP slewing for it to differ from. */
#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW 4

/* LogitOS has no timezone database and no TZ handling: local time IS UTC.
 * localtime() is therefore gmtime(), tm_gmtoff is 0 and tm_zone is "GMT".
 * That is a real limitation, not a placeholder -- a program that formats a
 * local timestamp gets UTC, correctly labelled, rather than a wrong offset. */
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
struct tm *gmtime_r(const time_t *, struct tm *);
struct tm *localtime_r(const time_t *, struct tm *);
time_t mktime(struct tm *);
time_t timegm(struct tm *);
char  *asctime(const struct tm *);
char  *asctime_r(const struct tm *, char *);
char  *ctime(const time_t *);
char  *ctime_r(const time_t *, char *);
size_t strftime(char *, size_t, const char *, const struct tm *);
size_t wcsftime(wchar_t *, size_t, const wchar_t *, const struct tm *);
void   tzset(void);

extern char *tzname[2];
extern long  timezone;
extern int   daylight;

#endif
