/* logit_fmt.h -- the one formatter set for sizes, counts, dates, weekdays,
 * clocks and relative times, shared by the WM and every app (design
 * 3.6 "Units" / "Dates", 5.2 menu-bar clock, 5.7 notification times).
 * Addendum §E: these strings are never hand-formatted anywhere else.
 *
 * Every formatter writes UTF-8 into a caller buffer and returns the length
 * in bytes (without the terminator). If the result does not fit in `cap`
 * bytes including the terminator, the buffer is left empty and -1 is
 * returned. Integer only: no libc, no float, no static storage.
 *
 * Chinese and Latin are separated by a regular space: "170 字节",
 * "609 KB", "1.2 MB", "4 项", "2 分钟前". */
#ifndef LOGIT_UI_FMT_H
#define LOGIT_UI_FMT_H

#include <stdint.h>

/* ---------------------------------------------------- the writer */

struct logit_fmtw { char *b; int cap, n, err; };

static inline struct logit_fmtw logit_fmtw_begin(char *buf, int cap)
{
    struct logit_fmtw w = { buf, cap, 0, cap <= 0 };
    if (cap > 0) buf[0] = 0;
    return w;
}
static inline void logit_fmtw_s(struct logit_fmtw *w, const char *s)
{
    for (; *s && !w->err; s++) {
        if (w->n + 1 >= w->cap) { w->err = 1; break; }
        w->b[w->n++] = *s;
    }
}
static inline void logit_fmtw_u(struct logit_fmtw *w, uint64_t v, int min_digits)
{
    char t[24];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 20);
    while (k < min_digits && k < 20) t[k++] = '0';
    while (k > 0 && !w->err) {
        if (w->n + 1 >= w->cap) { w->err = 1; break; }
        w->b[w->n++] = t[--k];
    }
}
static inline int logit_fmtw_end(struct logit_fmtw *w)
{
    if (w->cap <= 0) return -1;
    if (w->err) { w->b[0] = 0; return -1; }
    w->b[w->n] = 0;
    return w->n;
}

/* ---------------------------------------------------- sizes and counts */

/* "170 字节" under 1024; then KB, MB, GB, TB, PB in powers of 1024 with one
 * decimal under 10 ("1.2 MB", "9.9 KB") and an integer otherwise ("609 KB",
 * "10 MB"). Rounds half up at the shown precision; a value that rounds to
 * 1024 moves to the next unit ("1.0 MB", not "1024 KB"). */
static inline int logit_fmt_size(char *buf, int cap, uint64_t bytes)
{
    static const char *const units[] = { "KB", "MB", "GB", "TB", "PB" };
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    if (bytes < 1024) {
        logit_fmtw_u(&w, bytes, 1);
        logit_fmtw_s(&w, " 字节");
        return logit_fmtw_end(&w);
    }
    for (int u = 0; u < 5; u++) {
        uint64_t div = (uint64_t)1 << (10 * (u + 1));
        uint64_t q = bytes / div, r = bytes % div;
        uint64_t tenths = q * 10 + (r * 10 + div / 2) / div;
        uint64_t whole = q + (r >= div - r ? 1 : 0);
        if (tenths >= 100 && whole >= 1024 && u < 4) continue;
        if (tenths < 100) {
            logit_fmtw_u(&w, tenths / 10, 1);
            logit_fmtw_s(&w, ".");
            logit_fmtw_u(&w, tenths % 10, 1);
        } else {
            logit_fmtw_u(&w, whole, 1);
        }
#ifdef UI_FMT_NEGCTL
        if (u != 0)                /* the control drops the space before KB only */
#endif
        logit_fmtw_s(&w, " ");
        logit_fmtw_s(&w, units[u]);
        break;
    }
    return logit_fmtw_end(&w);
}

/* A folder's item count: "4 项" (never "--"). */
static inline int logit_fmt_items(char *buf, int cap, uint64_t n)
{
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    logit_fmtw_u(&w, n, 1);
    logit_fmtw_s(&w, " 项");
    return logit_fmtw_end(&w);
}

/* ---------------------------------------------------- calendar */

/* Broken-down LOCAL time. mon 1..12, mday 1..31, wday 0 = Sunday. */
struct logit_tm { int year, mon, mday, hour, min, sec, wday; };

