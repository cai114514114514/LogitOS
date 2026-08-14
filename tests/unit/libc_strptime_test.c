/* strptime() case list, compiled twice by tests/libc.mk (see its header
 * comment and c/apps/libc/src/fnmatch.c's test for the established pattern):
 * once against OUR OWN <time.h>/strptime.c (our headers, but still linked
 * against the host libc, so anything we did NOT write -- strftime, printf,
 * memset -- resolves to glibc's; our strptime.o supplies the `strptime`
 * symbol itself, so the linker never needs glibc's), once as a completely
 * ordinary host program against glibc. Byte-identical stdout from both runs
 * is the proof.
 *
 * SENTINEL PATTERN: every case memsets its struct tm to 0xFF before parsing
 * (every int/long field becomes -1; the tm_zone pointer becomes an all-ones
 * bit pattern, captured right after the memset and compared back against,
 * never dereferenced unless it changed -- strptime never sets tm_zone, see
 * strptime.c's file header, so it should never change). -1 is chosen
 * specifically (not a larger byte pattern such as the classic memset(0x7e))
 * because glibc's OWN internal wday/yday recompute is done with plain `int`
 * arithmetic that is NOT overflow-safe for wildly out-of-range field values
 * -- verified directly: a large memset pattern feeds glibc's recompute
 * numbers around +/-2 billion, which is signed-overflow UB in glibc's own
 * implementation and therefore not a stable oracle to diff against. -1 is
 * small enough that both glibc's formula and ours (see strptime.c) compute
 * it without overflowing, so the comparison is meaningful. Every field is
 * printed every time (return offset, then all nine struct tm ints plus
 * tm_gmtoff and tm_zone) rather than only the ones a given case is "about" --
 * that is what catches a conversion that clobbers a field it has no business
 * touching (tm_isdst, most obviously: nothing in the required conversion
 * list ever sets it, and every single line below should show it unchanged).
 *
 * EXCLUDED ON PURPOSE: strptime's "%Z" accepts (and glibc's genuinely does
 * too, see strptime.c) a bare letter run with NO validation against a real
 * timezone database -- so unlike %z there is no malformed-%Z case to add
 * here for the same reason fnmatch's test excludes an unterminated bracket
 * expression: there is nothing for %Z to fail ON. That is a documented
 * property (see the case list below), not an omission.
 */
/* _GNU_SOURCE: the glibc build of this file needs it for strptime() (POSIX
 * XSI) AND for struct tm's tm_gmtoff/tm_zone (a BSD/glibc extension gated
 * separately from XSI) -- both are otherwise hidden under -std=c11's strict
 * ANSI mode with no feature-test macro set. Harmless for the "ours" build:
 * our own <time.h> does not gate anything on it (see c/apps/libc/include). */
#define _GNU_SOURCE
#include <time.h>
#include <stdio.h>
#include <string.h>

static int total_cases = 0;

static void run(const char *fmt, const char *in)
{
    struct tm tm;
    memset(&tm, 0xFF, sizeof tm);
    void *zone_sentinel = (void *)tm.tm_zone;   /* captured post-memset, see file header */

    char *r = strptime(in, fmt, &tm);
    total_cases++;
    if (!r) { printf("NULL\n"); return; }

    printf("%ld sec=%d min=%d hour=%d mday=%d mon=%d year=%d wday=%d yday=%d isdst=%d gmtoff=%ld zone=%s\n",
           (long)(r - in),
           tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year,
           tm.tm_wday, tm.tm_yday, tm.tm_isdst, tm.tm_gmtoff,
           ((void *)tm.tm_zone == zone_sentinel) ? "UNSET" : tm.tm_zone);
}

/* Like run(), but prints the tm fields even when strptime() FAILS (prefixed
 * "NULL" instead of an offset). The original run() suppresses all field
 * output on failure, which means the harness as originally written could
 * never tell whether a directive that succeeded BEFORE a later directive
 * failed left its partial write behind in *tm -- a real, observable glibc
 * behaviour (one-pass parser, no rollback) that a byte-identical "NULL\n" vs
 * "NULL\n" diff would silently fail to check. run_full() closes that hole. */
