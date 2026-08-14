/* Four <stdlib.h> functions that share this file for no reason deeper than
 * "the unit that added them was scoped as one file": realpath() and
 * mkdtemp() genuinely need the filesystem, reallocarray() and getsubopt()
 * are pure computation and could live anywhere. All four target GLIBC
 * behaviour, not bare POSIX -- see the per-function notes below for the
 * specific spots where the two disagree and glibc was picked, matching the
 * rest of this library's stated policy (ported programs were built against
 * glibc; the diff test in tests/unit/libc_pathx_test.c only has an oracle
 * because of that choice).
 */
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* ========================================================================
 * realpath() -- canonicalise a path: resolve ".", "..", collapse repeated
 * '/', and follow every symlink.
 *
 * WHAT THIS IS BUILT ON, AND WHY IT NEEDS ALMOST NEITHER stat() NOR
 * lstat(). The only two questions realpath ever has to ask about a path it
 * has built so far are "does the next component exist, and if so is it a
 * symlink" -- and readlink() alone answers both, plus a third question for
 * free:
 *   - success            -> it exists and is a symlink; the bytes are the target.
 *   - -1/EINVAL           -> it exists and is NOT a symlink (POSIX defines
 *                            EINVAL to mean exactly that).
 *   - -1/ENOENT            -> it does not exist.
 *   - -1/ENOTDIR            -> some EARLIER component in this same path was
 *                            not a directory. This one is not something we
 *                            compute -- it is the kernel's own path walk
 *                            inside readlink() refusing to descend through
 *                            a non-directory, surfacing for free because we
 *                            hand it the WHOLE accumulated path every time,
 *                            not just the new leaf.
 * A prior component being a directory was already established when it was
 * processed (or this path would already have failed), so ENOTDIR here can
 * only be about the component appended most recently -- which is exactly
 * the one case left uncovered otherwise, with ONE exception: a trailing
 * slash after the very last component (e.g. "plainfile/", with nothing
 * after the slash) has no FOLLOWING component to trigger that check on, so
 * nothing would otherwise notice that "plainfile" is not a directory --
 * verified against real glibc, which does reject it, ENOTDIR. That one
 * spot uses stat() to ask directly. Every other component, including
 * every other trailing slash, is still readlink()-only.
 *
 * SYMLINK SPLICING. `left` holds the part of the path not yet consumed.
 * Hitting a symlink does not recurse -- it SPLICES: the link's target,
 * followed by whatever of the original path came after the component that
 * named the symlink, becomes the new `left`, and processing continues from
 * its start. This is what makes ".." after a symlink apply to the link's
 * resolved location rather than to text that happens to precede a '/' in
 * the original string (POSIX requires the former). An absolute target
 * resets the output back to "/"; a relative one pops the symlink's own
 * (just-appended) name back off the output first, so the target is
 * resolved against the symlink's parent directory, per POSIX.
 *
 * ELOOP at 40. Not a made-up round number: verified against this machine's
 * real glibc (tests/unit/libc_pathx_test.c builds a chain of symlinks and
 * checks the boundary) -- a chain of exactly 40 resolves; 41 is ELOOP. This
 * is glibc's __eloop_threshold(), which the task description also names.
 *
 * THE FINAL COMPONENT MUST EXIST. Also verified against real glibc, and
 * easy to get backwards: realpath("/a/b", ...) where "b" does not exist
 * fails with ENOENT, even though every earlier component is fine and even
 * though canonicalize_file_name-style "resolve as much as you can" variants
 * exist elsewhere in the GNU world. This function is exactly as strict.
 * Because every appended component -- including the last -- goes through
 * the readlink() existence check above, this falls out on its own; there
 * is deliberately no separate "does the finished path exist" check at the
 * end. */

#define RP_MAXSYMLINKS 40
/* Working buffer for "path not yet consumed", generous enough to hold a
 * spliced-in symlink target (up to PATH_MAX-1 bytes) ahead of whatever of
 * the caller's original path is still pending, several times over. A
 * pathological input that needs more than this many bytes of un-consumed
 * path at once fails ENAMETOOLONG rather than growing without bound --
 * deliberate, see the file's freestanding/no-allocator neighbours for why
 * this library prefers a bounded stack buffer to malloc() where either
 * would do. */
#define RP_WORK (PATH_MAX * 4)

