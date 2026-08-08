#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>

/* Directory reading over SYS_GETDENTS.
 *
 * This used to be one SYS_DIR_NAME per entry, with names truncated at 63 bytes
 * and no way to detect it, and d_ino always 0. All three are fixed: a batch of
 * entries per syscall, names up to 255 bytes, and a real d_ino wherever the
 * backend has inode numbers. d_type is real and now distinguishes a symlink
 * from a regular file, which the old dir/file boolean could not.
 *
 * The one caveat that remains, stated because it is observable: the kernel's
 * cursor is a POSITION in the directory, not a snapshot, so a directory
 * modified between two readdir() calls can still make an entry repeat or
 * vanish. A dirfd-based implementation would not; there is no dirfd here. */

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

struct dirent {
    ino_t          d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct __dirstream DIR;

DIR *opendir(const char *path);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *d);
int  readdir_r(DIR *d, struct dirent *entry, struct dirent **result);
int  closedir(DIR *d);
void rewinddir(DIR *d);
void seekdir(DIR *d, long loc);
long telldir(DIR *d);
int  dirfd(DIR *d);
int  alphasort(const struct dirent **a, const struct dirent **b);
int  scandir(const char *path, struct dirent ***namelist,
             int (*filter)(const struct dirent *),
             int (*compar)(const struct dirent **, const struct dirent **));

#endif /* _DIRENT_H */