static void run_full(const char *fmt, const char *in)
{
    struct tm tm;
    memset(&tm, 0xFF, sizeof tm);
    void *zone_sentinel = (void *)tm.tm_zone;

    char *r = strptime(in, fmt, &tm);
    total_cases++;
    printf("%s sec=%d min=%d hour=%d mday=%d mon=%d year=%d wday=%d yday=%d isdst=%d gmtoff=%ld zone=%s\n",
           r ? "OK" : "NULL",
           tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year,
           tm.tm_wday, tm.tm_yday, tm.tm_isdst, tm.tm_gmtoff,
           ((void *)tm.tm_zone == zone_sentinel) ? "UNSET" : tm.tm_zone);
}

/* Resumption: feed the pointer strptime() returns back in as the buf of a
 * SECOND, separate strptime() call against the same struct tm, the way real
 * callers split "date" and "time" parsing. Each call gets its OWN fresh
 * internal pstate (century/yy/hour12/is_pm/have_*), so this also checks that
 * a field the first call's end-of-parse recompute derived (e.g. tm_wday from
 * a month seen only in call 1) survives untouched through a second call whose
 * format never mentions it -- glibc's recompute is entirely intra-call, and
 * this is the only place in the suite that calls strptime() twice. */
static void chain(const char *s0, const char *fmt1, const char *fmt2)
{
    struct tm tm;
    memset(&tm, 0xFF, sizeof tm);
    void *zone_sentinel = (void *)tm.tm_zone;

    char *p1 = strptime(s0, fmt1, &tm);
    total_cases++;
    if (!p1) { printf("CHAIN1-NULL\n"); return; }
    printf("chain1_off=%ld ", (long)(p1 - s0));

    char *p2 = strptime(p1, fmt2, &tm);
    total_cases++;
    if (!p2) { printf("CHAIN2-NULL\n"); return; }
    printf("chain2_off=%ld sec=%d min=%d hour=%d mday=%d mon=%d year=%d wday=%d yday=%d isdst=%d gmtoff=%ld zone=%s\n",
           (long)(p2 - p1),
           tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year,
           tm.tm_wday, tm.tm_yday, tm.tm_isdst, tm.tm_gmtoff,
           ((void *)tm.tm_zone == zone_sentinel) ? "UNSET" : tm.tm_zone);
}

/* Round-trip: strftime() a real date through FMT, then strptime() it back
 * through the SAME format, printing the result exactly like run() does.
 * This is the strongest test available (task description) because it
 * exercises the two directions of the SAME directive set against each
 * other, not just against a hand-picked literal string. strftime here is
 * glibc's (see the file header -- we do not own time.c, so nothing of ours
 * provides that symbol in the "ours" build either). */
static void roundtrip(const char *fmt, int sec, int min, int hour, int mday, int mon, int year)
{
    struct tm src;
    memset(&src, 0, sizeof src);
    src.tm_sec = sec; src.tm_min = min; src.tm_hour = hour;
    src.tm_mday = mday; src.tm_mon = mon; src.tm_year = year;
    char buf[128];
    size_t n = strftime(buf, sizeof buf, fmt, &src);
    total_cases++;
    if (n == 0 && fmt[0]) { printf("STRFTIME-EMPTY\n"); return; }
    printf("text=[%s] ", buf);
    run(fmt, buf);
}

