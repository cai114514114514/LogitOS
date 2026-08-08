#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#include <time.h>

/* WHAT LOGITFS ACTUALLY KNOWS, AND WHAT IT DOES NOT.
 *
 * This block used to say "there is no stat syscall" and list which fields were
 * honest constants. There is one now (SYS_STAT / SYS_LSTAT / SYS_FSTAT), and
 * the list is much shorter:
 *
 *   st_size, st_blocks   REAL, off the inode.
 *   st_mode              REAL. Type bits from the backend; permission bits from
 *                        the inode when somebody has set them, otherwise the
 *                        0644 / 0755 default -- see the WARNING below.
 *   st_uid / st_gid      REAL, off the inode, and they survive a reboot.
 *   st_nlink             REAL, but hard links are VFS records kept in RAM, so a
 *                        count above 1 does NOT survive a reboot.
 *   st_ino               REAL on LogitFS. (st_dev, st_ino) identifies a file.
 *   st_atime/mtime/ctime REAL, whole seconds from the RTC. ZERO means "never
 *                        stamped" -- an image written before timestamps existed
 *                        -- and must not be read as 1970.
 *   st_dev               The mount index, 1-based. 0 = unknown.
 *   st_rdev              0. There are no device nodes.
 *
 * THE WARNING. struct stat has nowhere to record that a mode is the DEFAULT
 * rather than a choice, so an unset 0644 and a chosen 0644 are the same six
 * bytes here. If that distinction matters to you -- an installer, an archiver,
 * anything that will write the mode back -- call logit_statx() below and read
 * the LSTA_* bits. LSTA_MODE_STORED means somebody set it; LSTA_MODE_DURABLE
 * means it is on the medium and will still be there after a reboot. */

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
int lstat(const char *path, struct stat *st);   /* does NOT follow a symlink */
int fstat(int fd, struct stat *st);

/* The kernel's own answer, with the LSTA_* provenance bits POSIX has no room
 * for. `struct logit_stat` is in include/abi/logit_abi.h; a caller that does not
 * need to know where a field came from should use stat() and ignore this. */
struct logit_stat;
int logit_statx(const char *path, struct logit_stat *out);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t mask);

#endif /* _SYS_STAT_H */
