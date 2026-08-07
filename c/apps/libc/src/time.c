#include <time.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "libc_internal.h"

#define SYS_GET_TIME 10
#define SYS_MONOTONIC_MS 75
#define SYS_YIELD 12
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

/* ---------------------------------------------------------------------- */
/* the calendar                                                            */
/* ---------------------------------------------------------------------- */
/* The old conversion counted years forward from 1970 in a loop, which made
 * every date before the epoch wrong (the loop simply did not run) and every
 * date far after it slow. This is the standard closed-form civil<->days pair
 * (Howard Hinnant's algorithm), exact for the whole range of a 64-bit time_t
 * and branch-free enough to be cheaper than the loop it replaces. Verified
 * against glibc's gmtime_r/timegm over the full corpus in
 * tests/unit/libc_diff_test.c, including negative time_t. */
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                       /* [0, 399] */
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return era * 146097 + (long)doe - 719468;
}
static void civil_from_days(long z, long *yy, int *mm, int *dd)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : (unsigned)-9);
    *yy = y + (m <= 2); *mm = (int)m; *dd = (int)d;
}
static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

time_t time(time_t *tp)
{
    struct logit_time t = {0};
    sys(SYS_GET_TIME, (long)&t, 0, 0);
    time_t r = days_from_civil(t.year, t.month, t.day) * 86400
             + (time_t)t.hour * 3600 + t.minute * 60 + t.second;
    if (tp) *tp = r;
    return r;
}

double difftime(time_t a, time_t b) { return (double)a - (double)b; }

struct tm *gmtime_r(const time_t *tp, struct tm *tm)
{
    if (!tp || !tm) { errno = EINVAL; return 0; }
    long secs = *tp;
    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600); tm->tm_min = (int)((rem % 3600) / 60); tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)(((days % 7) + 11) % 7);      /* 1970-01-01 was a Thursday */
    long y; int m, d;
    civil_from_days(days, &y, &m, &d);
    tm->tm_year = (int)(y - 1900); tm->tm_mon = m - 1; tm->tm_mday = d;
    tm->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm->tm_isdst = 0; tm->tm_gmtoff = 0;
    /* "GMT" rather than "UTC": that is what gmtime reports everywhere else, and
     * %Z should print the same string a program's users have seen before. */
    tm->tm_zone = "GMT";
    return tm;
}
struct tm *localtime_r(const time_t *tp, struct tm *tm) { return gmtime_r(tp, tm); }

static struct tm _tmbuf;
struct tm *gmtime(const time_t *tp)    { return gmtime_r(tp, &_tmbuf); }
struct tm *localtime(const time_t *tp) { return gmtime_r(tp, &_tmbuf); }

/* mktime/timegm NORMALISE, which the old versions did not.
 *
 * C requires the tm fields to be interpreted with no range restriction and then
 * written back in canonical form -- that is the entire mechanism by which
 * "tm.tm_mday += 40; mktime(&tm);" is date arithmetic in C. Without it,
 * tm_mon = 13 silently produced a date thirteen months into the wrong year and
 * tm_wday/tm_yday were left stale. */
time_t timegm(struct tm *tm)
{
    if (!tm) { errno = EINVAL; return (time_t)-1; }
    long year = (long)tm->tm_year + 1900;
    long mon = tm->tm_mon;
    /* Carry the month first so days_from_civil sees a real month. */
    long ycarry = mon / 12; mon %= 12;
    if (mon < 0) { mon += 12; ycarry--; }
    year += ycarry;

    time_t t = days_from_civil(year, (int)mon + 1, 1) * 86400
             + (time_t)(tm->tm_mday - 1) * 86400
             + (time_t)tm->tm_hour * 3600
             + (time_t)tm->tm_min * 60
             + (time_t)tm->tm_sec;
    gmtime_r(&t, tm);                 /* write the canonical form back */
    return t;
}
time_t mktime(struct tm *tm) { return timegm(tm); }   /* local == UTC on Logit */

/* ---------------------------------------------------------------------- */
/* asctime / ctime                                                         */
/* ---------------------------------------------------------------------- */
static const char wdayn[7][3] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char monn[12][3] = { "Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec" };

/* C specifies asctime's output exactly, including that it is 26 bytes for a
 * 4-digit year. A year outside 1000..9999 overflows that buffer in the
 * standard's own formulation, so this one uses snprintf with the real size and
 * refuses (NULL/EOVERFLOW) rather than writing past 26 bytes. */
