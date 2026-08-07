#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>

/* Directory reading over SYS_DIR_COUNT / SYS_DIR_NAME.
 *
 * The kernel enumerates a directory by INDEX, not by an open handle, so a DIR
 * here is an index plus the directory's path. The consequence, stated because
 * it is observable: a directory modified while it is being read can make
 * readdir() skip or repeat an entry, where a real dirfd-based implementation
 * would give a stable snapshot. d_ino is 0 (the kernel does not report inode
 * numbers -- see <sys/stat.h>); d_type IS real, because SYS_DIR_NAME
 * distinguishes a directory from a file. */

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
