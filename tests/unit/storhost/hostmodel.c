/* The machine underneath the storage host test -- the pollhost pattern
 * (tests/unit/pollhost/hostsched.c) applied to c/kernel/exec/fd/file.c.
 *
 * WHAT IS REAL IN THIS BUILD AND WHAT IS NOT, because that is the only thing
 * that decides what the gate is worth:
 *
 *   REAL, compiled from the tree, unmodified:
 *       c/kernel/exec/fd/file.c -- the F_VFS backend entire: open (read-only
 *       stream / writable slurp / O_TRUNC), read, write INCLUDING the
 *       zero-filled-hole rule, lseek, file_truncate (SYS_FTRUNCATE's body),
 *       fsync, and the close-time whole-file write-back with its 0/-1
 *       convention. That is every line the storage wave touched.
 *   MODELLED, faithfully to the CONTRACT rather than the code:
 *       the VFS. file.c talks to it through exactly six path-addressed calls
 *       (vfs_size/access/may_create/read/pread/write), and the contract those
 *       carry is documented in c/fs/vfs.h: ->write is create-or-overwrite
 *       WHOLE FILE, ->read is all-or-nothing, ->pread is short-at-EOF. This
 *       model implements that contract over a host-side byte array, so a
 *       whole-file flush at close lands or does not exactly as on the
 *       machine. The real logitfs (journal, block allocator) is deliberately
 *       not modelled: file.c never sees it, and the crash gates own it.
 *   MODELLED, and never driven:
 *       the console, the signals, the scheduler, the timer. The host test
 *       opens no tty, posts no signal and blocks on nothing; these stubs
 *       exist so the TU links, and every one of them aborts if reached,
 *       because a test that wandered into them would be measuring the model.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* kernel headers, resolved exactly as the kernel resolves them (the flat -I
 * scan minus mini-libc, which owns some of the same basenames). */
#include "kheap.h"
#include "sched.h"
#include "serial.h"
#include "kprintf.h"
#include "percpu.h"
#include "spinlock.h"
#include "pit.h"
#include "ksignal.h"
#include "kpoll.h"
#include "vfs.h"

/* --- the memory underneath --------------------------------------------- */

void *kmalloc(size_t n)
{
    /* POISONED ON PURPOSE. The bug this gate exists for is that a grown
     * buffer's gap was never zeroed, so whatever kmalloc handed back became
     * file content. A host malloc that returns zeroed memory would hide the
     * bug the way the machine's kmalloc only SOMETIMES hid it (10 of 16 hole
     * bytes were non-zero on the pre-fix kernel -- stale heap, not always
     * dirty). 0xA5 everywhere makes "uninitialized" indistinguishable from
     * WRONG, deterministically, on every run of the control. */
    void *p = malloc(n ? n : 1);
    if (p) memset(p, 0xA5, n ? n : 1);
    return p;
}

void kfree(void *p) { free(p); }

void kprintf(const char *fmt, ...)
{
    (void)fmt;
}

void serial_putc(char c) { (void)c; abort(); }   /* never driven: no tty in this test */

/* --- locks / per-cpu / clock: single-threaded, uncontended ------------- */

/* BKL-free file.c uses a per-description sleeping lock. This fixture is
 * deliberately single-threaded: abort on contention instead of simulating it. */
void mutex_init(struct mutex *m) { memset(m, 0, sizeof *m); }
void mutex_lock(struct mutex *m) { assert(!m->owner); m->owner = (struct thread *)(uintptr_t)1; }
void mutex_unlock(struct mutex *m) { assert(m->owner); m->owner = NULL; }
void sched_poll_wait(void) { abort(); }

uint64_t spin_lock_irqsave(spinlock_t *l) { spin_lock(l); return 0; }
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; spin_unlock(l); }
/* Plain non-atomic lock/unlock: this build is single-threaded, so the ticket
 * algorithm spinlock.h describes has nothing to contend with; the shapes and
 * the pairings are what file.c's critical sections depend on, and those are
 * checked by inspection of the REAL spinlock.c under the SMP gates, not here. */
void spin_lock(spinlock_t *l)   { l->ticket++; }
void spin_unlock(spinlock_t *l) { l->ticket--; }