/* Append `name` (length `namelen`, no '/'). Maintains the invariant that
 * `out[0..*dlen)` is always a canonical path with NO trailing slash, except
 * when it is exactly "/" (*dlen == 1) -- so callers never need to special-
 * case root before deciding whether a separator is needed. */
static int rp_append(char *out, size_t *dlen, const char *name, size_t namelen)
{
    size_t d = *dlen;
    if (d > 1) {
        if (d + 1 >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        out[d++] = '/';
    }
    if (d + namelen >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    memcpy(out + d, name, namelen);
    d += namelen;
    out[d] = '\0';
    *dlen = d;
    return 0;
}

/* Undo one component for "..". Walks back to the previous '/' and drops it
 * too, except never past root -- "/../.." at the root is a no-op, which is
 * also what real realpath() does (verified on the host). */
static void rp_pop(char *out, size_t *dlen)
{
    size_t d = *dlen;
    if (d <= 1) return;
    d--;
    while (d > 0 && out[d] != '/') d--;
    if (d == 0) d = 1;
    out[d] = '\0';
    *dlen = d;
}

char *realpath(const char *restrict path, char *restrict resolved)
{
    if (!path) { errno = EINVAL; return NULL; }
    if (path[0] == '\0') { errno = ENOENT; return NULL; }   /* glibc: ENOENT, not EINVAL */

    char *out = resolved;
    int out_owned = 0;
    if (!out) {
        out = malloc(PATH_MAX);
        if (!out) { errno = ENOMEM; return NULL; }
        out_owned = 1;
    }

    size_t dlen;
    if (path[0] == '/') {
        out[0] = '/'; out[1] = '\0'; dlen = 1;
    } else {
        if (!getcwd(out, PATH_MAX)) { if (out_owned) free(out); return NULL; }
        dlen = strlen(out);
        /* getcwd() never returns a trailing '/' except for root itself
         * (dlen == 1, "/", which already satisfies this function's
         * no-trailing-slash-except-root invariant) -- so nothing further
         * is needed here. A previous version of this function forced a
         * trailing '/' onto `out` unconditionally, which rp_append() below
         * (which ALSO inserts a separator before every component past
         * root) then doubled into "cwd//first-component". */
    }

    char left[RP_WORK];
    size_t plen = strlen(path);
    if (plen >= sizeof left) { if (out_owned) free(out); errno = ENAMETOOLONG; return NULL; }
    memcpy(left, path, plen + 1);
    char *p = left;
    int nlinks = 0;

    for (;;) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        char save = *p;
        *p = '\0';   /* `start` is now a NUL-terminated component, in place */

        if (clen == 1 && start[0] == '.') {
            *p = save;
            continue;
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            *p = save;
            rp_pop(out, &dlen);
            continue;
        }

        if (rp_append(out, &dlen, start, clen) < 0) { if (out_owned) free(out); return NULL; }

        char linkbuf[PATH_MAX];
        ssize_t n = readlink(out, linkbuf, sizeof linkbuf - 1);
        if (n < 0) {
            if (errno == EINVAL) {
                /* Exists, not a symlink. A trailing slash after the LAST
                 * component (e.g. realpath("plainfile/")) requires that
                 * component to be a directory -- POSIX, and verified
                 * against real glibc: "plainfile/" fails ENOTDIR even
                 * though "plainfile" alone resolves fine. A component with
                 * MORE path after it gets this check for free on the next
                 * iteration (readlink() on the longer path fails ENOTDIR by
                 * walking through a non-directory itself, same as any other
                 * component); only the truly last one, with nothing but
                 * slashes left, needs it asked for explicitly -- via
                 * stat(), the one place in this function that uses it (see
                 * the file banner: everything else needs only readlink()). */
                if (save == '/') {
                    const char *look = p + 1;   /* *p is '\0' right now (we nulled it above); the rest of the ORIGINAL string resumes at p+1 */
                    while (*look == '/') look++;
                    if (*look == '\0') {
                        struct stat st;
                        if (stat(out, &st) < 0) { if (out_owned) free(out); return NULL; }
                        if (!S_ISDIR(st.st_mode)) { if (out_owned) free(out); errno = ENOTDIR; return NULL; }
                    }
                }
                *p = save;
                continue;
            }
            if (out_owned) free(out);
            return NULL;   /* ENOENT / ENOTDIR / other -- propagate as-is */
        }
        if ((size_t)n == sizeof linkbuf - 1) {
            /* Cannot tell whether this was an exact fit or a silent
             * truncation (POSIX readlink() does not NUL-terminate or
             * report it either way) -- refuse rather than resolve a
             * possibly-truncated target as if it were complete. */
            if (out_owned) free(out);
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (++nlinks > RP_MAXSYMLINKS) { if (out_owned) free(out); errno = ELOOP; return NULL; }
        linkbuf[n] = '\0';

        /* remainder of the ORIGINAL path after this component */
        const char *rest = (save == '/') ? (p + 1) : p;
        size_t restlen = strlen(rest);
        size_t ll = (size_t)n;
        if (ll + 1 + restlen >= sizeof left) { if (out_owned) free(out); errno = ENAMETOOLONG; return NULL; }

        /* Splice into a temp buffer first: `rest` aliases into `left`, the
         * very buffer we are about to overwrite, so this cannot be done
         * in place without an explicit ordering that is easy to get wrong
         * (memmove-style) -- the temp buffer sidesteps that entirely. The
         * '/' separator is inserted ONLY when there is a `rest` to join to
         * -- a symlink that was the last thing in the original path (e.g.
         * realpath(".../l1") where l1 -> target) has an EMPTY rest, and
         * unconditionally inserting '/' there manufactured a trailing
         * slash the original path never had, which made a symlink-to-a-
         * plain-file wrongly fail the trailing-slash-means-directory check
         * a few lines up (caught by this file's own diff test: chain
         * link l1, one hop to a plain file, has to resolve, not ENOTDIR). */
        char spliced[RP_WORK];
        size_t sl = ll;
        memcpy(spliced, linkbuf, ll);
        if (restlen > 0) {
            spliced[sl++] = '/';
            memcpy(spliced + sl, rest, restlen + 1);   /* + NUL */
            sl += restlen;
        } else {
            spliced[sl] = '\0';
        }
        memcpy(left, spliced, sl + 1);
        p = left;

        if (linkbuf[0] == '/') {
            out[0] = '/'; out[1] = '\0'; dlen = 1;
        } else {
            rp_pop(out, &dlen);   /* undo the symlink's own name; resolve target against ITS parent */
        }
    }

    /* out_owned's PATH_MAX allocation is returned as-is, not shrunk to the
     * resolved length -- glibc's own malloc(NULL)-form doesn't shrink it
     * either; content is what a caller reads, not capacity. */
    return out;
}

/* ========================================================================
 * mkdtemp() -- like mkstemp() but a directory, mode 0700 (S_IRWXU), no
 * O_EXCL race to worry about because mkdir() already refuses an existing
 * name atomically. Reuses mktemp() (stdlib.c) for the actual randomness
 * rather than inventing a second RNG -- that file's block comment covers
 * what it is (unique-enough-for-one-machine, not attacker-resistant, and
 * there is no local attacker on this system to resist) and this function
 * inherits that same tradeoff verbatim.
 *
 * THE RETRY DANCE mirrors mkstemp() in this same tree exactly, and for the
 * same reason: mktemp() insists the template's last six bytes are still the
 * literal "XXXXXX" (it is how it recognises a template at all), but after
 * one call they are a real random suffix -- so a second call on the same
 * buffer would see something that no longer looks like a template and bail
 * out. Restoring "XXXXXX" before every retry after the first is what keeps
 * the buffer looking like a fresh template each time.
 *
 * GLIBC DETAIL, verified on the host: on a bad template (not >=6 chars
 * ending in "XXXXXX"), mkdtemp() reports EINVAL and leaves the caller's
 * buffer UNTOUCHED -- unlike this library's own mktemp(), which zeroes
 * tmpl[0] on that same failure. Different function, different contract;
 * matched here rather than "simplified" to share mktemp()'s failure
 * behaviour, because a program checking mkdtemp()'s return and then still
 * reading its template argument is relying on exactly this. */
char *mkdtemp(char *tmpl)
{
    if (!tmpl) { errno = EINVAL; return NULL; }
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) { errno = EINVAL; return NULL; }

    for (int att = 0; att < 64; att++) {
        if (att) memcpy(tmpl + n - 6, "XXXXXX", 6);
        mktemp(tmpl);
        if (!tmpl[0]) return NULL;   /* only our own precondition above can make mktemp() fail, and it's already checked */
        if (mkdir(tmpl, 0700) == 0) return tmpl;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST;
    return NULL;
}

/* ========================================================================
 * reallocarray() -- realloc(ptr, nmemb * size), except the multiplication
 * is never actually performed until it is known safe. This function exists
 * FOR that check: a caller who trusted nmemb*size not to overflow is
 * exactly the caller this replaces. So the check has to be done in a form
 * that cannot itself overflow -- computing the product first and comparing
 * it against something is already too late, the wraparound has happened by
 * then and the (wrong, small) result looks like a perfectly ordinary
 * allocation size. Dividing SIZE_MAX by one factor and comparing against
 * the other has no such failure mode: both operands of the comparison are
 * bounded by SIZE_MAX by construction.
 *
 * THE SECOND ERRNO SITE, easy to miss: an in-range product that is still
 * too big to satisfy (this arena is bounded; see malloc.c) fails inside
 * realloc(), not inside the overflow check above -- and malloc.c's
 * realloc_nl() returns NULL on that path without setting errno itself (it
 * is not this library's only allocation failure that does not; ENOMEM is
 * the caller-facing contract, not something every internal allocator path
 * is required to set on its own). reallocarray() is a POSIX/BSD entry
 * point with a documented ENOMEM contract, so it sets errno itself
 * whenever realloc() hands back NULL for a genuinely nonzero request --
 * never for a zero-sized one, where NULL is realloc()'s ordinary (if
 * library-specific -- see mkdtemp()'s neighbour note, this one's file
 * comment) answer, not a failure. Verified against real glibc
 * (tests/unit/libc_pathx_test.c): a legal-but-astronomical product that
 * cannot possibly be satisfied is ENOMEM there too, by the same reasoning
 * applied inside glibc's own allocator. */
void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > (size_t)-1 / nmemb) { errno = ENOMEM; return NULL; }
    size_t total = nmemb * size;
    void *r = realloc(ptr, total);
    if (!r && total != 0) errno = ENOMEM;
    return r;
}

