/* <err.h>: err/errx/warn/warnx and their va_list twins, plus the
 * getprogname()/setprogname() pair that stands in for glibc's automatic
 * __progname (see err.h for why there is no automatic version here).
 *
 * THE EXACT OUTPUT FORMAT WAS NOT GUESSED AT. It was probed against real
 * glibc on the host (a five-case program run through both a plain call and
 * `bash -c 'exec -a /path/name ...'` to control argv[0]) because the prose
 * description ("progname: fmt: strerror\n") hides two branches that only
 * show up at the byte level:
 *
 *   warnx("hello %d", 5)   -> "prog: hello 5\n"
 *   warnx(NULL)            -> "prog: \n"            <- ": " STILL prints
 *   warn("open %s", "f"), errno=ENOENT
 *                           -> "prog: open f: No such file or directory\n"
 *   warn(NULL), errno=EACCES
 *                           -> "prog: Permission denied\n"   <- NO extra ": "
 *   warn(""), errno=EIO    -> "prog: : Input/output error\n" <- DOUBLE ':'
 *
 * So: a NULL fmt is not the same as an empty-but-non-NULL fmt. NULL means
 * "print nothing here, and skip the separator that would have followed
 * it too" (warn) or "print nothing here" (warnx, whose leading "progname: "
 * is unconditional regardless). "" is a real, if empty, formatted string,
 * and still gets its own trailing ": " before strerror(). Getting this
 * exactly backwards -- e.g. always emitting the separator, or suppressing
 * it whenever the printed text happens to be empty -- is the natural bug
 * here, which is exactly why it was checked against a real implementation
 * byte-for-byte instead of implemented from memory. tests/unit/
 * libc_libgen_test.c's "err" mode exercises all five shapes above (plus
 * err/errx/verr/verrx, which additionally exit) against this exact probe,
 * with argv[0] pinned via `exec -a` so glibc's automatically-derived
 * progname and this file's explicitly-set one name the same string --
 * see that file's header comment for why that step is necessary at all.
 *
 * ERRNO IS CAPTURED BEFORE ANY OUTPUT CALL, ON PURPOSE. warn()/vwarn() are
 * the ONLY functions here that read errno, and vwarn() reads it as its
 * first statement, before the "%s: " prefix is even printed. This is not
 * paranoia: fprintf/vfprintf go through this library's buffered stdio,
 * which can make its own system calls (a buffer flush, for instance), and
 * a failing or partial one of THOSE would overwrite errno out from under a
 * version that read errno only right before formatting the strerror()
 * call at the end. Reading errno first and holding it in a local makes the
 * bug structurally impossible rather than merely unlikely. warn() itself
 * does not re-read errno at all -- it forwards straight to vwarn() via
 * va_list, so there is exactly one place errno is ever read.
 *
 * err()/errx() ARE NOT DECLARED __attribute__((noreturn)) EVEN THOUGH THEY
 * NEVER RETURN. This library's own <stdlib.h> declares `void exit(int)`
 * without a noreturn attribute (nothing marks it, because nothing here
 * models it specially), so a noreturn err()/errx() whose body's only
 * termination is a call to a non-noreturn exit() would trip clang's
 * -Winvalid-noreturn ("function declared 'noreturn' should not return")
 * under the -Wall -Wextra this tree's target build requires to be clean.
 * Matching our own exit()'s prototype -- not decorating either -- avoids
 * manufacturing that warning; callers still get the real BSD behaviour
 * (the process is gone after err()/errx() return control nowhere), just
 * without the attribute advertising it to the optimizer/-Wreturn-type. */
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Longest name this library will remember. Real program names on this
 * system are short path leaves ("sh", "libctest2", ...); this is generous
 * headroom, not a tight fit -- a longer setprogname() argument is
 * truncated (see setprogname() below) rather than rejected, because a
 * truncated-but-present name in a diagnostic is still more useful than no
 * diagnostic at all. */
#define PROGNAME_MAX 64

/* Default is "?", not "": an empty getprogname() would print "prog: " as
 * "" followed by immediately ": ", i.e. a diagnostic that LOOKS complete
 * but silently has no name in it -- indistinguishable from a name that
 * really is the empty string. "?" is visibly a placeholder, which is the
 * honest state before any setprogname() call: this system has no CRT hook
 * to populate the name automatically (see err.h), so "nobody has told me
 * my name yet" is a real, expected state, not a bug to paper over. */
static char g_progname[PROGNAME_MAX] = "?";

void setprogname(const char *name)
{
    if (!name || !name[0])
        return;   /* keep whatever name (or placeholder) was already set */

    /* Derive the printed name the way glibc's CRT derives __progname from
     * argv[0]: the text after the LAST '/', or the whole string if there is
     * none. That is a RAW split -- strrchr(name,'/') ? +1 : name -- and it
     * is NOT the same rule as libgen.h's basename(), even though basename()
     * is right here in the same header set and looks like the obvious
     * function to reuse for "the last path component". POSIX basename()
     * first strips a trailing RUN of '/' before it looks for the separator
     * (basename("foo/") == "foo", basename("/") == "/", basename("//") ==
     * "//"), but glibc's __progname derivation does not do that stripping
     * at all, so for exactly those trailing-slash inputs glibc's name is
     * the EMPTY STRING while basename()'s is not empty. That divergence is
     * not a corner case invented for this comment: this function used to
     * call basename() here, and `exec -a "foo/" ...` / `exec -a "/" ...` /
     * `exec -a "a/b/c/" ...` against real glibc all showed err()/warn()
     * printing an empty name where the basename()-based version printed
     * "foo"/"/"/"c" -- caught by the adversarial cases added to tests/unit/
     * libc_libgen_test.c's "longname" gate. Because this is a plain
     * strrchr(), not basename(), it never mutates `name`, so unlike the
     * old code there is no need to copy into a local buffer first either --
     * the caller's argv[0] (which many ported programs print again later,
     * e.g. in a usage banner) was never at risk from this operation to
     * begin with, once it isn't handed to basename(). */
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    size_t j = 0;
    for (; base[j] && j < sizeof(g_progname) - 1; j++)
        g_progname[j] = base[j];
    g_progname[j] = '\0';
}

const char *getprogname(void)
{
    return g_progname;
}

void vwarnx(const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", g_progname);
    if (fmt)
        vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

void vwarn(const char *fmt, va_list ap)
{
    int e = errno;   /* capture FIRST -- see the file comment */
    fprintf(stderr, "%s: ", g_progname);
    if (fmt) {
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, ": ");
    }
    fprintf(stderr, "%s\n", strerror(e));
}

void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarn(fmt, ap);   /* errno is read inside vwarn(), before any I/O */
    va_end(ap);
}

void verrx(int status, const char *fmt, va_list ap)
{
    vwarnx(fmt, ap);
    exit(status);
}

void verr(int status, const char *fmt, va_list ap)
{
    vwarn(fmt, ap);
    exit(status);
}

void errx(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verrx(status, fmt, ap);
    /* unreachable: verrx() -> exit(). No va_end() -- there is nothing left
     * to clean up on this ABI (va_list is a plain pointer here, not a
     * resource), and the process is gone by this point regardless. */
}

void err(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verr(status, fmt, ap);   /* unreachable past this line, see errx() above */
}
