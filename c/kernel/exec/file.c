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
#include "kbench.h"     /* kb_rdtsc: what the console wait actually costs */
#include "kprintf.h"    /* the exhaustion census -- see file_alloc */
/* Path-qualified: mini-libc ships c/apps/libc/include/sys/wait.h and that
 * directory sorts before c/kernel/core in INCDIRS, so the bare form resolves to
 * the userland header. Same note as syscall.c, same build failure. */
#include "kernel/core/wait.h"   /* M27: a pipe waits, it does not poll */

void *memcpy(void *, const void *, size_t);

/* The F_SOCK backend lives in c/net/core/lsock.c. WEAK on purpose: file.c is
 * linked into host test binaries that have no network stack at all, and a hard
 * reference would make every one of them fail to link over a type they never
 * create. NULL here means "this build has no sockets", which read/write already
 * report as -1. */
long lsock_file_read(struct file *f, void *buf, long len) __attribute__((weak));
long lsock_file_write(struct file *f, const void *buf, long len) __attribute__((weak));
void lsock_file_release_backing(void *backing) __attribute__((weak));

/* --------------------------------------------------------------------------
 * WHAT THE CONSOLE WAIT COSTS.
 *
 * The sampling profiler reported ~25% of an IDLE four-core machine's samples in
 * `file_read+0x13c` and ~25% in `wm_run+0xe9`, which reads as two busy-waits
 * eating half the machine. Both of those addresses are the instruction after a
 * `hlt`. The sampler is an ordinary interrupt: when it fires on a halted core
 * the CPU wakes to take it and the RIP it records is whatever follows the halt,
 * so a core that is doing nothing at all is indistinguishable from a core in a
 * tight loop -- they both report as "in the function containing the hlt".
 *
 * That is an interpretation trap, not a bug in either file, and refuting it
 * needs a number rather than an argument. So this loop counts its own wakes and
 * the cycles it is actually AWAKE (BKL re-acquire plus the UART poll), which is
 * the only part of it that costs the machine anything. Two rdtsc per wake, on a
 * path that wakes at interrupt rate at worst: unmeasurable against what it
 * measures, and always on, because the question recurs.
 * -------------------------------------------------------------------------- */
static uint64_t g_tty_wakes, g_tty_awake_cyc;

void tty_wait_stats(uint64_t *wakes, uint64_t *awake_cyc);
void tty_wait_stats(uint64_t *wakes, uint64_t *awake_cyc)
{
    if (wakes)     *wakes     = g_tty_wakes;
    if (awake_cyc) *awake_cyc = g_tty_awake_cyc;
}

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
    uint64_t woke_at = 0;
    while ((c = serial_getc()) < 0) {
        /* Charge the previous wake with everything from `hlt` returning to here:
         * the BKL re-acquire (which may spin) and the UART poll. Nothing else in
         * this loop executes while the core is not halted. */
        if (woke_at) g_tty_awake_cyc += kb_rdtsc() - woke_at;
        /* BOTH the release window (in_kernel=0 .. spin_unlock) and the re-acquire
         * window (spin_lock .. in_kernel=1) must run with IF=0: a nested IRQ in
         * either gap reads nested=0 and re-acquires the BKL this core holds ->
         * self-deadlock. `hlt` returns via iretq with IF=1, so cli AFTER hlt too. */
        __asm__ volatile ("cli");
        this_cpu()->in_kernel = 0;
        spin_unlock(&g_bkl);
        __asm__ volatile ("sti\n\thlt\n\tcli");
        woke_at = kb_rdtsc();
        spin_lock(&g_bkl);
        this_cpu()->in_kernel = 1;
        g_tty_wakes++;
    }
    if (woke_at) g_tty_awake_cyc += kb_rdtsc() - woke_at;
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
/* A PIPE IS THE ONE WAIT IN THIS FILE THAT HAS A REAL EVENT TO WAIT ON.
 *
 * The console has no serial receive interrupt, so a read of the tty has nothing
 * to be woken BY and can only poll; a pipe's event is another thread in this
 * same kernel calling write(), which can wake the reader directly. It was
 * nevertheless a poll: bkl_hlt_wait() re-dispatched the blocked thread on every
 * interrupt -- ~100 times a second per blocked end, each time re-acquiring the
 * global lock (spinning with interrupts off if another core held it), re-testing
 * a counter, and halting again. The GUI Terminal's shell sits in exactly this
 * loop for the whole life of every command it runs.
 *
 * With a waitqueue the blocked thread is UNLINKED from the run ring: it costs
 * the scheduler's pick loop nothing, it takes the BKL zero times while waiting,
 * and it wakes on the write itself rather than up to 10 ms later. That is the
 * measurement the wait-queue line quoted -- a 200 ms wait going from 117
 * dispatches to 0 -- applied to the place the shell actually waits.
 *
 * The queue serves both directions (readers waiting for data, writers waiting
 * for room, and both waiting for the other end to close). wait_event() re-tests
 * under the queue's own lock and loops on spurious wakes, so one queue for
 * several predicates is exactly what it is built for. */
