/* tests/unit/ui_fmt_test.c -- c/include/ui/logit_fmt.h: table of inputs to
 * exact UTF-8 outputs (design 3.6 units and dates, 5.2 clock, 5.7 times).
 *
 * Every mismatch prints `FAIL fmt-<group> <input>: got "<x>" want "<y>"` and
 * the exit status is 1. The negative control (-DUI_FMT_NEGCTL) drops the
 * space before "KB" inside the header and must fail ONLY `fmt-size`
 * (tests/ui-tokens-headers.mk greps for it). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ui/logit_fmt.h"

static int fails, checks;

static void expect(const char *group, const char *input, int rc, const char *got, const char *want)
{
    checks++;
    int wl = (int)strlen(want);
    if (rc != wl || strcmp(got, want) != 0) {
        printf("FAIL %s %s: got \"%s\" (rc %d) want \"%s\" (len %d)\n", group, input, got, rc, want, wl);
        fails++;
    }
}

static void t_size(void)
{
    static const struct { uint64_t in; const char *want; } T[] = {
        { 0, "0 字节" },
        { 1, "1 字节" },
        { 170, "170 字节" },
        { 1023, "1023 字节" },
        { 1024, "1.0 KB" },
        { 1536, "1.5 KB" },
        { 10137, "9.9 KB" },            /* 9.899 */
        { 10189, "10 KB" },             /* 9.9502: one decimal would show 10.0 */
        { 10240, "10 KB" },
        { 623616, "609 KB" },           /* design 3.6 */
        { 1047552, "1023 KB" },
        { 1048575, "1.0 MB" },          /* 1023.999 KB rounds into the next unit */
        { 1048576, "1.0 MB" },
        { 1258291, "1.2 MB" },          /* design 3.6 */
        { 10485760, "10 MB" },          /* 10 MB */
        { 157286400, "150 MB" },
        { 1073741824, "1.0 GB" },
        { 5905580032ull, "5.5 GB" },
        { 1099511627776ull, "1.0 TB" },
        { 1125899906842624ull, "1.0 PB" },
        { UINT64_MAX, "16384 PB" },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        char b[48], in[32];
        snprintf(in, sizeof in, "%llu", (unsigned long long)T[i].in);
        int rc = logit_fmt_size(b, sizeof b, T[i].in);
        expect("fmt-size", in, rc, b, T[i].want);
    }
}

static void t_items(void)
{
    char b[32];
    int rc = logit_fmt_items(b, sizeof b, 4);   expect("fmt-items", "4", rc, b, "4 项");
    rc = logit_fmt_items(b, sizeof b, 0);       expect("fmt-items", "0", rc, b, "0 项");
    rc = logit_fmt_items(b, sizeof b, 1234);    expect("fmt-items", "1234", rc, b, "1234 项");
}

/* Local epoch seconds (computed with Python's calendar.timegm). */
#define T_2026_10_27_1330 1793107800LL
#define T_2026_10_27_1308 1793106480LL
#define T_2026_10_27_0000 1793059200LL
#define T_2026_10_26_1840 1793040000LL
#define T_2026_10_26_0000 1792972800LL
#define T_2026_10_25_2359 1792972799LL
#define T_2026_10_25_0900 1792918800LL
#define T_2025_10_25_0900 1761382800LL
#define T_2026_01_01_0800 1767254400LL
#define T_2026_10_28_0900 1793178000LL
#define T_2027_01_01_1000 1798797600LL
#define T_2026_12_31_2300 1798758000LL
#define T_2026_12_30_1200 1798632000LL
#define T_2026_10_27_130005 1793106005LL

