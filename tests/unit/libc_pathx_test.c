/* _GNU_SOURCE unlocks reallocarray()/getsubopt()/nftw()'s FTW_SL & co. on
 * the GLIBC build only -- the "ours" build never sees this header at all
 * (-nostdinc), so it has no effect there; without it, plain -std=c11
 * hides every one of those behind glibc's strict-conformance feature
 * test macros and the glibc build fails to compile, not just to link. */
#define _GNU_SOURCE

/* Host-diffed against glibc (tests/libc.mk's pattern -- see the model,
 * tests/unit/libc_fnmatch_test.c, and its own file comment for the
 * strategy this follows exactly): compiled once against OUR headers plus
 * OUR pathx.c/ftw.c (no glibc headers at all), once as an ordinary host
 * program against glibc, and the two runs' stdout diffed byte for byte.
 *
 * getsubopt() and reallocarray() are pure computation and need nothing
 * beyond that. realpath(), mkdtemp() and nftw()/ftw() touch a real
 * filesystem, so this file builds a small, FIXED, deterministic directory
 * tree under /tmp (not randomly named -- the whole point is that both
 * separate process runs build the IDENTICAL tree at the IDENTICAL path, so
 * their outputs are directly comparable), walks it, and removes it again.
 *
 * RUN THE TWO BUILDS ONE AFTER ANOTHER, NOT `diff <(a) <(b)`. That fixed
 * path is also the reason: process substitution starts both sides as
 * concurrent subshells, and two of THIS PROGRAM racing on the same
 * BASE -- one's rm_rf() deleting the directory the other just fopen()'d a
 * file into -- reliably produces a spurious diff or a hard "FIXTURE SETUP
 * FAILED" exit(97) from whichever one loses the race, neither of which is
 * a real result. Redirect each build's stdout to its own file first (the
 * ordinary sequential fnmatch-style test model this file otherwise
 * follows never had to say this, because it never touches anything
 * outside its own process), THEN diff the two files.
 *
 * WHY NFTW's OUTPUT IS SORTED BEFORE IT IS PRINTED. readdir() order is not
 * specified by POSIX and is NOT the same thing across filesystems, kernel
 * versions, or even two directories created the same way five minutes
 * apart on the same filesystem (ext4's htree hashing has a random seed).
 * An unsorted diff between two SEPARATE PROCESSES -- which is what "ours"
 * vs "glibc" are here, unlike the fnmatch-style tests that need no
 * filesystem at all -- would then fail for a reason that is nobody's bug:
 * both sides could visit the exact same set of entries and still print
 * them in a different order. Every nftw()/ftw() walk below collects its
 * per-entry lines into an array and sorts them lexicographically before
 * printing; only the final "did the walk itself succeed" line (which is
 * not per-entry) is left unsorted, since it is always exactly one line.
 *
 * WHY THE SYMLINK LOOP LIVES IN ITS OWN, ISOLATED DIRECTORY. Verified
 * against real glibc (see ftw.c's file banner, point 2): under the default
 * (symlinks-followed) mode, hitting a loop while resolving ANY entry
 * aborts the ENTIRE walk with -1/ELOOP, not just that one entry. If the
 * loop lived alongside the rest of the main tree, EXACTLY HOW MUCH of the
 * tree got visited before the abort would depend on unspecified readdir()
 * order -- the same nondeterminism sorting fixes for a completed walk
 * cannot fix for a walk that stops partway through for an order-dependent
 * reason. So the loop gets its own directory and its own dedicated,
 * minimal test (assert r == -1 && errno == ELOOP, nothing about which
 * entries were seen first) instead.
 *
 * THE HOST-ONLY stat()/lstat() SHIM, AND WHY IT EXISTS. Build (a) (this
 * file + pathx.c + ftw.c, our headers, no glibc headers) still LINKS
 * against the host's real libc for every function it does not define
 * itself -- opendir/readdir/closedir/readlink/mkdir/chdir/getcwd all
 * resolve to glibc's real implementations that way, and that is fine,
 * because every one of those has a scalar-only signature (or, for
 * opendir/readdir, a struct dirent layout this library deliberately mirrors
 * byte-for-byte -- ino_t/off_t/unsigned short/unsigned char/char[256], the
 * same order glibc uses) that is safe to call through OUR header while the
 * REAL glibc code runs underneath. struct stat is not one of those: this
 * library's struct stat (c/apps/libc/include/sys/stat.h) orders st_mode
 * right after st_ino, while glibc's real kernel-matching struct stat puts
 * st_nlink there instead -- same total size, different field order. Calling
 * the REAL stat()/lstat() through OUR struct stat's layout would silently
 * read every field from the wrong offset (not crash -- just quietly wrong
 * data, which is worse). ftw.c needs a real, correct struct stat (its
 * caller-visible contract is "here is the object's stat", not just a type
 * code), so build (a) alone -- gated by LIBC_OURS_HOST_TEST, defined only
 * on that compile, never on the glibc build -- provides its OWN stat()/
 * lstat() for this test, going around glibc's libc entirely via the raw
 * x86-64 stat(2)/lstat(2) syscalls (syscall numbers 4 and 6, stable ABI,
 * unchanged since x86-64 Linux shipped) and hand-filling THIS library's
 * struct stat from the kernel's actual 144-byte answer. This is test-only
 * plumbing for exactly this reason (matching tests/unit/libc_host_errno_shim.c's
 * own justification for existing) -- it is not part of what ships, and the
 * struct layout it decodes is documented stable kernel ABI, verified byte
 * offset by byte offset against a real glibc struct stat on this host
 * before being trusted here. realpath() needs none of this: it is built
 * entirely on readlink(), which is scalar-only and never touches struct
 * stat at all (see pathx.c's own file comment for why that is enough).
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ftw.h>
#include <limits.h>

#ifdef LIBC_OURS_HOST_TEST
/* ---- host-only stat()/lstat()/readlink(), see the file banner above for
 * why struct stat needs this. There turned out to be a SECOND, sharper
 * reason any of this has to go through raw syscalls rather than glibc's
 * own readlink()/opendir()/etc, discovered by this test failing in a
 * confusing way (every realpath() case NULL, errno always 0) until traced:
 * glibc's own <errno.h> makes `errno` a MACRO for `(*__errno_location())`,
 * a per-thread (here, effectively just "real") location -- but THIS
 * library's <errno.h> (c/apps/libc/include/errno.h, correct for the actual
 * target: one thread per process, no TLS anywhere) declares errno as a
 * PLAIN GLOBAL VARIABLE instead. tests/unit/libc_host_errno_shim.c gives
 * that plain variable a DEFINITION so the link succeeds -- but a real
 * glibc function that fails (readlink(), opendir(), the plain syscall()
 * wrapper used below) writes its error code through `__errno_location()`,
 * i.e. into GLIBC's location, never into this plain variable at all. Our
 * own code's `errno = X` assignments (pathx.c/ftw.c's self-detected
 * failures) DO land in the plain variable, and this test reads the SAME
 * plain variable -- so anything OUR code sets is seen correctly, and
 * anything a REAL GLIBC CALL sets is silently invisible (read back as
 * whatever the plain variable last held, typically 0). The fix used
 * throughout this block: never let a call whose errno this test needs to
 * observe reach glibc's real implementation. Go around it via the raw
 * syscall instead (ABI-stable, scalar arguments only) and copy the result
 * out of glibc's *real* location with __errno_location() by hand, into
 * OUR plain variable, so pathx.c's and ftw.c's OWN `errno` reads -- which
 * is the only errno they have ever known how to read -- see the right
 * value. */
