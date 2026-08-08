/* <dirent.h> and <sys/stat.h> over the kernel's file-metadata syscalls.
 *
 * WHAT THESE USED TO BE. There was no stat syscall at all, so stat() opened the
 * file and seeked to the end for the size, guessed S_IFDIR from SYS_DIR_COUNT
 * succeeding, and filled everything else with constants: mode 0644, uid 0, gid
 * 0, nlink 1, ino 0, and all three timestamps zero. The header said so plainly,
 * which was the right thing to do about it at the time -- but a program cannot
 * act on a comment. readdir() was SYS_DIR_NAME, one syscall per entry, names
 * truncated at 63 bytes with no way to detect it, and d_ino always 0.
 *
 * WHAT THEY ARE NOW. SYS_STAT/SYS_LSTAT/SYS_FSTAT and SYS_GETDENTS. Mode, owner,
 * link count, inode number and all three timestamps come off the medium. What
 * is still not knowable is still zero, and there is now a way to ASK which is
 * which: struct logit_stat carries LSTA_* bits, exposed here as logit_statx()
 * below, because POSIX's struct stat has nowhere to put "this 0644 is a
 * default". Ordinary POSIX code needs none of that and is unchanged. */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* ---------------------------------------------------------------------- */
/* stat                                                                    */
/* ---------------------------------------------------------------------- */
/* The caller's buffer is filled ONLY on success. POSIX leaves its contents
 * unspecified after a failure, but "unspecified" in practice means callers do
 * `stat(a,&st); if (stat(b,&st) == 0) ...` and expect a's result to survive a
 * failed second call -- zeroing it first turned that into a silent wrong
 * answer. Nothing is written until the answer is known. */
static void unpack(const struct logit_stat *ls, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev     = (dev_t)ls->dev;
    st->st_ino     = (ino_t)ls->ino;
    st->st_mode    = (mode_t)ls->mode;      /* LST_IF* are POSIX's numbers, so no map */
    st->st_nlink   = (nlink_t)ls->nlink;
    st->st_uid     = (uid_t)ls->uid;
    st->st_gid     = (gid_t)ls->gid;
    st->st_size    = (off_t)ls->size;
    st->st_blksize = (long)(ls->blksize ? ls->blksize : 4096);
    st->st_blocks  = (long)ls->blocks;
    st->st_atime   = (time_t)ls->atime;
    st->st_mtime   = (time_t)ls->mtime;
    st->st_ctime   = (time_t)ls->ctime;
}

/* The kernel's own answer, LSTA_* bits and all. Not POSIX -- there is nowhere in
 * struct stat to say "this mode is the default nobody chose", and a program that
 * cares (an installer, a backup, a `ls -l` that wants to be honest) has to be
 * able to ask. Returns 0 or -1/errno like stat(). */
int logit_statx(const char *path, struct logit_stat *out)
{
    if (!path || !out) { errno = EINVAL; return -1; }
    if (sys(SYS_STAT, (long)path, (long)out, (long)sizeof *out) < 0) { errno = ENOENT; return -1; }
    return 0;
}

int stat(const char *path, struct stat *st)
{
    if (!path || !st) { errno = EINVAL; return -1; }
    struct logit_stat ls;
    if (sys(SYS_STAT, (long)path, (long)&ls, (long)sizeof ls) < 0) { errno = ENOENT; return -1; }
    unpack(&ls, st);
    return 0;
}

int lstat(const char *path, struct stat *st)
{
    if (!path || !st) { errno = EINVAL; return -1; }
    struct logit_stat ls;
    if (sys(SYS_LSTAT, (long)path, (long)&ls, (long)sizeof ls) < 0) { errno = ENOENT; return -1; }
    unpack(&ls, st);
    return 0;
}

int fstat(int fd, struct stat *st)
{
    if (!st) { errno = EINVAL; return -1; }
    struct logit_stat ls;
    if (sys(SYS_FSTAT, fd, (long)&ls, (long)sizeof ls) < 0) { errno = EBADF; return -1; }
    unpack(&ls, st);
    return 0;
}

/* No longer degenerate: the mode reaches the inode and survives a reboot. It can
 * now FAIL, and it must -- an unprivileged process chmod'ing somebody else's
 * file is refused, and reporting success would be the same lie the old
 * accept-and-discard version told. */
int chmod(const char *path, mode_t mode)
{
    if (!path) { errno = EINVAL; return -1; }
    if (sys(SYS_CHMOD, (long)path, (long)(mode & 07777), 0) < 0) { errno = EPERM; return -1; }
    return 0;
}

/* fchmod has no fd-to-path map in the kernel to work from. Refusing is the only
 * truthful answer; silently succeeding would report a permission change that
 * never happened. */
int fchmod(int fd, mode_t mode) { (void)fd; (void)mode; errno = ENOSYS; return -1; }

mode_t umask(mode_t mask) { return (mode_t)sys(SYS_UMASK, (long)(mask & 0777), 0, 0); }

int chown(const char *path, uid_t uid, gid_t gid)
{
    if (!path) { errno = EINVAL; return -1; }
    if (sys(SYS_CHOWN, (long)path, (long)uid, (long)gid) < 0) { errno = EPERM; return -1; }
    return 0;
}

int symlink(const char *target, const char *linkpath)
{
    if (!target || !linkpath) { errno = EINVAL; return -1; }
    if (sys(SYS_SYMLINK, (long)target, (long)linkpath, 0) < 0) { errno = EEXIST; return -1; }
    return 0;
}