static void t_reldate(void)
{
    static const struct { const char *name; int64_t now, t; const char *want; } T[] = {
        { "today",            T_2026_10_27_1330, T_2026_10_27_1308, "今天 13:08" },
        { "today-midnight",   T_2026_10_27_1330, T_2026_10_27_0000, "今天 00:00" },
        { "yesterday",        T_2026_10_27_1330, T_2026_10_26_1840, "昨天 18:40" },
        { "yesterday-0000",   T_2026_10_27_1330, T_2026_10_26_0000, "昨天 00:00" },
        { "this-year",        T_2026_10_27_1330, T_2026_10_25_0900, "10月25日" },
        { "two-days-2359",    T_2026_10_27_1330, T_2026_10_25_2359, "10月25日" },
        { "jan-1",            T_2026_10_27_1330, T_2026_01_01_0800, "1月1日" },
        { "last-year",        T_2026_10_27_1330, T_2025_10_25_0900, "2025年10月25日" },
        { "tomorrow",         T_2026_10_27_1330, T_2026_10_28_0900, "10月28日" },
        { "new-year-yday",    T_2027_01_01_1000, T_2026_12_31_2300, "昨天 23:00" },
        { "new-year-prev",    T_2027_01_01_1000, T_2026_12_30_1200, "2026年12月30日" },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        char b[64];
        int rc = logit_fmt_reldate(b, sizeof b, T[i].now, T[i].t);
        expect("fmt-reldate", T[i].name, rc, b, T[i].want);
    }
}

static void t_tm(void)
{
    static const struct { int64_t t; int y, mo, d, h, mi, s, wd; } T[] = {
        { T_2026_10_27_130005, 2026, 10, 27, 13, 0, 5, 2 },
        { T_2025_10_25_0900, 2025, 10, 25, 9, 0, 0, 6 },
        { T_2026_01_01_0800, 2026, 1, 1, 8, 0, 0, 4 },
        { 951782400LL, 2000, 2, 29, 0, 0, 0, 2 },        /* leap day */
        { -43200LL, 1969, 12, 31, 12, 0, 0, 3 },         /* before the epoch */
        { 0, 1970, 1, 1, 0, 0, 0, 4 },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        struct logit_tm tm;
        logit_tm_from_epoch(T[i].t, &tm);
        checks++;
        if (tm.year != T[i].y || tm.mon != T[i].mo || tm.mday != T[i].d || tm.hour != T[i].h
            || tm.min != T[i].mi || tm.sec != T[i].s || tm.wday != T[i].wd) {
            printf("FAIL fmt-tm %lld: got %d-%d-%d %d:%d:%d wd %d\n", (long long)T[i].t,
                   tm.year, tm.mon, tm.mday, tm.hour, tm.min, tm.sec, tm.wday);
            fails++;
        }
    }
}

static void t_weekday(void)
{
    static const char *const S[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    static const char *const L[7] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    for (int i = 0; i < 7; i++) {
        char b[32], in[16];
        snprintf(in, sizeof in, "%d-short", i);
        int rc = logit_fmt_weekday(b, sizeof b, i, LOGIT_WD_SHORT);
        expect("fmt-weekday", in, rc, b, S[i]);
        snprintf(in, sizeof in, "%d-long", i);
        rc = logit_fmt_weekday(b, sizeof b, i, LOGIT_WD_LONG);
        expect("fmt-weekday", in, rc, b, L[i]);
    }
    char b[32];
    int rc = logit_fmt_weekday(b, sizeof b, 7, LOGIT_WD_SHORT);
    checks++;
    if (rc != -1 || b[0]) { printf("FAIL fmt-weekday 7: rc %d \"%s\"\n", rc, b); fails++; }
}

static void t_clock(void)
{
    struct logit_tm tue = { 2026, 10, 27, 13, 0, 5, 2 };
    struct logit_tm morn = { 2026, 10, 27, 9, 5, 0, 2 };
    struct logit_tm midnight = { 2026, 10, 27, 0, 7, 0, 2 };
    struct logit_tm noon = { 2026, 10, 27, 12, 30, 0, 2 };
    static const struct { const char *name; int which; unsigned flags; const char *want; } T[] = {
        { "menubar",           0, LOGIT_CLK_MENUBAR, "10月27日 周二 13:00" },
        { "menubar+sec",       0, LOGIT_CLK_MENUBAR | LOGIT_CLK_SECONDS, "10月27日 周二 13:00:05" },
        { "date+24h",          0, LOGIT_CLK_DATE | LOGIT_CLK_24H, "10月27日 13:00" },
        { "weekday+24h",       0, LOGIT_CLK_WEEKDAY | LOGIT_CLK_24H, "周二 13:00" },
        { "24h",               0, LOGIT_CLK_24H, "13:00" },
        { "24h+sec",           0, LOGIT_CLK_24H | LOGIT_CLK_SECONDS, "13:00:05" },
        { "12h",               0, 0, "下午1:00" },
        { "12h+sec",           0, LOGIT_CLK_SECONDS, "下午1:00:05" },
        { "12h-menubar",       0, LOGIT_CLK_DATE | LOGIT_CLK_WEEKDAY, "10月27日 周二 下午1:00" },
        { "24h-morning",       1, LOGIT_CLK_24H, "09:05" },
        { "12h-morning",       1, 0, "上午9:05" },
        { "12h-midnight",      2, 0, "上午12:07" },
        { "24h-midnight",      2, LOGIT_CLK_24H, "00:07" },
        { "12h-noon",          3, 0, "下午12:30" },
        { "greeter-date",      0, LOGIT_CLK_DATE | LOGIT_CLK_WEEKDAY_LONG | LOGIT_CLK_NO_TIME, "10月27日 星期二" },
        { "date-only",         0, LOGIT_CLK_DATE | LOGIT_CLK_NO_TIME, "10月27日" },
        { "weekday-only",      0, LOGIT_CLK_WEEKDAY | LOGIT_CLK_NO_TIME, "周二" },
        { "long-weekday+24h",  0, LOGIT_CLK_WEEKDAY_LONG | LOGIT_CLK_24H, "星期二 13:00" },
    };
    const struct logit_tm *tm[4] = { &tue, &morn, &midnight, &noon };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        char b[64];
        int rc = logit_fmt_clock(b, sizeof b, tm[T[i].which], T[i].flags);
        expect("fmt-clock", T[i].name, rc, b, T[i].want);
    }
}

