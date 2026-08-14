/* basename()/dirname() (<libgen.h>) and err/warn (<err.h>) case list.
 *
 * TWO DIFFERENT GATING STRATEGIES IN ONE FILE, because the two headers have
 * different relationships to glibc:
 *
 *   - basename()/dirname() are PURE COMPUTATION (no syscall, no global
 *     state but the buffer passed in), so this file is compiled twice --
 *     once against our own <libgen.h>/libgen.c, once as an ordinary host
 *     program against glibc's -- and stdout is diffed byte for byte. Model:
 *     tests/unit/libc_fnmatch_test.c. This is the REQUIRED gate and it runs
 *     with no arguments (or "libgen").
 *
 *   - err()/warn() CANNOT be gated that way, for a reason specific to this
 *     pair: their output embeds the program's OWN NAME, and the two
 *     compiled binaries in the diff strategy above are conventionally given
 *     DIFFERENT filenames ("..._ours" / "..._glibc") -- so even a perfectly
 *     correct implementation would fail a raw diff on the progname alone,
 *     for a reason that has nothing to do with correctness. This file's
 *     "err" mode sidesteps that: argv[0] is pinned to the SAME string for
 *     both binaries by the invoking shell (`exec -a NAME ./binary err ...`,
 *     done by the gate commands in this unit's report, not by this file),
 *     and on our side main() explicitly threads that argv[0] through
 *     setprogname() -- glibc needs no such call because its CRT derives
 *     __progname from argv[0] automatically (see err.h). With argv[0]
 *     pinned identically, both sides' output is directly diffable again.
 *     err()/errx()/verr()/verrx() additionally call exit(), so each such
 *     case is run as its OWN process invocation (one case per argv, this
 *     file's "argv-driven mode") rather than in a loop inside one process
 *     -- there would be no way to observe the second case if the first one
 *     already exited. warn()/warnx()/vwarn()/vwarnx() don't exit, so all of
 *     those cases run together in a single "warnfam" invocation.
 *
 * Named errno macros are used throughout (ENOENT, EIO, EACCES, EPERM),
 * never raw integer literals assigned to `errno` -- they resolve against
 * whichever <errno.h> each build sees, exactly like the FNM_ and REG_
 * constants do in the sibling tests, so the strerror() text compared is
 * each build's own, not a shared assumption about what number means what.
 *
 * ADVERSARIAL ADDITIONS (independent verification pass). Everything below
 * the "required list" marker in `cases[]`, plus warnfam's errno==0/"%%"
 * lines, plus the "errzero"/"longname" argv-driven cases, were added by a
 * reviewer attacking this unit rather than by whoever wrote it. Two things
 * were found this way and are worth knowing about before touching this file
 * again:
 *
 *  1. dirname()'s leading-slash-run (exactly-2-vs-3+) special case and its
 *     trailing-slash-run stripping are two SEPARATE branches in libgen.c,
 *     and nothing in the original list exercised both at once (e.g.
 *     "//foo/", "///foo/"). They turned out to compose correctly, but that
 *     was worth checking rather than assuming -- the two branches share no
 *     code, so a bug in their interaction would not have shown up in either
 *     branch's cases alone.
 *  2. setprogname() used to call THIS FILE'S OWN basename() to derive the
 *     printed name from argv[0]. That is wrong: glibc's automatic
 *     __progname is a raw strrchr(argv0,'/')+1 split, not POSIX basename()
 *     -- basename() strips a trailing run of '/' first and __progname does
 *     not, so for any argv[0] ending in '/' ("foo/", "a/b/c/", "/", "//",
 *     "/usr/bin/") glibc's name is EMPTY while the basename()-based version
 *     printed a non-empty one ("foo", "c", "/", "/", "bin"). Caught by
 *     driving the "longname" case below through `exec -a` with those exact
 *     argv[0] values (see this unit's gate_rerun) and fixed in err.c's
 *     setprogname() by replacing the basename() call with the same raw
 *     strrchr() split glibc actually uses -- see the comment there. Not a
 *     contrived input either way: argv[0] is an ordinary C string and POSIX
 *     places no restriction on what bytes it holds, so a trailing '/' is a
 *     legal value on any system, this one included, whether or not the one
 *     shell shipped today happens to produce it (checked: c/apps/coreutils/
 *     sh.c's run_external() passes the interactively-typed argv[0] through
 *     unmodified to execve() -- it only prepends "/bin/" to the separate
 *     PATH it resolves, not to the argv[0] the child actually receives --
 *     so this specific shell will not hand a trailing slash to a child
 *     today; a directly execve()'d program, or a future shell change, still
 *     can, and getprogname() has to be correct for that argv[0] regardless).
 *
 * NOT added: argv[0] longer than PROGNAME_MAX-1 (63) bytes. err.c's
 * g_progname buffer is a fixed 64 bytes and truncates past that -- glibc's
 * __progname has no such bound, so those two disagree by construction, not
 * by a bug in either. That truncation is a deliberate, disclosed choice
 * (see PROGNAME_MAX's comment in err.c: real names on this system are short
 * path leaves) and there is no buffer size that would make a bounded
 * implementation agree with an unbounded one for every input, so it is
 * excluded from this gate rather than encoded as an expected-to-fail case. */