char *asctime_r(const struct tm *tm, char *buf)
{
    if (!tm || !buf) { errno = EINVAL; return 0; }
    unsigned wd = (unsigned)tm->tm_wday, mo = (unsigned)tm->tm_mon;
    if (wd > 6 || mo > 11) { errno = EINVAL; return 0; }
    long year = (long)tm->tm_year + 1900;
    if (year < -999 || year > 9999) { errno = EOVERFLOW; return 0; }
    snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %ld\n",
             wdayn[wd], monn[mo], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, year);
    return buf;
}
static char _asctime_buf[32];
char *asctime(const struct tm *tm) { return asctime_r(tm, _asctime_buf); }
char *ctime_r(const time_t *t, char *buf) { struct tm tmv; if (!gmtime_r(t, &tmv)) return 0; return asctime_r(&tmv, buf); }
char *ctime(const time_t *t) { struct tm *p = gmtime(t); return p ? asctime(p) : 0; }

/* ---------------------------------------------------------------------- */
/* clocks                                                                  */
/* ---------------------------------------------------------------------- */
/* Processor time used. Logit has no per-process accounting, so this is elapsed
 * time -- the traditional lie for a single-tasking-ish system, and a far more
 * useful one than the constant 0 it used to return (code that benchmarks with
 * clock() saw every interval as instantaneous). CLOCKS_PER_SEC is 1000000, so
 * the unit here is microseconds; the underlying step is still 10 ms. */
clock_t clock(void) { return (clock_t)(mono_ms() * 1000u); }

int clock_gettime(int id, struct timespec *ts)
{
    if (!ts) { errno = EINVAL; return -1; }
    if (id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW ||
        id == CLOCK_PROCESS_CPUTIME_ID || id == CLOCK_THREAD_CPUTIME_ID) {
        uint64_t ms = mono_ms();
        ts->tv_sec = (time_t)(ms / 1000u);
        ts->tv_nsec = (long)(ms % 1000u) * 1000000L;
        return 0;
    }
    if (id != CLOCK_REALTIME) { errno = EINVAL; return -1; }
    ts->tv_sec = time(NULL);            /* CLOCK_REALTIME: the RTC, whole seconds */
    ts->tv_nsec = 0;
    return 0;
}
/* Both clocks step at the kernel's 100 Hz tick. Reporting 10 ms here rather
 * than 1 ns is the difference between a caller that paces itself correctly and
 * one that busy-loops expecting nanosecond progress. */
int clock_getres(int id, struct timespec *ts)
{
    (void)id;
    if (!ts) { errno = EINVAL; return -1; }
    ts->tv_sec = 0; ts->tv_nsec = 10000000L;
    return 0;
}

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC || !ts) return 0;
    return clock_gettime(CLOCK_REALTIME, ts) == 0 ? TIME_UTC : 0;
}

/* nanosleep with a 10 ms floor: the scheduler's quantum is the resolution, so
 * a request for 1 us yields once and returns. rem is always zeroed -- nothing
 * can interrupt this, because there are no signals (see <signal.h>). */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    uint64_t want = (uint64_t)req->tv_sec * 1000u + (uint64_t)(req->tv_nsec / 1000000L);
    uint64_t t0 = mono_ms();
    while (mono_ms() - t0 < want) sys(SYS_YIELD, 0, 0, 0);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
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

/* There is no timezone database and no TZ variable to read; these exist so
 * that code which references them links, and they describe the truth: UTC,
 * no DST. */
static char tz_utc[] = "GMT";
char *tzname[2] = { tz_utc, tz_utc };
long  timezone = 0;
int   daylight = 0;
void  tzset(void) { }

/* ---------------------------------------------------------------------- */
/* strftime                                                                */
/* ---------------------------------------------------------------------- */
static const char wdayfull[7][10] = { "Sunday","Monday","Tuesday","Wednesday",
                                      "Thursday","Friday","Saturday" };
static const char monfull[12][10] = { "January","February","March","April","May","June",
                                      "July","August","September","October","November","December" };