extern int *__errno_location(void);   /* real glibc symbol: returns int*, no struct -- safe to declare by hand */

struct __host_kstat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int  st_mode;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned int  __pad0;
    unsigned long st_rdev;
    long          st_size;
    long          st_blksize;
    long          st_blocks;
    long          st_atime_sec, st_atime_nsec;
    long          st_mtime_sec, st_mtime_nsec;
    long          st_ctime_sec, st_ctime_nsec;
    long          __unused[3];
};
extern long syscall(long number, ...);   /* real glibc symbol; ABI-generic, no struct in ITS OWN signature */

static int host_stat_raw(long nr, const char *path, struct stat *out)
{
    struct __host_kstat k;
    long rc = syscall(nr, path, &k);
    if (rc < 0) { errno = *__errno_location(); return -1; }   /* see the TLS/plain-errno note above */
    memset(out, 0, sizeof *out);
    out->st_dev         = (dev_t)k.st_dev;
    out->st_ino         = (ino_t)k.st_ino;
    out->st_mode        = (mode_t)k.st_mode;
    out->st_nlink       = (nlink_t)k.st_nlink;
    out->st_uid         = (uid_t)k.st_uid;
    out->st_gid         = (gid_t)k.st_gid;
    out->st_rdev        = (dev_t)k.st_rdev;
    out->st_size        = (off_t)k.st_size;
    out->st_blksize     = k.st_blksize;
    out->st_blocks      = k.st_blocks;
    out->st_atime       = (time_t)k.st_atime_sec; out->st_atime_nsec = k.st_atime_nsec;
    out->st_mtime       = (time_t)k.st_mtime_sec; out->st_mtime_nsec = k.st_mtime_nsec;
    out->st_ctime       = (time_t)k.st_ctime_sec; out->st_ctime_nsec = k.st_ctime_nsec;
    return 0;
}
int stat(const char *path, struct stat *st)  { return host_stat_raw(4, path, st); }
int lstat(const char *path, struct stat *st) { return host_stat_raw(6, path, st); }

/* realpath() (pathx.c) is built on readlink() -- see pathx.c's own file
 * banner for why that is enough -- so ITS errno needs the same raw-syscall
 * treatment as stat()/lstat() above, for the same reason. Syscall number 89
 * is readlink(2) on x86-64, stable ABI, scalar arguments only (no struct,
 * so unlike stat -- this one didn't even need translating, only the errno
 * fix). Not needed for opendir()/readdir()/closedir(): ftw.c uses those for
 * real, full directory iteration (this library's struct dirent already
 * matches glibc's field-for-field, see the file banner), and none of this
 * test's printed assertions depend on THEIR errno on failure -- only
 * stat()/lstat()/readlink()'s failures are ever the reason a printed line
 * differs. */
ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    long rc = syscall(89, path, buf, bufsiz);
    if (rc < 0) { errno = *__errno_location(); return -1; }
    return (ssize_t)rc;
}
#endif

/* ---------------------------------------------------------------------- */
/* output: one incrementing-index line per assertion, printed via a small
 * helper so every call site looks the same. Booleans/strings/small
 * integers ONLY -- never a pointer VALUE (addresses are not the same
 * between two separate process runs even when the code is byte-identical,
 * which would make the diff fail for a reason that has nothing to do with
 * correctness). Where a case wants to show a resolved path, that is safe
 * to print because the whole fixture tree lives at a FIXED, non-random
 * path both builds construct identically. */
static int g_idx;
static void emit(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%04d %s\n", g_idx++, buf);
}

static const char *ename(int e)
{
    /* Named against each build's OWN <errno.h> -- the point of the whole
     * exercise (see tests/libc.mk's header comment): a build's errno macros
     * resolve to THAT build's numbers, so comparing by NAME compares
     * behaviour, not which build assigned ENOENT the smaller integer. */
    switch (e) {
    case ENOENT:       return "ENOENT";
    case ENOTDIR:      return "ENOTDIR";
    case EISDIR:       return "EISDIR";
    case ELOOP:        return "ELOOP";
    case EACCES:       return "EACCES";
    case EINVAL:       return "EINVAL";
    case EEXIST:       return "EEXIST";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case ENOMEM:       return "ENOMEM";
    case 0:             return "0";
    default:             return "OTHER";
    }
}

/* ---------------------------------------------------------------------- */
/* fixture setup / teardown. Uses only unlink/rmdir/opendir/readdir/d_type
 * -- no lstat -- so it needs neither build's shim and works identically
 * whether this TU sees our headers or glibc's. */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }   /* not a directory (or gone already): try removing it as a file */
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (e->d_type == DT_DIR) {
            rm_rf(child);
        } else if (unlink(child) < 0 && errno == EISDIR) {
            rm_rf(child);   /* DT_UNKNOWN fallback: turned out to be a directory after all */
        }
    }
    closedir(d);
    rmdir(path);
}