#include <libgen.h>
#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  basename()/dirname() cases
 * ------------------------------------------------------------------ */

static const char *const cases[] = {
    /* the required list from the spec, verbatim */
    "",                       /* -> "." / "."                         */
    "/",                      /* -> "/" / "/"                         */
    "//",                     /* POSIX: exactly two leading slashes is
                                * allowed to be special; glibc keeps it
                                * ("//" / "//") -- see libgen.c          */
    "///",                    /* three+ collapses: "/" / "/"          */
    "usr",                    /* -> "usr" / "."                       */
    "/usr/",                  /* -> "usr" / "/"                       */
    "a/b/",                   /* trailing slash on a multi-component path */
    "a//b",                   /* internal doubled slash                */

    /* trailing-slash-run stripping, one and many, with and without a
     * leading slash */
    "foo", "foo/", "foo//",
    "/foo", "/foo/", "/foo//",
    "trailing///", "/trailing///",

    /* ordinary multi-component splits, incl. internal doubled slashes and
     * trailing runs on the LAST component */
    "/foo/bar", "/foo/bar/", "/foo/bar//",
    "/foo//bar", "foo/bar", "foo/bar/", "foo//bar//",
    "a/b/c", "a/b/c/", "a//b//c//",
    "/a", "a/", "/a/", "/a/b",
    "/usr/lib", "/usr/lib/", "/usr//lib//",

    /* the leading-"//"-is-special case combined with more path after it */
    "//foo", "//foo/bar",

    /* dot and dot-dot, alone, trailing-slashed, and as a component */
    ".", "..", "./", "../", "./.", "../..",
    "a/.", "a/..",

    /* names that merely start with '.' or contain one -- must not be
     * mistaken for the dot/dot-dot special forms above */
    "...", "/...", ".../", "a.b/c.d",

    /* whitespace is an ordinary character to these functions -- exercises
     * that no byte is treated as special beyond '/' itself */
    "  ", " / ", "/  ",

    /* a few more depths / shapes for headroom past the required list */
    "a", "/a/b/c/d", "a/b/c/d/e/", "////a", "a////",
    "bin", "/bin", "/bin/sh", "/bin/sh/", "etc/passwd", "/etc/passwd",
    "x/y", "x/y/", "//x", "//x/", "x//", "..//", "..///", "./..//",

    /* ==================================================================
     * ADVERSARIAL: leading-slash-run (2 vs 3+) COMBINED with a trailing-
     * slash-run on the SAME string -- see the file header comment.
     * ================================================================== */
    "//foo/", "//foo//", "//foo///",
    "///foo/", "////foo/", "///foo//",
    "//a/b/", "///a/b/",

    /* degenerate all-slash strings LONGER than the 3 already covered --
     * confirms "3+ collapses to 1" isn't accidentally only true at 3. */
    "////", "/////", "//////////",

    /* leading-2-special combined with dot/dot-dot right after it */
    "//.", "//..", "//../..", "//./.",

    /* single control/whitespace bytes, and none at all */
    " ", "\t", "\t/",

    /* dot-run leading/trailing combinations not in the original list */
    ".//", "..//../", "..///..", "./..///..", "./././",
    "..a", "a..", ".....", "/.....",

    /* deep multi-component path -- strrchr/memrchr correctness shouldn't
     * depend on shallow nesting */
    "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p",
    "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/",
    "/a/b/c/d/e/f/g/h////",

    /* single components at varying leading-slash counts, with and without
     * a trailing run, rounding out the leading x trailing matrix */
    "x", "/x", "//x/", "///x", "////x////",

    /* a long single path component -- exercises that string-scan isn't
     * quietly bounded to some small length */
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/",
    "//aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/",
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

static void run_libgen(void)
{
    /* NULL is not representable inside the `cases[]` string array (it would
     * be indistinguishable from a deliberate terminator), so it is its own
     * explicit pair of calls rather than a table entry. */
    printf("NULL\tbasename=[%s]\tdirname=[%s]\n", basename(NULL), dirname(NULL));

    for (size_t i = 0; i < NCASES; i++) {
        /* basename() and dirname() each may write into the buffer they are
         * given (see libgen.h) -- two independent copies, or the dirname()
         * call would be operating on whatever basename() already mutated. */
        char bbuf[256], dbuf[256];
        size_t n = strlen(cases[i]);
        if (n >= sizeof(bbuf)) n = sizeof(bbuf) - 1;   /* cases are all short; guard anyway */
        memcpy(bbuf, cases[i], n); bbuf[n] = '\0';
        memcpy(dbuf, cases[i], n); dbuf[n] = '\0';

        char *bn = basename(bbuf);
        char *dn = dirname(dbuf);
        printf("%zu\t[%s]\tbasename=[%s]\tdirname=[%s]\n", i, cases[i], bn, dn);
    }
}

/* ------------------------------------------------------------------ *
 *  err()/warn() cases -- argv-driven, see the file header comment
 * ------------------------------------------------------------------ */

/* verr/verrx/vwarn/vwarnx take a va_list, not "...", so each needs a thin
 * varargs-to-va_list wrapper to call from a fixed-shape test case. This is
 * not extra product code, only test plumbing: verr()/verrx()/vwarn()/
 * vwarnx() themselves are exercised directly by these wrappers, and err()/
 * errx()/warn()/warnx() are exercised directly (they ARE varargs already)
 * by the cases below that call them with no wrapper at all. Between the
 * two, every one of the eight functions in err.h is called at least once. */
static void call_vwarn(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); }
static void call_vwarnx(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); }
static void call_verr(int status, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); verr(status, fmt, ap); va_end(ap); }   /* exits; va_end unreachable */
static void call_verrx(int status, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); verrx(status, fmt, ap); va_end(ap); }  /* exits; va_end unreachable */