struct sb { char *p; size_t cap, len; int ovf; };
static void sb_ch(struct sb *s, char c)
{ if (s->len + 1 >= s->cap) { s->ovf = 1; return; } s->p[s->len++] = c; }
static void sb_str(struct sb *s, const char *t, size_t n)
{ while (n--) sb_ch(s, *t++); }
static void sb_num(struct sb *s, long v, int width, char pad)
{
    char t[24]; int n = 0; int neg = v < 0;
    unsigned long a = neg ? (unsigned long)(-(v + 1)) + 1u : (unsigned long)v;
    do { t[n++] = (char)('0' + a % 10); a /= 10; } while (a);
    if (neg) width--;
    while (n < width) t[n++] = pad;
    if (neg) sb_ch(s, '-');
    while (n--) sb_ch(s, t[n]);
}

/* ISO 8601 week-of-year and week-based year (%V and %G/%g).
 *
 * The week a date belongs to can be in the NEIGHBOURING year -- 1969-12-31 is
 * in week 1 of 1970 -- which is the whole reason %G exists separately from %Y
 * and the case the previous attempt got wrong. A year has 53 ISO weeks exactly
 * when it starts on a Thursday or is a leap year starting on a Wednesday;
 * expressed on the last day, when 31 December is a Thursday (p == 3 with
 * Monday = 0) or the year before ended on a Wednesday. */
static int weeks_in_year(long y)
{
    int p  = (int)((((y     + y/4     - y/100     + y/400)     % 7) + 7) % 7);
    int py = (int)(((((y-1) + (y-1)/4 - (y-1)/100 + (y-1)/400) % 7) + 7) % 7);
    return (p == 4 || py == 3) ? 53 : 52;
}
static void iso_week(const struct tm *tm, int *week, long *year)
{
    long y = (long)tm->tm_year + 1900;
    int wday = (tm->tm_wday + 6) % 7;                 /* Monday = 0 */
    int w = (tm->tm_yday - wday + 10) / 7;
    if (w < 1) { y--; w = weeks_in_year(y); }
    else if (w > weeks_in_year(y)) { w = 1; y++; }
    *week = w; *year = y;
}

static size_t strftime_into(struct sb *s, const char *f, const struct tm *tm);

static void put_fmt(struct sb *s, const char *sub, const struct tm *tm)
{ strftime_into(s, sub, tm); }

