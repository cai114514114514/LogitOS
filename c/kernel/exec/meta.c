/* SYS_STAT / SYS_LSTAT / SYS_FSTAT / SYS_GETDENTS / SYS_CHMOD / SYS_UMASK /
 * SYS_SYMLINK / SYS_READLINK / SYS_LINK / SYS_CHOWN.
 *
 * WHAT THIS REPLACES. Until this file existed, the only way anything in ring 3
 * could learn a file's size was SYS_DIR_NAME: enumerate the file's PARENT by
 * index, match the name, and read the size out of the return value -- with -2
 * overloaded to mean "this entry is a directory" and the name buffer capped at
 * 64 bytes. There was no way to ask about a path, and no way at all to see a
 * mode, an owner, a link count, an inode number or a timestamp, although
 * LogitFS inodes have carried atime/mtime/ctime and the VFS has kept modes and
 * owners for as long as either has existed.
 *
 * THE ONE RULE THIS FILE FOLLOWS. Every field it reports either comes from
 * somewhere, or is accompanied by a bit saying it does not. `struct vattr`
 * carries VA_STORED / VA_DURABLE / VA_TIMES / VA_INO through from the backend
 * and they are copied straight out as LSTA_*. A stat that silently substitutes
 * 0644 for "nobody ever set a mode" is indistinguishable from a stat that works
 * -- which is exactly the failure this line exists to prevent, and exactly what
 * `make test-statmeta-negctl` builds on purpose to prove the tests can see it.
 */

#include <stdint.h>
#include <stddef.h>
#include "meta.h"
#include "logit_abi.h"
#include "usercopy.h"
#include "proc.h"
#include "file.h"
#include "vfs.h"
#include "kheap.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* --------------------------------------------------------------------------
 * vattr -> struct logit_stat
 * ------------------------------------------------------------------------ */

static unsigned int type_bits(int vt)
{
    switch (vt) {
    case VT_DIR: return LST_IFDIR;
    case VT_LNK: return LST_IFLNK;
    default:     return LST_IFREG;
    }
}

static unsigned int attr_bits(uint32_t vaflags, int nlink)
{
    unsigned int f = 0;
    if (vaflags & VA_STORED)  f |= LSTA_MODE_STORED;
    if (vaflags & VA_DURABLE) f |= LSTA_MODE_DURABLE;
    if (vaflags & VA_TIMES)   f |= LSTA_TIMES;
    if (vaflags & VA_INO)     f |= LSTA_INO;
    /* Hard links are the VFS's RAM link groups, so a link count above 1 is a
     * fact that does not survive a reboot. Said in a bit rather than in a
     * comment nobody reads at 3am. */
    if (nlink > 1)            f |= LSTA_NLINK_RAM;
    return f;
}

static void pack(const struct vattr *a, struct logit_stat *st)
{
    memset(st, 0, sizeof *st);
    st->len     = (unsigned int)sizeof *st;
    st->version = LOGIT_STAT_VERSION;
    st->mode    = type_bits(a->type) | (a->mode & 07777);
    st->attr    = attr_bits(a->flags, a->nlink);
    st->uid     = a->uid;
    st->gid     = a->gid;
    st->nlink   = (unsigned int)(a->nlink > 0 ? a->nlink : 1);
    st->blksize = a->blksize ? a->blksize : 4096;
    st->ino     = a->ino;
    st->dev     = a->dev;
    st->size    = a->size;
    st->blocks  = a->blocks;
    st->atime   = a->atime;
    st->mtime   = a->mtime;
    st->ctime   = a->ctime;
}

/* Copy out at most `len` bytes and stamp `len` with what was actually written.
 *
 * This is the whole forward-compatibility story and it is three lines: an OLD
 * binary passing a SMALLER struct gets its own struct filled and no more, and a
 * NEW binary on an OLD kernel reads a smaller `len` and knows the tail is
 * untouched rather than trusting whatever was on its stack. Cheap now,
 * impossible to add later. */
