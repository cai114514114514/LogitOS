/* <sys/statvfs.h> statvfs()/fstatvfs(), and <utime.h> utime()/utimes()/
 * utimensat() -- both "the kernel has real numbers, but no syscall hands them
 * to ring 3" cases, and both handled the same way: ENOSYS, not a guess.
 *
 * WHY NOT INVENT PLAUSIBLE NUMBERS. A `struct statvfs` with a fabricated
 * f_bsize/f_blocks is worse than one this call refuses to fill in: a caller
 * that sizes a buffer from f_bsize, or decides whether a write will fit from
 * f_bavail, gets a wrong answer that LOOKS like a right one. The same is true
 * of utime() silently returning 0 while the inode's clock does not move --
 * `touch -t`, or tar restoring an archived mtime, would believe it worked.
 * See io.c's header comment for the rule this file is following.
 *
 * WHAT WAS CHECKED before reaching ENOSYS, so this is a researched answer and
 * not a shrug:
 *   - statvfs: c/fs/logitfs_fmt.h's `struct lfs_super` has total_blocks and
 *     the free-block bitmap is real (c/fs/logitfs.c bit_set/bit_clear/
 *     flush_bitmap), so the KERNEL knows free vs. total space. But grepping
 *     every SYS_* in include/abi/logit_abi.h turns up nothing that returns
 *     it structured: SYS_SYSINFO (c/kernel/gui/wm.c sysinfo_text) prints
 *     free-form text about uptime/memory/processes, not disk space, and is
 *     not a committed binary ABI even where it overlaps.
 *   - utime family: SYS_STAT/SYS_LSTAT/SYS_FSTAT (c/kernel/exec/meta.c) read
 *     atime/mtime/ctime off the inode, and SYS_CHMOD/SYS_CHOWN prove the
 *     pattern for "let ring 3 set an inode field" exists for mode and owner
 *     -- but no SYS_UTIME-shaped call exists for the three timestamps, and
 *     vfs_meta.h's setattr path is not wired to any syscall either.
 * A kernel fix for either would be a small, focused syscall in the shape of
 * SYS_CHMOD/SYS_CHOWN; this file does not add one (out of scope for a
 * userland-only unit) and does not pretend one exists. */
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <utime.h>
#include <errno.h>

int statvfs(const char *path, struct statvfs *buf)
{
    (void)buf;
    if (!path) { errno = EFAULT; return -1; }
    /* A real statvfs distinguishes "no such path" from "can't report usage";
     * this kernel can answer the first half for free and it is worth doing,
     * since a caller that mistyped a path deserves ENOENT, not ENOSYS. */
    struct stat st;
    if (stat(path, &st) < 0) return -1;   /* stat() already set errno (ENOENT, etc.) */
    errno = ENOSYS;
    return -1;
}

int fstatvfs(int fd, struct statvfs *buf)
{
    (void)buf;
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;    /* fstat() already set errno (EBADF, etc.) */
    errno = ENOSYS;
    return -1;
}

int utime(const char *path, const struct utimbuf *times)
{
    (void)times;
    if (!path) { errno = EFAULT; return -1; }
    struct stat st;
    if (stat(path, &st) < 0) return -1;   /* a missing path is ENOENT, not ENOSYS */
    errno = ENOSYS;
    return -1;
}

int utimes(const char *path, const struct timeval times[2])
{
    (void)times;
    if (!path) { errno = EFAULT; return -1; }
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    errno = ENOSYS;
    return -1;
}

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    (void)dirfd; (void)times; (void)flags;
    if (!path) { errno = EFAULT; return -1; }
    struct stat st;
    if (stat(path, &st) < 0) return -1;   /* dirfd is never consulted -- see <utime.h> */
    errno = ENOSYS;
    return -1;
}
