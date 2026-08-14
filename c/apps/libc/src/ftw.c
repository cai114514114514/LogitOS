/* <ftw.h>: ftw() and nftw(), a recursive directory walk with a callback per
 * entry. Built on opendir/readdir (dirstat.c) plus lstat/stat for the type
 * information the callback actually gets handed (a `const struct stat *`,
 * not just a type code -- a caller computing du-style totals needs
 * st_size, one summing mtimes needs st_mtime, and so on. This is the one
 * place in this pass that DOES need real stat()/lstat(), unlike pathx.c's
 * realpath(), which gets away with readlink() alone).
 *
 * THE FD-BUDGET QUESTION THE TASK ASKS THIS FILE TO ANSWER, ANSWERED
 * HONESTLY RATHER THAN BY ASSUMPTION. On Linux, nopenfd exists because
 * every open DIR* holds a kernel file descriptor, and this process's
 * fd table (c/kernel/exec/proc.h, NFD == 16, three of which are stdio
 * before a program does anything) makes that scarce -- a naive recursive
 * walk that keeps a DIR* open at every stack level dies around depth 13.
 * THAT IS NOT WHAT HAPPENS HERE, and it is worth being precise about why:
 * this library's opendir() (dirstat.c) never calls SYS_OPEN and never
 * touches proc->fd[] at all -- a DIR* is a malloc'd path-plus-cursor
 * struct, and readdir() re-issues a path-scoped SYS_GETDENTS each time.
 * There is no kernel fd behind it to run out of.
 *
 * So nopenfd is honoured here for a DIFFERENT, still-real reason: every
 * open DIR* is real heap memory (struct __dirstream is a few hundred bytes,
 * dominated by two full-size dirent-shaped buffers), and this is plain C
 * recursion, so depth also costs C stack. Bounding how many directories
 * stay open AT ONCE bounds both, and it is also simply the contract a
 * ported program calling nftw() with a specific nopenfd is relying on --
 * whether or not the reason it was relying on it still applies here.
 * The mechanism: nopenfd is a per-level budget, not a global fd pool to
 * portion out. Before opening a CHILD's directory would push the open
 * count past the budget, a level closes its OWN already-open DIR*(saving
 * its position with telldir()) and reopens it with opendir()+seekdir()
 * once that child's whole subtree is done. Because a level always frees
 * its own slot before acquiring a new one, the open count can never exceed
 * the budget -- no ancestor-scanning eviction scheme is needed. nopenfd < 1
 * is clamped to 1: reading the CURRENT directory needs at least one.
 *
 * FOUR THINGS VERIFIED AGAINST A RUNNING GLIBC (tests/unit/libc_pathx_test.c
 * builds the tree and diffs), because none of the four is obvious from the
 * man page and each is easy to get wrong:
 *   1. A root path that does not exist (or cannot be lstat'd) makes nftw()
 *      return -1 immediately WITHOUT ever calling fn() -- unlike a child
 *      discovered via readdir() whose lstat() later fails, which DOES get a
 *      callback, with type FTW_NS, and lets the walk continue (verified with
 *      a directory that is readable but not searchable: readdir() lists a
 *      child, lstat() on that child then fails EACCES, and fn() runs once
 *      with FTW_NS while the rest of the walk proceeds normally).
 *   2. Under !FTW_PHYS (the default -- symlinks are followed), a symlink
 *      whose target genuinely does not exist is FTW_SLN and the walk
 *      continues; a symlink that is part of a LOOP is a harder failure --
 *      stat() itself returns ELOOP, and that aborts the ENTIRE walk (nftw()
 *      returns -1/ELOOP) rather than reporting that one entry and moving
 *      on. This is why the test tree's loop and its dangling symlink are
 *      walked separately (see the test file's own comment).
 *   3. ftw() (the older, FTW_SLN-less interface) reports a dangling
 *      symlink as FTW_NS, not FTW_SLN -- it is folded, not left to whatever
 *      the type code number happens to mean to an old caller.
 *   4. Under !FTW_PHYS, a directory reachable by more than one path (a
 *      symlink next to the real directory, or two symlinks to the same
 *      place -- either way, no loop, stat() never fails) is walked ONCE,
 *      full stop: every path after the first gets no callback at all, as
 *      if it were not there. See `already_visited()`/`mark_visited()`
 *      below for the mechanism this reproduces it with. */
#include <ftw.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>

