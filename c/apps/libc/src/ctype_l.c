/* POSIX locale-explicit classification and collation.  Since the only locale
 * handle newlocale can create is C, these are exact adapters to the base C
 * locale functions, not selectors that silently ignore a requested locale. */
#include <ctype.h>
#include <wctype.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <locale.h>

#define CTYPE_L(name) int name##_l(int c, locale_t l) { (void)l; return name(c); }
CTYPE_L(isalnum)
CTYPE_L(isalpha)
CTYPE_L(isblank)
CTYPE_L(iscntrl)
CTYPE_L(isdigit)
CTYPE_L(isgraph)
CTYPE_L(islower)
CTYPE_L(isprint)
CTYPE_L(ispunct)
CTYPE_L(isspace)
CTYPE_L(isupper)
CTYPE_L(isxdigit)
CTYPE_L(isascii)
#undef CTYPE_L
int tolower_l(int c, locale_t l) { (void)l; return tolower(c); }
int toupper_l(int c, locale_t l) { (void)l; return toupper(c); }

int strcasecmp_l(const char *a, const char *b, locale_t l)
{ (void)l; return strcasecmp(a, b); }
int strncasecmp_l(const char *a, const char *b, size_t n, locale_t l)
{ (void)l; return strncasecmp(a, b, n); }
int strcoll_l(const char *a, const char *b, locale_t l)
{ (void)l; return strcoll(a, b); }
size_t strxfrm_l(char *d, const char *s, size_t n, locale_t l)
{ (void)l; return strxfrm(d, s, n); }
int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t l)
{ (void)l; return wcscoll(a, b); }
size_t wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, locale_t l)
{ (void)l; return wcsxfrm(d, s, n); }

#define WCTYPE_L(name) int name##_l(wint_t c, locale_t l) { (void)l; return name(c); }
WCTYPE_L(iswalnum)
WCTYPE_L(iswalpha)
WCTYPE_L(iswblank)
WCTYPE_L(iswcntrl)
WCTYPE_L(iswdigit)
WCTYPE_L(iswgraph)
WCTYPE_L(iswlower)
WCTYPE_L(iswprint)
WCTYPE_L(iswpunct)
WCTYPE_L(iswspace)
WCTYPE_L(iswupper)
WCTYPE_L(iswxdigit)
#undef WCTYPE_L
wint_t towlower_l(wint_t c, locale_t l) { (void)l; return towlower(c); }
wint_t towupper_l(wint_t c, locale_t l) { (void)l; return towupper(c); }
wctype_t wctype_l(const char *s, locale_t l) { (void)l; return wctype(s); }
int iswctype_l(wint_t c, wctype_t t, locale_t l) { (void)l; return iswctype(c, t); }
wctrans_t wctrans_l(const char *s, locale_t l) { (void)l; return wctrans(s); }
wint_t towctrans_l(wint_t c, wctrans_t t, locale_t l)
{ (void)l; return towctrans(c, t); }
