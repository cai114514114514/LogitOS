#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#include <time.h>

/* WHAT LOGITFS ACTUALLY KNOWS, AND WHAT IT DOES NOT.
 *
 * There is no stat syscall: the kernel exposes a file's size (SYS_LSEEK to the
 * end, or SYS_DIR_NAME) and whether a path is a directory (SYS_DIR_COUNT
 * succeeds), and nothing else. So st_size, st_mode's file-type bits and
 * st_blocks are REAL, and the rest are honest constants:
 *
 *   st_mode permissions  0644 for files, 0755 for directories -- LogitFS has
 *                        no permission bits and no users to apply them to.
 *   st_uid / st_gid      0. There is one user.
 *   st_nlink             1. No hard links (LINK_MAX is 1).
 *   st_ino               0. LogitFS has inodes, but no syscall reports the
 *                        number, so this cannot be used to detect aliasing.
 *   st_atime/mtime/ctime 0. Nothing records file times.
 *   st_dev / st_rdev     0.
 *
 * The consequence worth knowing: a build system that decides what to rebuild by
 * comparing mtimes will see every file as equally old. That is a limitation of
 * the filesystem, not of this header, and it is better to see zeros than to see
 * a plausible-looking invented timestamp. */

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    long      st_blksize;
    long      st_blocks;
    time_t    st_atime;
    long      st_atime_nsec;
    time_t    st_mtime;
    long      st_mtime_nsec;
    time_t    st_ctime;
    long      st_ctime_nsec;
};

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000
#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);   /* no symlinks: same as stat */
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t mask);

#endif /* _SYS_STAT_H */
