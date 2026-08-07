#ifndef _LOCALE_H
#define _LOCALE_H
#include <stddef.h>

/* THE ONLY LOCALE IS "C".
 *
 * This is not a stub in the "returns whatever, calls it done" sense -- it is
 * the standard-conformant minimum, which C11 7.11 explicitly provides for: an
 * implementation may support only the "C" locale. setlocale() therefore SUCCEEDS
 * for "C", "POSIX" and "" (the last because C says "" selects the
 * implementation-defined native environment, and here that IS the C locale),
 * and FAILS -- returns NULL -- for anything else. A program that asks for
 * de_DE.UTF-8 gets a NULL it can act on, not a silent lie that leaves it
 * formatting numbers with the wrong decimal separator and never knowing.
 *
 * localeconv() returns the C locale's lconv, which the standard specifies
 * exactly: "." for the decimal point and CHAR_MAX / "" for everything not
 * available. Every field below is that specified value.
 *
 * The one place LogitOS deviates from the C locale is the multibyte encoding,
 * which is UTF-8 rather than ASCII; see the note at the top of <wchar.h>. */

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

char *setlocale(int, const char *);
struct lconv *localeconv(void);

#endif /* _LOCALE_H */
