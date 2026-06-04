#include <stdint.h>
#include <stddef.h>
#include "file.h"
#include "kheap.h"
#include "vfs.h"
#include "sched.h"      /* schedule() -- block by yielding */
#include "serial.h"     /* F_TTY console */
#include "aqua_abi.h"   /* O_*, SEEK_* */

void *memcpy(void *, const void *, size_t);

/* --- F_TTY backend: the serial console. Single shared device; fd 0/1/2 of the
 *     shell point at one F_TTY file (dup'd). Reads block (yield) for one key,
 *     echo it, translate CR->LF; writes expand LF->CRLF for serial terminals. */
static long tty_read(struct file *f, void *vbuf, long len)
{
    (void)f;
    if (len <= 0) return 0;
    char *out = (char *)vbuf;
    int c;
    while ((c = serial_getc()) < 0) schedule();     /* block until a key */
    if (c == '\r') c = '\n';
    if (c == '\n')      { serial_putc('\r'); serial_putc('\n'); out[0] = '\n'; }
    else if (c == 127 || c == 8) { serial_putc(8); serial_putc(' '); serial_putc(8); out[0] = 8; }
    else                { serial_putc((char)c); out[0] = (char)c; }
    return 1;
}

static long tty_write(struct file *f, const void *vbuf, long len)
{
    (void)f;
    const char *p = (const char *)vbuf;
    for (long i = 0; i < len; i++) { if (p[i] == '\n') serial_putc('\r'); serial_putc(p[i]); }
    return len;
}

struct file *file_open_tty(void)
{
    struct file *f = file_alloc();
    if (f) f->type = F_TTY;
    return f;
}

/* A pipe: an in-kernel ring buffer with a read end and a write end. Each end is
 * a separate struct file sharing this buffer; readers/writers are 1 while that
 * end's file is open (refcount > 0), 0 once fully closed. EOF = no writers. */
#define PIPE_SZ 8192
struct pipe {
    char buf[PIPE_SZ];
    int  head, tail, count;
    int  readers, writers;
};

static long pipe_read(struct file *f, void *vbuf, long len)
{
    struct pipe *p = (struct pipe *)f->backing;
    char *out = (char *)vbuf;
    while (p->count == 0) {
        if (p->writers == 0) return 0;     /* EOF: all write ends closed */
        schedule();                        /* wait for a writer */
    }
    long n = 0;
    while (n < len && p->count > 0) {
        out[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_SZ;
        p->count--;
    }
    return n;
}

static long pipe_write(struct file *f, const void *vbuf, long len)
{
    struct pipe *p = (struct pipe *)f->backing;
    const char *in = (const char *)vbuf;
    long n = 0;
    while (n < len) {
        while (p->count == PIPE_SZ) {
            if (p->readers == 0) return n > 0 ? n : -1;   /* broken pipe */
            schedule();
        }
        if (p->readers == 0) return n > 0 ? n : -1;
        while (n < len && p->count < PIPE_SZ) {
            p->buf[p->head] = in[n++];
            p->head = (p->head + 1) % PIPE_SZ;
            p->count++;
        }
    }
    return n;
}

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
    if (f->type == F_PIPE) return pipe_read(f, buf, len);
    if (f->type == F_TTY)  return tty_read(f, buf, len);
    return -1;
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
    if (f->type == F_PIPE) return pipe_write(f, buf, len);
    if (f->type == F_TTY)  return tty_write(f, buf, len);
    return -1;
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

/* Create a pipe: two struct files (read end flags=0, write end flags=1) sharing
 * one ring buffer. */
int file_pipe(struct file **rd, struct file **wr)
{
    struct pipe *p = (struct pipe *)kmalloc(sizeof *p);
    if (!p) return -1;
    p->head = p->tail = p->count = 0; p->readers = 1; p->writers = 1;
    struct file *r = file_alloc();
    struct file *w = file_alloc();
    if (!r || !w) {
        if (r) { r->refcount = 0; r->type = F_NONE; }
        if (w) { w->refcount = 0; w->type = F_NONE; }
        kfree(p);
        return -1;
    }
    r->type = F_PIPE; r->flags = 0; r->backing = p;   /* read end */
    w->type = F_PIPE; w->flags = 1; w->backing = p;   /* write end */
    *rd = r; *wr = w;
    return 0;
}

/* Release a reference. At the last one: flush a dirty F_VFS file back to disk, or
 * drop a pipe end (freeing the buffer when both ends are gone). */
void file_close(struct file *f)
{
    if (!f || f->refcount <= 0) return;
    if (--f->refcount > 0) return;
    if (f->type == F_VFS) {
        if (f->dirty && f->path[0])
            vfs_write(f->path, f->backing ? f->backing : "", (int)f->size);
        if (f->backing) kfree(f->backing);
    } else if (f->type == F_PIPE) {
        struct pipe *p = (struct pipe *)f->backing;
        if (p) {
            if (f->flags) p->writers--; else p->readers--;
            if (p->readers == 0 && p->writers == 0) kfree(p);
        }
    }
    f->backing = 0;
    f->type = F_NONE;
}
