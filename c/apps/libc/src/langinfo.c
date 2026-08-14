/* nl_langinfo() for the "C" locale, plus catopen/catgets/catclose (see
 * <langinfo.h> and <nl_types.h> for why those three live here rather than in
 * a nl_types.c of their own -- there isn't one in this unit's file list, and
 * langinfo.c is the closest existing TU to "locale-adjacent string lookup").
 *
 * EVERY STRING BELOW WAS READ OFF REAL GLIBC, NOT GUESSED. CODESET is the
 * classic place a hand-written libc gets this wrong: it is tempting to
 * return "UTF-8" for CODESET because this whole OS IS UTF-8 end to end (see
 * <wchar.h>'s C-locale-is-UTF-8 note), but that is not what glibc's "C"
 * locale reports -- glibc's C locale is nominally ASCII, and CODESET names
 * that nominal encoding ("ANSI_X3.4-1968", the ASCII standard's own
 * designation), not whatever the process happens to be doing with bytes
 * above 0x7F. Reporting "UTF-8" here would be internally consistent for
 * LogitOS but would silently break any ported program that checks CODESET
 * before deciding how to handle 8-bit input -- and it would make this file's
 * one deliberately-stated deviation from glibc (see <wchar.h>) leak into a
 * second function that was never supposed to carry it. tests/unit/
 * libc_iconv_test.c's LANGINFO section pins every string here against a live
 * glibc nl_langinfo() call, so "looks right" was never good enough on its
 * own. (That diffed section was added during adversarial re-verification --
 * an earlier revision of this sentence claimed that coverage while the test
 * file called nl_langinfo() zero times; the values below were only ever
 * hand-checked before that. They matched once actually diffed, but the claim
 * of being diffed was, until then, simply false -- see that test file's
 * header for the full story.)
 *
 * REUSE, NOT A SECOND COPY, WHERE THE OWNERSHIP BOUNDARY ALLOWS IT.
 * RADIXCHAR/THOUSEP are read from localeconv() (<locale.h>, defined in
 * locale.c) rather than hardcoded -- localeconv() is public API, not a
 * private symbol, so calling it is real reuse, and it means these two values
 * can never drift from what localeconv() itself reports.
 *
 * WHAT COULD NOT BE REUSED, AND WHY. The day/month names and AM/PM/format
 * strings duplicate literal content that also appears in c/apps/libc/src/
 * time.c's strftime (wdayn/wdayfull/monn/monfull) -- content that SHOULD be
 * one table, per this codebase's own stated rule ("two copies of the month
 * names that can drift apart is exactly the bug to avoid"). It is a second
 * table here only because time.c's arrays are `static` (file-local) and
 * time.c is outside this unit's ownership list, so there is no symbol this
 * file is permitted to link against and no header this file is permitted to
 * add a declaration to. Duplicating the literal text (rather than, say,
 * re-deriving it some other way) is the smallest surface for the two to
 * disagree, and both copies are independently pinned against glibc by the
 * same test, so a drift would fail loudly rather than silently. This is
 * flagged again in the unit's final report as a deviation, for whoever next
 * has ownership of both files to collapse into one table. */
#include <langinfo.h>
#include <locale.h>
#include <errno.h>
#include <stddef.h>

static const char *const wday_full[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const wday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const mon_full[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *const mon_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

char *nl_langinfo(nl_item item)
{
    switch (item) {
    case CODESET:   return (char *)"ANSI_X3.4-1968";
    case D_T_FMT:   return (char *)"%a %b %e %H:%M:%S %Y";
    case D_FMT:     return (char *)"%m/%d/%y";
    case T_FMT:     return (char *)"%H:%M:%S";
    case AM_STR:    return (char *)"AM";
    case PM_STR:    return (char *)"PM";
    case RADIXCHAR: return localeconv()->decimal_point;
    case THOUSEP:   return localeconv()->thousands_sep;
    case YESEXPR:   return (char *)"^[yY]";
    case NOEXPR:    return (char *)"^[nN]";
    default: break;
    }
    if (item >= DAY_1 && item <= DAY_7)     return (char *)wday_full[item - DAY_1];
    if (item >= ABDAY_1 && item <= ABDAY_7) return (char *)wday_abbr[item - ABDAY_1];
    if (item >= MON_1 && item <= MON_12)    return (char *)mon_full[item - MON_1];
    if (item >= ABMON_1 && item <= ABMON_12) return (char *)mon_abbr[item - ABMON_1];
    return (char *)"";      /* unsupported item: nl_langinfo's own documented answer */
}

/* ---------------------------------------------------------------------- */
/* nl_types.h: catopen/catgets/catclose -- see that header for why these    */
/* three functions are the correct behaviour and not a stub.               */
/* ---------------------------------------------------------------------- */
nl_catd catopen(const char *name, int flag)
{
    (void)name; (void)flag;
    errno = ENOENT;              /* "the requested catalog could not be found" */
    return (nl_catd)(size_t)-1;  /* see iconv.c for why the cast goes via size_t */
}

char *catgets(nl_catd catalog, int set, int number, const char *dflt)
{
    (void)catalog; (void)set; (void)number;
    /* The catalog is never available (catopen() never succeeds), so this is
     * always the standard fallback path, not a special case of it. */
    return (char *)dflt;
}

int catclose(nl_catd catalog)
{
    (void)catalog;
    /* No catopen() call here ever returns a valid descriptor, so there is
     * never a valid one to close. */
    errno = EBADF;
    return -1;
}