int link(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath) { errno = EINVAL; return -1; }
    if (sys(SYS_LINK, (long)oldpath, (long)newpath, 0) < 0) { errno = EPERM; return -1; }
    return 0;
}

/* POSIX: does NOT NUL-terminate, returns the byte count. */
ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    if (!path || !buf || bufsiz == 0) { errno = EINVAL; return -1; }
    long n = sys(SYS_READLINK, (long)path, (long)buf, (long)bufsiz);
    if (n < 0) { errno = EINVAL; return -1; }
    return (ssize_t)n;
}

/* ---------------------------------------------------------------------- */
/* dirent                                                                  */
/* ---------------------------------------------------------------------- */
/* A DIR now holds a BATCH. SYS_GETDENTS fills many entries per call, so a
 * directory of 200 files costs a handful of syscalls instead of 200 -- and the
 * cursor it hands back is what readdir resumes from, so seekdir/telldir are the
 * cursor itself rather than an entry index the kernel would have to re-derive.
 *
 * The caveat that was here before is smaller but not gone, and is restated
 * because it is observable: the cursor is a POSITION, so a directory modified
 * between two readdir() calls can still make an entry repeat or vanish. What is
 * fixed is the name length (255 bytes, and the kernel no longer truncates
 * silently at 63), d_ino (real, when the backend has inode numbers), and the
 * syscall count. */
#define DIRBATCH 8

struct __dirstream {
    char path[256];
    int  cursor;          /* the kernel's opaque resume token */
    int  n, i;            /* entries in `batch`, and the next to hand out */
    int  eof;
    struct logit_dirent batch[DIRBATCH];
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    if (!path) { errno = EINVAL; return NULL; }
    if (sys(SYS_DIR_COUNT, (long)path, 0, 0) < 0) { errno = ENOTDIR; return NULL; }
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    memset(d, 0, sizeof *d);
    size_t l = strlen(path);
    if (l >= sizeof d->path) { free(d); errno = ENAMETOOLONG; return NULL; }
    memcpy(d->path, path, l + 1);
    return d;
}
/* There is no fd-to-path mapping in the kernel, so a DIR cannot be built from
 * an fd. Failing is the only truthful answer. */
DIR *fdopendir(int fd) { (void)fd; errno = ENOSYS; return NULL; }

static int refill(DIR *d)
{
    if (d->eof) return 0;
    struct logit_dirreq req;
    memset(&req, 0, sizeof req);
    req.path   = d->path;
    req.buf    = (unsigned char *)d->batch;
    req.max    = (int)sizeof d->batch;
    req.cursor = d->cursor;
    long got = sys(SYS_GETDENTS, (long)&req, 0, 0);
    if (got <= 0) { d->eof = 1; return 0; }
    d->cursor = req.cursor;
    d->n = req.count;
    d->i = 0;
    return d->n;
}

static unsigned char dt_of(unsigned int mode)
{
    switch (mode & LST_IFMT) {
    case LST_IFDIR:  return DT_DIR;
    case LST_IFLNK:  return DT_LNK;
    case LST_IFREG:  return DT_REG;
    case LST_IFCHR:  return DT_CHR;
    case LST_IFIFO:  return DT_FIFO;
    default:         return DT_UNKNOWN;
    }
}

struct dirent *readdir(DIR *d)
{
    if (!d) { errno = EBADF; return NULL; }
    if (d->i >= d->n && !refill(d)) return NULL;

    struct logit_dirent *e = &d->batch[d->i++];
    d->ent.d_ino    = (ino_t)e->ino;
    d->ent.d_off    = d->cursor;
    d->ent.d_reclen = (unsigned short)sizeof d->ent;
    d->ent.d_type   = dt_of(e->type);
    size_t l = strnlen(e->name, sizeof d->ent.d_name - 1);
    memcpy(d->ent.d_name, e->name, l);
    d->ent.d_name[l] = 0;
    return &d->ent;
}

int readdir_r(DIR *d, struct dirent *entry, struct dirent **result)
{
    struct dirent *e = readdir(d);
    if (!e) { *result = NULL; return 0; }
    *entry = *e; *result = entry;
    return 0;
}

int closedir(DIR *d) { if (!d) { errno = EBADF; return -1; } free(d); return 0; }
void rewinddir(DIR *d) { if (d) { d->cursor = 0; d->n = d->i = 0; d->eof = 0; } }
/* seekdir/telldir carry the KERNEL's cursor, which is what makes them mean
 * anything: an entry index would have to be re-derived by walking, and would be
 * wrong the moment the directory changed. */
void seekdir(DIR *d, long loc) { if (d) { d->cursor = (int)loc; d->n = d->i = 0; d->eof = 0; } }
long telldir(DIR *d) { return d ? (long)d->cursor : -1; }
int  dirfd(DIR *d) { (void)d; errno = ENOTSUP; return -1; }

int alphasort(const struct dirent **a, const struct dirent **b)
{ return strcmp((*a)->d_name, (*b)->d_name); }

int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent **v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (filter && !filter(e)) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            struct dirent **nv = realloc(v, (size_t)nc * sizeof *nv);
            if (!nv) goto oom;
            v = nv; cap = nc;
        }
        v[n] = malloc(sizeof **v);
        if (!v[n]) goto oom;
        *v[n] = *e;
        n++;
    }
    closedir(d);
    if (compar && n > 1)
        qsort(v, (size_t)n, sizeof *v,
              (int (*)(const void *, const void *))compar);
    *namelist = v;
    return n;
oom:
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
    closedir(d);
    errno = ENOMEM;
    return -1;
}
