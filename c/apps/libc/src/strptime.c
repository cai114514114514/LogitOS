/* <time.h> strptime(): the inverse of strftime() (time.c), and deliberately
 * matched against GLIBC's strptime, not the bare POSIX text -- POSIX leaves
 * most of the interesting behaviour here unspecified (which of %a/%A accept
 * which name forms, what a partially-determined date recomputes, how "%y"
 * combines with a separately-seen "%C", how a numeric field backtracks when
 * greedy reading would overshoot its range) and glibc's actual choices are
 * what ported code was written against and tested by hand. Every rule below
 * was verified against a real glibc strptime() (tests/unit/libc_strptime_test.c
 * diffs stdout byte for byte), not recalled from the standard.
 *
 * THE NUMBER READER is the one piece of glibc behaviour that looks like a bug
 * until you see why it's not: a bounded numeric field does not read N digits
 * and then range-check -- it stops EARLY, one digit before the value could
 * possibly exceed the field's maximum. "%M" against "60" reads only the '6'
 * (0-59 cannot hold a value beginning "60...", so a second digit is never
 * attempted) and succeeds with tm_min=6, leaving "0" for whatever comes next
 * in the format. "%H" against "24" reads BOTH digits, because 20 <= 23 still
 * held after the first one, and only then fails the range check with no
 * fallback -- once the greedy read runs to completion, an out-of-range result
 * is final. Reading this backwards (either "always read up to N digits, then
 * range-check" or "always stop at the first in-range prefix") passes some
 * cases and silently disagrees with glibc on others; get_number() below
 * implements the real rule and is gated on exactly this boundary (see the
 * %M/%H/%j cases in the test).
 *
 * WHAT RECOMPUTES tm_wday/tm_yday, AND WHAT DOESN'T, is the other trap named
 * in the task and it is genuinely specific: glibc recomputes both fields at
 * the very end of a parse -- but ONLY if the format supplied tm_mon (%m, %b,
 * %B, %h, or a composite like %c/%x/%D/%F that contains one of those). %d
 * alone does not trigger it; %Y alone does not; %j alone sets tm_yday
 * directly and does NOT cause tm_wday to be (re)computed from it. And the
 * recompute never overwrites a field the format DID determine directly (an
 * explicit %a is never replaced by the date-derived weekday, even when they
 * disagree -- glibc trusts whichever the input actually said). Confirmed
 * against real glibc with a poison-filled (memset 0xFF -> every int field is
 * -1) struct tm: "%m" alone on "3" leaves tm_mday=-1 and tm_year=-1 in place,
 * and STILL recomputes tm_wday/tm_yday from those very sentinel values (using
 * whatever is currently sitting in the struct, set or not) -- so the trigger
 * is "was tm_mon supplied", full stop, not "is the date now complete". The
 * recompute below reuses the same closed-form civil-calendar algorithm
 * time.c's gmtime_r() uses (Howard Hinnant's days_from_civil, exact and free
 * of the overflow that glibc's own simpler cumulative-month-table formula
 * would hit on wildly out-of-range input) -- which is why the sentinel here
 * is -1 (small in magnitude) rather than a raw memset(0x7e...) byte pattern:
 * -1 is a real, well-defined "poison" value that exercises the untouched-vs-
 * recomputed distinction without depending on int-overflow behaviour neither
 * implementation defines. See the test file's header comment for the exact
 * sentinel and why.
 *
 * %j PLUS A YEAR IS THE OTHER DATE-RECONSTRUCTION CASE, and it is a
 * genuinely separate mechanism from the tm_mon one above, gated differently
 * -- this one only fires with BOTH tm_yday AND a year (%y/%Y/%C, any of the
 * three -- verified) already known, AND only when tm_wday was NOT supplied
 * directly (an explicit %a/%A/%u/%w disables the whole thing, not just the
 * weekday part of it -- verified: "%a %j %Y" on "Mon 045 2023" leaves
 * tm_mday/tm_mon at their sentinel, it does NOT derive them and then just
 * skip overwriting tm_wday). When it fires, it inverts the civil calendar
 * (year, yday) -> (month, day), writes whichever of tm_mon/tm_mday the
 * format did NOT already set directly (an explicit %m or %d, either one,
 * survives untouched -- verified both ways with the OTHER field still
 * getting filled from the derived pair), and only THEN computes tm_wday --
 * from whatever ends up in tm_mon/tm_mday at that point, which can be a mix
 * of one explicit field and one derived one. That composite is real, not a
 * corner case to round away: "%j %Y %m" on "045 2023 06" keeps the explicit
 * June (tm_mon) but still derives tm_mday=14 from yday 45, and the weekday
 * it reports is June 14's, not February 14's (the date yday 45 actually
 * names) -- glibc computes tm_wday from whatever the struct holds at that
 * moment, inconsistent inputs and all. This block runs BEFORE the tm_mon
 * mechanism above, in this order: on "%j %Y %m" (%m present so BOTH
 * mechanisms are live), the tm_mon mechanism's own tm_wday recompute would
 * need tm_mday already filled in, and it is, because this block ran first.
 *
 * WHAT %Z AND %z DO NOT DO: %Z consumes a maximal run of non-whitespace input
 * (even zero characters -- it always succeeds) and sets NOTHING -- not
 * tm_zone, not tm_gmtoff. That looks useless and it is: glibc's strptime has
 * no timezone-name database to resolve "PST" against, so it just skips the
 * token so the rest of the format can still line up. Do not "fix" this to
 * populate tm_zone; it would diverge from the oracle. %z is the opposite --
 * it DOES set tm_gmtoff (glibc extension) from a numeric offset ("+0000",
 * "-0730", "+05:30", "+05", or the bare upper-case letter "Z" for +0000 --
 * lower-case "z" is REJECTED, verified directly; ISO 8601 allows either
 * case but glibc's strptime does not) -- but it still never touches
 * tm_isdst or tm_zone. %z also skips its OWN leading whitespace first (" Z"
 * and " +0530" both parse) independently of the general format-whitespace
 * rule, since the format string here is the bare two characters "%z" with
 * no literal space of its own to trigger it.
 *
 * %z's HOUR/MINUTE ARE NOT get_number(): this looks like just another pair
 * of bounded numeric reads and it verified out to be a genuinely different
 * rule, caught only by probing glibc directly with inputs get_number's own
 * boundary logic does not predict (a first implementation used
 * get_number(0,23,2) / get_number(0,59,2) here, which is wrong on three
 * counts). HOUR is a MANDATORY fixed-width 2-digit field with NO upper
 * bound at all -- "+9900" and "+2400" both SUCCEED (hour 99, hour 24, both
 * verbatim; %z's hour is never clamped to 23) -- while "+9" (a single digit
 * with nothing after it) FAILS outright, where get_number's backtrack would
 * have happily accepted a 1-digit hour. MINUTE is OPTIONAL (attempted only
 * when a digit follows an optional ':') but once attempted is EQUALLY
 * mandatory-2-digit -- "+05:6" and "+994" (a lone trailing digit) are hard
 * failures, not a minute silently skipped -- and IS range-checked to 0-59
 * ("+0060", "+05:99" both fail), unlike hour. The ':' itself is consumed
 * only as part of a successful minute read: "+05:" and "+05:a" both return
 * an offset of 3 (right after the hour), leaving the ':' itself in the
 * unconsumed tail. All of this was confirmed by direct probing (see the
 * case list), not inferred from the one HH:MM / HHMM shape the original
 * test set happened to check.
 *
 * %I / %p ORDER: glibc accepts "%I %p" and "%p %I" and produces the same
 * result either way, which means the two directives cannot be independent --
 * "%p" arriving before "%I" has to be remembered and applied once %I shows
 * up, not just when %p itself is seen. See struct pstate below.
 *
 * %C / %y ORDER: same shape of problem. Whichever of the two is seen LAST
 * recombines the century (default: from %C, or the 19xx/20xx POSIX pivot if
 * %C was never given) with the raw two-digit year (from %y) into tm_year, so
 * "%C%y" and "%y%C" against the same digits produce the same tm_year.
 *
 * E/O MODIFIERS: accepted and ignored, per the task -- this is the C locale,
 * there is no alternate calendar/numeral system to switch to, so "%Ex" reads
 * exactly like "%x". A dangling '%E'/'%O' with no conversion character after
 * it is a FORMAT error (glibc returns NULL for it), not a literal -- verified
 * against glibc directly (a bare trailing '%' is ALSO an error here, unlike
 * strftime, which prints it literally: the two functions disagree on this by
 * design, and matching strftime's leniency would be the wrong answer).
 *
 * FLAG/WIDTH PREFIX (glibc extension, not POSIX): a run of the same
 * strftime() flag characters used elsewhere in this library (_, -, 0, ^, #)
 * plus plain width digits, in any order, between '%' and the conversion
 * letter, is accepted and IGNORED -- glibc parses it but the width never
 * changes how many digits a numeric field reads (verified: "%2d" against
 * "1234" reads exactly 2 digits, same as plain "%d", because %d's own
 * hard-coded max is already 2). The one ordering rule that DOES matter: the
 * flag/width run must come BEFORE any E/O modifier ("%_EY" parses; "%E_Y"
 * is a format error) -- also verified directly, not assumed.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: no locale support (the C locale is the
 * only locale -- see <locale.h>), so %c/%x/%X are the fixed strings time.c's
 * strftime() already uses for them, not something a locale could vary. No
 * timezone-name resolution (see %Z above). No attempt to reconstruct a full
 * date from an ISO week date (%G/%V/%u): verified against glibc that even
 * "%G-W%V-%u" does NOT populate tm_mday/tm_mon/tm_year -- %U/%V/%W/%G/%g are
 * parsed, range-checked, and DISCARDED (only %u/%w, which set tm_wday
 * directly, have any observable effect). This is a DIFFERENT case from the
 * %j-plus-year reconstruction documented above: the ISO week number alone is
 * not enough to name a day (ISO week date needs the week-day too, and glibc
 * simply doesn't do that arithmetic here), whereas day-of-year plus any
 * ordinary year is a complete, unambiguous date and glibc DOES derive it.
 * Matching glibc on %G/%V/%U/%W means matching that reconstruction does not
 * exist for THEM specifically, not that it never happens anywhere. */
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

