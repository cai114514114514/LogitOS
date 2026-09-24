/* <sys/statvfs.h> statvfs()/fstatvfs(), and <utime.h> utime()/utimes()/
 * utimensat() -- both "the kernel has real numbers, but no syscall hands them
 * to ring 3" cases, and both handled the same way: ENOSYS, not a guess.
 *
 * CORRECTION 2026-09-24: statvfs() is no longer one of them. SYS_STATVFS hands
 * logitfs's counts to ring 3 (include/abi/logit_abi.h, struct logit_statvfs),
 * and statvfs() below uses it. fstatvfs() and the utime family are still
 * ENOSYS for the reasons given here; the paragraphs about statvfs are kept as
 * the record of why it used to be.
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
 *   - utime family: SYS_STAT/SYS_LSTAT/SYS_FSTAT (c/kernel/exec/load/meta.c) read
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
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

int statvfs(const char *path, struct statvfs *buf)
{
    if (!path || !buf) { errno = EFAULT; return -1; }
    /* SYS_STATVFS (2026-09-24). A path that does not exist is still ENOENT
     * (stat first, so errno says so); a filesystem that cannot report its
     * capacity (the kernel's VFS_ENOSYS) is still ENOSYS -- never a guess. */
    struct stat st;
    if (stat(path, &st) < 0) return -1;   /* stat() already set errno (ENOENT, etc.) */
    struct logit_statvfs k;
    long r = sys(SYS_STATVFS, (long)path, (long)&k, (long)sizeof k);
    if (r < 0 || k.len < sizeof k) { errno = ENOSYS; return -1; }
    buf->f_bsize = buf->f_frsize = (unsigned long)k.bsize;
    buf->f_blocks = (fsblkcnt_t)k.blocks;
    buf->f_bfree = buf->f_bavail = (fsblkcnt_t)k.bfree;
    buf->f_files = (fsfilcnt_t)k.files;
    buf->f_ffree = buf->f_favail = (fsfilcnt_t)k.ffree;
    buf->f_fsid = (unsigned long)st.st_dev;
    buf->f_flag = (k.flags & LSVFS_RDONLY) ? ST_RDONLY : 0;
    buf->f_namemax = k.namemax;
    return 0;
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