#define PIPE_SZ 8192
struct pipe {
    char buf[PIPE_SZ];
    int  head, tail, count;
    int  readers, writers;
    struct waitq wq;
};

static long pipe_read(struct file *f, void *vbuf, long len)
{
    struct pipe *p = (struct pipe *)f->backing;
    char *out = (char *)vbuf;
    if (!(f->flags & O_NONBLOCK))
        wait_event(&p->wq, p->count > 0 || p->writers == 0);
    if (p->count == 0)
        return p->writers == 0 ? 0 : EAGAIN_RC;   /* EOF, or "would block" */
    long n = 0;
    while (n < len && p->count > 0) {
        out[n++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_SZ;
        p->count--;
    }
    waitq_wake_all(&p->wq);                       /* there is room now */
    return n;
}

static long pipe_write(struct file *f, const void *vbuf, long len)
{
    struct pipe *p = (struct pipe *)f->backing;
    const char *in = (const char *)vbuf;
    long n = 0;
    while (n < len) {
        if (p->readers == 0) return n > 0 ? n : -1;          /* broken pipe */
        if (p->count == PIPE_SZ) {
            if (f->flags & O_NONBLOCK) return n > 0 ? n : EAGAIN_RC;
            wait_event(&p->wq, p->count < PIPE_SZ || p->readers == 0);
            continue;
        }
        long before = n;
        while (n < len && p->count < PIPE_SZ) {
            p->buf[p->head] = in[n++];
            p->head = (p->head + 1) % PIPE_SZ;
            p->count++;
        }
        if (n > before) waitq_wake_all(&p->wq);              /* there is data now */
    }
    return n;
}

/* Open-file-description pool. Every fd in every process points at one of these;
 * dup/fork bump refcount rather than copying.
 *
 * WHY 512 AND NOT 64. The GUI Terminal fork+execve's /bin/sh over two pipes, so
 * every launch takes four descriptions at once, and wm_launch gives each GUI
 * app fd 0/1/2 = tty besides. 64 is therefore about sixteen concurrent
 * terminals -- which nobody hit until a memory fix let the machine launch apps
 * 2.6x faster, at which point a two-minute run produced 309 `[pipe] file_pipe
 * failed`. Before that the launches were failing earlier for a different
 * reason, so the table was never the binding constraint and its size had never
 * been tested.
 *
 * struct file is ~176 bytes, so 512 is ~90 KiB of .bss -- bounded, static, and
 * cheap against a 511 MiB machine.
 *
 * The number is still a guess, and a bigger guess is not a fix by itself.
 * file_alloc therefore CENSUSES the table when it fails (below), because
 * "raise the limit" and "find the leak" look identical from the outside and
 * the census is what tells them apart. */
#define NFILE 512
static struct file files[NFILE];

/* High-water mark and the exhaustion census. Neither is on the fast path: the
 * mark is a compare in an already-held critical section, and the census only
 * runs when an allocation has already failed. */
static int  g_file_hiwater;
static long g_file_exhausted;

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
    int inuse = 0;
    for (int i = 0; i < NFILE; i++) {
        if (files[i].refcount == 0) {
            if (!f) {
                f = &files[i];
                f->type = F_NONE; f->refcount = 1; f->flags = 0; f->is_write = 0;  /* claim under lock */
                f->amode = 0;
                f->off = 0; f->size = 0; f->cap = 0; f->dirty = 0;
                f->backing = 0; f->path[0] = 0;
                inuse++;                     /* the one just claimed */
            }
        } else {
            inuse++;
        }
    }
    if (inuse > g_file_hiwater) g_file_hiwater = inuse;
    int census_vfs = 0, census_pipe = 0, census_tty = 0, census_sock = 0, census_other = 0;
    long nth = 0;
    if (!f) {
        /* The table is full. Say WHAT is in it: a leak and honest load are
         * indistinguishable from "file_pipe failed", and this is the line that
         * separates them. Rate-limited to powers of two so an exhausted
         * machine reports the shape of the problem instead of drowning the
         * serial console in it. */
        nth = ++g_file_exhausted;
        for (int i = 0; i < NFILE; i++) {
            switch (files[i].type) {
            case F_VFS:  census_vfs++;   break;
            case F_PIPE: census_pipe++;  break;
            case F_TTY:  census_tty++;   break;
            case F_SOCK: census_sock++;  break;
            default:     census_other++; break;
            }
        }
    }
    spin_unlock_irqrestore(&g_file_lock, fl);

    if (!f && (nth & (nth - 1)) == 0)        /* 1, 2, 4, 8, ... */
        kprintf("[file] table exhausted (%ld total): %d/%d used -- "
                "vfs=%d pipe=%d tty=%d sock=%d free-but-typed=%d\n",
                nth, NFILE, NFILE, census_vfs, census_pipe, census_tty, census_sock, census_other);
    return f;
}