/* ========================================================================
 * getsubopt() -- comma-separated "name" / "name=value" suboption parsing,
 * as used by mount(8)-style -o option strings. This is glibc's own
 * algorithm (misc/getsubopt.c), not a reimplementation from the man page:
 * the man page describes the contract but not the pointer-aliasing
 * behaviour below, which was checked against a running glibc
 * (tests/unit/libc_pathx_test.c) rather than guessed.
 *
 * MUTATES *optionp'S STRING IN PLACE (writes a NUL over the comma that
 * ends each consumed suboption) -- POSIX requires this of any conforming
 * implementation and callers are documented to pass a writable buffer, so
 * this is not a bug to work around.
 *
 * THE ALIASING CASE WORTH NAMING: when the current suboption does not
 * match any token, *valuep is set to point at the ORIGINAL text -- and
 * that pointer is written to *valuep BEFORE the trailing comma is
 * overwritten with '\0'. Since the comma and the value can be the very
 * same byte (an empty suboption, e.g. the "" between "a,,b"), *valuep can
 * end up pointing at a byte this same call is about to zero -- so the
 * caller reads back an empty string, not the comma. That is not a bug in
 * the algorithm below; it is glibc's actual observed behaviour, and this
 * function reproduces it by using the same order of operations rather than
 * by special-casing it. */
int getsubopt(char **optionp, char *const *tokens, char **valuep)
{
    if (!optionp || !*optionp || !tokens || !valuep) { errno = EINVAL; return -1; }
    if (**optionp == '\0') return -1;

    char *endp = strchrnul(*optionp, ',');
    char *vstart = memchr(*optionp, '=', (size_t)(endp - *optionp));
    if (!vstart) vstart = endp;

    for (int cnt = 0; tokens[cnt] != NULL; cnt++) {
        if (tokens[cnt][0] == (*optionp)[0]
            && strncmp(tokens[cnt], *optionp, (size_t)(vstart - *optionp)) == 0
            && tokens[cnt][vstart - *optionp] == '\0') {
            *valuep = (vstart != endp) ? vstart + 1 : NULL;
            if (*endp != '\0') *endp++ = '\0';
            *optionp = endp;
            return cnt;
        }
    }

    *valuep = *optionp;
    if (*endp != '\0') *endp++ = '\0';
    *optionp = endp;
    return -1;
}
