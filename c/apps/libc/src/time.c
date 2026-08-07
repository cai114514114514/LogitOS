#include <time.h>
#include <stdint.h>

#define SYS_GET_TIME 10
#define SYS_MONOTONIC_MS 75
struct logit_time { int year, month, day, hour, minute, second, weekday; };
static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* Milliseconds since boot. Shape note -- this is exposed to mini-libc's users
 * as POSIX clock_gettime(CLOCK_MONOTONIC), NOT as a `uint64_t millis(void)`,
 * even though the raw syscall is exactly the latter and the wrapper below is
 * pure overhead. mini-libc exists so that code written for a real system --
 * QuickJS, musl's libm, whatever gets ported next -- compiles unmodified. Such
 * code asks for a monotonic clock by calling clock_gettime; nothing anywhere
 * calls millis(). A convenience spelling that no existing source uses is not a
 * convenience, it is a second name for the same thing that ports then have to
 * be edited to use.
 *
 * GRANULARITY IS 10 ms (the kernel's 100 Hz tick), so tv_nsec is always a whole
 * multiple of 10,000,000. Nanoseconds are the struct's unit, not a claim. */
static uint64_t mono_ms(void) { return (uint64_t)sys(SYS_MONOTONIC_MS, 0, 0, 0); }

static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static time_t ymd_to_unix(int Y, int Mo, int D, int h, int mi, int s)
{
    long days = 0;
    for (int y = 1970; y < Y; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < Mo; m++) { days += mdays[m-1]; if (m == 2 && leap(Y)) days++; }
    days += D - 1;
    return ((time_t)days * 24 + h) * 3600 + mi * 60 + s;
}

time_t time(time_t *tp)
{
    struct logit_time t = {0};
    sys(SYS_GET_TIME, (long)&t, 0, 0);
    time_t r = ymd_to_unix(t.year, t.month, t.day, t.hour, t.minute, t.second);
    if (tp) *tp = r;
    return r;
}

struct tm *gmtime_r(const time_t *tp, struct tm *tm)
{
    long secs = *tp, days = secs / 86400; int rem = (int)(secs % 86400);
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = rem / 3600; tm->tm_min = (rem % 3600) / 60; tm->tm_sec = rem % 60;
    tm->tm_wday = (int)((days % 7 + 4 + 7) % 7);     /* 1970-01-01 = Thursday */
    int y = 1970;
    for (;;) { int dy = leap(y) ? 366 : 365; if (days < dy) break; days -= dy; y++; }
    tm->tm_year = y - 1900; tm->tm_yday = (int)days;
    int mo = 0;
    for (;;) { int dm = mdays[mo] + (mo == 1 && leap(y) ? 1 : 0); if (days < dm) break; days -= dm; mo++; }
    tm->tm_mon = mo; tm->tm_mday = (int)days + 1;
    tm->tm_isdst = 0; tm->tm_gmtoff = 0; tm->tm_zone = "UTC";
    return tm;
}
struct tm *localtime_r(const time_t *tp, struct tm *tm) { return gmtime_r(tp, tm); }

static struct tm _tmbuf;
struct tm *gmtime(const time_t *tp)    { return gmtime_r(tp, &_tmbuf); }
struct tm *localtime(const time_t *tp) { return gmtime_r(tp, &_tmbuf); }

time_t timegm(struct tm *tm)
{ return ymd_to_unix(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec); }
time_t mktime(struct tm *tm) { return timegm(tm); }   /* UTC == local on Logit */

/* Processor time used. Logit has no per-process accounting, so this is elapsed
 * time -- the traditional lie for a single-tasking-ish system, and a far more
 * useful one than the constant 0 it used to return (code that benchmarks with
 * clock() saw every interval as instantaneous). CLOCKS_PER_SEC is 1000000, so
 * the unit here is microseconds; the underlying step is still 10 ms. */
clock_t clock(void) { return (clock_t)(mono_ms() * 1000u); }

int clock_gettime(int id, struct timespec *ts)
{
    if (!ts) return -1;
    if (id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW) {
        uint64_t ms = mono_ms();
        ts->tv_sec = (time_t)(ms / 1000u);
        ts->tv_nsec = (long)(ms % 1000u) * 1000000L;
        return 0;
    }
    ts->tv_sec = time(NULL);            /* CLOCK_REALTIME: the RTC, whole seconds */
    ts->tv_nsec = 0;
    return 0;
}

struct timeval { time_t tv_sec; long tv_usec; };

/* Wall clock with sub-second resolution, which is what Date.now() is built on.
 * The RTC alone gives whole seconds; the monotonic clock alone has the wrong
 * epoch. So latch the two together ONCE and count milliseconds off the
 * monotonic one from there. The result is a wall clock that advances smoothly
 * and never steps backwards inside one process -- which matters, because
 * Date.now() going backwards breaks timers and animation in ways that look like
 * anything but a clock bug.
 *
 * The seam is deliberate: after the latch this no longer tracks the RTC, so a
 * long-lived process drifts by whatever the PIT and the CMOS disagree about.
 * Re-reading the RTC each call would fix the drift and reintroduce the
 * backwards step, and for the callers that exist (timestamps, elapsed time)
 * monotonicity is worth more than agreement with a clock nobody is setting. */
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) return 0;
    static time_t base_sec;
    static uint64_t base_ms;
    static int latched;
    uint64_t now = mono_ms();
    if (!latched) { base_sec = time(NULL); base_ms = now; latched = 1; }
    uint64_t elapsed = now - base_ms;
    tv->tv_sec = base_sec + (time_t)(elapsed / 1000u);
    tv->tv_usec = (long)(elapsed % 1000u) * 1000L;
    return 0;
}