/* ---- the number reader: see the file header for why this backtracks ---- */
static int get_number(const char **sp, long from, long to, int n, long *out)
{
    const char *s = *sp;
    while (isspace((unsigned char)*s)) s++;   /* leading whitespace, any amount */
    if (!isdigit((unsigned char)*s)) return 0;
    long val = 0;
    int count = 0;
    do {
        val = val * 10 + (*s - '0');
        s++;
        count++;
        /* Stop BEFORE reading a digit that could only push val past `to` --
         * this is the early-backtrack rule the file header documents. */
    } while (count < n && val * 10 <= to && isdigit((unsigned char)*s));
    if (val < from || val > to) return 0;
    *sp = s;
    *out = val;
    return 1;
}

/* ---- weekday/month name tables, matched full-name-first ----
 * Every abbreviation here is a literal 3-letter PREFIX of its own full name
 * (Jul/July, Thu/Thursday, ...) and of no OTHER entry's -- so trying the full
 * names first and falling back to abbreviations is what makes "July" consume
 * all 4 characters instead of stopping at "Jul" the way an abbrev-first (or
 * shorter-match-wins) search would. Verified directly: glibc's %b against
 * "July" returns an offset of 4, not 3. */
static const char *const wdayfull[7] = {
    "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};
static const char *const wdayabbr[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *const monfull[12] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};
static const char *const monabbr[12] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

/* %a accepts full weekday names too, and %A accepts abbreviations too --
 * verified against glibc ("%A" against "Mon" parses; "%a" against "Monday"
 * parses the whole word). Both directives share this one matcher. */
static int match_name(const char *const *full, const char *const *abbr, int n,
                       const char **sp, int *idx)
{
    const char *s = *sp;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(full[i]);
        if (strncasecmp(full[i], s, len) == 0) { *sp = s + len; *idx = i; return 1; }
    }
    for (int i = 0; i < n; i++) {
        size_t len = strlen(abbr[i]);
        if (strncasecmp(abbr[i], s, len) == 0) { *sp = s + len; *idx = i; return 1; }
    }
    return 0;
}