static void must(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FIXTURE SETUP FAILED: %s (errno=%d)\n", what, errno); exit(97); }
}

#define BASE "/tmp/logit_u6_pathx_fixture"

static void build_tree(void)
{
    rm_rf(BASE);
    must(mkdir(BASE, 0755) == 0, "mkdir BASE");

    /* main tree: safe under BOTH FTW_PHYS and default/logical full walks --
     * no cycle anywhere in it (see the file banner for why the loop is
     * elsewhere). */
    must(mkdir(BASE "/tree", 0755) == 0, "mkdir tree");
    must(mkdir(BASE "/tree/a", 0755) == 0, "mkdir tree/a");
    must(mkdir(BASE "/tree/a/b", 0755) == 0, "mkdir tree/a/b");
    must(mkdir(BASE "/tree/d", 0755) == 0, "mkdir tree/d");
    {
        FILE *f = fopen(BASE "/tree/a/file1.txt", "w");
        must(f != NULL, "create file1.txt");
        fputs("hi\n", f);
        fclose(f);
    }
    {
        FILE *f = fopen(BASE "/tree/d/file2.txt", "w");
        must(f != NULL, "create file2.txt");
        fputs("lo\n", f);
        fclose(f);
    }
    must(symlink("a", BASE "/tree/e") == 0, "symlink e->a");
    must(symlink("nowhere_xyz_missing", BASE "/tree/dangling") == 0, "symlink dangling");
    /* a symlink whose TARGET itself contains ".." -- distinct from "e" above,
     * whose target is a bare name with no ".." in it to get wrong. Verifies
     * a relative target is resolved against the SYMLINK'S OWN parent
     * (tree/a) and not, say, the caller's cwd or the splice point: "../d/
     * file2.txt" from tree/a must land on tree/d/file2.txt. */
    must(symlink("../d/file2.txt", BASE "/tree/a/linkdd") == 0, "symlink linkdd->../d/file2.txt");

    /* isolated symlink loop -- never walked together with `tree` */
    must(mkdir(BASE "/loop", 0755) == 0, "mkdir loop");
    must(symlink("loop2", BASE "/loop/loop1") == 0, "symlink loop1");
    must(symlink("loop1", BASE "/loop/loop2") == 0, "symlink loop2");

    /* isolated FTW_DNR fixture: chmod'd during its one test only, restored
     * immediately after -- kept out of `tree` so no other assertion's
     * expected output depends on a permission bit that changes mid-run.
     * (Under an environment running these tests as root, EACCES simply
     * never fires and this becomes a same-answer no-op on both builds --
     * see ftw.c's nftw() notes; it is not a source of a false failure
     * either way.) */
    must(mkdir(BASE "/dnrtest", 0755) == 0, "mkdir dnrtest");
    must(mkdir(BASE "/dnrtest/blocked", 0755) == 0, "mkdir dnrtest/blocked");
    {
        FILE *f = fopen(BASE "/dnrtest/blocked/child", "w");
        must(f != NULL, "create dnrtest child");
        fclose(f);
    }

    /* relative-path base for realpath()'s relative-path cases */
    must(mkdir(BASE "/relbase", 0755) == 0, "mkdir relbase");
    must(mkdir(BASE "/relbase/x", 0755) == 0, "mkdir relbase/x");
    must(mkdir(BASE "/relbase/x/y", 0755) == 0, "mkdir relbase/x/y");

    /* a plain file, for the "trailing slash after a non-directory" case */
    {
        FILE *f = fopen(BASE "/plainfile", "w");
        must(f != NULL, "create plainfile");
        fclose(f);
    }

    /* a chain of exactly 45 symlinks, l1 -> target, l2 -> l1, ..., l45 ->
     * l44, so the ELOOP boundary (verified against real glibc: 40 resolves,
     * 41 does not) can be tested exactly at and past the edge. */
    must(mkdir(BASE "/chain", 0755) == 0, "mkdir chain");
    {
        FILE *f = fopen(BASE "/chain/target", "w");
        must(f != NULL, "create chain target");
        fclose(f);
    }
    char prev[32] = "target";
    for (int i = 1; i <= 45; i++) {
        char name[32], path[PATH_MAX];
        snprintf(name, sizeof name, "l%d", i);
        snprintf(path, sizeof path, BASE "/chain/%s", name);
        must(symlink(prev, path) == 0, "symlink chain link");
        strcpy(prev, name);
    }
}

/* ========================================================================
 * getsubopt()
 * ========================================================================
 * Algorithm verified against a running glibc (not just the man page --
 * see pathx.c's file comment on the aliasing case this uncovered). */
static char *const g_tokens[] = { (char *)"ro", (char *)"rw", (char *)"size", NULL };

/* A second token table where one token is a strict PREFIX of two others
 * ("r" of "ro" of "rot"). getsubopt() must match by EXACT length
 * (tokens[cnt][vstart-*optionp] == '\0' in pathx.c) -- a "starts with"
 * comparison would wrongly match "ro" or "rot" against token "r". */
static char *const g_tokens_prefix[] = { (char *)"r", (char *)"ro", (char *)"rot", NULL };

