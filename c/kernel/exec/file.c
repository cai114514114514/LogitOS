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
#include "ksignal.h"    /* signals: ^C on the console, SIGPIPE, EINTR */
#include "kpoll.h"      /* poll_wait(): the readiness half of every backend here.
                         * NOT "poll.h" -- mini-libc owns that basename and
                         * INCDIRS is flat; kpoll.h's own header says why. */
#include "pit.h"        /* timerfd: the 100 Hz tick IS its clock */

void *memcpy(void *, const void *, size_t);

/* The F_SOCK backend lives in c/net/core/lsock.c. WEAK on purpose: file.c is
 * linked into host test binaries that have no network stack at all, and a hard
 * reference would make every one of them fail to link over a type they never
 * create. NULL here means "this build has no sockets", which read/write already
 * report as -1. */
long lsock_file_read(struct file *f, void *buf, long len) __attribute__((weak));
long lsock_file_write(struct file *f, const void *buf, long len) __attribute__((weak));
void lsock_file_release_backing(void *backing) __attribute__((weak));
/* The readiness half of the same seam, and the ONE function the network line
 * has to write for a socket to become pollable. Same weak treatment and the
 * same meaning when absent: this build has no sockets. Its exact contract --
 * which queue to register on for a connection and for a listener -- is in
 * c/kernel/exec/kpoll.h, written for whoever picks it up. */