/* ---- calendar recompute: the same closed-form Hinnant algorithm time.c's
 * gmtime_r() uses, duplicated here rather than shared because time.c keeps
 * its copy `static` (see that file) and this is the same well-understood
 * math, not a second design. Exact for the whole range of a 64-bit day
 * count, and in particular does NOT overflow the way glibc's own simpler
 * cumulative-month-table formula would on wildly out-of-range input -- which
 * is exactly why the test's poison pattern is -1 per field rather than a
 * large memset byte value (see the test file header). For any date that
 * doesn't overflow EITHER implementation's arithmetic, both compute the same
 * proleptic-Gregorian civil day number, so this matches glibc's real
 * recompute for every case this unit actually gates. */
static long strp_days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void recompute_wday(struct tm *tm)
{
    long days = strp_days_from_civil((long)tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    tm->tm_wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
}
static void recompute_yday(struct tm *tm)
{
    long y = (long)tm->tm_year + 1900;
    long days = strp_days_from_civil(y, tm->tm_mon + 1, tm->tm_mday);
    long jan1 = strp_days_from_civil(y, 1, 1);
    tm->tm_yday = (int)(days - jan1);
}
static int strp_is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* Inverse of the day-of-year calendar: (full year, 0-based tm_yday) ->
 * (0-based month, 1-based day-of-month). Used only by the %j-plus-year
 * reconstruction documented in the file header. `yday` is trusted to already
 * be in [0,365] (get_number's %j case enforces [1,366] before subtracting
 * 1) and the cumulative table's last real boundary (365/366) is never
 * consulted as an upper bound -- month 11 (December) absorbs everything
 * from its start to the end of the array on purpose. */
static void date_from_yday(long year, int yday, int *mon, int *mday)
{
    static const short cum[2][12] = {
        {0,31,59,90,120,151,181,212,243,273,304,334},
        {0,31,60,91,121,152,182,213,244,274,305,335},
    };
    const short *c = cum[strp_is_leap(year) ? 1 : 0];
    int m = 0;
    while (m < 11 && yday >= c[m + 1]) m++;
    *mon = m;
    *mday = yday - c[m] + 1;
}

/* Cross-directive state that has to survive across the whole parse (a single
 * struct tm* is not enough -- see the file header for why %p/%I and %C/%y
 * each need a value that is NOT yet committed to *tm until both halves are
 * known). century/yy start at -1 ("not seen"); hour12/is_pm start at -1
 * ("not seen") too, distinct from the 0/1 they hold once %I/%p fire. */
struct pstate {
    int century, yy;
    int hour12, is_pm;
    int have_wday, have_yday, have_mon, have_mday, have_year;
};

static const char *strp_body(const char *s, const char *f, struct tm *tm, struct pstate *st);

/* Composite directives (%c/%x/%X/%D/%F/%r/%R/%T) are literally the same
 * building blocks strftime() prints them from (time.c) -- verified against
 * glibc directly, e.g. %c against "Wed Mar 15 12:00:00 2023" parses exactly
 * like "%a %b %e %H:%M:%S %Y" would. Recursing through strp_body with the
 * SAME tm/state pointers (not a fresh state) is what lets a %C seen earlier
 * in the outer format still combine with a %y inside a recursed %x, etc. */
static const char *strp_sub(const char *s, const char *sub, struct tm *tm, struct pstate *st)
{
    return strp_body(s, sub, tm, st);
}

static const char *strp_body(const char *s, const char *f, struct tm *tm, struct pstate *st)
{
    for (;;) {
        if (*f == 0) return s;

        /* A run of whitespace IN THE FORMAT matches any amount -- including
         * NONE -- of whitespace in the input. This is the general POSIX rule
         * for strptime's literal-whitespace directives; %n/%t (below) apply
         * the identical rule for a single format character. */
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*f)) f++;
            while (isspace((unsigned char)*s)) s++;
            continue;
        }

        if (*f != '%') {
            /* Literal characters match case-SENSITIVELY and exactly -- unlike
             * the name/meridiem conversions, glibc does not fold case here
             * (verified: format "T" against input "t" fails). */
            if (*s != *f) return 0;
            f++; s++;
            continue;
        }
        f++;   /* skip '%' */

        /* Flag/width prefix: accepted and ignored (see file header). Must
         * come before any E/O modifier -- glibc rejects the reverse order. */
        while (*f == '_' || *f == '-' || *f == '0' || *f == '^' || *f == '#' ||
               (*f >= '0' && *f <= '9'))
            f++;
        if (*f == 'E' || *f == 'O') f++;   /* accepted & ignored: C locale only */

        char c = *f;
        if (!c) return 0;   /* dangling '%'/flags/E-O with no conversion: format error */
        f++;

        long val;
        switch (c) {
        case 'a': case 'A': {
            int idx;
            if (!match_name(wdayfull, wdayabbr, 7, &s, &idx)) return 0;
            tm->tm_wday = idx; st->have_wday = 1;
            break;
        }
        case 'b': case 'B': case 'h': {
            int idx;
            if (!match_name(monfull, monabbr, 12, &s, &idx)) return 0;
            tm->tm_mon = idx; st->have_mon = 1;
            break;
        }
        case 'c': s = strp_sub(s, "%a %b %e %H:%M:%S %Y", tm, st); if (!s) return 0; break;
        case 'C':
            if (!get_number(&s, 0, 99, 2, &val)) return 0;
            st->century = (int)val;
            tm->tm_year = (int)(st->century * 100 + (st->yy >= 0 ? st->yy : 0) - 1900);
            st->have_year = 1;
            break;
        case 'd': case 'e':
            if (!get_number(&s, 1, 31, 2, &val)) return 0;
            tm->tm_mday = (int)val; st->have_mday = 1;
            break;
        case 'D': s = strp_sub(s, "%m/%d/%y", tm, st); if (!s) return 0; break;
        case 'F': s = strp_sub(s, "%Y-%m-%d", tm, st); if (!s) return 0; break;
        case 'g':   /* ISO week-based year, 2-digit -- parsed & discarded, see file header */
            if (!get_number(&s, 0, 99, 2, &val)) return 0;
            break;
        case 'G':   /* ISO week-based year, 4-digit -- parsed & discarded */
            if (!get_number(&s, 0, 9999, 4, &val)) return 0;
            break;
        case 'H':
            if (!get_number(&s, 0, 23, 2, &val)) return 0;
            tm->tm_hour = (int)val;
            break;
        case 'I':
            if (!get_number(&s, 1, 12, 2, &val)) return 0;
            st->hour12 = (int)(val % 12);
            tm->tm_hour = st->hour12 + (st->is_pm == 1 ? 12 : 0);
            break;
        case 'j':
            if (!get_number(&s, 1, 366, 3, &val)) return 0;
            tm->tm_yday = (int)(val - 1); st->have_yday = 1;
            break;
        case 'm':
            if (!get_number(&s, 1, 12, 2, &val)) return 0;
            tm->tm_mon = (int)(val - 1); st->have_mon = 1;
            break;
        case 'M':
            if (!get_number(&s, 0, 59, 2, &val)) return 0;
            tm->tm_min = (int)val;
            break;
        case 'n': case 't':
            /* Same "any amount, including none" rule as literal format
             * whitespace -- glibc's %n/%t are not "require one space". */
            while (isspace((unsigned char)*s)) s++;
            break;
        case 'p':
            /* %p can arrive before OR after %I; glibc handles both orders by
             * applying the +12/+0 adjustment whenever the SECOND of the pair
             * shows up (see struct pstate's comment). Case-insensitive, like
             * the name conversions. */
            if (strncasecmp(s, "AM", 2) == 0) st->is_pm = 0;
            else if (strncasecmp(s, "PM", 2) == 0) st->is_pm = 1;
            else return 0;
            s += 2;
            if (st->hour12 >= 0) tm->tm_hour = st->hour12 + (st->is_pm ? 12 : 0);
            break;
        case 'r': s = strp_sub(s, "%I:%M:%S %p", tm, st); if (!s) return 0; break;
        case 'R': s = strp_sub(s, "%H:%M", tm, st); if (!s) return 0; break;
        case 'S':
            if (!get_number(&s, 0, 61, 2, &val)) return 0;   /* 60/61: leap seconds */
            tm->tm_sec = (int)val;
            break;
        case 'T': s = strp_sub(s, "%H:%M:%S", tm, st); if (!s) return 0; break;
        case 'u':
            if (!get_number(&s, 1, 7, 1, &val)) return 0;
            tm->tm_wday = (val == 7) ? 0 : (int)val; st->have_wday = 1;
            break;
        case 'U':   /* week-of-year (Sunday-first) -- parsed & discarded */
            if (!get_number(&s, 0, 53, 2, &val)) return 0;
            break;
        case 'V':   /* ISO week number -- parsed & discarded (see file header) */
            if (!get_number(&s, 0, 53, 2, &val)) return 0;
            break;
        case 'w':
            if (!get_number(&s, 0, 6, 1, &val)) return 0;
            tm->tm_wday = (int)val; st->have_wday = 1;
            break;
        case 'W':   /* week-of-year (Monday-first) -- parsed & discarded */
            if (!get_number(&s, 0, 53, 2, &val)) return 0;
            break;
        case 'x': s = strp_sub(s, "%m/%d/%y", tm, st); if (!s) return 0; break;
        case 'X': s = strp_sub(s, "%H:%M:%S", tm, st); if (!s) return 0; break;
        case 'y':
            if (!get_number(&s, 0, 99, 2, &val)) return 0;
            st->yy = (int)val;
            /* POSIX pivot only applies when no %C has fixed the century;
             * once one has (either order -- see file header), %y always
             * recombines with it instead. */
            tm->tm_year = (st->century >= 0)
                ? (int)(st->century * 100 + st->yy - 1900)
                : (int)(st->yy + (st->yy < 69 ? 100 : 0));
            st->have_year = 1;
            break;
        case 'Y':
            if (!get_number(&s, 0, 9999, 4, &val)) return 0;
            tm->tm_year = (int)(val - 1900);
            st->have_year = 1;
            break;
        case 'z': {
            /* Leading whitespace, any amount, is skipped first -- verified
             * directly (" +0530" and " Z" both parse, consuming the space)
             * even though the format string is bare "%z" with no literal
             * space of its own to trigger the general format-whitespace
             * rule above. This is %z's OWN behaviour, not inherited from
             * anything else in strp_body. */
            while (isspace((unsigned char)*s)) s++;
            /* Only upper-case 'Z' is the UTC marker -- verified directly
             * against glibc: lower-case 'z' is REJECTED (returns NULL), even
             * though ISO 8601 itself allows either case. Do not "fix" this
             * to accept both; it would diverge from the oracle. */
            if (*s == 'Z') { s++; tm->tm_gmtoff = 0; break; }
            int sign;
            if (*s == '+') { sign = 1; s++; }
            else if (*s == '-') { sign = -1; s++; }
            else return 0;
            /* THE HOUR AND MINUTE HERE ARE NOT get_number(): this was the
             * bug this comment replaces. get_number()'s early-backtrack rule
             * is right for every OTHER bounded numeric field in this file
             * (%H, %M, %S, ...) but %z's own two digit-pairs follow a
             * DIFFERENT, verified-by-direct-probing rule:
             *   - HOUR is a MANDATORY, fixed-width 2-digit field with NO
             *     upper-bound check at all -- "+9900" and "+2400" both
             *     SUCCEED (hour 99 and 24 both accepted verbatim; glibc's
             *     %z never clamps hour to 23), but "+9" (only one digit
             *     available) FAILS outright rather than accepting a
             *     1-digit hour the way get_number's backtrack would.
             *   - MINUTE is OPTIONAL -- attempted only when, after a
             *     possible ':', the very next character is a digit -- but
             *     once attempted it is JUST as mandatory-2-digit as the
             *     hour: a single trailing digit with nothing valid after it
             *     ("+05:6", "+994") is a hard FAILURE, not a minute that
             *     gets silently skipped. Minute IS range-checked to 0-59
             *     ("+0060" and "+05:99" both fail) -- hour and minute are
             *     validated completely differently from each other.
             *   - The ':' itself is consumed ONLY as part of a successful
             *     minute read. If no digit follows it, the ':' is left
             *     UNCONSUMED and the return offset stops right after the
             *     hour ("+05:" and "+05:a" both return an offset of 3, not
             *     4 -- the colon is still sitting in the unconsumed tail).
             * A 4-digit run with no separator (e.g. "+0530") hits the same
             * two mandatory-2-digit reads with no ':' to skip; a 6-digit
             * run (e.g. "+994059") reads exactly 2+2 and leaves the rest,
             * same as every other directive in this file that has a fixed
             * maximum width -- there is no "read everything digits allow". */
            if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) return 0;
            long hh = (s[0] - '0') * 10 + (s[1] - '0');
            s += 2;
            long mm = 0;
            const char *mp = (*s == ':') ? s + 1 : s;
            if (isdigit((unsigned char)mp[0])) {
                if (!isdigit((unsigned char)mp[1])) return 0;
                mm = (mp[0] - '0') * 10 + (mp[1] - '0');
                if (mm > 59) return 0;
                s = mp + 2;
            }
            tm->tm_gmtoff = sign * (hh * 3600 + mm * 60);
            break;
        }
        case 'Z':
            /* Consumes a maximal run of non-whitespace and sets NOTHING --
             * see the file header for why matching glibc here means matching
             * that it does nothing, not adding a timezone lookup. Always
             * succeeds, even on zero characters (verified: "%Z" against ""
             * returns an offset of 0, not NULL). */
            while (*s && !isspace((unsigned char)*s)) s++;
            break;
        case '%':
            if (*s != '%') return 0;
            s++;
            break;
        default:
            return 0;   /* unrecognized conversion: format error, not a literal */
        }
    }
}