/* Directories nftw() has already descended into, by (st_dev, st_ino) --
 * bounded, see NFTW_MAXVISITED below. Exists for a case easy to miss
 * entirely (this library's first attempt did): under the default,
 * symlink-following mode, a directory reachable by TWO paths -- a plain
 * directory and a symlink next to it pointing at the same place, or two
 * symlinks pointing at the same place, it does not matter which -- is NOT
 * a symlink LOOP (stat() on it succeeds every time; nothing ever returns
 * ELOOP), so nothing else in this file would ever stop it from being
 * visited, and descended, more than once. Verified against real glibc
 * (tests/unit/libc_pathx_test.c): the SECOND path to an already-visited
 * directory gets no callback AT ALL, not even a leaf report -- it is
 * treated as if it were not there, and this is what the check below
 * reproduces (`return;` before fn() is ever called for it). */
#define NFTW_MAXVISITED 128
struct dev_ino { dev_t dev; ino_t ino; };

struct nctx {
    __nftw_fn_t fn;
    int flags;
    int nopenfd;
    int open_count;
    dev_t root_dev;
    int have_root_dev;
    int stopped;   /* sticky: once set, every frame unwinds without further callbacks */
    int result;    /* nftw()'s eventual return value once stopped */
    struct dev_ino visited[NFTW_MAXVISITED];
    int nvisited;
};

static int already_visited(struct nctx *ctx, dev_t dev, ino_t ino)
{
    for (int i = 0; i < ctx->nvisited; i++)
        if (ctx->visited[i].dev == dev && ctx->visited[i].ino == ino) return 1;
    return 0;
}

/* Silently stops recording past NFTW_MAXVISITED rather than failing the
 * walk -- a walk deeper/wider than that loses only the cycle-avoidance
 * guarantee for directories past the limit, in exchange for staying a
 * fixed-size, allocator-free structure (this library's usual tradeoff;
 * see pathx.c's RP_WORK for the same call made the same way). A REAL
 * symlink loop (as opposed to a merely-revisited directory) is still
 * caught unconditionally by the ELOOP path above, which does not depend
 * on this list at all. */
static void mark_visited(struct nctx *ctx, dev_t dev, ino_t ino)
{
    if (ctx->nvisited < NFTW_MAXVISITED) {
        ctx->visited[ctx->nvisited].dev = dev;
        ctx->visited[ctx->nvisited].ino = ino;
        ctx->nvisited++;
    }
}

