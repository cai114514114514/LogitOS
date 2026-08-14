#ifndef _LANGINFO_H
#define _LANGINFO_H

/* nl_langinfo() -- locale-specific string lookups (POSIX <langinfo.h>).
 *
 * ONLY THE "C" LOCALE EXISTS ON THIS MACHINE (see <locale.h>), so every value
 * returned here is the one fixed answer C11/glibc's own "C" locale gives --
 * there is no locale argument because there is nothing to select between.
 * Every string below was read off REAL glibc (nl_langinfo() called in the C
 * locale on the host that built this library, not transcribed from memory or
 * a spec skim -- CODESET in particular is a common place to guess wrong, see
 * langinfo.c) and is pinned by tests/unit/libc_iconv_test.c's host diff (the
 * LANGINFO section of that file -- added during adversarial re-verification;
 * an earlier revision of this comment claimed that coverage while the test
 * file actually called nl_langinfo() zero times, which was itself a bug --
 * see that file's header for the full story).
 *
 * Only the items ported code actually asks for are implemented: CODESET, the
 * date/time format strings strftime's %c/%x/%X build from, AM/PM, the full
 * and abbreviated day/month names, the decimal-point and thousands
 * separators (RADIXCHAR/THOUSEP -- these mirror localeconv(), see
 * langinfo.c), and the yes/no regex pair. Anything else -- an nl_item this
 * header doesn't name, or a garbage integer -- returns "" rather than
 * crashing or reading out of bounds; that is nl_langinfo's own documented
 * behaviour for an unsupported item, not a shortfall specific to this
 * library.
 *
 * The values assigned to these constants are internal to this header and
 * need not (and do not) match glibc's own nl_item numbering -- nl_langinfo()
 * is always called with the named constant, never a raw integer, so each
 * build (this header vs. glibc's) resolves the name against its own numbering
 * and the two are never compared as integers. */

#include <nl_types.h>

enum {
    CODESET = 1,
    D_T_FMT,
    D_FMT,
    T_FMT,
    AM_STR,
    PM_STR,
    DAY_1, DAY_2, DAY_3, DAY_4, DAY_5, DAY_6, DAY_7,
    ABDAY_1, ABDAY_2, ABDAY_3, ABDAY_4, ABDAY_5, ABDAY_6, ABDAY_7,
    MON_1, MON_2, MON_3, MON_4, MON_5, MON_6,
    MON_7, MON_8, MON_9, MON_10, MON_11, MON_12,
    ABMON_1, ABMON_2, ABMON_3, ABMON_4, ABMON_5, ABMON_6,
    ABMON_7, ABMON_8, ABMON_9, ABMON_10, ABMON_11, ABMON_12,
    RADIXCHAR,
    THOUSEP,
    YESEXPR,
    NOEXPR
};

char *nl_langinfo(nl_item item);

#endif /* _LANGINFO_H */