static void t_ago(void)
{
    static const struct { int64_t s; const char *want; } T[] = {
        { -5, "现在" }, { 0, "现在" }, { 59, "现在" }, { 60, "1 分钟前" }, { 125, "2 分钟前" },
        { 3599, "59 分钟前" }, { 3600, "1 小时前" }, { 86399, "23 小时前" }, { 86400, "1 天前" },
        { 3 * 86400 + 5, "3 天前" },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        char b[32], in[24];
        snprintf(in, sizeof in, "%lld", (long long)T[i].s);
        int rc = logit_fmt_ago(b, sizeof b, T[i].s);
        expect("fmt-ago", in, rc, b, T[i].want);
    }
}

/* Too-small buffers: -1 and an empty string, never a truncated UTF-8 tail. */
static void t_cap(void)
{
    char b[16];
    struct { const char *name; int rc; } R[6];
    memset(b, 'x', sizeof b);
    R[0].name = "size-cap8";   R[0].rc = logit_fmt_size(b, 8, 170);       /* "170 字节" is 10 bytes */
    int e0 = b[0] == 0;
    R[1].name = "size-cap11";  R[1].rc = logit_fmt_size(b, 11, 170);      /* fits exactly */
    int e1 = strcmp(b, "170 字节") == 0;
    R[2].name = "clock-cap4";  R[2].rc = logit_fmt_clock(b, 4, &(struct logit_tm){ 2026, 10, 27, 13, 0, 0, 2 }, LOGIT_CLK_MENUBAR);
    int e2 = b[0] == 0;
    R[3].name = "ago-cap0";    R[3].rc = logit_fmt_ago(b, 0, 10);
    R[4].name = "items-cap1";  R[4].rc = logit_fmt_items(b, 1, 4);
    int e4 = b[0] == 0;
    int ok[5] = { R[0].rc == -1 && e0, R[1].rc == 10 && e1, R[2].rc == -1 && e2, R[3].rc == -1, R[4].rc == -1 && e4 };
    for (int i = 0; i < 5; i++) {
        checks++;
        if (!ok[i]) { printf("FAIL fmt-cap %s: rc %d\n", R[i].name, R[i].rc); fails++; }
    }
}

int main(void)
{
    t_size();
    t_items();
    t_reldate();
    t_tm();
    t_weekday();
    t_clock();
    t_ago();
    t_cap();
    if (fails) { printf("ui_fmt_test: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("ui_fmt_test: all %d checks ok\n", checks);
    return 0;
}
