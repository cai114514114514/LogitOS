#include <stdint.h>
#include <stddef.h>
#include "file.h"
#include "kheap.h"
#include "vfs.h"
#include "sched.h"      /* bkl_hlt_wait() -- block without hogging the BKL */
#include "serial.h"     /* F_TTY console */
#include "logit_abi.h"   /* O_*, SEEK_* */
#include "percpu.h"     /* this_cpu (SMP: drop BKL while blocked on input) */
#include "spinlock.h"   /* g_bkl */

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
    /* Block until a key WITHOUT hogging the BKL. The old `schedule()` here did
     * nothing when no other thread was runnable (next==prev), so this loop
     * busy-polled serial_getc while holding the global BKL with IF=0 -- under SMP
     * that froze every other core (the WM compositor, other apps) whenever the
     * shell sat at its prompt. Instead drop the BKL and idle until the next
     * interrupt (timer 100Hz / serial), exactly like the WM idle loop, so the
     * other cores keep running while we wait. */
    while ((c = serial_getc()) < 0) {
        /* BOTH the release window (in_kernel=0 .. spin_unlock) and the re-acquire
         * window (spin_lock .. in_kernel=1) must run with IF=0: a nested IRQ in
         * either gap reads nested=0 and re-acquires the BKL this core holds ->
         * self-deadlock. `hlt` returns via iretq with IF=1, so cli AFTER hlt too. */
        __asm__ volatile ("cli");
        this_cpu()->in_kernel = 0;
        spin_unlock(&g_bkl);
        __asm__ volatile ("sti\n\thlt\n\tcli");
        spin_lock(&g_bkl);
        this_cpu()->in_kernel = 1;
    }
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
        if (f->flags & O_NONBLOCK) return EAGAIN_RC;   /* would block */
        bkl_hlt_wait();                    /* wait for a writer WITHOUT hogging the BKL */
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
            if (f->flags & O_NONBLOCK) return n > 0 ? n : EAGAIN_RC;  /* would block */
            bkl_hlt_wait();                /* wait for a reader WITHOUT hogging the BKL */
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

/* M25 P2: file lifecycle counters are peeled out from under the BKL so SYS_FORK
 * (which file_dup's the inherited fds) can run BKL-free concurrently. g_file_lock
 * guards the refcount RMW (dup/close), the files[] slot claim (alloc), and the
 * pipe readers/writers decrement. Held for tiny critical sections only: the actual
 * cleanup on the last close (vfs flush, kfree backing/pipe) runs OUTSIDE it, so
 * the lock never nests above kheap/vfs (order ... g_file_lock -> g_kheap_lock). */
static spinlock_t g_file_lock = SPINLOCK_INIT;

void file_init(void)
{
    for (int i = 0; i < NFILE; i++) { files[i].type = F_NONE; files[i].refcount = 0; }
}

struct file *file_alloc(void)
{
    uint64_t fl = spin_lock_irqsave(&g_file_lock);
    struct file *f = 0;
    for (int i = 0; i < NFILE; i++) {
        if (files[i].refcount == 0) {
            f = &files[i];
            f->type = F_NONE; f->refcount = 1; f->flags = 0; f->is_write = 0;  /* claim under lock */
            f->off = 0; f->size = 0; f->cap = 0; f->dirty = 0;
            f->backing = 0; f->path[0] = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_file_lock, fl);
    return f;
}

void file_dup(struct file *f)
{
    if (!f) return;
    uint64_t fl = spin_lock_irqsave(&g_file_lock);
    f->refcount++;
    spin_unlock_irqrestore(&g_file_lock, fl);
}

static void scopy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* --- F_VFS backend: the whole file lives in a kmalloc buffer with an offset
 *     cursor; writes grow the buffer and mark dirty; the last close flushes it
 *     back to the on-disk filesystem. Avoids touching logitfs block logic. --- */

static int vfs_ensure_cap(struct file *f, long need)
{
    if (need <= f->cap) return 0;
    long ncap = f->cap ? f->cap : 4096;
    while (ncap < need) {
        if (ncap > (long)0x3fffffffffffffffL) return -1;   /* doubling would overflow -> never terminates */
        ncap *= 2;
    }
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
        if (f->off > (long)0x7fffffffffffffffL - len) return -1;   /* off+len would wrap negative */
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
    /* base+off can overflow signed long (UB). base >= 0 here, so a positive off
     * overflows iff off > LONG_MAX - base; a negative off can't overflow. */
    if (off > 0 && off > (long)0x7fffffffffffffffL - base) return -1;
    long no = base + off;
    if (no < 0) return -1;
    f->off = no;
    return no;
}

/* Create a pipe: two struct files (read end is_write=0, write end is_write=1)
 * sharing one ring buffer. */
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
    r->type = F_PIPE; r->flags = 0; r->is_write = 0; r->backing = p;   /* read end */
    w->type = F_PIPE; w->flags = 0; w->is_write = 1; w->backing = p;   /* write end */
    *rd = r; *wr = w;
    return 0;
}

/* Release a reference. At the last one: flush a dirty F_VFS file back to disk, or
 * drop a pipe end (freeing the buffer when both ends are gone). */
void file_close(struct file *f)
{
    if (!f) return;
    /* Drop the reference under the lock; only the caller that takes it to 0 owns the
     * teardown. The slot is fully detached (fields reset) BEFORE the lock is
     * released: a concurrent file_alloc can reclaim a refcount==0 slot immediately,
     * so teardown writes to f->* after the unlock would clobber the NEW owner's
     * state. The vfs flush / kfree still run OUTSIDE the lock, on local copies. */
    uint64_t fl = spin_lock_irqsave(&g_file_lock);
    if (f->refcount <= 0) { spin_unlock_irqrestore(&g_file_lock, fl); return; }
    int last = (--f->refcount == 0);
    int type = 0, dirty = 0, is_write = 0;
    long size = 0;
    void *backing = 0;
    char path[sizeof f->path];
    if (last) {
        type = f->type; dirty = f->dirty; is_write = f->is_write;
        size = f->size; backing = f->backing;
        memcpy(path, f->path, sizeof path);
        f->backing = 0; f->path[0] = 0; f->type = F_NONE;
    }
    spin_unlock_irqrestore(&g_file_lock, fl);
    if (!last) return;

    if (type == F_VFS) {
        if (dirty && path[0])
            vfs_write(path, backing ? backing : "", (int)size);
        if (backing) kfree(backing);
    } else if (type == F_PIPE) {
        struct pipe *p = (struct pipe *)backing;
        if (p) {
            /* readers/writers are shared with the other pipe end -> decrement under
             * the lock; the side that drops the last count frees the pipe (outside). */
            uint64_t fl2 = spin_lock_irqsave(&g_file_lock);
            if (is_write) p->writers--; else p->readers--;
            int free_pipe = (p->readers == 0 && p->writers == 0);
            spin_unlock_irqrestore(&g_file_lock, fl2);
            if (free_pipe) kfree(p);
        }
    }
}