/* warn()/warnx()/vwarn()/vwarnx() never exit, so every sub-case here runs
 * in one process and the exit code of the whole thing is 0 -- only the
 * stderr TEXT is being compared for this mode. */
static void run_warnfam(void)
{
    errno = ENOENT; warn("read %s", "a.txt");
    warnx("plain %d", 42);
    errno = EIO;    call_vwarn("vw %s", "x");
    call_vwarnx("vwx %d", 7);
    errno = EACCES; warn(NULL);          /* NULL fmt: no extra ": " before strerror */
    warnx(NULL);                         /* NULL fmt: "progname: " then just "\n"   */
    errno = EPERM;  warn("");            /* "" fmt: DOES still get its own ": " -- double colon */
    /* ADVERSARIAL: errno == 0 (success) into warn(). The file comment only
     * discusses NULL-vs-"" fmt, never what strerror(0) prints; glibc's
     * answer ("Success") is the oracle since this unit calls strerror(),
     * not reimplements it. */
    errno = 0;      warn("no error here");
    /* ADVERSARIAL: a fmt containing a literal "%%" -- confirms vfprintf is
     * really doing the formatting (collapses to one '%'), not some
     * hand-rolled substring copy that would pass the '%' through raw. */
    warnx("100%% done");
}

int main(int argc, char **argv)
{
#ifndef __GLIBC__
    /* Our own CRT (c/apps/crt0_cli.asm) has no hook to populate a progname
     * automatically (see err.h) -- so unlike the glibc build, which derives
     * its diagnostic name from argv[0] with no help from this file, our
     * side must be told explicitly. glibc's err.h does not declare
     * setprogname() at all (it is a BSD addition this library provides,
     * not a glibc one), so this call has to be compiled out of the glibc
     * build entirely, not merely made a no-op -- hence the #ifndef rather
     * than an `if`. __GLIBC__ is defined by <features.h>, which every real
     * glibc header pulls in transitively; the "ours" build never includes
     * a glibc header at all (-nostdinc), so the macro is simply absent
     * there, which is exactly the distinction this needs. */
    if (argc > 0) setprogname(argv[0]);
#endif

    if (argc <= 1 || strcmp(argv[1], "libgen") == 0) {
        run_libgen();
        return 0;
    }

    if (strcmp(argv[1], "err") == 0 && argc >= 3) {
        const char *which = argv[2];
        if (strcmp(which, "warnfam") == 0) { run_warnfam(); return 0; }
        if (strcmp(which, "err") == 0)     { errno = ENOENT; err(3, "cannot open %s", "b.txt"); }
        if (strcmp(which, "errnull") == 0) { errno = EACCES; err(2, NULL); }
        if (strcmp(which, "errx") == 0)    { errx(5, "bad arg %d", 9); }
        if (strcmp(which, "errxnull") == 0){ errx(4, NULL); }
        if (strcmp(which, "verr") == 0)    { errno = EIO; call_verr(7, "verr %s", "y"); }
        if (strcmp(which, "verrx") == 0)   { call_verrx(9, "verrx only"); }
        /* ADVERSARIAL: status 0. err()/errx() take any int status (BSD's
         * signature has no special-cases 0) and must still print + exit(0)
         * -- not be short-circuited by an implementation that conflates
         * "status" with "is this an error". */
        if (strcmp(which, "errzero") == 0) { errno = ENOENT; err(0, "zero status"); }
        /* ADVERSARIAL: no formatting of its own -- this case exists so the
         * GATE SCRIPT can drive it through `exec -a NAME` at argv[0] values
         * chosen to attack getprogname()/setprogname(), in particular any
         * argv[0] ending in '/' (see this file's header comment, item 2). */
        if (strcmp(which, "longname") == 0) { warnx("under a long progname"); return 0; }
        fprintf(stderr, "unknown err case: %s\n", which);
        return 64;
    }

    fprintf(stderr, "usage: %s [libgen | err <case>]\n", argv[0]);
    return 64;
}
