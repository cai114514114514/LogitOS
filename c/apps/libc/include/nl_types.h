#ifndef _NL_TYPES_H
#define _NL_TYPES_H

/* Message catalogs (POSIX <nl_types.h>: catopen/catgets/catclose).
 *
 * THERE IS NO MESSAGE-CATALOG FILE FORMAT ON THIS MACHINE -- no gencat, no
 * on-disk .cat files, no LC_MESSAGES lookup path, nothing. Building one would
 * mean inventing a binary format, a compiler for it, and a loader, all to
 * serve programs that call catgets() as a portability shim around strings
 * they already carry as C literals (which is what catgets() is FOR: `catgets
 * (cat, 1, 1, "hello")` returns "hello" itself whenever the catalog doesn't
 * have a translation, by design -- see the standard). That fallback path is
 * exactly the one every caller must already handle, since even on a system
 * WITH catalogs the requested one may be missing.
 *
 * So: catopen() always reports "no such catalog" (returns (nl_catd)-1),
 * catgets() always takes that fallback and returns the caller's own default
 * string, and catclose() always reports "not a valid catalog" (there is
 * never a valid one to close). None of this is a stub pretending to succeed
 * -- every one of these is the REAL, standard-mandated behaviour for "the
 * catalog you asked for is not available," which is always true here. A
 * program written against catgets() for portability (rather than as its
 * primary string source) builds and runs correctly against these three
 * functions with no code of its own aware that LogitOS has no catalog
 * subsystem. See nl_types.c-in-langinfo.c (there is no separate nl_types.c;
 * these three functions live at the bottom of langinfo.c, the closest
 * existing TU, since this header has no .c file of its own) for the
 * implementation. */

typedef void *nl_catd;
typedef int   nl_item;

/* The default message set gencat would use, and the XPG4-compliance flag for
 * catopen()'s second argument. Neither has any effect here (there is no
 * gencat, and every catopen() fails identically regardless of `flag`); they
 * are declared only so that a program's own use of the names compiles. */
#define NL_SETD       1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char *name, int flag);
char   *catgets(nl_catd catalog, int set, int number, const char *dflt);
int     catclose(nl_catd catalog);

#endif /* _NL_TYPES_H */