int main(void)
{
    /* ---- %a / %A: abbreviated and full weekday, and glibc accepts either
     * spelling for EITHER directive (verified against real glibc). ---- */
    run("%a", "Mon");
    run("%a", "Monday");
    run("%A", "Thu");
    run("%A", "Thursday");
    run("%a", "sun");            /* case-insensitive */
    run("%A", "SATURDAY");
    run("%a", "Frx");            /* malformed: no weekday matches */

    /* ---- %b / %B / %h: abbreviated and full month; %h is %b's alias.
     * "July" vs its own abbrev "Jul" is the case that catches an
     * abbrev-first search (see strptime.c's match_name comment). ---- */
    run("%b", "Jan");
    run("%b", "January");
    run("%B", "Jul");
    run("%B", "July");
    run("%h", "Dec");
    run("%h", "December");
    run("%b", "Smarch");         /* malformed */

    /* ---- %c: the fixed C-locale string ("%a %b %e %H:%M:%S %Y"). ---- */
    run("%c", "Wed Mar 15 12:00:00 2023");
    run("%c", "Sun Jan  1 00:00:01 1999");

    /* ---- %C / %y interplay: century combines with a 2-digit year seen
     * either before or after it (see strptime.c's file header). ---- */
    run("%C", "20");
    run("%C", "19");
    run("%C%y", "2023");
    run("%y%C", "2320");
    run("%y", "68");             /* POSIX pivot: 69-99 -> 1900s */
    run("%y", "69");
    run("%y", "00");             /* 0-68 -> 2000s */
    run("%y", "5");

    /* ---- %d / %e: day of month, 1 or 2 digits, leading whitespace
     * skipped, %e additionally allows a space in place of a leading zero. */
    run("%d", "5");
    run("%d", "05");
    run("%d", "31");
    run("%d", "-5");             /* malformed: no sign accepted */
    run("%d", "32");             /* malformed: out of range */
    run("%d", "00");             /* malformed: below range */
    run("%e", "15");
    run("%e", " 5");
    run("%e", "5");

    /* ---- %D ("%m/%d/%y") and %F ("%Y-%m-%d"). ---- */
    run("%D", "03/15/23");
    run("%D", "12/31/99");
    run("%F", "2023-03-15");
    run("%F", "1999-12-31");

    /* ---- %g / %G: ISO week-based year, parsed and DISCARDED -- verified
     * directly against glibc (see strptime.c file header): even a full
     * "%G-W%V-%u" leaves tm_year/tm_mon/tm_mday untouched. ---- */
    run("%g", "23");
    run("%g", "99");
    run("%G", "2023");
    run("%G", "1999");
    run("%G-W%V-%u", "2023-W11-3");

    /* ---- %H / %I / %p: 24h vs 12h+meridiem, both %p orders. ---- */
    run("%H", "0");
    run("%H", "23");
    run("%H", "24");             /* malformed: out of range */
    run("%H", " 5");
    run("%I", "1");
    run("%I", "12");
    run("%I", "00");             /* malformed: below range */
    run("%I", "13");             /* malformed: out of range */
    run("%I %p", "05 PM");
    run("%p %I", "PM 05");
    run("%I %p", "12 PM");       /* noon */
    run("%I %p", "12 AM");       /* midnight */
    run("%I %p", "05 am");       /* case-insensitive meridiem */
    run("%p", "AM");             /* %p alone never touches tm_hour */
    run("%p", "PM");
    run("%p", "XX");             /* malformed */

    /* ---- %j: day of year, 1-366, 3 digits with the get_number backtrack
     * (see strptime.c's file header for exactly why "367" fails outright
     * while "%M" against "60" below does NOT). ---- */
    run("%j", "1");
    run("%j", "045");
    run("%j", "366");
    run("%j", "367");            /* malformed */
    run("%j", "000");            /* malformed: below range */

    /* ---- %m: month number, 1-12. ---- */
    run("%m", "1");
    run("%m", "12");
    run("%m", "13");             /* malformed */
    run("%m", "00");             /* malformed */

    /* ---- %M: minute, 0-59, and the backtrack case named above: "60"
     * succeeds by reading only ONE digit (0-59 cannot hold "60..."). ---- */
    run("%M", "0");
    run("%M", "59");
    run("%M", "60");
    run("%M", "99");

    /* ---- %n / %t: any amount of whitespace, including none -- same rule
     * as literal whitespace in the format (tested separately below). ---- */
    run("%nX", "X");
    run("%nX", "   X");
    run("a%tb", "a\tb");
    run("a%tb", "a\n\n b");

    /* ---- %r ("%I:%M:%S %p"), %R ("%H:%M"), %T ("%H:%M:%S"). ---- */
    run("%r", "12:00:00 PM");
    run("%r", "12:00:00 AM");
    run("%R", "13:05");
    run("%R", "00:00");
    run("%T", "13:05:07");
    run("%T", "23:59:61");       /* leap second, valid per %S's range */

    /* ---- %S: second, 0-61 (leap seconds), 2 digits with backtrack. ---- */
    run("%S", "0");
    run("%S", "59");
    run("%S", "60");
    run("%S", "61");
    run("%S", "62");             /* malformed */

    /* ---- %u / %w: ISO (1-7, Sunday=7) vs POSIX (0-6, Sunday=0) weekday
     * numbering; both set tm_wday directly. ---- */
    run("%u", "1");
    run("%u", "7");              /* Sunday -> tm_wday 0 */
    run("%u", "0");              /* malformed */
    run("%u", "8");              /* malformed */
    run("%w", "0");
    run("%w", "6");
    run("%w", "7");              /* malformed */

    /* ---- %U / %W / %V: week-of-year, parsed and DISCARDED (see file
     * header) -- range is 0-53 for all three per glibc, verified directly
     * (54 fails on each; note %V accepts 00, not just 01+). ---- */
    run("%U", "00");
    run("%U", "53");
    run("%U", "54");             /* malformed */
    run("%W", "00");
    run("%W", "53");
    run("%W", "54");             /* malformed */
    run("%V", "00");
    run("%V", "53");
    run("%V", "54");             /* malformed */

    /* ---- %x ("%m/%d/%y"), %X ("%H:%M:%S"). ---- */
    run("%x", "03/15/23");
    run("%x", "12/31/99");
    run("%X", "12:00:00");
    run("%X", "23:59:00");

    /* ---- %Y: absolute year, up to 4 digits, no sign. ---- */
    run("%Y", "2023");
    run("%Y", "23");
    run("%Y", "0");
    run("%Y", "-5");             /* malformed: no sign accepted */
    run("%Y", "202399");         /* reads only 4 digits, trailing "99" left */

    /* ---- %z: numeric offset, all forms named in the task. ---- */
    run("%z", "+0000");
    run("%z", "-0730");
    run("%z", "Z");
    run("%z", "z");              /* malformed: glibc accepts 'Z' but NOT 'z' */
    run("%z", "+05:30");
    run("%z", "+05");
    run("%z", "-1200");
    run("%z", "junk");           /* malformed */

    /* ---- %Z: consumes a maximal non-whitespace run and sets NOTHING (see
     * file header) -- always succeeds, even on zero characters. ---- */
    run("%Z", "UTC");
    run("%Z", "PST");
    run("%Z", "+05:00");         /* not validated as a real zone name */
    run("%Z", "");               /* zero characters: still succeeds */
    run("%Z next", "PST next");  /* stops at the whitespace boundary */

    /* ---- %%, E/O modifiers (accepted & ignored in the C locale, see file
     * header), and the flag/width prefix glibc also accepts & ignores. ---- */
    run("100%%", "100%");
    run("%%d", "%d");
    run("%Ex", "03/15/23");
    run("%Oy", "23");
    run("%EY", "2023");
    run("%Od", "05");
    run("%E", "%E");             /* malformed: dangling E, no conversion */
    run("%O", "5");              /* malformed: dangling O */
    run("%_d", "5");             /* flag prefix accepted & ignored */
    run("%-d", "5");
    run("%0d", "5");
    run("%2d", "5");             /* width prefix accepted & ignored */
    run("%_EY", "2023");         /* flags-then-E/O: accepted */
    run("%E_Y", "2023");         /* E/O-then-flags: malformed (order matters) */

    /* ---- malformed / unrecognized conversions and literal mismatches. */
    run("%q", "x");              /* unrecognized conversion: format error */
    run("%", "x");                /* dangling '%' at end of format: error */
    run("T", "t");                /* literal match is case-SENSITIVE */
    run("t", "T");
    run("hello", "hello");
    run("hello", "goodbye");

    /* ---- literal whitespace in the format matches any amount, including
     * none, of whitespace in the input. ---- */
    run("a  b", "a b");
    run("a  b", "ab");
    run("a b", "a    b");
    run("a b", "a\t\n b");

    /* ---- partial matches: strptime succeeds on a PREFIX of the input and
     * leaves the rest for the caller; only the consumed offset differs. */
    run("%d", "15abc");
    run("%Y-%m", "2023-03-15");   /* format ends before the day */
    run("%H:%M", "13:05:07");

    /* ---- empty format / empty input. ---- */
    run("", "abc");
    run("", "");
    run("%d", "");                /* malformed: no digit to read */

    /* ---- wday/yday recompute: ONLY triggered by tm_mon being present in
     * the format (%m/%b/%B/%h, or a composite that contains one), and only
     * for whichever of wday/yday the format did not set directly. Every
     * one of these is checked against the real glibc oracle with the SAME
     * -1 sentinel in the untouched fields -- see strptime.c's file header
     * for the exact rule and why the sentinel magnitude matters. ---- */
    run("%d", "15");                          /* mday alone: NO recompute */
    run("%Y", "2023");                        /* year alone: NO recompute */
    run("%j", "045");                         /* yday alone: does NOT set wday */
    run("%a", "Mon");                         /* wday alone: does NOT set yday */
    run("%m", "3");                           /* mon alone: recomputes BOTH,
                                                  from sentinel mday/year */
    run("%b", "January");                     /* same, via the name form */
    run("%m/%d", "3/15");                     /* mon+mday, sentinel year */
    run("%m/%d/%Y", "3/15/2023");             /* full date: real weekday */
    run("%Y-%m", "2023-03");                  /* year+mon, sentinel mday */
    run("%a %Y-%m-%d", "Wed 2023-03-15");     /* explicit wday matches computed */
    run("%a %Y-%m-%d", "Mon 2023-03-15");     /* explicit wday WINS over computed */
    run("%a %j", "Mon 045");                  /* both explicit: neither recomputed */

    /* ---- the SECOND, separate reconstruction mechanism: %j plus any year
     * (%y/%Y/%C) derives the calendar date (tm_mon, tm_mday), gated on its
     * own by "!have_wday" for the WHOLE block, not just the weekday half --
     * see strptime.c's file header. Every one of these was checked against
     * real glibc with the exact scenario that pins the ordering/gating
     * down, not just a case that happens to look right. ---- */
    run("%j %Y", "059 2024");                 /* leap year: day 59 -> Feb 28 */
    run("%j %Y", "060 2024");                 /* day 60 -> Feb 29 (leap) */
    run("%j %Y", "060 2023");                 /* day 60 -> Mar 1 (non-leap) */
    run("%Y %j", "2023 045");                 /* order doesn't matter */
    run("%j %y", "045 23");                   /* %y (not %Y/%C) also counts */
    run("%j %C", "045 20");                   /* %C alone also counts */
    run("%j %Y %m", "045 2023 06");           /* explicit %m WINS; %d still derived */
    run("%j %Y %d", "045 2023 09");           /* explicit %d WINS; %m still derived */
    run("%a %j %Y", "Mon 045 2023");          /* explicit wday: whole block skipped */
    run("%m %d %j %Y", "01 01 045 2023");     /* self-inconsistent input, on purpose:
                                                  yday says Feb14, %m/%d say Jan1 --
                                                  glibc keeps both explicit fields and
                                                  computes tm_wday from Jan1, not Feb14 */

    /* ---- round trip: strftime() a real date through a format, then
     * strptime() the exact text back through the same format. Several real
     * calendar dates (incl. a leap-year Feb 29 and a year boundary) times
     * several representative formats. ---- */
    roundtrip("%Y-%m-%d %H:%M:%S", 7, 5, 13, 15, 2, 123);   /* 2023-03-15 */
    roundtrip("%c", 0, 0, 0, 1, 0, 100);                    /* 2000-01-01 */
    roundtrip("%a %b %e %H:%M:%S %Y", 59, 59, 23, 31, 11, 99); /* 1999-12-31 */
    roundtrip("%F", 0, 0, 0, 29, 1, 124);                   /* 2024-02-29 leap */
    roundtrip("%D", 0, 0, 0, 4, 6, 76);                     /* 1976-07-04 */
    roundtrip("%x %X", 30, 15, 9, 1, 0, 70);                /* 1970-01-01 */
    roundtrip("%j %Y", 0, 0, 0, 1, 0, 123);                 /* yday round trip */
    roundtrip("%u %w", 0, 0, 0, 15, 2, 123);                /* weekday numbering */
    roundtrip("%r", 0, 30, 14, 15, 2, 123);                 /* 12h + meridiem */
    roundtrip("%T", 45, 30, 8, 15, 2, 123);
    roundtrip("%y-%m-%d", 0, 0, 0, 1, 5, 105);              /* 2005-06-01: %y pivot */
    roundtrip("%Y-%m-%dT%H:%M:%S%z", 0, 0, 12, 1, 0, 123);  /* %z is always +0000 here, see time.c */

    /* =====================================================================
     * ADVERSARIAL ADDITIONS (verification pass) -- targeting get_number's
     * early-backtrack boundary on every bounded field that has one, the
     * %z compound-number path (which embeds get_number twice and was never
     * exercised at its own backtrack boundary), the %j+year reconstruction's
     * genuinely wild inputs (an out-of-range yday for the given year), state
     * that must survive recursion into a composite directive, match_name's
     * true-prefix-no-match case, and -- the actual blind spot in the
     * original harness -- whether a directive's write to *tm survives a
     * LATER directive's failure in the same call, which run()'s "print
     * nothing but NULL" path could never distinguish from a bug.
     * ===================================================================== */

    /* ---- get_number backtrack at the SAME boundary the file header claims
     * for %M/%H, on every other bounded field that can exhibit it (i.e.
     * `to` needs fewer digits than the field's max width `n`). Each of these
     * stops after ONE digit because a second digit would already exceed
     * `to`, leaving the second digit for the caller -- unlike the field's
     * own "read all N digits, then fail" case already covered above. ---- */
    run("%d", "99");              /* to=31,n=2: stops after '9', leaves "9" */
    run("%e", "99");
    run("%m", "99");              /* to=12,n=2: stops after '9', leaves "9" */
    run("%I", "99");              /* to=12,n=2 */
    run("%S", "70");              /* to=61,n=2: stops after '7', leaves "0" */
    run("%H", "99");              /* to=23,n=2 */
    run("%j", "999");             /* to=366,n=3: stops after 2 digits ("99"),
                                      leaves "9" -- distinct from the full-3-
                                      digit-then-fail case ("367") above */
    run("%j", "400");             /* stops after 2 digits ("40"), leaves "0" */

    /* ---- %z's hh/mm are each their OWN get_number call and each has its
     * own backtrack boundary (hh: to=23,n=2; mm: to=59,n=2) -- not exercised
     * by the file's plain-offset cases above, which all use in-range
     * 2-digit hh/mm that never hit the early-stop path. ---- */
    run("%z", "+9900");           /* hh backtracks to '9' (90>23), then the
                                      leftover '9' is itself read as mm and
                                      ALSO backtracks (90>59) */
    run("%z", "+2400");           /* hh reads both digits (20<=23 first, so
                                      no backtrack), THEN fails range: 24>23 */
    run("%z", "+0060");           /* hh in range at 2 digits; mm backtracks
                                      to '6' (60>59), leaves "0" */
    run("%z", "+05X30");          /* no ':' and no digit follows hh, so mm is
                                      never attempted (stays glibc's parsed 0)
                                      -- offset should stop right after "05",
                                      leaving "X30" for the caller */
    run("%z", "+");               /* sign with no digits at all: hh's
                                      get_number sees EOS, fails -> NULL */
    run("%z", "+9");              /* hour needs BOTH digits present, unlike
                                      every other bounded field in this file
                                      -- one digit then EOS fails outright */
    run("%z", "+994");            /* hour "99" ok, then a lone trailing "4"
                                      with nothing after it: minute was
                                      attempted (digit seen) and must also be
                                      exactly 2 digits -- fails, does NOT
                                      fall back to "no minute" */
    run("%z", "+05:");            /* colon present but no digit after it:
                                      minute is not attempted at all, and the
                                      ':' itself is left UNCONSUMED -- offset
                                      stops right after the hour */
    run("%z", "+05:6");           /* colon + one digit + EOS: same
                                      mandatory-2-digit failure as "+994",
                                      just reached via the ':' path instead */
    run(" %z", " +0530");         /* %z skips ITS OWN leading whitespace
                                      even with no literal space in the
                                      format to trigger the general rule --
                                      this format has one anyway so the
                                      general rule and %z's own agree here */
    run("%z", " +0530");          /* the format has NO leading space at all;
                                      only %z's own whitespace-skip can
                                      account for this succeeding */

    /* ---- %j + year reconstruction fed a yday that does not exist in the
     * given year (367-1=366th day of a 365-day year): date_from_yday's own
     * comment says month 11 "absorbs everything ... on purpose" with no
     * upper validation, i.e. it does not clamp -- checking this against the
     * real oracle rather than trusting the comment's own claim. ---- */
    run("%j %Y", "366 2023");     /* 2023 is NOT a leap year: yday 365 is
                                      one past its last real day */

    /* ---- %C's century state is process-wide pstate, not per-directive --
     * the file header claims it survives recursion into a composite
     * sub-format (%x/%D contain their own %y). Not exercised anywhere above:
     * every existing %C case combines it with a %y in the SAME literal
     * format string, never through a recursed composite. ---- */
    run("%C%x", "2003/15/23");    /* %x = "%m/%d/%y"; the %y inside should
                                      still combine with the outer %C=20 */
    run("%C%D", "2003/15/23");    /* %D = "%m/%d/%y", same claim, different
                                      composite */
    run("%C", "0");               /* century=0 alone: degenerate tm_year */
    run("%y%C", "0000");          /* yy=0, century=0: both paths agree on 0 */

    /* ---- sentinel recompute (mechanism 1) at tm_mon's OWN boundary: the
     * Hinnant formula's `y -= (m <= 2)` branch is taken for Jan/Feb and not
     * for any other month -- the existing sentinel cases only ever use
     * %m="3" (March, NOT in that branch) or a name form of a later month.
     * Feed it Jan and Feb explicitly, sentinel mday/year still -1. ---- */
    run("%m", "1");                /* January: y -= 1 branch taken */
    run("%m", "2");                /* February: same branch, boundary month */

    /* ---- match_name: a genuine PREFIX of the abbreviation with nothing
     * else after it must NOT match -- strncasecmp comparing "Mon"/"Monday"
     * against a 2-character "Mo" has to fail on the 3rd byte ('n' vs the
     * input's NUL), not succeed because the first two bytes agreed. ---- */
    run("%A", "Mo");                /* too short for "Monday" or "Mon" */
    run("%a", "Su");                /* too short for "Sunday" or "Sun" */

    /* ---- a numeric field with nothing but whitespace to skip: get_number
     * must still fail (no digit ever appears), not read isspace() forever
     * or misread the terminator as '0'. ---- */
    run("%d", "   ");

    /* ---- bare %n/%t with no leading whitespace to consume, and nothing
     * after it in the format -- checks the "including none" rule holds even
     * at end-of-format, and that the returned offset is exactly 0. ---- */
    run("%n", "X");
    run("%n", "  X");

    /* ---- %p alone, lower/mixed case (the existing lone-%p cases are both
     * upper-case; case-insensitivity for %p was only previously checked
     * combined with %I). ---- */
    run("%p", "am");
    run("%p", "Pm");

    /* ---- a dangling '%' AFTER an otherwise-valid directive: the format
     * error has to invalidate the WHOLE parse, not just be ignored because
     * something upstream of it already matched. ---- */
    run("%Y%", "2023");

    /* ---- width-prefix digits with NO conversion character following them
     * at all (as opposed to "%2d", where the width digits are followed by a
     * real conversion) -- must be a format error, same as a bare "%". ---- */
    run("%9", "5");

    /* ---- digits AFTER an E/O modifier (as opposed to "%E_Y", which tests
     * flags-then-letters-then-E; this is E-then-digit, still the wrong
     * order) -- must also be a format error. ---- */
    run("%E2Y", "2023");

    /* ---- three-way gate interaction: %a supplies tm_wday directly (which
     * alone would disable the %j+year mechanism), %m is ALSO present (which
     * would normally trigger mechanism 1's yday recompute), but %j is ALSO
     * given directly -- mechanism 1 gates its yday half on "!have_yday"
     * independently of "!have_wday", so neither recompute should fire and
     * every field should reflect exactly what was read, not derived. ---- */
    run("%a %m %j %Y", "Mon 06 045 2023");

    /* ---- %Z's maximal-non-whitespace consumption swallowing a literal
     * delimiter that immediately follows it with no space in between --
     * contrasts with the existing "%Z next"/"PST next" (WITH a space) case,
     * which succeeds. ---- */
    run("%Z,", "PST,more");
    run("%Z next", "PSTnext");

    /* ---- exact upper-bound %Y with nothing left over (the existing
     * "202399" case only tests the width-cutoff-with-leftover shape). ---- */
    run("%Y", "9999");
    run("%Y", "0000");

    /* ---- lower-case meridiem inside a composite (%r's embedded %p), not
     * previously checked -- only the bare %I/%p pair tested case-folding. */
    run("%r", "12:00:00 am");

    /* ---- empty input against a composite: must fail on the FIRST
     * sub-directive, not read past the input's NUL. ---- */
    run("%D", "");

    /* ---- run_full(): does a directive's write to *tm survive a LATER
     * directive's failure in the SAME call? glibc's strptime is one pass
     * with no rollback, so a field a successful directive wrote before the
     * point of failure should remain in *tm even though the call returns
     * NULL -- and the end-of-parse wday/yday recompute (which only runs
     * after strp_body returns non-NULL) must NOT fire, even though enough
     * inputs for it (tm_mon, or tm_yday+year) were successfully read before
     * the failure. run()'s "NULL\n"-only output could never catch either
     * one going wrong; run_full() dumps the fields either way. ---- */
    run_full("%m %H", "03 24");          /* %m succeeds (mon=1) THEN %H fails
                                             (24 out of range) -- mon should
                                             stay 1, but wday/yday must NOT
                                             be recomputed (parse failed) */
    run_full("%d %m", "05 13");          /* %d succeeds (mday=5) THEN %m
                                             fails (13 out of range) -- mday
                                             should stay 5 */
    run_full("%Y %j %H", "2023 045 24"); /* both year and yday are known
                                             (mechanism 2's trigger) but %H
                                             then fails -- mday/mon must NOT
                                             get derived, because the whole
                                             call returns NULL before the
                                             end-of-parse block ever runs */
    run_full("%m %d", "03 15");          /* control: same shape, succeeds --
                                             confirms run_full agrees with
                                             run() when nothing fails */

    /* ---- resumption: feed strptime()'s return pointer back in as a second
     * call's buf against the SAME tm, the way a caller splitting "date" and
     * "time" parsing would. Checks that call 1's end-of-parse recompute
     * (tm_mon was supplied, so tm_wday/tm_yday get derived) is still intact
     * after call 2, whose format never mentions month/day/wday/yday at all. */
    chain("2023-03-15 13:05:07", "%Y-%m-%d", " %H:%M:%S");
    chain("Mon 2023-03-15 extra", "%a %Y-%m-%d", " extra");

    fprintf(stderr, "total cases: %d\n", total_cases);
    return 0;
}
