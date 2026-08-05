#include <time.h>

#define SYS_GET_TIME 10
struct logit_time { int year, month, day, hour, minute, second, weekday; };
static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

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

clock_t clock(void) { return (clock_t)0; }
int clock_gettime(int id, struct timespec *ts) { (void)id; ts->tv_sec = time(NULL); ts->tv_nsec = 0; return 0; }

struct timeval { time_t tv_sec; long tv_usec; };
int gettimeofday(struct timeval *tv, void *tz) { (void)tz; if (tv) { tv->tv_sec = time(NULL); tv->tv_usec = 0; } return 0; }
