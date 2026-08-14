#ifndef _UTIME_H
#define _UTIME_H
#include <sys/types.h>
#include <sys/time.h>   /* struct timeval, for utimes() -- see below */

/* utime()/utimes()/utimensat(): set a file's access/modification times.
 *
 * THE DATA EXISTS. LogitFS inodes carry real atime/mtime/ctime (CLAUDE.md's
 * Storage section, c/fs/logitfs_fmt.h) and SYS_STAT/SYS_LSTAT/SYS_FSTAT
 * (c/kernel/exec/meta.c) read them out to ring 3 already -- see <sys/stat.h>.
 * But nothing in include/abi/logit_abi.h SETS them: there is no syscall
 * analogous to SYS_CHMOD (mode) or SYS_CHOWN (owner) for timestamps, and
 * `struct vattr`'s setattr path (c/fs/vfs_meta.h) is not wired to any ring-3
 * entry point either. mtime/ctime move on their own as a side effect of a
 * real write (logitfs.c's touch_times()), which is correct but is not the
 * same thing as a caller choosing a value -- `touch -t`, tar extracting an
 * archive's recorded mtime, and make's timestamp games all need the latter.
 * So: real ENOSYS, not a lie that returns 0 and leaves the inode's clock
 * exactly where it was, which a caller that checks the return value (as one
 * must; utime can fail for permissions on any real system too) will believe
 * worked. A kernel fix that would close this gap: a SYS_UTIME analogous to
 * SYS_CHMOD, taking (path, atime, mtime).
 *
 * WHERE THESE ARE DECLARED. POSIX puts utime()/struct utimbuf in <utime.h>
 * (here), utimes() in <sys/time.h>, and utimensat() in <sys/stat.h>. This
 * library's <sys/time.h> and <sys/stat.h> belong to other units of this same
 * refactor and this unit does not touch files it does not own, so utimes()
 * and utimensat() are declared HERE instead, guarded so a program that also
 * includes the "proper" header sees one prototype either way (both of those
 * headers are otherwise silent about these two names, so there is nothing to
 * conflict with today). Fixing the split -- moving utimes() into
 * <sys/time.h> and utimensat() into <sys/stat.h> once those files belong to
 * this same pass -- would be a pure header move with no behaviour change. */

struct utimbuf {
    time_t actime;    /* access time */
    time_t modtime;   /* modification time */
};

int utime(const char *path, const struct utimbuf *times);
int utimes(const char *path, const struct timeval times[2]);

/* utimensat()'s AT_FDCWD / relative-fd form: dirfd is accepted but, like the
 * rest of this call, never reaches success on this kernel, so its value is
 * never actually used to resolve `path`. `flags` (AT_SYMLINK_NOFOLLOW) is
 * declared with the value <fcntl.h> already assigns AT_FDCWD's sibling
 * constants; UTIME_NOW/UTIME_OMIT are POSIX's sentinel tv_nsec values for
 * "set to now" / "leave alone" inside `times`, defined here because that is
 * the only header that mentions utimensat at all in this library. */
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)
#define AT_SYMLINK_NOFOLLOW 0x100

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);

#endif /* _UTIME_H */