static struct cpu dummy_cpu;
struct cpu *this_cpu(void) { return &dummy_cpu; }

uint64_t timer_ticks(void) { abort(); }          /* never driven: no timerfds */

/* --- wait queues / poll / signals: linked, never driven ---------------- */

void waitq_init(struct waitq *q) { memset(q, 0, sizeof *q); }
int  waitq_wake_all(struct waitq *q) { (void)q; return 0; }
void waitq_enqueue(struct waitq *q, struct waiter *w) { (void)q; (void)w; abort(); }
void waitq_dequeue(struct waitq *q, struct waiter *w) { (void)q; (void)w; abort(); }
void sched_block_self_unlock(spinlock_t *outer, uint64_t flags) { (void)outer; (void)flags; abort(); }
void poll_wait(struct poll_table *pt, struct waitq *q) { (void)pt; (void)q; abort(); }

void ksig_tty_claim_fg(void) { abort(); }
int  ksig_tty_getc(void) { abort(); }
int  ksig_interrupted(void) { return 0; }
struct waitq *ksig_tty_waitq(void) { abort(); }
int  ksig_tty_avail(void) { return 0; }
int  ksig_post_current(int signo) { (void)signo; abort(); }

/* --- the VFS model: the CONTRACT, not the code --------------------------
 *
 * One file per test, addressed by path, held in a host array. The semantics
 * are the ones c/fs/vfs.h documents for the ops file.c calls:
 *   vfs_write  create-or-overwrite, whole file, exact size; -1 on refusal
 *              (STOR_VFS_REFUSE simulates a full disk: that is the only
 *              failure file_close()/file_fsync() are required to propagate)
 *   vfs_read   ALL OR NOTHING: a buffer smaller than the file is REFUSED
 *              (-1), not truncated -- the property twenty tree callers rely
 *              on and the reason vfs_pread exists separately
 *   vfs_pread  read(2) shape: short at EOF, 0 at or past it
 *   vfs_size   the length, or -1
 * The model counts writes so the test can assert a no-op truncate scheduled
 * no write-back -- the -2-from-close trap the fix's comment names.
 * ------------------------------------------------------------------------ */

static unsigned char vfs_bytes[65536];
static int    vfs_len   = -1;                    /* -1: the path does not exist */
static int    vfs_writes;                        /* whole-file write-backs seen */
static int    stor_vfs_refuse;                   /* "disk full" switch */

int stor_vfs_writecount(void) { return vfs_writes; }
void stor_vfs_set_refuse(int on) { stor_vfs_refuse = on; }
void stor_vfs_reset(void) { vfs_len = -1; vfs_writes = 0; stor_vfs_refuse = 0; }
int  stor_vfs_size(void) { return vfs_len; }

int vfs_size(const char *path)
{
    (void)path;                                   /* one path: "/store" */
    return vfs_len;
}

int vfs_write(const char *path, const void *buf, int size)
{
    (void)path;
    if (stor_vfs_refuse || size < 0 || (size_t)size > sizeof vfs_bytes) return -1;
    if (size > 0) memcpy(vfs_bytes, buf, (size_t)size);
    vfs_len = size;
    vfs_writes++;
    return size;
}

int vfs_read(const char *path, void *buf, int max)
{
    (void)path;
    if (vfs_len < 0 || max < vfs_len) return -1;  /* all-or-nothing, per vfs.h */
    if (vfs_len > 0) memcpy(buf, vfs_bytes, (size_t)vfs_len);
    return vfs_len;
}

int vfs_pread(const char *path, void *buf, int max, long long off)
{
    (void)path;
    if (vfs_len < 0 || off < 0) return -1;
    if (off >= vfs_len) return 0;
    int n = (int)((long long)vfs_len - off < max ? (long long)vfs_len - off : max);
    if (n > 0) memcpy(buf, vfs_bytes + off, (size_t)n);
    return n;
}

int vfs_access(const char *path, int want)
{
    (void)path; (void)want;
    return vfs_len >= 0 ? 0 : -1;                 /* the model enforces no modes */
}

int vfs_may_create(const char *path)
{
    (void)path;
    return 0;
}

int vfs_cred_pid(void) { return 1; }
void vfs_cred_current(struct vcred *c) { memset(c, 0, sizeof *c); }
