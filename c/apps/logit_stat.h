#ifndef LOGIT_APP_STAT_H
#define LOGIT_APP_STAT_H

/* The file-metadata syscalls for the CLI programs.
 *
 * The coreutils link no libc -- they are crt0_cli plus one .c plus the inline
 * syscalls in logit.h -- so mini-libc's <sys/stat.h> is not available to them.
 * This is the same ten calls in the same header-only shape.
 *
 * Its own header rather than more lines in logit.h on purpose: logit.h is
 * included by every app in the tree and edited by every line at once, and a
 * separate file is a merge that cannot conflict.
 *
 * HOW TO READ A MODE HONESTLY. `attr & LSTA_MODE_STORED` says somebody actually
 * chose these permission bits; without it, the 0644 you are looking at is the
 * default for a file nobody has ever chmod'd. `LSTA_MODE_DURABLE` says the
 * choice is on the medium and survives a reboot. Anything that prints or copies
 * a mode should look, because "0644" and "no opinion" are not the same fact. */

#include "logit.h"
#include "logit_abi.h"

static inline int st_stat(const char *p, struct logit_stat *s)
{ return (int)_sys(SYS_STAT, (long)p, (long)s, (long)sizeof *s); }
static inline int st_lstat(const char *p, struct logit_stat *s)
{ return (int)_sys(SYS_LSTAT, (long)p, (long)s, (long)sizeof *s); }
static inline int st_fstat(int fd, struct logit_stat *s)
{ return (int)_sys(SYS_FSTAT, fd, (long)s, (long)sizeof *s); }

/* Fill up to `n` entries from *cursor (start with 0). Returns the count, 0 at
 * the end of the directory, negative on error. */
static inline int st_getdents(const char *dir, int *cursor,
                              struct logit_dirent *out, int n)
{
    struct logit_dirreq q;
    q.path = dir; q.buf = (unsigned char *)out;
    q.max = n * (int)sizeof(struct logit_dirent);
    q.cursor = *cursor; q.count = 0; q.pad = 0;
    int r = (int)_sys(SYS_GETDENTS, (long)&q, 0, 0);
    if (r > 0) *cursor = q.cursor;
    return r;
}

static inline int st_chmod(const char *p, int mode)
{ return (int)_sys(SYS_CHMOD, (long)p, mode & 07777, 0); }
static inline int st_chown(const char *p, int uid, int gid)
{ return (int)_sys(SYS_CHOWN, (long)p, uid, gid); }
static inline int st_umask(int mask)          /* mask < 0 = query */
{ return (int)_sys(SYS_UMASK, mask, 0, 0); }
static inline int st_symlink(const char *target, const char *linkpath)
{ return (int)_sys(SYS_SYMLINK, (long)target, (long)linkpath, 0); }
static inline int st_link(const char *o, const char *n)
{ return (int)_sys(SYS_LINK, (long)o, (long)n, 0); }
static inline int st_readlink(const char *p, char *buf, int max)
{ return (int)_sys(SYS_READLINK, (long)p, (long)buf, max); }

/* "drwxr-xr-x" into a caller's 11-byte buffer. One copy of it, because ls and
 * stat would otherwise each grow their own and drift. */
static inline void st_modestr(unsigned int mode, char *out)
{
    unsigned int t = mode & LST_IFMT;
    out[0] = t == LST_IFDIR ? 'd' : t == LST_IFLNK ? 'l'
           : t == LST_IFCHR ? 'c' : t == LST_IFIFO ? 'p' : '-';
    static const char *rwx = "rwx";
    for (int i = 0; i < 9; i++)
        out[1 + i] = (mode & (0400u >> i)) ? rwx[i % 3] : '-';
    out[10] = 0;
}

#endif /* LOGIT_APP_STAT_H */