static inline int64_t logit__floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
/* Days since 1970-01-01 -> civil date (proleptic Gregorian). */
static inline void logit__civil(int64_t days, int *y, int *m, int *d)
{
    int64_t z = days + 719468;
    int64_t era = logit__floordiv(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400 + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}
/* Seconds since the epoch in LOCAL time (the caller applies its zone). */
static inline void logit_tm_from_epoch(int64_t t, struct logit_tm *tm)
{
    int64_t days = logit__floordiv(t, 86400);
    int64_t s = t - days * 86400;
    logit__civil(days, &tm->year, &tm->mon, &tm->mday);
    tm->hour = (int)(s / 3600);
    tm->min = (int)(s / 60 % 60);
    tm->sec = (int)(s % 60);
    int64_t wd = (days + 4) % 7;           /* 1970-01-01 was a Thursday */
    tm->wday = (int)(wd < 0 ? wd + 7 : wd);
}

/* ---------------------------------------------------- weekdays */

#define LOGIT_WD_SHORT 0   /* 周二 */
#define LOGIT_WD_LONG  1   /* 星期二 */
static inline int logit_fmt_weekday(char *buf, int cap, int wday, int style)
{
    static const char *const d[7] = { "日", "一", "二", "三", "四", "五", "六" };
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    if (wday < 0 || wday > 6) { w.err = 1; return logit_fmtw_end(&w); }
    logit_fmtw_s(&w, style == LOGIT_WD_LONG ? "星期" : "周");
    logit_fmtw_s(&w, d[wday]);
    return logit_fmtw_end(&w);
}

/* ---------------------------------------------------- clock (5.2) */

#define LOGIT_CLK_DATE         1u   /* "10月27日" */
#define LOGIT_CLK_WEEKDAY      2u   /* "周二" */
#define LOGIT_CLK_SECONDS      4u   /* "13:00:05" */
#define LOGIT_CLK_24H          8u   /* "13:00"; otherwise "下午1:00" */
#define LOGIT_CLK_WEEKDAY_LONG 16u  /* "星期二" (implies WEEKDAY) */
#define LOGIT_CLK_NO_TIME      32u  /* date and/or weekday only */
/* The menu bar default (5.2): date, weekday, 24 h, no seconds. */
#define LOGIT_CLK_MENUBAR (LOGIT_CLK_DATE | LOGIT_CLK_WEEKDAY | LOGIT_CLK_24H)

static inline void logit__fmt_time(struct logit_fmtw *w, const struct logit_tm *tm, unsigned flags)
{
    if (flags & LOGIT_CLK_24H) {
        logit_fmtw_u(w, (uint64_t)tm->hour, 2);
    } else {
        int h = tm->hour % 12;
        logit_fmtw_s(w, tm->hour < 12 ? "上午" : "下午");
        logit_fmtw_u(w, (uint64_t)(h ? h : 12), 1);
    }
    logit_fmtw_s(w, ":");
    logit_fmtw_u(w, (uint64_t)tm->min, 2);
    if (flags & LOGIT_CLK_SECONDS) {
        logit_fmtw_s(w, ":");
        logit_fmtw_u(w, (uint64_t)tm->sec, 2);
    }
}

/* "10月27日 周二 13:00": the groups the flags select, in that order,
 * separated by one regular space. */
static inline int logit_fmt_clock(char *buf, int cap, const struct logit_tm *tm, unsigned flags)
{
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    int any = 0;
    if (flags & LOGIT_CLK_DATE) {
        logit_fmtw_u(&w, (uint64_t)tm->mon, 1);
        logit_fmtw_s(&w, "月");
        logit_fmtw_u(&w, (uint64_t)tm->mday, 1);
        logit_fmtw_s(&w, "日");
        any = 1;
    }
    if (flags & (LOGIT_CLK_WEEKDAY | LOGIT_CLK_WEEKDAY_LONG)) {
        char wd[16];
        if (logit_fmt_weekday(wd, sizeof wd, tm->wday,
                              (flags & LOGIT_CLK_WEEKDAY_LONG) ? LOGIT_WD_LONG : LOGIT_WD_SHORT) < 0)
            w.err = 1;
        if (any) logit_fmtw_s(&w, " ");
        logit_fmtw_s(&w, wd);
        any = 1;
    }
    if (!(flags & LOGIT_CLK_NO_TIME)) {
        if (any) logit_fmtw_s(&w, " ");
        logit__fmt_time(&w, tm, flags);
    }
    return logit_fmtw_end(&w);
}

/* ---------------------------------------------------- relative dates (3.6) */

/* For a list's 修改日期 column. now and t are LOCAL epoch seconds:
 * same day "今天 13:08", the day before "昨天 18:40", this year "10月25日",
 * any other year "2025年10月25日". */
static inline int logit_fmt_reldate(char *buf, int cap, int64_t now, int64_t t)
{
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    struct logit_tm a, b;
    logit_tm_from_epoch(now, &a);
    logit_tm_from_epoch(t, &b);
    int64_t dn = logit__floordiv(now, 86400), dt = logit__floordiv(t, 86400);
    if (dt == dn || dt == dn - 1) {
        logit_fmtw_s(&w, dt == dn ? "今天 " : "昨天 ");
        logit__fmt_time(&w, &b, LOGIT_CLK_24H);
        return logit_fmtw_end(&w);
    }
    if (b.year != a.year) {
        logit_fmtw_u(&w, (uint64_t)(b.year < 0 ? 0 : b.year), 1);
        logit_fmtw_s(&w, "年");
    }
    logit_fmtw_u(&w, (uint64_t)b.mon, 1);
    logit_fmtw_s(&w, "月");
    logit_fmtw_u(&w, (uint64_t)b.mday, 1);
    logit_fmtw_s(&w, "日");
    return logit_fmtw_end(&w);
}

/* ---------------------------------------------------- relative times (5.7) */

/* Notification times: under a minute "现在", then "N 分钟前", "N 小时前",
 * and from a day on "N 天前". A negative age (clock skew) is "现在". */
static inline int logit_fmt_ago(char *buf, int cap, int64_t sec)
{
    struct logit_fmtw w = logit_fmtw_begin(buf, cap);
    if (sec < 60) {
        logit_fmtw_s(&w, "现在");
    } else if (sec < 3600) {
        logit_fmtw_u(&w, (uint64_t)(sec / 60), 1);
        logit_fmtw_s(&w, " 分钟前");
    } else if (sec < 86400) {
        logit_fmtw_u(&w, (uint64_t)(sec / 3600), 1);
        logit_fmtw_s(&w, " 小时前");
    } else {
        logit_fmtw_u(&w, (uint64_t)(sec / 86400), 1);
        logit_fmtw_s(&w, " 天前");
    }
    return logit_fmtw_end(&w);
}

#endif /* LOGIT_UI_FMT_H */
