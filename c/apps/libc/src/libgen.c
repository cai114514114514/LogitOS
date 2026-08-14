/* <libgen.h>: basename() and dirname(), the POSIX/XPG forms.
 *
 * See libgen.h for the glibc string.h-vs-libgen.h trap this pair exists to
 * sidestep. What is left to say here is WHY the two functions look as
 * fiddly as they do: almost every hand-rolled basename()/dirname() gets the
 * ALL-SLASHES and TRAILING-SLASH-RUN cases wrong, because "split on the
 * last '/'" only works once those runs have been collapsed away first, and
 * getting that collapsing right is most of the function.
 *
 * BOTH FUNCTIONS MAY MODIFY THE STRING PASSED IN. POSIX explicitly permits
 * this (it is how they strip trailing slashes and truncate the directory
 * part in place, with no allocation and no static buffer for the common
 * case) and glibc's own __xpg_basename/dirname do it too -- this is not a
 * shortcut, it is the specified contract. Callers must pass a mutable
 * buffer, never a string literal.
 *
 * THE ALGORITHM WAS NOT GUESSED AT: every branch below was checked against
 * glibc's actual output for the hard cases (single "/", "//", "///", a
 * leading "//foo" and "//foo/bar", trailing slash RUNS, an all-slash
 * string) before being written down, because the POSIX prose description
 * ("delete trailing slashes, then split on the last remaining slash") is
 * not suffient on its own to reproduce glibc's answer for these. Two facts
 * fall out of that check that a naive implementation gets wrong:
 *
 *  1. EXACTLY TWO leading slashes are special. POSIX allows an
 *     implementation to treat a pathname starting with precisely "//" as
 *     meaningful (historically: a network-root prefix), and glibc does:
 *     dirname("//") is "//", not "/", and dirname("//foo") is "//" too.
 *     THREE OR MORE leading slashes are NOT special and collapse to a
 *     single "/" same as everywhere else: dirname("///") is "/". So the
 *     "how many leading slashes" check must distinguish exactly 2 from
 *     everything else, not just test "any".
 *  2. dirname() must swallow an entire trailing run of slashes, not just
 *     one, before it goes looking for the LAST slash that actually
 *     separates a directory from a name -- "foo/bar/" and "foo/bar///" and
 *     "foo/bar" all have to produce dirname "foo", i.e. the trailing run
 *     must not be mistaken for a separator itself.
 */
#include <libgen.h>
#include <string.h>

char *basename(char *path)
{
    /* POSIX: NULL or empty -> ".". A string literal is intentionally
     * *not* accepted for this literal case either -- we return one, but
     * only ever "." or "/", both of which the caller must never write
     * through (same contract as glibc: the return value may be a pointer
     * to static/literal storage in the "nothing to split" cases). */
    if (path == NULL || path[0] == '\0')
        return (char *)".";

    /* Strip a trailing run of '/' in place by overwriting with '\0', same
     * as glibc's __xpg_basename. Do NOT stop after removing just one --
     * "foo///" must become "foo" not "foo//". */
    char *p = path + strlen(path) - 1;
    while (p > path && *p == '/')
        *p-- = '\0';

    /* If that run ate the ENTIRE string, path was all slashes to begin
     * with (e.g. "/", "//", "///"): p sits back at path[0], which is still
     * '/' (we only ever null out positions strictly after p during the
     * loop above, so path[0] itself survives). Return it as-is -- POSIX
     * wants "/" for this case, not ".". */
    if (p == path && *p == '/')
        return path;

    /* Now there is no trailing slash run left (either there never was one,
     * or it has been nulled out above), so the last '/' still in the
     * string, if any, really is the separator before the final component. */
    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *dirname(char *path)
{
    static const char dot[] = ".";

    if (path == NULL)
        return (char *)dot;

    char *last_slash = strrchr(path, '/');

    /* If the last '/' in the string is also its last character (a
     * trailing-slash case, e.g. "foo/bar/" or "foo/bar///"), that slash is
     * NOT the separator we want -- it is part of a trailing run that has
     * to be skipped before looking for the real one. Walk back over the
     * whole run, then re-search for '/' only in what precedes it. */
    if (last_slash != NULL && last_slash != path && last_slash[1] == '\0') {
        char *runp = last_slash;
        while (runp != path && runp[-1] == '/')
            runp--;
        /* runp now sits at the front of the trailing run. If that front is
         * not the very start of the string, there is more path before it
         * to search; if it IS the start, the whole string was one trailing
         * run of slashes (e.g. "///") and last_slash is left as-is for the
         * all-slashes handling below. */
        if (runp != path)
            last_slash = memrchr(path, '/', (size_t)(runp - path));
    }

    if (last_slash == NULL)
        return (char *)dot;   /* no '/' anywhere -> "no directory part" */

    /* Determine whether EVERYTHING from the start of the string up to
     * last_slash is itself slashes (i.e. last_slash is part of a leading
     * run reaching all the way back to path[0]) -- that is the "//" /
     * "/" / "///" special case, distinct from an ordinary "dir/name"
     * split. */
    char *runp = last_slash;
    while (runp != path && runp[-1] == '/')
        runp--;

    if (runp == path) {
        /* last_slash is part of a leading slash-run starting at path[0].
         * EXACTLY two leading slashes is the one case POSIX lets an
         * implementation keep distinct from a single "/" -- glibc keeps
         * it, so this does too (see the file comment). Three or more
         * collapse to a single "/", same as one does. */
        if (last_slash == path + 1)
            last_slash++;          /* keep "//" whole: truncate after it */
        else
            last_slash = path + 1; /* keep just "/": truncate after path[0] */
    } else {
        last_slash = runp;         /* ordinary case: truncate right after
                                     * the component preceding the slash run */
    }

    last_slash[0] = '\0';
    return path;
}