/* Peak simultaneous open descriptions since boot, and how many allocations
 * have been refused. The point of exporting these is that NFILE can then be
 * checked against a measurement instead of argued about. */
void file_watermark(int *peak, long *exhausted)
{
    uint64_t fl = spin_lock_irqsave(&g_file_lock);
    if (peak) *peak = g_file_hiwater;
    if (exhausted) *exhausted = g_file_exhausted;
    spin_unlock_irqrestore(&g_file_lock, fl);
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

/* The permission check belongs HERE, at open, and not only inside vfs_read.
 *
 * A vfs_read that refuses returns a negative count, and the F_VFS backend
 * treats a failed slurp as "the file is empty" -- so `cat` on a file it may
 * not read printed nothing and exited 0. That is a refusal nobody can see,
 * which is the failure mode a permission model is supposed to prevent. Failing
 * the open means the caller gets -1 from open(2), which is what every program
 * already knows how to report.
 *
 * The mask follows the access mode, and O_TRUNC counts as a write even on a
 * descriptor that is never written through: truncation IS the modification. */
static int open_want(int flags)
{
    int acc = flags & 3;
    int want = (acc == O_WRONLY) ? MAY_WRITE
             : (acc == O_RDWR)   ? (MAY_READ | MAY_WRITE)
             :                      MAY_READ;
    if (flags & (O_TRUNC | O_APPEND)) want |= MAY_WRITE;
    return want;
}

struct file *file_open_vfs(const char *path, int flags)
{
    int sz = vfs_size(path);
    int exists = (sz >= 0);
    if (!exists && !(flags & O_CREAT)) return 0;

    /* An existing file is checked against its own mode; a file about to be
     * created is checked against the directory that would gain the name --
     * there is nothing else to check, and skipping it is the hole where an
     * unprivileged process creates files in a root-owned directory. */
    if (exists) { if (vfs_access(path, open_want(flags)) < 0) return 0; }
    else        { if (vfs_may_create(path) < 0) return 0; }

    struct file *f = file_alloc();
    if (!f) return 0;
    f->type = F_VFS; f->flags = flags; f->off = 0; f->dirty = 0;
    f->amode = flags & 3;
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
        /* The access mode is a property of the open file DESCRIPTION, so it is
         * checked here rather than at the descriptor: a dup of a write-only fd
         * is still write-only, and a fork inherits the same answer. */
        if (f->amode == O_WRONLY) return -1;
        long avail = f->size - f->off;
        if (avail <= 0) return 0;                 /* EOF */
        long n = len < avail ? len : avail;
        memcpy(buf, (char *)f->backing + f->off, (size_t)n);
        f->off += n;
        return n;
    }
    if (f->type == F_PIPE) return pipe_read(f, buf, len);
    if (f->type == F_TTY)  return tty_read(f, buf, len);
    if (f->type == F_SOCK) return lsock_file_read ? lsock_file_read(f, buf, len) : -1;
    return -1;
}

