#ifndef _WCTYPE_H
#define _WCTYPE_H

#include <locale.h>

/* Wide character classification.
 *
 * DEGENERATE CASE, STATED PLAINLY: classification is ASCII-correct and
 * Unicode-incomplete. iswalpha(U+00E9) is false here and true under glibc's
 * en_US.UTF-8. That is CONFORMING -- in the "C" locale, which is the only
 * locale this library has, C says the alphabetic characters are exactly the 52
 * ASCII letters, and glibc agrees when it is actually in the C locale. It is
 * still a real limitation for a program that expects a UTF-8 locale's answers,
 * so it is written down rather than hidden. towlower/towupper likewise map only
 * the ASCII pair; every other code point maps to itself.
 *
 * There is deliberately NO exception: adding, say, the Unicode space
 * separators to iswspace would make this library disagree with a real C locale
 * in a way no caller asked for. Code that needs Unicode properties should use
 * the browser's own tables (c/lib/text), which have them. */

#ifndef __LIBC_WCHAR_T_DEFINED
#define __LIBC_WCHAR_T_DEFINED
typedef __WCHAR_TYPE__ wchar_t;
#endif
#ifndef WEOF
typedef int wint_t;
#define WEOF ((wint_t)-1)
#endif

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t);
int iswalpha(wint_t);
int iswblank(wint_t);
int iswcntrl(wint_t);
int iswdigit(wint_t);
int iswgraph(wint_t);
int iswlower(wint_t);
int iswprint(wint_t);
int iswpunct(wint_t);
int iswspace(wint_t);
int iswupper(wint_t);
int iswxdigit(wint_t);
int iswctype(wint_t, wctype_t);
wctype_t wctype(const char *);
wint_t towlower(wint_t);
wint_t towupper(wint_t);
wint_t towctrans(wint_t, wctrans_t);
wctrans_t wctrans(const char *);
int iswalnum_l(wint_t, locale_t); int iswalpha_l(wint_t, locale_t);
int iswblank_l(wint_t, locale_t); int iswcntrl_l(wint_t, locale_t);
int iswdigit_l(wint_t, locale_t); int iswgraph_l(wint_t, locale_t);
int iswlower_l(wint_t, locale_t); int iswprint_l(wint_t, locale_t);
int iswpunct_l(wint_t, locale_t); int iswspace_l(wint_t, locale_t);
int iswupper_l(wint_t, locale_t); int iswxdigit_l(wint_t, locale_t);
wint_t towlower_l(wint_t, locale_t); wint_t towupper_l(wint_t, locale_t);
wctype_t wctype_l(const char *, locale_t);
int iswctype_l(wint_t, wctype_t, locale_t);
wctrans_t wctrans_l(const char *, locale_t);
wint_t towctrans_l(wint_t, wctrans_t, locale_t);

#endif /* _WCTYPE_H */