short lsock_file_poll(struct file *f, struct poll_table *pt) __attribute__((weak));

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
    /* THE FOREGROUND PID. Whoever is blocking on the console IS the foreground
     * process, as far as this machine can tell -- there are no sessions and no
     * process groups here (no SYS_SETSID, no SYS_GETPGID), and half a job
     * control layer would be worse than none. So the tty holds one pid and the
     * timer's ^C goes to it. It gets the shell right, which is the case that
     * matters; it does not reach the shell's child, so `sleep 100` is not
     * interruptible until /bin/sh forwards the signal. Said out loud in
     * include/abi/logit_abi.h as well, because it is a limit and not a detail. */
    ksig_tty_claim_fg();
    uint64_t woke_at = 0;
    /* ksig_tty_getc(), not serial_getc(): the timer drains the UART now, so
     * that a ^C is seen while a child is running rather than sitting in the
     * receive register until the shell next asks for a line. This takes bytes
     * from the queue the timer fills. */
    while ((c = ksig_tty_getc()) < 0) {
        /* EINTR. A read of the console cut short by a signal has to return
         * something distinguishable from a byte and from EOF -- otherwise a
         * SIGINT handler that longjmps back to the prompt has no way to be
         * reached, because this loop would simply resume waiting. */
        if (ksig_interrupted()) return SIG_E_INTR;
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
    if (!(f->flags & O_NONBLOCK)) {
        /* ksig_interrupted() is in the PREDICATE, not tested after the wait,
         * and that is the only place it works: a signal's wake is spurious as
         * far as "there is data" goes, so a wait_event() that did not know
         * about it would simply re-park and the check after the loop would
         * never be reached. It is deliberately lock-free for this -- it is
         * evaluated under the waitqueue's own lock, and taking g_sig_lock there
         * would invert the lock order this kernel is built on. */
        wait_event(&p->wq, p->count > 0 || p->writers == 0 || ksig_interrupted());
        if (p->count == 0 && p->writers != 0 && ksig_interrupted())
            return SIG_E_INTR;
    }
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
        if (p->readers == 0) {
            /* SIGPIPE. The condition was already detected here -- the pipe has
             * refcounted its readers since M18 -- and all that could be done
             * with it was to return an error the caller was free to ignore.
             * That is why a shell pipeline whose head keeps writing after the
             * tail exits never died: `yes | head -1` ran forever.
             *
             * Its default action is terminate, so posting it is what makes the
             * writer stop; a program that has asked to handle it (or ignored
             * it, as every server does) still gets its -1/EPIPE here. Posted
             * before the return so that delivery happens at this syscall's own
             * exit to ring 3, not at some later one. */
            ksig_post_current(LOGIT_SIGPIPE);
            return n > 0 ? n : -1;                           /* broken pipe */
        }
        if (p->count == PIPE_SZ) {
            if (f->flags & O_NONBLOCK) return n > 0 ? n : EAGAIN_RC;
            wait_event(&p->wq, p->count < PIPE_SZ || p->readers == 0 || ksig_interrupted());
            if (p->count == PIPE_SZ && p->readers != 0 && ksig_interrupted())
                return n > 0 ? n : SIG_E_INTR;
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

/* ==========================================================================
 * F_EVENT: eventfd and timerfd.
 *
 * WHY THEY ARE ONE TYPE. An eventfd is a 64-bit counter, a wait queue, and a
 * rule for how read() drains it. A timerfd is a 64-bit counter, a wait queue,
 * and a rule for how read() drains it. The ONLY difference is what increments
 * the counter -- another thread's write(), or the timer tick -- so two types
 * would have been two copies of the read side, the blocking side and the poll
 * side in order to distinguish two producers. `is_timer` is that difference,
 * and it appears in exactly three places below.
 *
 * WHY EVENTFD AT ALL, when this kernel has had pipes since M18. A poll loop
 * that can only be woken by its own descriptors cannot be told to stop and
 * cannot be handed work. The portable answer is the self-pipe trick, which
 * works here -- and costs TWO of the 512 open-file descriptions plus an 8 KiB
 * ring buffer (PIPE_SZ above) to move one bit. This is one description and
 * eight bytes. It also cannot desynchronise: a self-pipe's byte count and the
 * work queue it stands for are two numbers that have to be kept equal by hand.
 *
 * THE COUNTER SATURATES rather than blocking the writer. Linux blocks a write
 * that would overflow; doing that here would mean a writer wait queue and a
 * second blocking path, to protect against a case that needs 2^64 wakeups
 * nobody read. It saturates at UINT64_MAX-1 and that is stated in the ABI.
 * ======================================================================== */
struct eventobj {
    struct waitq wq;
    uint64_t     val;         /* eventfd counter / timerfd expirations pending */
    int          semflag;     /* EFD_SEMAPHORE: read() takes 1, not all */
    int          is_timer;
    uint64_t     deadline;    /* timer_ticks() value of the next expiry; 0 = disarmed */
    uint64_t     interval;    /* ticks between expiries; 0 = one-shot */
};

/* THE ARMED-TIMER REGISTRY, and why it is a fixed array rather than a walk of
 * files[].
 *
 * file_timerfd_tick() runs inside the 100 Hz timer interrupt. Walking all 512
 * open-file descriptions there would put 51,200 loads a second on the IRQ path
 * to serve a facility that is usually unused, and it would do it while holding
 * whatever the interrupt entry holds. This array is scanned instead: it is as
 * long as the number of timerfds that can exist, and the scan is skipped
 * entirely when the count is zero, which is the state of every machine that is
 * not using timerfd.
 *
 * SIXTEEN. It is a limit and it is refused out loud (file_timerfd() returns
 * NULL, SYS_TIMERFD answers POLL_E_NOMEM) rather than silently creating a timer
 * that never fires -- which is what an unregistered timerfd would be, and which
 * would look exactly like a poll() bug. */
#define TIMERFD_MAX 16
static struct eventobj *g_timers[TIMERFD_MAX];
static int              g_ntimers;
static spinlock_t       g_timer_lock = SPINLOCK_INIT;

static int timer_register(struct eventobj *e)
{
    uint64_t f = spin_lock_irqsave(&g_timer_lock);
    int ok = 0;
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (!g_timers[i]) { g_timers[i] = e; g_ntimers++; ok = 1; break; }
    spin_unlock_irqrestore(&g_timer_lock, f);
    return ok;
}

static void timer_unregister(struct eventobj *e)
{
    uint64_t f = spin_lock_irqsave(&g_timer_lock);
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (g_timers[i] == e) { g_timers[i] = 0; if (g_ntimers) g_ntimers--; break; }
    spin_unlock_irqrestore(&g_timer_lock, f);
}

/* Called from the 100 Hz timer interrupt (ksig_tick). Interrupt context, so it
 * must never block and never allocate: it does neither -- waitq_wake_all() is
 * explicitly interrupt-safe (c/kernel/core/wait.h rule 3) and the registry is
 * static.
 *
 * The expiry count is computed rather than incremented by one, so a machine
 * that spent 300 ms in a device poll reports THREE expirations of a 100 ms
 * timer instead of quietly losing two. That is the whole reason read() returns
 * a count instead of a byte. */
void file_timerfd_tick(void)
{
    if (!g_ntimers) return;                     /* the common case: no timerfds */
    uint64_t now = timer_ticks();
    struct eventobj *fire[TIMERFD_MAX];
    int nf = 0;

    uint64_t f = spin_lock_irqsave(&g_timer_lock);
    for (int i = 0; i < TIMERFD_MAX; i++) {
        struct eventobj *e = g_timers[i];
        if (!e || !e->deadline) continue;
        if ((int64_t)(now - e->deadline) < 0) continue;
        if (e->interval) {
            uint64_t n = 1 + (now - e->deadline) / e->interval;
            e->val     += n;
            e->deadline += n * e->interval;
        } else {
            e->val++;
            e->deadline = 0;                    /* one-shot: disarmed by firing */
        }
        if (nf < TIMERFD_MAX) fire[nf++] = e;
    }
    spin_unlock_irqrestore(&g_timer_lock, f);

    /* The wakes happen OUTSIDE g_timer_lock. waitq_wake_all takes the queue's
     * own lock and, for a poll registration, the poller's lock inside it -- so
     * doing it under g_timer_lock would add a third lock to that chain from
     * interrupt context for no reason. Lock order stays q->lock -> xlock. */
    for (int i = 0; i < nf; i++) waitq_wake_all(&fire[i]->wq);
}

static long evt_read(struct file *f, void *vbuf, long len)
{
    struct eventobj *e = (struct eventobj *)f->backing;
    /* EXACTLY 8 BYTES, refused rather than truncated. Linux's contract, and
     * every ported program assumes it; a short read that "worked" would hand
     * back half a counter and leave the rest to be misread as the next one. */
    if (len < 8) return -1;
    if (!(f->flags & O_NONBLOCK)) {
        /* ksig_interrupted() in the PREDICATE for the reason pipe_read gives
         * above: a signal's wake says nothing about the counter, so a
         * wait_event that did not know about it would simply re-park. */
        wait_event(&e->wq, e->val > 0 || ksig_interrupted());
        if (e->val == 0 && ksig_interrupted()) return SIG_E_INTR;
    }
    if (e->val == 0) return EAGAIN_RC;
    uint64_t out;
    if (e->semflag && !e->is_timer) { out = 1; e->val--; }
    else                            { out = e->val; e->val = 0; }
    memcpy(vbuf, &out, sizeof out);
    /* Wake the queue: with EFD_SEMAPHORE several readers share one counter, and
     * a reader that took one of five has left four for somebody else. */
    if (e->val) waitq_wake_all(&e->wq);
    return 8;
}

static long evt_write(struct file *f, const void *vbuf, long len)
{
    struct eventobj *e = (struct eventobj *)f->backing;
    /* A timerfd is written by the clock and by nothing else. Refused rather
     * than accepted-and-ignored: a program that thinks it armed a timer by
     * writing to it would wait forever with no error to look at. */
    if (e->is_timer) return -1;
    if (len < 8) return -1;
    uint64_t add;
    memcpy(&add, vbuf, sizeof add);
    /* A write of 0 wakes nobody, deliberately -- see the ABI note. It is not an
     * error, so a caller looping over a buffer of counts does not have to
     * special-case it. */
    if (add == 0) return 8;
    if (e->val > 0xfffffffffffffffeULL - add) e->val = 0xfffffffffffffffeULL;
    else                                      e->val += add;
    waitq_wake_all(&e->wq);
    return 8;
}

static short evt_poll(struct file *f, struct poll_table *pt)
{
    struct eventobj *e = (struct eventobj *)f->backing;
    poll_wait(pt, &e->wq);                      /* FIRST -- see kpoll.h */
    short m = 0;
    if (e->val > 0) m |= LPOLLIN;
    /* A timerfd is never writable: evt_write refuses it, so claiming LPOLLOUT
     * would be promising that a call which always fails will not block. */
    if (!e->is_timer) m |= LPOLLOUT;
    return m;
}

static struct file *evt_alloc(int is_timer, uint64_t initval, int flags)
{
    struct eventobj *e = (struct eventobj *)kmalloc(sizeof *e);
    if (!e) return 0;
    waitq_init(&e->wq);
    e->val      = initval;
    e->semflag  = (flags & EFD_SEMAPHORE) ? 1 : 0;
    e->is_timer = is_timer;
    e->deadline = 0;
    e->interval = 0;
    if (is_timer && !timer_register(e)) { kfree(e); return 0; }
    struct file *f = file_alloc();
    if (!f) { if (is_timer) timer_unregister(e); kfree(e); return 0; }
    f->type    = F_EVENT;
    f->flags   = flags & O_NONBLOCK;
    f->amode   = O_RDWR;
    f->backing = e;
    return f;
}

struct file *file_eventfd(uint64_t initval, int flags) { return evt_alloc(0, initval, flags); }
struct file *file_timerfd(int flags)                   { return evt_alloc(1, 0, flags); }

/* Arm, re-arm or disarm. value_ms == 0 disarms and CLEARS the pending count:
 * a program that cancels a timer and then reads the fd should not receive an
 * expiry that happened before the cancel.
 *
 * The conversion rounds UP and adds a tick, exactly as wait_deadline_ms() does
 * and for the same reason: a 10 ms timer must never fire early, which a
 * truncating conversion plus an about-to-arrive tick would make it do. The
 * consequence, stated because it is visible: a 1 ms timer fires on the next
 * tick, i.e. after up to 10 ms, and its "interval" is one tick. */
int file_timerfd_arm(struct file *f, long value_ms, long interval_ms)
{
    if (!f || f->type != F_EVENT) return -1;
    struct eventobj *e = (struct eventobj *)f->backing;
    if (!e || !e->is_timer) return -1;
    if (value_ms < 0 || interval_ms < 0) return -1;

    uint64_t fl = spin_lock_irqsave(&g_timer_lock);
    if (value_ms == 0) {
        e->deadline = 0;
        e->interval = 0;
        e->val      = 0;
    } else {
        uint64_t vt = ((uint64_t)value_ms * TIMER_HZ + 999) / 1000;
        uint64_t it = ((uint64_t)interval_ms * TIMER_HZ + 999) / 1000;
        if (interval_ms > 0 && it == 0) it = 1;   /* sub-tick interval = one tick */
        e->deadline = timer_ticks() + (vt ? vt : 1) + 1;
        e->interval = it;
        e->val      = 0;
    }
    spin_unlock_irqrestore(&g_timer_lock, fl);
    return 0;
}

/* ==========================================================================
 * file_poll -- the type switch, and the ONE place it is allowed to be.
 *
 * kpoll.c has no switch over fd types on purpose (its header argues why). This
 * is the switch, and it is here because this is the file that already knows
 * what each type's state means -- the pipe's counters are three lines up, the
 * console's queue belongs to ksignal.c, and the socket's belongs to a file this
 * line of work does not own.
 *
 * READ p->count AND p->readers/p->writers WITHOUT A LOCK, exactly as
 * pipe_read() and pipe_write() do fifty lines above. That is not an oversight
 * being copied: those counters are protected by the BKL (every syscall holds
 * it) plus g_file_lock for the close accounting, and taking g_file_lock here
 * would introduce a lock this file takes INSIDE a poll table's registration
 * window -- a new edge in the lock graph to serve a read that the BKL already
 * serialises. When the BKL goes, all four sites move together.
 * ======================================================================== */
short file_poll(struct file *f, struct poll_table *pt)
{
    if (!f) return LPOLLNVAL;
    switch (f->type) {
    case F_VFS:
        /* Always ready, and it is not an approximation: the whole file is in a
         * kernel buffer, so neither read() nor write() can block or have "not
         * yet" to report. Registering on nothing is correct here -- there is no
         * state change to wait for.
         *
         * A GENERATED file (`live`, /proc) has no buffer and the sentence above
         * is not its reason, but the answer is the same and for a stronger one:
         * its bytes are computed synchronously inside the read, so there is
         * nothing it could ever wait for either. */
        return LPOLLIN | LPOLLOUT;

    case F_PIPE: {
        struct pipe *p = (struct pipe *)f->backing;
        if (!p) return LPOLLNVAL;
        poll_wait(pt, &p->wq);                  /* FIRST -- see kpoll.h */
        short m = 0;
        if (f->is_write) {
            /* No readers left: a write() would post SIGPIPE and fail. That is
             * LPOLLERR, not LPOLLOUT -- reporting writable would send the
             * caller into a write that kills it. */
            if (p->readers == 0) m |= LPOLLERR;
            else if (p->count < PIPE_SZ) m |= LPOLLOUT;
        } else {
            if (p->count > 0)  m |= LPOLLIN;
            /* LPOLLHUP without LPOLLIN once the ring is drained, which is
             * Linux's answer and the one every event loop is written against:
             * revents is non-zero, so the fd is returned, and the read() that
             * follows gets 0 = EOF. A poll loop that watched only LPOLLIN would
             * otherwise spin on a dead pipe forever. */
            if (p->writers == 0) m |= LPOLLHUP;
        }
        return m;
    }

    case F_TTY:
        /* The console queue the 100 Hz tick drains the UART into. There is no
         * serial receive interrupt on this machine, so a byte can sit in the
         * UART for up to one tick before it is visible here -- 10 ms of
         * latency, the same 10 ms a blocking read of the console already had.
         * The alternative, peeking at the port directly, is NOT available:
         * serial_getc() is destructive and would eat the byte the subsequent
         * read() needs, and ksig_tick's ^C handling would never see it. */
        poll_wait(pt, ksig_tty_waitq());
        return (short)(LPOLLOUT | (ksig_tty_avail() ? LPOLLIN : 0));

    case F_EVENT:
        return evt_poll(f, pt);

    case F_SOCK:
        /* LPOLLNVAL, not 0, while the hook does not exist. "This kernel cannot
         * answer for that fd" and "that fd will never be ready" are different
         * findings: the first returns immediately and tells the caller, the
         * second parks it forever. mini-libc's old poll() made the second
         * choice for pipes and said so in its own header; this does not repeat
         * it. The one function that closes this is named in kpoll.h. */
        return lsock_file_poll ? lsock_file_poll(f, pt) : LPOLLNVAL;
    }
    return LPOLLNVAL;
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
                /* Cleared here, in the one place a slot is claimed, for the
                 * same reason `dirty` and `backing` are: a recycled
                 * description that inherited live = 1 would serve an ordinary
                 * disk file by re-reading its path on every read -- which
                 * happens to work, right up until the file is written or
                 * deleted underneath it. */
                f->live = 0;
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

/* Which gate refused an open, said once per gate per boot.
 *
 * `open` returning -1 reaches userland as a single bit, and every caller then
 * prints its own guess ("sh: cannot open output"). The two permission gates
 * below and the two allocation failures are four different problems with one
 * symptom, and telling them apart from the outside is impossible -- which is
 * how a permission refusal came to be read as a resource leak. One line each,
 * with the pid and the credentials in force, is enough to separate them and
 * cheap enough to leave in: it fires only on a path that was ABOUT to fail. */
/* The VFS refused a create, and this is the credential it actually used --
 * see the comment at the call site in vfs.c for why it is reported from there
 * rather than looked up again here. */
void vfs_note_refusal(const char *dir, unsigned mode, unsigned ouid, unsigned ogid,
                      unsigned cuid, unsigned cgid);
void vfs_note_refusal(const char *dir, unsigned mode, unsigned ouid, unsigned ogid,
                      unsigned cuid, unsigned cgid)
{
    static int said;
    if (said) return;
    said = 1;
    /* Octal by hand: this kprintf has no %o, and a mode in decimal is the one
     * number in the line nobody can read -- 420 is 0644 and looks like neither.
     * The first version of this printed "%o" literally and shifted every
     * argument after it by one, which made the owner read as the mode. */
    kprintf("[vfs] create refused in %s: mode 0%u%u%u owner %u:%u, asked by "
            "%u:%u -- said once per boot\n", dir,
            (mode >> 6) & 7, (mode >> 3) & 7, mode & 7,
            ouid, ogid, cuid, cgid);
}

static void open_refused(const char *path, const char *gate, int flags)
{
    static int said_access, said_create;
    int *once = gate[0] == 'a' ? &said_access : &said_create;
    if (*once) return;
    *once = 1;
    struct vcred c;
    int pid = vfs_cred_pid();
    vfs_cred_current(&c);
    kprintf("[file] open refused by %s: %s (flags 0x%x) pid %d uid %u gid %u "
            "-- said once per boot\n", gate, path, flags, pid, c.uid, c.gid);
}

/* Is `path` a file /proc GENERATES rather than stores? Weak, for the reason
 * vfs.c declares kdiag weakly: file.c is linked into host harnesses that have
 * no filesystem layer at all, and a hard reference would make every one of
 * them fail to link over a facility they never mount. Absent, this is 0 and
 * every file behaves exactly as it did before /proc existed. */
int procfs_owns_path(const char *abs) __attribute__((weak));
static int is_generated(const char *p) { return procfs_owns_path ? procfs_owns_path(p) : 0; }
/* KNOWN LIMIT, stated rather than found later: this asks about the path the
 * SYSCALL LAYER handed us -- absolute and with "." and ".." collapsed by
 * proc_resolve, but with symlinks NOT expanded. A symlink pointing into /proc
 * therefore opens as an ordinary file and is slurped, i.e. read as a snapshot
 * taken at open. Closing it means a vfs_resolve() on every open of every file
 * on the machine to serve a case nothing in this tree creates; the trade is
 * recorded here instead of paid. */

struct file *file_open_vfs(const char *path, int flags)
{
    int sz = vfs_size(path);
    int exists = (sz >= 0);
    if (!exists && !(flags & O_CREAT)) return 0;

    /* An existing file is checked against its own mode; a file about to be
     * created is checked against the directory that would gain the name --
     * there is nothing else to check, and skipping it is the hole where an
     * unprivileged process creates files in a root-owned directory. */
    if (exists) { if (vfs_access(path, open_want(flags)) < 0) { open_refused(path, "access", flags); return 0; } }
    else        { if (vfs_may_create(path) < 0) { open_refused(path, "may_create", flags); return 0; } }

    struct file *f = file_alloc();
    if (!f) return 0;
    f->type = F_VFS; f->flags = flags; f->off = 0; f->dirty = 0;
    f->amode = flags & 3;
    f->live = 0;
    scopy(f->path, path, sizeof f->path);

    /* A GENERATED file is not read here. This is the whole of what makes
     * /proc live rather than a snapshot: the bytes are fetched by file_read()
     * through vfs_pread() at the moment the reader asks for them.
     *
     * It also has to come BEFORE the O_CREAT/O_TRUNC branch below, which sets
     * dirty = 1 so an empty new file still materialises. A live description
     * has no buffer to write back, and a dirty one would try -- vfs_write on
     * /proc fails (the backend publishes no write op at all, deliberately),
     * so it would be a refusal reported at close(), where nobody is looking. */
    if (is_generated(path)) {
        f->live = 1;
        f->backing = 0; f->cap = 0; f->size = 0; f->dirty = 0;
        return f;
    }

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
        /* GENERATED (/proc): ask now. `f->size` is meaningless here -- nothing
         * was ever slurped -- so end of file is what vfs_pread reports (0), and
         * a negative is passed on as -1 rather than turned into EOF. That
         * distinction is the lifetime rule reaching userland: a read of a file
         * whose process has exited must be a FAILURE and not an empty file.
         * See c/fs/procfs.h, point 2. */
        if (f->live) {
            if (!f->path[0]) return -1;
            int n = vfs_pread(f->path, buf, (int)(len > 0x7ffffff0 ? 0x7ffffff0 : len), f->off);
            if (n < 0) return -1;
            f->off += n;
            return n;
        }
        long avail = f->size - f->off;
        if (avail <= 0) return 0;                 /* EOF */
        long n = len < avail ? len : avail;
        memcpy(buf, (char *)f->backing + f->off, (size_t)n);
        f->off += n;
        return n;
    }
    if (f->type == F_PIPE)  return pipe_read(f, buf, len);
    if (f->type == F_TTY)   return tty_read(f, buf, len);
    if (f->type == F_EVENT) return evt_read(f, buf, len);
    if (f->type == F_SOCK)  return lsock_file_read ? lsock_file_read(f, buf, len) : -1;
    return -1;
}

long file_write(struct file *f, const void *buf, long len)
{
    if (!f || len < 0) return -1;
    if (f->type == F_VFS) {
        if (f->amode == O_RDONLY) return -1;
        /* A generated file has no buffer to write into and no backend write op
         * to flush to. Refused HERE rather than at close, where the failure
         * would be reported to nobody. */
        if (f->live) return -1;
        if (f->flags & O_APPEND) f->off = f->size;
        if (f->off > (long)0x7fffffffffffffffL - len) return -1;   /* off+len would wrap negative */
        if (vfs_ensure_cap(f, f->off + len) < 0) return -1;
        memcpy((char *)f->backing + f->off, buf, (size_t)len);
        f->off += len;
        if (f->off > f->size) f->size = f->off;
        f->dirty = 1;
        return len;
    }
    if (f->type == F_PIPE)  return pipe_write(f, buf, len);
    if (f->type == F_TTY)   return tty_write(f, buf, len);
    if (f->type == F_EVENT) return evt_write(f, buf, len);
    if (f->type == F_SOCK)  return lsock_file_write ? lsock_file_write(f, buf, len) : -1;
    return -1;
}

long file_lseek(struct file *f, long off, int whence)
{
    if (!f || f->type != F_VFS) return -1;
    /* SEEK_END on a generated file asks the length NOW, because `f->size` is
     * 0 for one and always will be. It is still a length that was true at some
     * instant and may not be at the next read -- which is a property of the
     * file, not a defect here, and the reason nothing in /proc is meant to be
     * seeked to from the end. */
    long endlen = f->size;
    if (f->live && whence == SEEK_END) {
        int n = f->path[0] ? vfs_size(f->path) : -1;
        endlen = n > 0 ? n : 0;
    }
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->off
              : whence == SEEK_END ? endlen : -1;
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
    } else if (type == F_EVENT) {
        struct eventobj *e = (struct eventobj *)backing;
        if (e) {
            /* Off the tick registry FIRST, then wake, then free. The order is
             * the whole safety argument: after timer_unregister() the interrupt
             * can no longer reach this object, and the wake releases anybody
             * parked in evt_read() so that no thread is inside it when the
             * memory goes. A blocked reader holds an fd reference of its own,
             * so refcount cannot have reached 0 while one exists -- the wake is
             * the belt to that braces, and it costs one uncontended lock on a
             * path that runs once per descriptor. */
            timer_unregister(e);
            waitq_wake_all(&e->wq);
            kfree(e);
        }
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