long file_write(struct file *f, const void *buf, long len)
{
    if (!f || len < 0) return -1;
    if (f->type == F_VFS) {
        if (f->amode == O_RDONLY) return -1;
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
    if (f->type == F_SOCK) return lsock_file_write ? lsock_file_write(f, buf, len) : -1;
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

/* Flush a dirty F_VFS file back to the on-disk filesystem NOW, without waiting
 * for the last close. Clears dirty only on success, so a failed flush is still
 * retried at close instead of being silently dropped. The caller holds an fd
 * reference, so the file cannot be torn down mid-flush. */
int file_fsync(struct file *f)
{
    if (!f) return -1;
    if (f->type != F_VFS) return 0;            /* pipes/tty: nothing to persist */
    if (!f->dirty) return 0;
    if (!f->path[0]) return -1;
    int rc = vfs_write(f->path, f->backing ? f->backing : "", (int)f->size);
    if (rc < 0) return -1;
    f->dirty = 0;
    return 0;
}

/* Create a pipe: two struct files (read end is_write=0, write end is_write=1)
 * sharing one ring buffer. */
int file_pipe(struct file **rd, struct file **wr)
{
    struct pipe *p = (struct pipe *)kmalloc(sizeof *p);
    if (!p) return -1;
    p->head = p->tail = p->count = 0; p->readers = 1; p->writers = 1;
    waitq_init(&p->wq);
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
    } else if (type == F_SOCK) {
        /* The socket state is lsock.c's, and the LAST close is what releases the
         * listener/connection underneath -- so a dup'd or forked socket fd stays
         * live until every copy is gone, exactly like every other type here.
         * f->backing was cleared above, hence the local copy. */
        if (lsock_file_release_backing) lsock_file_release_backing(backing);
    } else if (type == F_PIPE) {
        struct pipe *p = (struct pipe *)backing;
        if (p) {
            /* readers/writers are shared with the other pipe end -> decrement under
             * the lock; the side that drops the last count frees the pipe (outside). */
            uint64_t fl2 = spin_lock_irqsave(&g_file_lock);
            if (is_write) p->writers--; else p->readers--;
            int free_pipe = (p->readers == 0 && p->writers == 0);
            spin_unlock_irqrestore(&g_file_lock, fl2);
            /* THE WAKE THAT MAKES CLOSING AN END VISIBLE. The old poll re-tested
             * `writers` every 10 ms, so it noticed EOF whether or not anyone
             * announced it; a parked reader is woken by events or not at all,
             * and "the last writer went away" is the event that turns its wait
             * into a 0-byte return. Omitting this is a hang, not a slowdown. */
            if (free_pipe) kfree(p);
            else           waitq_wake_all(&p->wq);
        }
    }
}