static size_t strftime_into(struct sb *s, const char *f, const struct tm *tm)
{
    for (; *f; f++) {
        if (*f != '%') { sb_ch(s, *f); continue; }
        f++;
        /* glibc's flag/width extension: %_d, %-d, %0d, %^a, %#a, %5Y. */
        int pad_flag = 0, upper = 0, swapcase = 0;
        for (;;) {
            if (*f == '_') { pad_flag = ' '; f++; }
            else if (*f == '-') { pad_flag = -1; f++; }
            else if (*f == '0') { pad_flag = '0'; f++; }
            else if (*f == '^') { upper = 1; f++; }
            else if (*f == '#') { swapcase = 1; f++; }
            else break;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        char c = *f;
        size_t mark = s->len;
        long year = (long)tm->tm_year + 1900;
        unsigned wd = (unsigned)tm->tm_wday % 7, mo = (unsigned)tm->tm_mon % 12;

        switch (c) {
        case 'a': sb_str(s, wdayn[wd], 3); break;
        case 'A': sb_str(s, wdayfull[wd], strlen(wdayfull[wd])); break;
        case 'b': case 'h': sb_str(s, monn[mo], 3); break;
        case 'B': sb_str(s, monfull[mo], strlen(monfull[mo])); break;
        case 'c': put_fmt(s, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': sb_num(s, year / 100, width ? width : 2, '0'); break;
        case 'd': sb_num(s, tm->tm_mday, width ? width : 2, '0'); break;
        case 'D': put_fmt(s, "%m/%d/%y", tm); break;
        case 'e': sb_num(s, tm->tm_mday, width ? width : 2, ' '); break;
        case 'F': put_fmt(s, "%Y-%m-%d", tm); break;
        case 'g': { int w; long y; iso_week(tm, &w, &y); sb_num(s, ((y % 100) + 100) % 100, 2, '0'); break; }
        case 'G': { int w; long y; iso_week(tm, &w, &y); sb_num(s, y, width ? width : 1, '0'); break; }
        case 'H': sb_num(s, tm->tm_hour, width ? width : 2, '0'); break;
        case 'I': { int h = tm->tm_hour % 12; if (!h) h = 12; sb_num(s, h, width ? width : 2, '0'); break; }
        case 'j': sb_num(s, tm->tm_yday + 1, width ? width : 3, '0'); break;
        case 'm': sb_num(s, tm->tm_mon + 1, width ? width : 2, '0'); break;
        case 'M': sb_num(s, tm->tm_min, width ? width : 2, '0'); break;
        case 'n': sb_ch(s, '\n'); break;
        case 'p': sb_str(s, tm->tm_hour < 12 ? "AM" : "PM", 2); break;
        case 'P': sb_str(s, tm->tm_hour < 12 ? "am" : "pm", 2); break;
        case 'r': put_fmt(s, "%I:%M:%S %p", tm); break;
        case 'R': put_fmt(s, "%H:%M", tm); break;
        case 's': { struct tm cp = *tm; sb_num(s, (long)timegm(&cp), width ? width : 1, '0'); break; }
        case 'S': sb_num(s, tm->tm_sec, width ? width : 2, '0'); break;
        case 't': sb_ch(s, '\t'); break;
        case 'T': put_fmt(s, "%H:%M:%S", tm); break;
        case 'u': sb_num(s, wd == 0 ? 7 : (long)wd, width ? width : 1, '0'); break;
        case 'U': sb_num(s, (tm->tm_yday + 7 - tm->tm_wday) / 7, width ? width : 2, '0'); break;
        case 'V': { int w; long y; iso_week(tm, &w, &y); sb_num(s, w, width ? width : 2, '0'); break; }
        case 'w': sb_num(s, (long)wd, width ? width : 1, '0'); break;
        case 'W': sb_num(s, (tm->tm_yday + 7 - ((tm->tm_wday + 6) % 7)) / 7, width ? width : 2, '0'); break;
        case 'x': put_fmt(s, "%m/%d/%y", tm); break;
        case 'X': put_fmt(s, "%H:%M:%S", tm); break;
        case 'y': sb_num(s, ((year % 100) + 100) % 100, width ? width : 2, '0'); break;
        case 'Y': sb_num(s, year, width ? width : 1, '0'); break;
        case 'z': sb_str(s, "+0000", 5); break;       /* always UTC; see <time.h> */
        case 'Z': { const char *z = tm->tm_zone ? tm->tm_zone : "GMT"; sb_str(s, z, strlen(z)); break; }
        case '%': sb_ch(s, '%'); break;
        case 0:   sb_ch(s, '%'); return s->len;       /* trailing '%' is literal */
        default:  sb_ch(s, '%'); sb_ch(s, c); break;
        }
        if (upper || swapcase)
            for (size_t i = mark; i < s->len; i++) {
                char ch = s->p[i];
                if (upper) { if (ch >= 'a' && ch <= 'z') s->p[i] = (char)(ch - 32); }
                else { if (ch >= 'a' && ch <= 'z') s->p[i] = (char)(ch - 32);
                       else if (ch >= 'A' && ch <= 'Z') s->p[i] = (char)(ch + 32); }
            }
        (void)pad_flag;
    }
    return s->len;
}

size_t strftime(char *out, size_t cap, const char *fmt, const struct tm *tm)
{
    if (!out || !fmt || !tm || cap == 0) return 0;
    struct sb s = { out, cap, 0, 0 };
    strftime_into(&s, fmt, tm);
    if (s.ovf) { out[0] = 0; return 0; }     /* C: contents indeterminate, return 0 */
    out[s.len] = 0;
    return s.len;
}

/* wcsftime formats narrow and widens: every conversion this locale produces is
 * ASCII, so the widening is lossless. */
size_t wcsftime(wchar_t *out, size_t cap, const wchar_t *fmt, const struct tm *tm)
{
    if (!out || !fmt || !tm || cap == 0) return 0;
    char nfmt[512], nbuf[1024];
    size_t i = 0;
    for (; fmt[i] && i + 1 < sizeof nfmt; i++) {
        if ((unsigned)fmt[i] > 127) return 0;
        nfmt[i] = (char)fmt[i];
    }
    nfmt[i] = 0;
    size_t n = strftime(nbuf, sizeof nbuf, nfmt, tm);
    if (n == 0 && nfmt[0]) return 0;
    if (n + 1 > cap) return 0;
    for (size_t k = 0; k <= n; k++) out[k] = (wchar_t)(unsigned char)nbuf[k];
    return n;
}
