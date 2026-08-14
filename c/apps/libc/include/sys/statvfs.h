#ifndef _SYS_STATVFS_H
#define _SYS_STATVFS_H
#include <sys/types.h>

/* statvfs()/fstatvfs(): filesystem capacity/usage. See the header comment in
 * fsmisc.c for why both are ENOSYS on this kernel -- short version, LogitFS
 * (c/fs/logitfs.c) tracks free/total blocks in `struct lfs_super`, but that
 * struct lives entirely kernel-side and NO syscall hands its numbers to ring
 * 3. SYS_SYSINFO prints a free-form TEXT report (uptime/mem/process list,
 * c/kernel/gui/wm.c's sysinfo_text()) that happens to mention memory, not
 * disk space, and is not a committed ABI to parse even if it did -- scraping
 * a human-readable string for a number is exactly the kind of fragile
 * invention this library's house rule (c/apps/libc/src/io.c's header) rules
 * out. `struct statvfs` is still defined here, fully, in the normal glibc
 * shape: a program that only ever checks the return value for failure must
 * still be able to declare a `struct statvfs` and compile. */

struct statvfs {
    unsigned long f_bsize;      /* file system block size */
    unsigned long f_frsize;     /* fragment size */
    fsblkcnt_t    f_blocks;     /* size of fs in f_frsize units */
    fsblkcnt_t    f_bfree;      /* free blocks */
    fsblkcnt_t    f_bavail;     /* free blocks for unprivileged users */
    fsfilcnt_t    f_files;      /* inodes */
    fsfilcnt_t    f_ffree;      /* free inodes */
    fsfilcnt_t    f_favail;     /* free inodes for unprivileged users */
    unsigned long f_fsid;       /* filesystem id */
    unsigned long f_flag;       /* mount flags, ST_* below */
    unsigned long f_namemax;    /* max filename length */
};

#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif /* _SYS_STATVFS_H */
