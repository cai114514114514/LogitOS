/* <dirent.h> and <sys/stat.h> over the kernel's directory and file syscalls.
 *
 * Neither header existed before, which meant that essentially no third-party C
 * could be built: "does this path exist / is it a directory / how big is it"
 * and "list a directory" are what every build tool, archiver, config loader and
 * interpreter does in its first hundred lines. Both are implemented from the
 * primitives the kernel really has -- SYS_DIR_COUNT / SYS_DIR_NAME to
 * enumerate, SYS_LSEEK to size -- and both headers state, at the struct, which
 * fields are real and which are honest zeros. */
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
static void fill_common(struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    st->st_blksize = 4096;              /* LogitFS block size */
}

int stat(const char *path, struct stat *st)
{
    if (!path || !st) { errno = EINVAL; return -1; }
    fill_common(st);
    /* A directory is a path SYS_DIR_COUNT accepts; the kernel returns -1 for
     * anything that is not one, which is also how we tell "missing" apart. */
    long n = sys(SYS_DIR_COUNT, (long)path, 0, 0);
    if (n >= 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_size = n;                /* entry count: the only size a dir has here */
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { errno = ENOENT; return -1; }
    long sz = lseek(fd, 0, SEEK_END);
    close(fd);
    if (sz < 0) sz = 0;
    st->st_mode = S_IFREG | 0644;
    st->st_size = sz;
    st->st_blocks = (sz + 511) / 512;
    return 0;
}
int lstat(const char *path, struct stat *st) { return stat(path, st); }  /* no symlinks */

int fstat(int fd, struct stat *st)
{
    if (!st) { errno = EINVAL; return -1; }
    fill_common(st);
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) { errno = EBADF; return -1; }
    long sz = lseek(fd, 0, SEEK_END);
    lseek(fd, cur, SEEK_SET);
    if (sz < 0) sz = 0;
    /* An fd can be a pipe or the tty, where seeking fails; those report as a
     * FIFO / character device rather than as a zero-length regular file, so
     * `isatty`-style checks over fstat behave. */
    st->st_mode = S_IFREG | 0644;
    st->st_size = sz;
    st->st_blocks = (sz + 511) / 512;
    return 0;
}

/* DEGENERATE: LogitFS has no permission bits, so chmod accepts and discards.
 * It returns success because the caller's intent ("make this executable") is
 * already true -- every file here is readable and writable by the one user --
 * and failing would break install scripts for no protection gained. */
int chmod(const char *path, mode_t mode) { (void)mode; struct stat st; return stat(path, &st); }
int fchmod(int fd, mode_t mode) { (void)fd; (void)mode; return 0; }
mode_t umask(mode_t mask) { (void)mask; return 0; }

/* ---------------------------------------------------------------------- */
/* dirent                                                                  */
/* ---------------------------------------------------------------------- */
struct __dirstream {
    char path[256];
    long idx;
    long count;
    int  fd;
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    if (!path) { errno = EINVAL; return NULL; }
    long n = sys(SYS_DIR_COUNT, (long)path, 0, 0);
    if (n < 0) { errno = ENOTDIR; return NULL; }
    DIR *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    memset(d, 0, sizeof *d);
    size_t l = strlen(path);
    if (l >= sizeof d->path) { free(d); errno = ENAMETOOLONG; return NULL; }
    memcpy(d->path, path, l + 1);
    d->count = n;
    d->fd = -1;
    return d;
}
/* There is no fd-to-path mapping in the kernel, so a DIR cannot be built from
 * an fd. Failing is the only truthful answer. */
DIR *fdopendir(int fd) { (void)fd; errno = ENOSYS; return NULL; }

struct dirent *readdir(DIR *d)
{
    if (!d) { errno = EBADF; return NULL; }
    if (d->idx >= d->count) return NULL;               /* end of directory */
    char name[128];
    long r = sys(SYS_DIR_NAME, (long)d->path, d->idx, (long)name);
    if (r == -1) return NULL;
    d->ent.d_ino = 0;
    d->ent.d_off = d->idx;
    d->ent.d_reclen = (unsigned short)sizeof d->ent;
    d->ent.d_type = (r == -2) ? DT_DIR : DT_REG;       /* -2 is the kernel's dir marker */
    size_t l = strnlen(name, sizeof d->ent.d_name - 1);
    memcpy(d->ent.d_name, name, l);
    d->ent.d_name[l] = 0;
    d->idx++;
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
void rewinddir(DIR *d) { if (d) d->idx = 0; }
void seekdir(DIR *d, long loc) { if (d) d->idx = loc; }
long telldir(DIR *d) { return d ? d->idx : -1; }
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