char *strptime(const char *buf, const char *format, struct tm *tm)
{
    if (!buf || !format || !tm) { errno = EINVAL; return 0; }
    struct pstate st = { -1, -1, -1, -1, 0, 0, 0, 0, 0 };
    const char *r = strp_body(buf, format, tm, &st);
    if (!r) return 0;

    /* End-of-parse reconstruction -- TWO separate mechanisms, run in THIS
     * order (mechanism 2 first), both verified field-by-field against real
     * glibc; see the file header for the cases that pin each one down.
     *
     * Mechanism 2 (%j + a year -> the calendar date): only when the format
     * gave BOTH tm_yday and a year, and did NOT give tm_wday directly. Fills
     * whichever of tm_mon/tm_mday the format didn't set, then computes
     * tm_wday from the result -- which may be a mix of one explicit field
     * and one derived one (see file header). Ordering it BEFORE mechanism 1
     * matters: on a format with both %j+year AND %m, mechanism 1's own
     * tm_wday recompute below needs tm_mday already filled in by this block. */
    if (st.have_yday && st.have_year && !st.have_wday) {
        int dm, dd;
        date_from_yday((long)tm->tm_year + 1900, tm->tm_yday, &dm, &dd);
        if (!st.have_mday) tm->tm_mday = dd;
        if (!st.have_mon)  tm->tm_mon  = dm;
        recompute_wday(tm);
    }
    /* Mechanism 1 (tm_mon -> tm_wday/tm_yday): fires whenever the format
     * supplied tm_mon at all (%m/%b/%B/%h), independently of whether
     * tm_mday/tm_year were ever set by anything -- it uses whatever is
     * CURRENTLY in the struct, sentinel or real. Each of the two fields is
     * gated on its own (an explicit tm_wday is never replaced; an explicit
     * tm_yday is never replaced) -- these are NOT one combined condition. */
    if (st.have_mon) {
        if (!st.have_wday) recompute_wday(tm);
        if (!st.have_yday) recompute_yday(tm);
    }
    return (char *)r;
}