static void run_getsubopt_tok(const char *input, char *const *toks, const char *tag)
{
    char buf[128];
    strncpy(buf, input, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *opt = buf;
    emit("getsubopt%s[%s] begin", tag, input);
    int guard = 0;
    while (*opt != '\0' && guard++ < 8) {
        char *value = NULL;
        int r = getsubopt(&opt, toks, &value);
        emit("  r=%d value=%s remaining=[%s]", r, value ? value : "(null)", opt);
    }
}

static void run_getsubopt(const char *input)
{
    run_getsubopt_tok(input, g_tokens, "");
}

static void test_getsubopt(void)
{
    run_getsubopt("ro");
    run_getsubopt("rw,size=10");
    run_getsubopt("size=10,ro");
    run_getsubopt("unknown");
    run_getsubopt("unknown=5");
    run_getsubopt("ro,,rw");
    run_getsubopt("size=");
    run_getsubopt("size,ro");
    run_getsubopt(",");
    run_getsubopt("ro,");
    run_getsubopt(",ro");
    run_getsubopt("ro,rw,size=1,unknown,size=2");

    /* PREFIX TRAP (see g_tokens_prefix's own comment): "r" is a real token,
     * and "ro"/"rot" both start with it -- none of them may cross-match. */
    run_getsubopt_tok("r", g_tokens_prefix, "2");
    run_getsubopt_tok("ro", g_tokens_prefix, "2");
    run_getsubopt_tok("rot", g_tokens_prefix, "2");
    run_getsubopt_tok("rot=5", g_tokens_prefix, "2");

    /* leading '=' -- an EMPTY suboption name, value is everything after it.
     * No token starts with '=' so this always falls to the "unmatched"
     * branch, but it still has to compute vstart/endp without underflowing
     * anything when the name-length is zero. */
    run_getsubopt("=foo");

    /* a SECOND '=' inside the value: only the FIRST one is the separator;
     * getsubopt() must not re-split on it. */
    run_getsubopt("size=10=20");

    /* direct call on an already-empty string: -1, no crash */
    {
        char empty[] = "";
        char *opt = empty;
        char *value = (char *)0x1;
        int r = getsubopt(&opt, g_tokens, &value);
        emit("empty-direct r=%d", r);
    }
}

/* ========================================================================
 * reallocarray()
 * ========================================================================
 * Every call goes through a noinline wrapper with non-constant arguments.
 * Verified necessary on this host: at -O1, a reallocarray() call whose
 * arguments are visible compile-time constants at the call site can take a
 * fortify-source fast path in glibc that skips setting errno on overflow
 * (still returns NULL, but leaves errno untouched) -- an artifact of THAT
 * optimisation, not of reallocarray()'s documented contract, and not
 * something this library's freestanding build has any equivalent of to
 * accidentally diverge on. Routing every call through a noinline function
 * of genuine runtime arguments sidesteps it on both builds identically. */
__attribute__((noinline)) static void *ra_call(void *ptr, size_t n, size_t sz)
{
    errno = 0;
    return reallocarray(ptr, n, sz);
}

static void run_reallocarray(const char *label, size_t n, size_t sz, int write_check)
{
    void *p = ra_call(NULL, n, sz);
    if (!p) { emit("reallocarray[%s] null errno=%s", label, ename(errno)); return; }
    if (write_check) {
        size_t total = n * sz;
        unsigned char *b = (unsigned char *)p;
        for (size_t i = 0; i < total; i++) b[i] = (unsigned char)(i * 37u + 11u);
        unsigned long sum = 0;
        for (size_t i = 0; i < total; i++) sum += b[i];
        emit("reallocarray[%s] nonnull checksum=%lu", label, sum);
    } else {
        emit("reallocarray[%s] nonnull", label);
    }
    free(p);
}

static void test_reallocarray(void)
{
    run_reallocarray("small-4x8", 4, 8, 1);
    run_reallocarray("small-1x1", 1, 1, 1);
    run_reallocarray("small-0x0-fromNULL", 0, 0, 0);     /* realloc(NULL,0): both libcs give non-NULL; see file note */
    run_reallocarray("n0", 0, 100, 0);
    run_reallocarray("sz0", 100, 0, 0);
    run_reallocarray("moderate-1024x64", 1024, 64, 1);
    run_reallocarray("overflow-n=-1,sz=2", (size_t)-1, 2, 0);
    run_reallocarray("overflow-n=2,sz=-1", 2, (size_t)-1, 0);
    run_reallocarray("overflow-both-huge", (size_t)1 << 40, (size_t)1 << 30, 0);
    run_reallocarray("boundary-exact-by7", (size_t)-1 / 7, 7, 0);       /* legal product, just astronomically huge -> ENOMEM */
    run_reallocarray("boundary-over-by7", (size_t)-1 / 7 + 1, 7, 0);     /* genuinely overflows */
    run_reallocarray("boundary-n1-szmax", 1, (size_t)-1, 0);
    run_reallocarray("boundary-nmax-sz1", (size_t)-1, 1, 0);

    /* errno must be LEFT ALONE on a SUCCESSFUL call. ra_call() resets
     * errno=0 before every call above, which would hide exactly this --
     * every "nonnull" case so far only proves errno isn't left at a value
     * reallocarray() itself set, not that it never touches errno at all on
     * the success path. Set a recognizable sentinel directly and call
     * reallocarray() un-wrapped (no ra_call reset) to check it verbatim. */
    {
        errno = EAGAIN;
        void *p = reallocarray(NULL, 4, 8);
        emit("reallocarray[errno-untouched-on-success] nonnull=%d errno_preserved=%d", p != NULL, errno == EAGAIN);
        free(p);
    }

    /* shrink-to-zero on a REAL, previously-live allocation -- distinct from
     * "small-0x0-fromNULL" above (ptr==NULL there). total==0 must never set
     * ENOMEM here either, regardless of what the underlying realloc(ptr, 0)
     * happens to return for a non-NULL ptr (that return value itself is
     * implementation-defined per POSIX and not what this case is checking). */
    {
        int *p = ra_call(NULL, 8, sizeof(int));
        errno = 0;
        void *q = reallocarray(p, 0, sizeof(int));
        emit("reallocarray[shrink-to-zero-existing] errno=%s", ename(errno));
        free(q);
    }

    /* grow an existing allocation, verify old content survives */
    {
        int *p = ra_call(NULL, 8, sizeof(int));
        if (p) {
            for (int i = 0; i < 8; i++) p[i] = i * i;
            int *q = ra_call(p, 64, sizeof(int));
            if (q) {
                int ok = 1;
                for (int i = 0; i < 8; i++) if (q[i] != i * i) ok = 0;
                emit("reallocarray[grow-preserves-content] ok=%d", ok);
                free(q);
            } else {
                emit("reallocarray[grow-preserves-content] ok=0-grow-failed");
            }
        } else {
            emit("reallocarray[grow-preserves-content] ok=0-alloc-failed");
        }
    }

    /* SAME total size, not a grow -- a plain "resize to what it already is"
     * is a legal reallocarray() call too (nmemb*size unchanged), and the
     * content must still survive it. */
    {
        int *p = ra_call(NULL, 8, sizeof(int));
        if (p) {
            for (int i = 0; i < 8; i++) p[i] = 100 + i;
            int *q = ra_call(p, 8, sizeof(int));
            int ok = 1;
            if (q) { for (int i = 0; i < 8; i++) if (q[i] != 100 + i) ok = 0; }
            else ok = 0;
            emit("reallocarray[same-size-preserves-content] ok=%d", ok);
            free(q);
        } else {
            emit("reallocarray[same-size-preserves-content] ok=0-alloc-failed");
        }
    }
}

/* ========================================================================
 * realpath()
 * ======================================================================== */
static void run_realpath(const char *label, const char *path)
{
    errno = 0;
    char *r = realpath(path, NULL);
    if (r) { emit("realpath[%s] = %s", label, r); free(r); }
    else    emit("realpath[%s] NULL errno=%s", label, ename(errno));
}

static void test_realpath(void)
{
    run_realpath("dotdot-mix", BASE "/tree/a/../a/./b/..//.");
    run_realpath("root", "/");
    run_realpath("past-root", BASE "/tree/../../../../../../../../../../../../../..");
    run_realpath("dup-slashes", BASE "/tree//a///b");
    run_realpath("symlink-dir-traverse", BASE "/tree/e/file1.txt");
    run_realpath("symlink-dir-itself", BASE "/tree/e");
    run_realpath("dangling", BASE "/tree/dangling");
    run_realpath("notdir-mid", BASE "/tree/a/file1.txt/x");
    run_realpath("trailing-slash-on-file", BASE "/plainfile/");
    run_realpath("missing-final", BASE "/tree/a/does_not_exist");
    run_realpath("missing-mid", BASE "/tree/does_not_exist/child");
    run_realpath("empty", "");
    run_realpath("dot-in-base", BASE "/tree/.");
    run_realpath("chain-l1", BASE "/chain/l1");
    run_realpath("chain-l40-at-boundary", BASE "/chain/l40");
    run_realpath("chain-l41-past-boundary", BASE "/chain/l41");
    run_realpath("chain-l45-well-past", BASE "/chain/l45");
    run_realpath("true-loop", BASE "/loop/loop1");

    /* leading "//" -- Linux/glibc treats a run of leading slashes (including
     * exactly two, the one case POSIX allows an implementation to special-
     * case) the same as a single one; not special-cased here either. */
    run_realpath("double-leading-slash-root", "//");
    run_realpath("double-leading-slash-path", "//tmp");

    /* the target of a symlink itself contains ".." (see build_tree()'s
     * linkdd fixture comment): must resolve relative to the LINK's own
     * parent, not the caller's cwd. */
    run_realpath("symlink-target-has-dotdot", BASE "/tree/a/linkdd");

    /* ".." immediately after a symlink component applies to the symlink's
     * RESOLVED location, not to text preceding a '/' in the original
     * string -- POSIX requires the former; a naive string-dirname("..")
     * would get this wrong for exactly this input. */
    run_realpath("symlink-then-dotdot", BASE "/tree/e/../d/file2.txt");

    /* more than one trailing slash -- the "trailing slash means directory"
     * check (see pathx.c's file banner) must still fire/not-fire correctly
     * when there is more than one slash to skip past. */
    run_realpath("many-trailing-slashes-dir", BASE "/tree/a////");
    run_realpath("many-trailing-slashes-file", BASE "/plainfile///");

    /* relative-path cases: chdir into a known directory first */
    char saved[PATH_MAX];
    must(getcwd(saved, sizeof saved) != NULL, "getcwd save");
    must(chdir(BASE "/relbase") == 0, "chdir relbase");
    run_realpath("relative-xy", "x/y");
    run_realpath("relative-xy-trailing-slash", "x/y/");
    run_realpath("relative-dot", ".");
    run_realpath("relative-dotdot-then-down", "x/../x/y");

    /* relative ".." chains that walk PAST root: rp_pop()'s "never past
     * root" clamp (see pathx.c) must hold starting from a RELATIVE path
     * too, not only from the absolute "past-root" case above (which starts
     * the walk already-rooted, never exercising getcwd()'s own output as
     * the base to pop from). */
    must(chdir("/") == 0, "chdir root for relative-past-root test");
    run_realpath("relative-dotdot-past-root", "../../../../..");
    run_realpath("relative-dot-at-root", ".");

    must(chdir(saved) == 0, "chdir restore");

    /* caller-supplied PATH_MAX buffer (the documented-safe size; an
     * undersized caller buffer is undefined behaviour in POSIX realpath()
     * itself -- confirmed by observing real glibc silently overrun a
     * 10-byte buffer on the host -- so it is deliberately not exercised
     * here; see deviations). */
    {
        char buf[PATH_MAX];
        errno = 0;
        char *r = realpath(BASE "/tree/a/b", buf);
        emit("realpath[caller-buffer] returned-buf=%d content=%s",
             r == buf, r ? r : "(null)");
    }
}

/* ========================================================================
 * nftw() / ftw()
 * ======================================================================== */
static const char *ftw_typename(int t)
{
    switch (t) {
    case FTW_F:   return "F";
    case FTW_D:   return "D";
    case FTW_DNR: return "DNR";
    case FTW_NS:  return "NS";
    case FTW_SL:  return "SL";
    case FTW_DP:  return "DP";
    case FTW_SLN: return "SLN";
    default:       return "?";
    }
}

#define MAXLINES 256
static char g_lines[MAXLINES][256];
static int g_nlines;
static int g_fncalls;

static int cmpline(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int nftw_cb(const char *path, const struct stat *sb, int type, struct FTW *ftwbuf)
{
    g_fncalls++;
    if (g_nlines < MAXLINES) {
        snprintf(g_lines[g_nlines], sizeof g_lines[0], "%s type=%s base=%d level=%d symsize=%lld",
                 path, ftw_typename(type), ftwbuf->base, ftwbuf->level,
                 (type == FTW_SL || type == FTW_SLN) ? (long long)sb->st_size : -1LL);
        g_nlines++;
    }
    return 0;
}

/* See the two early-stop cases in test_nftw() for why each of these is safe
 * to diff despite unspecified readdir() order: both key their stop on an
 * entry whose position in the callback sequence is fixed by the walk's
 * OWN structure (root-first for preorder, root-last for FTW_DEPTH's DP),
 * never on which sibling readdir() happens to return first. */
static int nftw_cb_stop_at_root(const char *path, const struct stat *sb, int type, struct FTW *ftwbuf)
{
    (void)path; (void)sb; (void)type;
    g_fncalls++;
    if (ftwbuf->level == 0) return 7;
    return 0;
}

static int nftw_cb_stop_at_dp(const char *path, const struct stat *sb, int type, struct FTW *ftwbuf)
{
    (void)path; (void)sb;
    g_fncalls++;
    if (type == FTW_DP && ftwbuf->level == 0) return -3;
    return 0;
}

static void run_nftw(const char *label, const char *path, int flags)
{
    g_nlines = 0;
    g_fncalls = 0;
    errno = 0;
    int r = nftw(path, nftw_cb, 6, flags);
    qsort(g_lines, (size_t)g_nlines, sizeof g_lines[0], cmpline);
    emit("nftw[%s] begin n=%d", label, g_nlines);
    for (int i = 0; i < g_nlines; i++) emit("  %s", g_lines[i]);
    if (r == 0) emit("nftw[%s] end r=0", label);
    else         emit("nftw[%s] end r=%d errno=%s", label, r, ename(errno));
}

static int ftw_cb(const char *path, const struct stat *sb, int type)
{
    (void)sb;
    g_fncalls++;
    if (g_nlines < MAXLINES) {
        snprintf(g_lines[g_nlines], sizeof g_lines[0], "%s type=%s", path, ftw_typename(type));
        g_nlines++;
    }
    return 0;
}

static void test_nftw(void)
{
    /* main tree, both symlink policies -- deterministic (no loop, no
     * unresolvable-during-walk aborts either way), so full enumeration is
     * safe to compare after sorting. */
    run_nftw("tree-phys-preorder", BASE "/tree", FTW_PHYS);
    run_nftw("tree-logical-preorder", BASE "/tree", 0);
    run_nftw("tree-phys-depth", BASE "/tree", FTW_PHYS | FTW_DEPTH);
    run_nftw("tree-logical-depth", BASE "/tree", FTW_DEPTH);

    /* FTW_MOUNT with everything on one filesystem must be a same-answer
     * no-op (there is no boundary in this fixture to cross) -- this is the
     * meaningful assertion this environment CAN make about FTW_MOUNT
     * without a second real device/mount to test the boundary itself; see
     * deviations. */
    run_nftw("tree-phys-mount-noop", BASE "/tree", FTW_PHYS | FTW_MOUNT);

    /* nopenfd=1 forces maximum close/reopen churn (every descent closes
     * the parent first); the VISIBLE result must be identical to a
     * generous budget, since nopenfd only bounds how many DIR*s stay open
     * at once, never what gets reported. run_nftw always passes nopenfd=6
     * above; this call uses 1 directly to compare. */
    {
        g_nlines = 0; g_fncalls = 0; errno = 0;
        int r = nftw(BASE "/tree", nftw_cb, 1, FTW_PHYS);
        qsort(g_lines, (size_t)g_nlines, sizeof g_lines[0], cmpline);
        emit("nftw[tree-phys-nopenfd1] begin n=%d", g_nlines);
        for (int i = 0; i < g_nlines; i++) emit("  %s", g_lines[i]);
        emit("nftw[tree-phys-nopenfd1] end r=%d", r);
    }

    /* nopenfd=0 must clamp to the same budget=1 as nopenfd=1 above (see
     * ftw.c: "nopenfd < 1 is clamped to 1") -- same-answer check against the
     * nopenfd=1 case just run. */
    {
        g_nlines = 0; g_fncalls = 0; errno = 0;
        int r = nftw(BASE "/tree", nftw_cb, 0, FTW_PHYS);
        qsort(g_lines, (size_t)g_nlines, sizeof g_lines[0], cmpline);
        emit("nftw[tree-phys-nopenfd0] begin n=%d", g_nlines);
        for (int i = 0; i < g_nlines; i++) emit("  %s", g_lines[i]);
        emit("nftw[tree-phys-nopenfd0] end r=%d", r);
    }

    /* every flag together: no novel bit, but if any pairwise interaction
     * were wrong (e.g. FTW_MOUNT's dev check firing before FTW_DEPTH's
     * deferred-callback bookkeeping expects it to) a single-fixture,
     * single-filesystem, no-mount-boundary tree makes this a same-answer
     * no-op against the individually-tested flags above -- cheap insurance. */
    run_nftw("tree-phys-depth-mount", BASE "/tree", FTW_PHYS | FTW_DEPTH | FTW_MOUNT);

    /* the ROOT argument is itself a symlink (to a directory), both symlink
     * policies. FTW_PHYS: reported once as FTW_SL, never descended (root is
     * never "the thing found via readdir", it's still a symlink under
     * PHYS). Default/logical: followed, reported as FTW_D, and descended --
     * the printed paths stay prefixed with the ORIGINAL "tree/e" text (walk()
     * builds child paths from the given string, not the resolved target),
     * which is exactly what this checks. */
    run_nftw("symlink-root-phys", BASE "/tree/e", FTW_PHYS);
    run_nftw("symlink-root-logical", BASE "/tree/e", 0);

    /* a RELATIVE root path, not just a relative path handed to realpath():
     * walk()'s own path/ftwbuf.base construction must work starting from a
     * non-absolute string too. */
    {
        char saved_cwd[PATH_MAX];
        must(getcwd(saved_cwd, sizeof saved_cwd) != NULL, "getcwd before relative nftw");
        must(chdir(BASE) == 0, "chdir BASE for relative nftw");
        run_nftw("relative-root-tree-a", "tree/a", FTW_PHYS);
        must(chdir(saved_cwd) == 0, "chdir restore after relative nftw");
    }

    /* EARLY STOP, deterministic regardless of unspecified readdir() order:
     * the callback stops on the very FIRST call it ever gets (the root
     * itself, level 0, guaranteed to be first and only once) -- so unlike a
     * stop keyed to some child's name, there is no ordering question here.
     * Checks: fn()'s return value becomes nftw()'s own return value, and no
     * descent happens at all once fn() has said stop (exactly one call). */
    {
        g_fncalls = 0; errno = 0;
        int r = nftw(BASE "/tree", nftw_cb_stop_at_root, 6, 0);
        emit("nftw[early-stop-at-root] r=%d calls=%d", r, g_fncalls);
    }

    /* EARLY STOP under FTW_DEPTH, keyed to the ROOT's own FTW_DP (postorder)
     * callback -- also deterministic despite readdir() order, because
     * FTW_DEPTH always visits EVERY descendant before revisiting the root
     * in FTW_DP: that revisit is definitionally the LAST callback of an
     * unaborted walk, so "how many callbacks happened before it fired" is
     * the size of the whole tree, not an order-dependent partial count.
     * Also exercises a NEGATIVE return code, distinct from every other
     * early-stop case here (nftw() must hand it back verbatim, not clamp
     * or reinterpret it). */
    {
        g_fncalls = 0; errno = 0;
        int r = nftw(BASE "/tree", nftw_cb_stop_at_dp, 6, FTW_DEPTH);
        emit("nftw[depth-stop-at-root-dp] r=%d calls=%d", r, g_fncalls);
    }

    /* isolated loop: PHYS never follows, so no loop is ever hit */
    run_nftw("loop-phys", BASE "/loop", FTW_PHYS);

    /* isolated loop: default mode hits the cycle and the WHOLE walk aborts
     * -- see the file banner. Not sorted/enumerated on purpose: which
     * entries were seen before the abort is order-dependent and not what
     * this case is testing. */
    {
        g_fncalls = 0; errno = 0;
        int r = nftw(BASE "/loop/loop1", nftw_cb, 6, 0);
        emit("nftw[loop-default-abort] r=%d errno=%s", r, ename(errno));
    }

    /* missing root: fn() must never be called at all */
    {
        g_fncalls = 0; errno = 0;
        int r = nftw(BASE "/tree/does_not_exist_root", nftw_cb, 6, 0);
        emit("nftw[missing-root] r=%d errno=%s fncalls=%d", r, ename(errno), g_fncalls);
    }

    /* FTW_DNR: a directory whose contents readdir() can see but lstat()
     * (and opendir(), which is what determines DNR here) cannot enter --
     * see build_tree()'s comment on why this is isolated and how it
     * degrades gracefully under a root test runner. */
    {
        int rc1 = chmod(BASE "/dnrtest/blocked", 0644);
        must(rc1 == 0, "chmod dnrtest/blocked 644");
        run_nftw("dnr", BASE "/dnrtest", FTW_PHYS);
        must(chmod(BASE "/dnrtest/blocked", 0755) == 0, "chmod dnrtest/blocked restore");
    }

    /* classic ftw(): logical + preorder always, and a dangling symlink is
     * folded to FTW_NS (no FTW_SLN in this interface) -- verified against
     * real glibc, see ftw.c's file banner point 3. */
    {
        g_nlines = 0; g_fncalls = 0; errno = 0;
        int r = ftw(BASE "/tree", ftw_cb, 6);
        qsort(g_lines, (size_t)g_nlines, sizeof g_lines[0], cmpline);
        emit("ftw[tree] begin n=%d", g_nlines);
        for (int i = 0; i < g_nlines; i++) emit("  %s", g_lines[i]);
        emit("ftw[tree] end r=%d", r);
    }

    /* ftw() rooted at a PLAIN FILE, not a directory -- every other ftw()/
     * nftw() case here roots at a directory; a file root must still get
     * exactly one F callback and a clean return, no different handling
     * required just because there's nothing to descend into. */
    {
        g_nlines = 0; g_fncalls = 0; errno = 0;
        int r = ftw(BASE "/plainfile", ftw_cb, 6);
        qsort(g_lines, (size_t)g_nlines, sizeof g_lines[0], cmpline);
        emit("ftw[plainfile] begin n=%d", g_nlines);
        for (int i = 0; i < g_nlines; i++) emit("  %s", g_lines[i]);
        emit("ftw[plainfile] end r=%d", r);
    }
}

/* ========================================================================
 * mkdtemp()
 * ========================================================================
 * The suffix is genuinely random (different seed per process, even between
 * "ours" and "glibc" run back to back), so this checks STRUCTURE -- length,
 * charset, that a real directory now exists, that it is empty, that its
 * mode is 0700 -- never the literal generated name. */
static int is_temp_charset_ok(const char *suffix)
{
    for (int i = 0; i < 6; i++) {
        char c = suffix[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

static void test_mkdtemp(void)
{
    /* bad template: too few X's -- template must be left UNCHANGED */
    {
        char t[64];
        strcpy(t, BASE "/mdtest_XXXXX");
        char before[64];
        strcpy(before, t);
        errno = 0;
        char *r = mkdtemp(t);
        emit("mkdtemp[short-x] null=%d errno=%s unchanged=%d", r == NULL, ename(errno), strcmp(t, before) == 0);
    }
    /* bad template: not X's at all */
    {
        char t[64];
        strcpy(t, BASE "/mdtest_YYYYYY");
        char before[64];
        strcpy(before, t);
        errno = 0;
        char *r = mkdtemp(t);
        emit("mkdtemp[not-x] null=%d errno=%s unchanged=%d", r == NULL, ename(errno), strcmp(t, before) == 0);
    }
    /* normal case */
    {
        char t[64];
        strcpy(t, BASE "/mdtest_dir_XXXXXX");
        errno = 0;
        char *r = mkdtemp(t);
        int returned_t = (r == t);
        int len_ok = (int)strlen(t) == (int)strlen(BASE "/mdtest_dir_XXXXXX");
        int charset_ok = r ? is_temp_charset_ok(t + strlen(t) - 6) : 0;
        int prefix_ok = r ? (strncmp(t, BASE "/mdtest_dir_", strlen(BASE "/mdtest_dir_")) == 0) : 0;
        int is_dir = 0, is_empty = 1, mode_ok = 0;
        if (r) {
            DIR *d = opendir(t);
            is_dir = (d != NULL);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) is_empty = 0;
                }
                closedir(d);
            }
            struct stat st;
            if (stat(t, &st) == 0) mode_ok = ((st.st_mode & 0777) == 0700);
        }
        emit("mkdtemp[normal] returned_t=%d len_ok=%d charset_ok=%d prefix_ok=%d is_dir=%d is_empty=%d mode_0700=%d",
             returned_t, len_ok, charset_ok, prefix_ok, is_dir, is_empty, mode_ok);
        if (r) rmdir(t);
    }
    /* two calls in a row must not collide (different names) */
    {
        char t1[64], t2[64];
        strcpy(t1, BASE "/mdtest_pair_XXXXXX");
        strcpy(t2, BASE "/mdtest_pair_XXXXXX");
        char *r1 = mkdtemp(t1);
        char *r2 = mkdtemp(t2);
        emit("mkdtemp[two-in-a-row] both_ok=%d distinct=%d", (r1 && r2), (r1 && r2) ? strcmp(t1, t2) != 0 : 0);
        if (r1) rmdir(t1);
        if (r2) rmdir(t2);
    }
    /* n < 6 boundary hit DIRECTLY -- the existing "short-x" case above is a
     * too-short X-RUN inside a longer templated string (n well over 6, but
     * the last 6 chars aren't "XXXXXX"); this is the OTHER half of the
     * check ("n < 6 || suffix != XXXXXX"), n literally 5. No mkdir side
     * effect either way, so no chdir dance needed. */
    {
        char t[64];
        strcpy(t, "XXXXX");
        char before[64];
        strcpy(before, t);
        errno = 0;
        char *r = mkdtemp(t);
        emit("mkdtemp[n-lt-6] null=%d errno=%s unchanged=%d", r == NULL, ename(errno), strcmp(t, before) == 0);
    }

    /* n == 6 boundary EXACTLY: template is nothing but "XXXXXX", no prefix
     * at all -- mkdtemp() has no directory argument, the template itself IS
     * the path, so this would create a directory in the CURRENT working
     * directory if run there; chdir into BASE first so it lands in the
     * fixture instead of wherever this test process happened to be
     * launched from (which may be this repo's own working tree). */
    {
        char saved_cwd[PATH_MAX];
        must(getcwd(saved_cwd, sizeof saved_cwd) != NULL, "getcwd before bare-six");
        must(chdir(BASE) == 0, "chdir BASE for bare-six");
        char t[64];
        strcpy(t, "XXXXXX");
        errno = 0;
        char *r = mkdtemp(t);
        int len_ok = r ? (int)strlen(t) == 6 : 0;
        int charset_ok = r ? is_temp_charset_ok(t) : 0;
        emit("mkdtemp[bare-six] nonnull=%d len_ok=%d charset_ok=%d", r != NULL, len_ok, charset_ok);
        if (r) rmdir(t);
        must(chdir(saved_cwd) == 0, "chdir restore after bare-six");
    }

    /* MORE than 6 X's: mktemp() (stdlib.c) only ever touches the LAST six
     * characters -- verified by reading that function, not guessed -- so an
     * extra leading X must survive the call untouched while the trailing
     * six become the random suffix. */
    {
        char t[64];
        strcpy(t, BASE "/mdtest_sevenx_XXXXXXX");   /* 7 X's */
        size_t n = strlen(t);
        errno = 0;
        char *r = mkdtemp(t);
        int first_extra_x_literal = r ? (t[n - 7] == 'X') : 0;
        int charset_ok = r ? is_temp_charset_ok(t + n - 6) : 0;
        emit("mkdtemp[seven-x] nonnull=%d first_extra_x_literal=%d charset_ok=%d", r != NULL, first_extra_x_literal, charset_ok);
        if (r) rmdir(t);
    }

    /* ends in "XXXXXX" somewhere in the MIDDLE, not at the very end -- glibc
     * requires the LAST six characters specifically; trailing non-X text
     * after a valid run must still be rejected. */
    {
        char t[64];
        strcpy(t, BASE "/mdtest_XXXXXXtail");
        char before[64];
        strcpy(before, t);
        errno = 0;
        char *r = mkdtemp(t);
        emit("mkdtemp[suffix-after-x] null=%d errno=%s unchanged=%d", r == NULL, ename(errno), strcmp(t, before) == 0);
    }

    /* Reusing the SAME buffer for a second, independent top-level call
     * WITHOUT resetting it to "XXXXXX" first -- distinct from mkdtemp()'s
     * own internal EEXIST retry dance (which DOES reset between attempts,
     * see pathx.c), this is what an external caller gets for skipping that
     * step: after the first call the buffer no longer ends in "XXXXXX" (it
     * ends in the real generated name), so a second call must see it as a
     * malformed template and fail EINVAL rather than, say, silently
     * reusing the stale name or crashing. */
    {
        char t[64];
        strcpy(t, BASE "/mdtest_reuse_XXXXXX");
        char *r1 = mkdtemp(t);
        errno = 0;
        char *r2 = mkdtemp(t);
        emit("mkdtemp[reuse-without-reset] first_ok=%d second_null=%d second_errno=%s",
             r1 != NULL, r2 == NULL, ename(errno));
        if (r1) rmdir(t);
    }

    /* mkdtemp(NULL) is deliberately NOT exercised here: confirmed by
     * running it on this host that real glibc's mkdtemp() does not NULL-
     * check its argument and segfaults immediately (strlen(NULL) inside).
     * This library's own mkdtemp() DOES check and returns EINVAL -- a
     * real, deliberate hardening difference (see pathx.c's "do NOT stub
     * to success" policy applied in the other direction: refusing loudly
     * beats a crash) -- but there is no glibc answer to diff it against
     * that would not just kill the reference process. */
}

int main(void)
{
    test_getsubopt();
    test_reallocarray();

    build_tree();
    test_realpath();
    test_nftw();
    test_mkdtemp();
    rm_rf(BASE);

    return 0;
}