static long emit(const struct logit_stat *st, void *ubuf, long len)
{
    if (len < (long)sizeof(unsigned int) * 2) return -1;   /* no room for len+version */
    long n = len < (long)sizeof *st ? len : (long)sizeof *st;
    if (!user_range_ok(ubuf, (uint64_t)n, 1)) return -1;
    struct logit_stat tmp = *st;
    tmp.len = (unsigned int)n;
    memcpy(ubuf, &tmp, (size_t)n);
    return 0;
}

/* A failed stat writes NOTHING. POSIX calls the buffer unspecified after a
 * failure, but in practice callers do `stat(a,&st); if (stat(b,&st)==0)...` and
 * expect a's answer to survive a failed second call; zeroing first turns that
 * into a silent wrong answer. */
static long stat_path(const char *upath, void *ubuf, long len, int follow)
{
    char path[128], abs[128];
    struct proc *p = proc_current();
    if (!p || user_copy_string(path, sizeof path, upath) < 0) return -1;
    proc_resolve(p, path, abs, sizeof abs);

    struct vattr a;
    int rc = vfs_statx(abs, &a, follow);
    if (rc < 0) return rc;

    struct logit_stat st;
    pack(&a, &st);
    return emit(&st, ubuf, len);
}

/* --------------------------------------------------------------------------
 * fstat
 *
 * An fd is not always a path. A pipe and the serial console have no name, no
 * inode and no mode on any medium, and reporting them as zero-length regular
 * files is what made `isatty`-shaped checks over fstat useless. They get their
 * real POSIX type instead, with LSTA_* empty because none of it is stored.
 * ------------------------------------------------------------------------ */
static long stat_fd(int fd, void *ubuf, long len)
{
    struct proc *p = proc_current();
    struct file *f = p ? proc_fd_get(p, fd) : NULL;
    if (!f) return -1;

    if (f->type == F_VFS && f->path[0]) {
        struct vattr a;
        if (vfs_statx(f->path, &a, 1) == 0) {
            struct logit_stat st;
            pack(&a, &st);
            /* The DESCRIPTION's length, not the directory entry's: a file
             * written through this fd and not yet flushed is longer here, and
             * an fstat that disagreed with a following read would be worse than
             * useless. */
            if (f->size >= 0) st.size = (unsigned long long)f->size;
            return emit(&st, ubuf, len);
        }
    }

    struct logit_stat st;
    memset(&st, 0, sizeof st);
    st.len     = (unsigned int)sizeof st;
    st.version = LOGIT_STAT_VERSION;
    st.nlink   = 1;
    st.blksize = 4096;
    st.attr    = 0;                       /* nothing here is stored anywhere */
    switch (f->type) {
    case F_PIPE: st.mode = LST_IFIFO | 0600; break;
    case F_TTY:  st.mode = LST_IFCHR | 0620; break;
    default:     st.mode = LST_IFREG | 0644;
                 st.size = (unsigned long long)(f->size > 0 ? f->size : 0);
                 st.blocks = (st.size + 511) / 512;
                 break;
    }
    return emit(&st, ubuf, len);
}

/* --------------------------------------------------------------------------
 * getdents
 *
 * The kernel-side buffer is one entry, filled and copied out one at a time
 * rather than in a batch. A batch would need a kmalloc proportional to the
 * caller's request in a path that runs under the BKL, and the copy is not what
 * this costs -- the directory lookups are.
 * ------------------------------------------------------------------------ */