static void walk(struct nctx *ctx, const char *fullpath, int level)
{
    if (ctx->stopped) return;

    char path[PATH_MAX];
    size_t l = strlen(fullpath);
    if (l >= sizeof path) { ctx->stopped = 1; ctx->result = -1; errno = ENAMETOOLONG; return; }
    memcpy(path, fullpath, l + 1);

    struct FTW ftwbuf;
    ftwbuf.level = level;
    const char *slash = strrchr(path, '/');
    ftwbuf.base = slash ? (int)(slash - path + 1) : 0;

    struct stat lst;
    if (lstat(path, &lst) < 0) {
        /* A child that vanished (or a permission-denied stat) between
         * readdir() naming it and us getting here: reported, not fatal --
         * see point 1 in the file banner for how this differs from the
         * ROOT failing the same way. */
        struct stat z;
        memset(&z, 0, sizeof z);
        int r = ctx->fn(path, &z, FTW_NS, &ftwbuf);
        if (r != 0) { ctx->stopped = 1; ctx->result = r; }
        return;
    }

    int type;
    struct stat use = lst;
    int is_dir_target = 0;

    if (S_ISLNK(lst.st_mode) && !(ctx->flags & FTW_PHYS)) {
        struct stat tst;
        if (stat(path, &tst) < 0) {
            if (errno == ELOOP) {
                /* A loop, not a dangling target: real glibc treats this as
                 * fatal to the whole walk, not a per-entry FTW_SLN -- see
                 * point 2 in the file banner. errno is left exactly as
                 * stat() set it. */
                ctx->stopped = 1;
                ctx->result = -1;
                return;
            }
            type = FTW_SLN;   /* use stays == lst: the symlink's own info, target unreachable */
        } else {
            use = tst;
            is_dir_target = S_ISDIR(tst.st_mode);
            type = is_dir_target ? FTW_D : FTW_F;
        }
    } else if (S_ISLNK(lst.st_mode)) {
        type = FTW_SL;   /* FTW_PHYS: report the link itself, never follow */
    } else if (S_ISDIR(lst.st_mode)) {
        type = FTW_D;
        is_dir_target = 1;
    } else {
        type = FTW_F;
    }

    if (level == 0) { ctx->root_dev = use.st_dev; ctx->have_root_dev = 1; }

    if (is_dir_target) {
        if (already_visited(ctx, use.st_dev, use.st_ino)) return;   /* see the visited-list note above the struct */
        mark_visited(ctx, use.st_dev, use.st_ino);
    }

    DIR *d = NULL;
    if (is_dir_target) {
        if ((ctx->flags & FTW_MOUNT) && ctx->have_root_dev && use.st_dev != ctx->root_dev) {
            /* Different filesystem: reported as an ordinary directory, just
             * never opened -- no descent, so nothing to gate on nopenfd. */
        } else {
            d = opendir(path);
            if (!d) type = FTW_DNR;
            else ctx->open_count++;
        }
    }

    int budget = ctx->nopenfd < 1 ? 1 : ctx->nopenfd;

    if ((ctx->flags & FTW_DEPTH) && type == FTW_D) {
        /* Postorder: the directory's own callback is FTW_DP, deferred
         * until after its children (below). Everything else (F, DNR, SL,
         * SLN) has no children to defer past, so it is reported now in
         * both modes -- that branch is shared, not duplicated per-flag. */
    } else {
        int r = ctx->fn(path, &use, type, &ftwbuf);
        if (r != 0) {
            ctx->stopped = 1; ctx->result = r;
            if (d) { closedir(d); ctx->open_count--; }
            return;
        }
    }

    if (d) {
        int chdired = 0;
        if (ctx->flags & FTW_CHDIR) {
            /* Correct for an ABSOLUTE starting path (every `path` this
             * function ever builds stays absolute once the top-level path
             * was); a RELATIVE starting path combined with FTW_CHDIR is a
             * known-unhandled corner, because "relative to a cwd that this
             * same walk keeps changing" has no single right answer and
             * nothing in this tree needs it. */
            if (chdir(path) < 0) {
                ctx->stopped = 1; ctx->result = -1;
                closedir(d); ctx->open_count--;
                return;
            }
            chdired = 1;
        }

        struct dirent *e;
        while (!ctx->stopped && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

            char child[PATH_MAX];
            size_t pl = strlen(path), nl = strlen(e->d_name);
            if (pl + 1 + nl >= sizeof child) {
                ctx->stopped = 1; ctx->result = -1; errno = ENAMETOOLONG;
                break;
            }
            memcpy(child, path, pl);
            child[pl] = '/';
            memcpy(child + pl + 1, e->d_name, nl + 1);

            /* nopenfd budget -- see the file banner. Free our own slot
             * before recursing if acquiring the child's would exceed it. */
            long saved_pos = 0;
            int reopen_after = 0;
            if (ctx->open_count >= budget) {
                saved_pos = telldir(d);
                closedir(d);
                d = NULL;
                ctx->open_count--;
                reopen_after = 1;
            }

            walk(ctx, child, level + 1);

            if (reopen_after && !ctx->stopped) {
                d = opendir(path);
                if (!d) { ctx->stopped = 1; ctx->result = -1; break; }
                ctx->open_count++;
                seekdir(d, saved_pos);
            }
        }

        if (chdired && !ctx->stopped) {
            if (chdir("..") < 0) { ctx->stopped = 1; ctx->result = -1; }
        }
        if (d) { closedir(d); ctx->open_count--; }
    }

    if ((ctx->flags & FTW_DEPTH) && type == FTW_D && !ctx->stopped) {
        int r = ctx->fn(path, &use, FTW_DP, &ftwbuf);
        if (r != 0) { ctx->stopped = 1; ctx->result = r; }
    }
}

int nftw(const char *path, __nftw_fn_t fn, int nopenfd, int flags)
{
    if (!path || !*path || !fn) { errno = EINVAL; return -1; }

    /* The root is checked on its own, separately from every other entry's
     * uniform (and non-fatal) FTW_NS handling inside walk() -- see point 1
     * in the file banner. */
    struct stat probe;
    if (lstat(path, &probe) < 0) return -1;

    struct nctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.fn = fn;
    ctx.flags = flags;
    ctx.nopenfd = nopenfd;

    walk(&ctx, path, 0);
    return ctx.stopped ? ctx.result : 0;
}

/* ftw() -- the pre-nftw() interface: no struct FTW, always logical
 * (symlinks followed) + preorder, and it predates FTW_SLN. One process-wide
 * slot for the user callback is safe because this library is single-
 * threaded throughout (no ftw()/nftw() call can be running on another
 * thread while this one uses it -- true of every other non-reentrant piece
 * of process-global state in this library, e.g. stdlib.c's mktemp()
 * sequence counter). */
static __ftw_fn_t g_ftw_fn;

static int ftw_trampoline(const char *path, const struct stat *sb, int type, struct FTW *ftwbuf)
{
    (void)ftwbuf;
    if (type == FTW_SLN) type = FTW_NS;   /* verified against real glibc -- see file banner point 3 */
    return g_ftw_fn(path, sb, type);
}

int ftw(const char *path, __ftw_fn_t fn, int nopenfd)
{
    if (!fn) { errno = EINVAL; return -1; }
    g_ftw_fn = fn;
    return nftw(path, ftw_trampoline, nopenfd, 0);
}
