#ifndef _LIBGEN_H
#define _LIBGEN_H

/* POSIX <libgen.h>: basename() and dirname().
 *
 * THE GLIBC TRAP THIS HEADER EXISTS TO AVOID. On glibc, <string.h> ALSO
 * declares a function called basename() -- but it is the GNU one: it never
 * modifies its argument, never allocates, and is just "everything after the
 * last '/', or the whole string if there is none" (so basename("/") is ""
 * on glibc's string.h version, not "/"). A source file that includes only
 * <string.h> and calls basename() on a Linux host silently gets that GNU
 * version. The SAME call, once <libgen.h> is also included (glibc lets
 * <libgen.h> win by #define-ing basename to __xpg_basename -- see glibc's
 * own libgen.h), gets THIS header's POSIX/XPG version instead, which:
 *   - DOES modify the buffer passed to it (POSIX explicitly permits this,
 *     and this implementation does, exactly like glibc's __xpg_basename),
 *   - treats "" as "." and "/" as "/" rather than "",
 *   - may return a pointer to internal/static storage instead of into the
 *     caller's buffer for the all-slashes cases.
 * A ported program that assumes the POSIX behaviour (most of them: it is the
 * one in the POSIX spec and the one every man page describes) but only
 * included <string.h> on Linux would have silently gotten the OTHER
 * function there. Our own <string.h> does not declare a basename() at all
 * (see string.h), so on THIS system there is no ambiguity to silently fall
 * into either way -- but ported source that writes `char *b = basename(p);`
 * still needs `#include <libgen.h>` to get a declaration at all, and this is
 * the only basename() this library has, so that's what it gets. Passing a
 * STRING LITERAL to either function is a caller bug on any system (POSIX
 * basename() may write into it); the test file below uses only mutable
 * buffers for exactly that reason.
 *
 * Both functions may return a pointer into the buffer passed in, OR a
 * pointer to a string literal / static buffer (for the "/" and "."
 * results) -- callers must not assume either, and must not free() the
 * result or expect it to survive past the next call. That is the POSIX
 * contract, not a shortcut taken here.
 */

char *basename(char *path);
char *dirname(char *path);

#endif /* _LIBGEN_H */