static long getdents(void *ureq)
{
    struct proc *p = proc_current();
    if (!p || !user_range_ok(ureq, sizeof(struct logit_dirreq), 1)) return -1;

    struct logit_dirreq req;
    memcpy(&req, ureq, sizeof req);
    if (req.max < (int)sizeof(struct logit_dirent) || req.cursor < 0) return -1;

    char path[128], abs[128];
    if (user_copy_string(path, sizeof path, req.path) < 0) return -1;
    proc_resolve(p, path, abs, sizeof abs);

    int room = req.max / (int)sizeof(struct logit_dirent);
    if (!user_range_ok(req.buf, (uint64_t)room * sizeof(struct logit_dirent), 1)) return -1;

    int written = 0, cursor = req.cursor;
    while (written < room) {
        struct vdirent v;
        int got = vfs_getdents(abs, &cursor, &v, 1);
        if (got < 0) return got;
        if (got == 0) break;

        struct logit_dirent e;
        memset(&e, 0, sizeof e);
        e.reclen = (unsigned int)sizeof e;
        e.type   = type_bits(v.type);
        e.ino    = v.ino;
        e.size   = v.size;
        int i = 0;
        for (; i < (int)sizeof e.name - 1 && v.name[i]; i++) e.name[i] = v.name[i];
        e.name[i] = 0;
        memcpy((unsigned char *)req.buf + (size_t)written * sizeof e, &e, sizeof e);
        written++;
    }

    req.cursor = cursor;
    req.count  = written;
    memcpy(ureq, &req, sizeof req);
    return written;
}

/* --------------------------------------------------------------------------
 * the rest
 * ------------------------------------------------------------------------ */

static long path_op(const char *ua, const char *ub, int op)
{
    char a[128], b[128], aa[128], ab[128];
    struct proc *p = proc_current();
    if (!p) return -1;
    if (user_copy_string(a, sizeof a, ua) < 0) return -1;

    if (op == 0) {                                   /* symlink(target, link) */
        /* The TARGET is not resolved: a symlink stores the text it was given,
         * relative or absolute, and resolving it here would silently turn every
         * relative link into an absolute one made against the wrong cwd. */
        if (user_copy_string(b, sizeof b, ub) < 0) return -1;
        proc_resolve(p, b, ab, sizeof ab);
        return vfs_symlink(a, ab);
    }
    if (user_copy_string(b, sizeof b, ub) < 0) return -1;
    proc_resolve(p, a, aa, sizeof aa);
    proc_resolve(p, b, ab, sizeof ab);
    return vfs_link(aa, ab);
}

static long readlink_op(const char *upath, char *ubuf, long max)
{
    char path[128], abs[128], tgt[256];
    struct proc *p = proc_current();
    if (!p || max <= 0 || user_copy_string(path, sizeof path, upath) < 0) return -1;
    if (!user_range_ok(ubuf, (uint64_t)max, 1)) return -1;
    proc_resolve(p, path, abs, sizeof abs);

    int n = vfs_readlink(abs, tgt, (int)sizeof tgt);
    if (n < 0) return n;
    if (n > max) n = (int)max;      /* POSIX truncates; it does not NUL-terminate */
    memcpy(ubuf, tgt, (size_t)n);
    return n;
}

long meta_syscall(long num, long a, long b, long c)
{
    switch (num) {
    case SYS_STAT:     return stat_path((const char *)a, (void *)b, c, 1);
    case SYS_LSTAT:    return stat_path((const char *)a, (void *)b, c, 0);
    case SYS_FSTAT:    return stat_fd((int)a, (void *)b, c);
    case SYS_GETDENTS: return getdents((void *)a);

    case SYS_CHMOD: {
        char path[128], abs[128];
        struct proc *p = proc_current();
        if (!p || user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        proc_resolve(p, path, abs, sizeof abs);
        return vfs_chmod(abs, (unsigned)b & 07777);
    }
    case SYS_CHOWN: {
        char path[128], abs[128];
        struct proc *p = proc_current();
        if (!p || user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        proc_resolve(p, path, abs, sizeof abs);
        return vfs_chown(abs, (unsigned)b, (unsigned)c);
    }
    /* Returns the PREVIOUS mask, which is what makes the save-and-restore idiom
     * every installer uses work. A negative argument only queries. */
    case SYS_UMASK:    return (long)vfs_umask((int)a);

    case SYS_SYMLINK:  return path_op((const char *)a, (const char *)b, 0);
    case SYS_LINK:     return path_op((const char *)a, (const char *)b, 1);
    case SYS_READLINK: return readlink_op((const char *)a, (char *)b, c);
    }
    return -1;
}
