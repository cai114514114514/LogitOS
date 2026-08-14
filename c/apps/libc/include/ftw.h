#ifndef _FTW_H
#define _FTW_H
/* <ftw.h> -- ftw()/nftw(), a recursive directory walk with a callback per
 * entry. Implemented in c/apps/libc/src/ftw.c; see that file for the FD
 * budget note (nopenfd) that is the whole reason this is not a five-line
 * recursive opendir/readdir loop. */
#include <sys/stat.h>

/* Per-entry position, handed to the nftw() callback only (ftw()'s callback
 * predates this struct and does not get one). `base` is the byte offset of
 * the final pathname component within the string fn() was called with
 * (so fn can print just the name: `path + ftw->base`); `level` is the
 * depth below the starting point, 0 for the starting point itself. */
struct FTW {
    int base;
    int level;
};

/* `type` values passed to the callback. Numeric values are this library's
 * own choice (a program branches on the FTW_* name, never the number --
 * same reasoning as every other constant in this tree, see fnmatch.h/
 * glob.h), but the SET matches glibc's, including the two that are easy to
 * leave out: FTW_DP (postorder revisit of a directory, FTW_DEPTH only) and
 * FTW_SLN (a symlink glibc could not stat because its target does not
 * exist -- distinct from FTW_NS, which is "stat failed on this object
 * ITSELF, for some other reason", and only reachable when FTW_PHYS is
 * clear, because FTW_PHYS never stats through a symlink in the first
 * place). */
#define FTW_F    0   /* a file that is not a directory (and, under !FTW_PHYS, not a symlink either -- see FTW_SL) */
#define FTW_D    1   /* a directory, reported before its contents (preorder) */
#define FTW_DNR  2   /* a directory that could not be opened for reading; fn() is still called, but the walk does not descend */
#define FTW_NS   3   /* stat()/lstat() failed on this object and it is not a symlink whose target is merely missing */
#define FTW_SL   4   /* a symbolic link, reported only when FTW_PHYS is set (otherwise it is followed and reported as whatever it points to) */
#define FTW_DP   5   /* a directory, reported again after its contents, only under FTW_DEPTH */
#define FTW_SLN  6   /* a symbolic link whose target could not be evaluated (typically: dangling), only reachable when FTW_PHYS is clear */

/* Flags for nftw(). */
#define FTW_PHYS   0x01   /* do not follow symlinks; report them as FTW_SL instead of descending */
#define FTW_MOUNT  0x02   /* do not descend into a directory on a different st_dev than the starting point */
#define FTW_DEPTH  0x04   /* postorder: a directory's contents are reported before the directory itself (FTW_DP) */
#define FTW_CHDIR  0x08   /* chdir() into each directory as it is entered (see ftw.c for the restore-on-the-way-out rule and its one failure mode) */

typedef int (*__ftw_fn_t)(const char *, const struct stat *, int);
typedef int (*__nftw_fn_t)(const char *, const struct stat *, int, struct FTW *);

int ftw(const char *path, __ftw_fn_t fn, int nopenfd);
int nftw(const char *path, __nftw_fn_t fn, int nopenfd, int flags);

#endif /* _FTW_H */
