/* The "C" locale, and only the "C" locale. See <locale.h> for why that is a
 * conformant position rather than a shortfall, and <wchar.h> for the one place
 * LogitOS deviates from it (the multibyte encoding is UTF-8). */
#include <locale.h>
#include <limits.h>
#include <string.h>
#include "libc_internal.h"

static char cur[] = "C";

int __libc_locale_is_c(void) { return 1; }

char *setlocale(int category, const char *name)
{
    (void)category;
    if (!name) return cur;                       /* query */
    /* "" means "the implementation-defined native environment", which here IS
     * the C locale -- so it succeeds. Anything else fails honestly, so a caller
     * checking the return value learns that its locale was not applied instead
     * of silently formatting in the wrong one. */
    if (!*name || !strcmp(name, "C") || !strcmp(name, "POSIX")) return cur;
    return 0;
}

/* Exactly the values C11 7.11.2.1 specifies for the "C" locale. CHAR_MAX in a
 * char field means "not available in this locale", which is the standard's own
 * encoding, not a sentinel we invented. */
static struct lconv c_lconv = {
    (char *)".",     /* decimal_point */
    (char *)"",      /* thousands_sep */
    (char *)"",      /* grouping */
    (char *)"",      /* int_curr_symbol */
    (char *)"",      /* currency_symbol */
    (char *)"",      /* mon_decimal_point */
    (char *)"",      /* mon_thousands_sep */
    (char *)"",      /* mon_grouping */
    (char *)"",      /* positive_sign */
    (char *)"",      /* negative_sign */
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX
};

struct lconv *localeconv(void) { return &c_lconv; }
