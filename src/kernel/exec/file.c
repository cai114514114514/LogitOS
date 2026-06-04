#include <stdint.h>
#include <stddef.h>
#include "file.h"
#include "kheap.h"
#include "vfs.h"
#include "aqua_abi.h"   /* O_*, SEEK_* */

void *memcpy(void *, const void *, size_t);

/* Open-file-description pool. Every fd in every process points at one of these;
 * dup/fork bump refcount rather than copying. */
#define NFILE 64
static struct file files[NFILE];

void file_init(void)
{
    for (int i = 0; i < NFILE; i++) { files[i].type = F_NONE; files[i].refcount = 0; }
}

struct file *file_alloc(void)
{
    for (int i = 0; i < NFILE; i++) {
        if (files[i].refcount == 0) {
            struct file *f = &files[i];
            f->type = F_NONE; f->refcount = 1; f->flags = 0;
            f->off = 0; f->size = 0; f->cap = 0; f->dirty = 0;
            f->backing = 0; f->path[0] = 0;
            return f;
        }
    }
    return 0;
}

void file_dup(struct file *f) { if (f) f->refcount++; }

static void scopy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* --- F_VFS backend: the whole file lives in a kmalloc buffer with an offset
 *     cursor; writes grow the buffer and mark dirty; the last close flushes it
 *     back to the on-disk filesystem. Avoids touching aquafs block logic. --- */

static int vfs_ensure_cap(struct file *f, long need)
{
    if (need <= f->cap) return 0;
    long ncap = f->cap ? f->cap : 4096;
    while (ncap < need) ncap *= 2;
    char *nb = kmalloc((size_t)ncap);
    if (!nb) return -1;
    if (f->backing && f->size) memcpy(nb, f->backing, (size_t)f->size);
    if (f->backing) kfree(f->backing);
    f->backing = nb; f->cap = ncap;
    return 0;
}

struct file *file_open_vfs(const char *path, int flags)
{
    int sz = vfs_size(path);
    int exists = (sz >= 0);
    if (!exists && !(flags & O_CREAT)) return 0;

    struct file *f = file_alloc();
    if (!f) return 0;
    f->type = F_VFS; f->flags = flags; f->off = 0; f->dirty = 0;
    scopy(f->path, path, sizeof f->path);

    if (exists && !(flags & O_TRUNC)) {
        long cap = sz > 0 ? sz : 1;
        f->backing = kmalloc((size_t)cap);
        if (!f->backing) { f->refcount = 0; f->type = F_NONE; return 0; }
        f->cap = cap;
        int n = sz > 0 ? vfs_read(path, f->backing, sz) : 0;
        f->size = n > 0 ? n : 0;
    } else {
        f->backing = 0; f->cap = 0; f->size = 0;
        f->dirty = 1;            /* O_CREAT/O_TRUNC: materialise even if empty */
    }
    return f;
}

long file_read(struct file *f, void *buf, long len)
{
    if (!f || len < 0) return -1;
    if (f->type == F_VFS) {
        long avail = f->size - f->off;
        if (avail <= 0) return 0;                 /* EOF */
        long n = len < avail ? len : avail;
        memcpy(buf, (char *)f->backing + f->off, (size_t)n);
        f->off += n;
        return n;
    }
    return -1;       /* F_PIPE (P3) / F_TTY (P5) dispatch added later */
}

long file_write(struct file *f, const void *buf, long len)
{
    if (!f || len < 0) return -1;
    if (f->type == F_VFS) {
        if (f->flags & O_APPEND) f->off = f->size;
        if (vfs_ensure_cap(f, f->off + len) < 0) return -1;
        memcpy((char *)f->backing + f->off, buf, (size_t)len);
        f->off += len;
        if (f->off > f->size) f->size = f->off;
        f->dirty = 1;
        return len;
    }
    return -1;       /* F_PIPE (P3) / F_TTY (P5) dispatch added later */
}

long file_lseek(struct file *f, long off, int whence)
{
    if (!f || f->type != F_VFS) return -1;
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->off
              : whence == SEEK_END ? f->size : -1;
    if (base < 0) return -1;
    long no = base + off;
    if (no < 0) return -1;
    f->off = no;
    return no;
}

int file_pipe(struct file **rd, struct file **wr) { (void)rd; (void)wr; return -1; }  /* P3 */

/* Release a reference. At the last one: flush a dirty F_VFS file, release the
 * pipe end (P3), and return the slot to the pool. */
void file_close(struct file *f)
{
    if (!f || f->refcount <= 0) return;
    if (--f->refcount > 0) return;
    if (f->type == F_VFS && f->dirty && f->path[0])
        vfs_write(f->path, f->backing ? f->backing : "", (int)f->size);
    if (f->backing && f->type == F_VFS) kfree(f->backing);
    f->backing = 0;
    f->type = F_NONE;
}
