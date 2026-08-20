/* POSIX threads over M30 (c/kernel/sched/uthread.c). See <pthread.h> for what
 * is real, what is refused and why; this file is the mechanism.
 *
 * THE ONE DESIGN DECISION EVERYTHING ELSE FOLLOWS: every blocking primitive
 * here -- mutex, condition variable, semaphore, once -- is built on SYS_FUTEX,
 * so the UNCONTENDED path is one atomic compare-exchange in ring 3 and no
 * syscall at all. On Linux that is a performance choice. Here it is closer to a
 * correctness-of-design one: this kernel has a big lock, and every syscall that
 * is not on the BKL-free list serialises the whole machine for its duration.
 * A mutex whose fast path entered the kernel would turn every `with lock:` in
 * a Python program into a global stop-the-world point.
 *
 * FOUR THINGS LIVE IN ONE mmap PER THREAD, deliberately:
 *
 *     base          base+guard                                 base+len
 *     [ PROT_NONE | stack, grows down ... | TLS image | TCB    ]
 *       ^guard                             ^tls_lo    ^tp = %fs base
 *
 * (it was three until the guard page landed -- see pthread_create for why the
 * guard is an mprotect INSIDE this mapping rather than a second one beside it)
 *
 * so a thread costs exactly one mapping, needs no malloc to start (which
 * matters: malloc's own lock is one of the things being bootstrapped), and its
 * control block cannot outlive its stack -- the kernel unmaps the whole region
 * when the thread ends. The return value does NOT live here, which is what
 * makes that safe: pthread_join gets it from the kernel, which held a copy the
 * moment the thread called SYS_THREAD_EXIT.
 */

#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* pthread_entry.asm cannot include this header, so it writes 112 out and this
 * is what stops the two drifting. Same shape crt0_cli.asm uses for SYS_EXIT. */
_Static_assert(SYS_SET_TLS == 112,
               "pthread_entry.asm has SYS_SET_TLS baked in as 112");

/* ---------------------------------------------------------------------------
 * The futex, and the two atomics everything is built from.
 * ------------------------------------------------------------------------- */

static int futex_wait(volatile int *addr, int val, unsigned timeout_ms)
{
    /* arg b packs val in the low 32 bits and the timeout in the high 32 --
     * include/abi/logit_calls.abi is the source for that layout. */
    long b = ((long)(unsigned)val) | ((long)timeout_ms << 32);
    return (int)sys(SYS_FUTEX, (long)addr, b, FUTEX_WAIT);
}

static int futex_wake(volatile int *addr, int n)
{
    return (int)sys(SYS_FUTEX, (long)addr, (long)(unsigned)n, FUTEX_WAKE);
}

static inline int cas(volatile int *p, int expect, int want)
{
    __atomic_compare_exchange_n(p, &expect, want, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expect;                       /* the value that was there */
}
static inline int xchg(volatile int *p, int v)
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }

/* ---------------------------------------------------------------------------
 * Thread-local storage.
 *
 * The linker publishes the PT_TLS bounds (see c/apps/libc/logit_tls.ld for why
 * they cannot be discovered any other way here). WEAK, so a program linked
 * WITHOUT that script still links: the symbols resolve to 0, tls_size() reads
 * 0, and the sentinel check below catches it and says so rather than letting
 * `__thread` writes land inside the control block.
 * ------------------------------------------------------------------------- */

extern char __logit_tls_start[] __attribute__((__weak__));
extern char __logit_tdata_end[] __attribute__((__weak__));
extern char __logit_tls_end[]   __attribute__((__weak__));

/* WHAT SITS AT THE THREAD POINTER, and why libc's own state is not in it.
 *
 * The x86-64 psABI's variant-II layout puts a self-pointer at %fs:0 and a dtv
 * slot at %fs:8, with thread-local data growing DOWNWARD from the thread
 * pointer. c/kernel/exec/elf.c's loader builds exactly that for a program's
 * main thread, from PT_TLS, and reserves TP_HEAD bytes above the thread pointer
 * for a control block -- so the shape is not this file's to choose, it is a
 * shape this file has to MATCH.
 *
 * libc's per-thread state does not fit in what the loader reserves and should
 * not try to: it is 64 key slots plus bookkeeping, and growing it would mean
 * growing a constant in a kernel file every time. So the thread pointer carries
 * a POINTER to it, at %fs:16 -- the first slot after self and dtv, inside the
 * area the loader already zeroes. One extra indirection on pthread_getspecific,
 * and the two layouts stop being coupled.
 *
 * The consequence that makes this worth it: a program whose main thread got its
 * thread pointer from the LOADER keeps it. Installing a second one would
 * discard whatever `__thread` values main() had already written, at whatever
 * moment the program first touched pthreads -- which is exactly the kind of bug
 * that gets blamed on the program. */
#define TP_HEAD 256              /* must match `tcb` in c/kernel/exec/elf.c */

struct tcb;

struct tp_head {
    void       *self;            /* %fs:0  -- the psABI self-pointer */
    void       *dtv;             /* %fs:8  -- nothing dynamic here */
    struct tcb *ext;             /* %fs:16 -- libc's block, below */
};

struct tcb {
    unsigned long tid;
    void        *(*start)(void *);
    void         *arg;
    void         *stack_base;    /* the mmap, for pthread_getattr_np */
    unsigned long stack_len;
    unsigned long guard_len;     /* the PROT_NONE bytes at stack_base, so
                                  * pthread_getattr_np can report the usable
                                  * stack rather than the whole reservation --
                                  * a caller that computes "how much room is
                                  * left" from the wrong one of those two walks
                                  * into the guard believing it is stack */
    void         *keys[PTHREAD_KEYS_MAX];
};

_Static_assert(sizeof(struct tp_head) <= TP_HEAD,
               "the psABI head no longer fits the loader's reserved TCB area");

static inline struct tcb *self_tcb(void)
{
    struct tcb *t;
    __asm__ volatile ("mov %%fs:16, %0" : "=r"(t));
    return t;
}

static inline uintptr_t self_tp(void)
{
    uintptr_t tp;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

/* A sentinel `__thread` object owned by this file. Its job is not to hold a
 * value: it is to have an ADDRESS, so that after a TLS block is installed the
 * runtime can check that the compiler's idea of where thread-locals live and
 * this file's idea of where it put them are the same. Without the linker script
 * they are not, and the difference is silent -- writes land in the TCB. */
static __thread unsigned long tls_sentinel;

static size_t tls_size(void)
{
    if (!__logit_tls_start || !__logit_tls_end) return 0;
    return (size_t)(__logit_tls_end - __logit_tls_start);
}
static size_t tls_initsize(void)
{
    if (!__logit_tls_start || !__logit_tdata_end) return 0;
    return (size_t)(__logit_tdata_end - __logit_tls_start);
}

/* Lay [TLS image][psABI head][libc block] out at the top of [mem, mem+len) and
 * return the thread pointer. The thread pointer is the TOP of the TLS image,
 * which is what the negative displacements the compiler emitted are relative
 * to; the ALIGN(64) in the linker script is what makes "top of the image" equal
 * "image + size" without having to know PT_TLS's p_align. Everything below the
 * image is the thread's stack. Returns 0 if the region cannot hold it. */
static uintptr_t tls_layout(void *mem, size_t len, void **stack_top_out,
                            struct tcb **tcb_out)
{
    uintptr_t hi  = (uintptr_t)mem + len;
    uintptr_t ext = (hi - sizeof(struct tcb)) & ~(uintptr_t)63;
    uintptr_t tp  = (ext - TP_HEAD) & ~(uintptr_t)63;
    size_t    tsz = tls_size();
    uintptr_t lo;

    if (ext < (uintptr_t)mem || tp < (uintptr_t)mem || tsz > tp) return 0;
    lo = tp - tsz;
    if (lo <= (uintptr_t)mem) return 0;

    if (tsz) {
        memcpy((void *)lo, __logit_tls_start, tls_initsize());
        memset((void *)(lo + tls_initsize()), 0, tsz - tls_initsize());
    }
    memset((void *)tp, 0, TP_HEAD);
    memset((void *)ext, 0, sizeof(struct tcb));
    {
        struct tp_head *h = (struct tp_head *)tp;
        h->self = (void *)tp;
        h->dtv  = 0;
        h->ext  = (struct tcb *)ext;
    }
    if (stack_top_out) *stack_top_out = (void *)(lo & ~(uintptr_t)15);
    if (tcb_out)       *tcb_out = (struct tcb *)ext;
    return tp;
}

static int  tls_ready;                     /* this thread's libc block is reachable */
static char main_tls[4096 + TP_HEAD + sizeof(struct tcb) + 64];
static struct tcb main_ext;

/* Make the CALLING (main) thread's libc control block reachable through %fs:16.
 *
 * TWO CASES, and asking which one applies is the whole reason SYS_SET_TLS has a
 * query:
 *
 *   THE LOADER ALREADY INSTALLED A THREAD POINTER. c/kernel/exec/elf.c builds
 *   one from the program's PT_TLS and exec.c installs it, so a program launched
 *   through execve arrives with `__thread` ALREADY WORKING. All this has to do
 *   then is hang its own block off %fs:16, inside the area the loader reserved
 *   and zeroed. Installing a SECOND thread pointer here would throw away the
 *   loader's block along with anything main() had already written into it --
 *   and would do it at whatever moment the program first touched pthreads.
 *
 *   THERE IS NO THREAD POINTER. A program with no PT_TLS, or one started down a
 *   path that does not go through the loader's TLS pass, arrives with %fs at 0,
 *   so libc lays a block out itself and installs it.
 *
 * Out of a STATIC buffer rather than malloc in the second case, and that is not
 * squeamishness: the malloc lock below is a futex whose slow path is in this
 * file, so a bootstrap that allocated would be one edit away from a cycle.
 *
 * Not thread-safe, and does not need to be: it runs before any thread exists.
 * Every entry point that can be the FIRST thing a program calls goes through
 * it, so there is no order in which a thread can be created before it has run. */
static int tls_init_main(void)
{
    if (tls_ready) return 0;

    long cur = sys(SYS_SET_TLS, -1, 0, 0);        /* what is installed right now? */
    if (cur > 0) {
        ((struct tp_head *)(uintptr_t)cur)->ext = &main_ext;   /* adopt it */
    } else {
        if (tls_size() + TP_HEAD + sizeof(struct tcb) + 64 > sizeof main_tls) {
            static const char msg[] =
                "libc: this program's thread-local storage exceeds the built-in "
                "main-thread block; raise main_tls in c/apps/libc/src/pthread.c\n";
            write(2, msg, sizeof msg - 1);
            return -1;
        }
        uintptr_t tp = tls_layout(main_tls, sizeof main_tls, NULL, NULL);
        if (!tp) return -1;
        if (sys(SYS_SET_TLS, (long)tp, 0, 0) != 0) return -1;
    }
    tls_ready = 1;
    self_tcb()->tid = (unsigned long)sys(SYS_THREAD_SELF, 0, 0, 0);

    /* THE CHECK THAT THE TLS LAYOUT IS REAL. `tls_sentinel` is a genuine
     * `__thread` object, so its address is computed by the compiler from %fs
     * and a displacement the LINKER chose. Asking whether it lands below the
     * thread pointer, and not implausibly far below it, tests whichever
     * mechanism produced the block: the loader's layout if the loader made it,
     * or the linker script plus this file's arithmetic if libc did.
     *
     * With neither -- no PT_TLS block from the loader AND no linker script --
     * tls_size() is 0, the displacement is still negative, and the address
     * lands below the thread pointer in nobody's memory. Caught here, before
     * anything writes through it.
     *
     * Reported on stderr and NOT made fatal: a program that never touches a
     * `__thread` variable is perfectly usable either way, and killing it would
     * be a worse trade than telling it. Everything else in this file lives in
     * the block at %fs:16 and works regardless. */
    {
        uintptr_t p = (uintptr_t)&tls_sentinel;
        uintptr_t tp = self_tp();
        if (!(p < tp && tp - p <= 0x100000)) {
            static const char msg[] =
                "libc: __thread is NOT usable -- link with -T c/apps/libc/logit_tls.ld "
                "(pthread_getspecific and the rest of pthreads are unaffected)\n";
            write(2, msg, sizeof msg - 1);
        } else {
            tls_sentinel = 0x10517;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * The internal lock other mini-libc TUs use (malloc, stdio).
 *
 * Deliberately NOT a pthread_mutex_t: it must be usable before tls_init_main()
 * has run, so it cannot consult pthread_self(). Three-state futex, no owner
 * tracking, no recursion -- which is exactly enough for "one thread at a time
 * inside the allocator".
 * ------------------------------------------------------------------------- */

static void lock3(volatile int *v)
{
    int c = cas(v, 0, 1);
    if (c == 0) return;                       /* uncontended: no syscall */
    if (c != 2) c = xchg(v, 2);
    while (c != 0) {
        futex_wait(v, 2, 0);
        c = xchg(v, 2);
    }
}

static void unlock3(volatile int *v)
{
    if (__atomic_fetch_sub(v, 1, __ATOMIC_SEQ_CST) != 1) {
        __atomic_store_n(v, 0, __ATOMIC_SEQ_CST);
        futex_wake(v, 1);
    }
}

/* Exposed to the rest of mini-libc through libc_internal.h. */
void __libc_lock(volatile int *v)   { lock3(v); }
void __libc_unlock(volatile int *v) { unlock3(v); }

/* ---------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

static void run_key_destructors(struct tcb *t);

/* Called by pthread_entry.asm once %fs is installed. Everything above this
 * point in the thread's life ran without thread-local storage. */
void __logit_thread_body(void *arg);
void __logit_thread_body(void *arg)
{
    struct tcb *t = self_tcb();
    (void)arg;                                 /* also in the TCB; that is the copy used */
    t->tid = (unsigned long)sys(SYS_THREAD_SELF, 0, 0, 0);
    void *r = t->start(t->arg);
    pthread_exit(r);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg)
{
    extern void __logit_thread_entry(void);

    if (!th || !start) return EINVAL;
    if (tls_init_main() < 0) return ENOMEM;

    size_t want = (attr && attr->stacksize) ? attr->stacksize
                                            : (size_t)LOGIT_THREAD_STACK_DEFAULT;
    if (want < LOGIT_THREAD_STACK_MIN) want = LOGIT_THREAD_STACK_MIN;
    if (want > LOGIT_THREAD_STACK_MAX) return EINVAL;
    want = (want + 4095) & ~(size_t)4095;

    /* THE GUARD PAGE, which is at the BOTTOM of the mapping because the stack
     * grows down and the bottom is the end it runs off:
     *
     *   mem                mem+guard                                mem+len
     *   [ PROT_NONE ...... | stack, grows down ... | TLS | TCB     ]
     *                      ^ the lowest address the thread may touch
     *
     * It is one mmap and then an mprotect, NOT two mmaps and not an mmap plus
     * a munmap of the first page. Both alternatives leave the guard's address
     * range UNRESERVED, and vma_reserve places first-fit from MM_MMAP_BASE
     * upward -- so the hole under thread 1's stack is the first thing a later
     * small mapping is handed, and the guard silently becomes somebody's data.
     * mprotect keeps the range reserved and merely makes it untouchable, which
     * is the difference the whole SYS_MPROTECT call exists for. */
    size_t guard = (attr ? attr->guardsize : (size_t)LOGIT_THREAD_GUARD_DEFAULT);
    guard = (guard + 4095) & ~(size_t)4095;
    /* NOT CLAMPED to something this function thinks is reasonable. The first
     * version of this line silently reduced a guard larger than the stack to
     * one page, which is the same silent downgrade <sys/mman.h> refuses for a
     * writable file mapping: the caller asked for N bytes of boundary, was told
     * yes, and got 4096. There is no reason to clamp -- the guard is added to
     * the reservation rather than carved out of it, so a large one costs
     * address space and nothing else. An absurd one is refused OUT LOUD. */
    if (guard > LOGIT_THREAD_STACK_MAX) return EINVAL;

    /* Room for the TLS block and the control block ON TOP of what the caller
     * asked for, so a stacksize request means what it says. The guard is on top
     * of it too, for the same reason. Cheap: the kernel's mmap is a
     * RESERVATION -- pages arrive on first touch through the ordinary fault
     * path -- so the whole 8 MiB default costs address space and two page
     * tables, not 8 MiB of RAM, and the guard costs neither (it is never
     * touched, so it never has a frame at all). */
    size_t len = guard + want + tls_size() + sizeof(struct tcb) + 4096;
    len = (len + 4095) & ~(size_t)4095;

    void *mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return EAGAIN;

    /* A FAILED GUARD FAILS THE THREAD. The alternative -- carry on without it
     * -- is a thread whose caller asked for a boundary, was told yes, and does
     * not have one; the overrun then lands in whatever the allocator put below
     * it and is blamed on that. EAGAIN, because the one way this fails on a
     * working kernel is running out of VMA slots (vma.h VMA_MAXAREA), which is
     * a resource and not a mistake. */
    if (guard && mprotect(mem, guard, PROT_NONE) != 0) {
        munmap(mem, len);
        return EAGAIN;
    }

    void *stack_top = NULL;
    struct tcb *t = NULL;
    uintptr_t tp = tls_layout((char *)mem + guard, len - guard, &stack_top, &t);
    if (!tp) { munmap(mem, len); return ENOMEM; }
    t->start = start;
    t->arg   = arg;
    t->stack_base = mem;
    t->stack_len  = len;
    t->guard_len  = guard;

    struct logit_thread_spec spec;
    spec.entry      = (unsigned long)(uintptr_t)&__logit_thread_entry;
    spec.stack_top  = (unsigned long)(uintptr_t)stack_top;
    spec.stack_base = (unsigned long)(uintptr_t)mem;
    spec.stack_len  = (unsigned long)len;
    spec.tls        = (unsigned long)tp;
    spec.arg        = (unsigned long)(uintptr_t)arg;

    long tid = sys(SYS_THREAD_CREATE, (long)&spec, 0, 0);
    if (tid < 0) {
        munmap(mem, len);
        /* The kernel's stack region is freed above, not by it: it only owns the
         * region once a thread exists to hand it back. */
        return (tid == THR_E_FULL) ? EAGAIN : EINVAL;
    }
    *th = (pthread_t)tid;

    if (attr && attr->detachstate == PTHREAD_CREATE_DETACHED)
        sys(SYS_THREAD_DETACH, tid, 0, 0);
    return 0;
}

int pthread_join(pthread_t th, void **retval)
{
    unsigned long r = 0;
    long rc = sys(SYS_THREAD_JOIN, (long)th, (long)&r, 0);
    if (rc == THR_E_DEADLK) return EDEADLK;
    if (rc == THR_E_SRCH)   return ESRCH;
    if (rc < 0)             return EINVAL;
    if (retval) *retval = (void *)(uintptr_t)r;
    return 0;
}

int pthread_detach(pthread_t th)
{
    long rc = sys(SYS_THREAD_DETACH, (long)th, 0, 0);
    if (rc == THR_E_SRCH) return ESRCH;
    return rc < 0 ? EINVAL : 0;
}

void pthread_exit(void *retval)
{
    if (tls_ready) run_key_destructors(self_tcb());
    sys(SYS_THREAD_EXIT, (long)(uintptr_t)retval, 0, 0);
    for (;;) { }                          /* SYS_THREAD_EXIT does not return */
}

pthread_t pthread_self(void)
{
    if (tls_init_main() < 0) return (pthread_t)(unsigned long)sys(SYS_THREAD_SELF, 0, 0, 0);
    return (pthread_t)self_tcb()->tid;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

/* --- attributes --------------------------------------------------------- */

/* `stacksize` 0 means "the default"; `guardsize` does NOT, because 0 is a legal
 * guard size that means "none". So the default has to be written in here rather
 * than resolved later the way the stack size is -- and it is a page, matching
 * pthread_create's fallback for a NULL attr (LOGIT_THREAD_GUARD_DEFAULT), so
 * passing an initialised attr and passing NULL give the same thread. */
int pthread_attr_init(pthread_attr_t *a)
{
    if (!a) return EINVAL;
    a->stacksize = 0;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->guardsize = (size_t)LOGIT_THREAD_GUARD_DEFAULT;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }

int pthread_attr_setstacksize(pthread_attr_t *a, size_t sz)
{
    if (!a || sz < PTHREAD_STACK_MIN || sz > LOGIT_THREAD_STACK_MAX) return EINVAL;
    a->stacksize = sz;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *sz)
{
    if (!a || !sz) return EINVAL;
    *sz = a->stacksize ? a->stacksize : (size_t)LOGIT_THREAD_STACK_DEFAULT;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int st)
{ if (!a || (st != PTHREAD_CREATE_JOINABLE && st != PTHREAD_CREATE_DETACHED)) return EINVAL;
  a->detachstate = st; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *st)
{ if (!a || !st) return EINVAL; *st = a->detachstate; return 0; }

/* Scope: this kernel schedules every thread against every other on every core
 * -- there is no process-local contention scope to select. Accepting SYSTEM and
 * refusing PROCESS is the truthful answer, not a courtesy. */
int pthread_attr_setscope(pthread_attr_t *a, int scope)
{ (void)a; return scope == 0 ? 0 : ENOTSUP; }

/* THE GUARD PAGE IS REAL NOW, and this function used to be the place that said
 * it was not: it forced `guardsize` to 0 and returned ENOTSUP for any request,
 * because "adding one is a PROT_NONE page at the bottom of the mmap, which
 * needs an mprotect this kernel does not have". SYS_MPROTECT exists, mman.c
 * reaches it, and pthread_create installs the page.
 *
 * Rounded UP to a page and never down: a caller asking for 1 byte of guard is
 * asking for a boundary, and rounding that to zero would answer "yes" and give
 * none. 0 is still accepted and still means no guard -- POSIX says so, and a
 * program that has counted its own stack use is entitled to spend the VMA slot
 * on something else. */
int pthread_attr_setguardsize(pthread_attr_t *a, size_t sz)
{
    if (!a) return EINVAL;
    if (sz > LOGIT_THREAD_STACK_MAX) return EINVAL;
    a->guardsize = sz ? ((sz + 4095) & ~(size_t)4095) : 0;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *sz)
{ if (!a || !sz) return EINVAL; *sz = a->guardsize; return 0; }

int pthread_getattr_np(pthread_t th, pthread_attr_t *a)
{
    if (!a || tls_init_main() < 0) return EINVAL;
    if (th != pthread_self()) return ENOSYS;    /* only the caller's own is knowable */
    struct tcb *t = self_tcb();
    /* THE GUARD IS SUBTRACTED FROM WHAT IS REPORTED AS STACK, and it has to be:
     * pthread_getattr_np is what a program calls to work out how much room it
     * has left before recursing, and glibc's contract is that the guard is
     * outside the reported stack. Reporting the whole reservation would tell
     * the caller it has one more page than it does, and the page it would
     * spend that on is the guard -- i.e. the number would be wrong in exactly
     * the direction that makes the guard fire. */
    a->stacksize   = t->stack_len ? t->stack_len - t->guard_len
                                  : (size_t)LOGIT_THREAD_STACK_DEFAULT;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->guardsize   = t->guard_len;
    return 0;
}
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *sz)
{
    if (!a || !addr || !sz || tls_init_main() < 0) return EINVAL;
    struct tcb *t = self_tcb();
    /* Same rule as getattr_np above: `addr` is the lowest address the thread
     * may TOUCH, which is past the guard, not the base of the reservation. */
    *addr = (char *)t->stack_base + t->guard_len;
    *sz   = t->stack_len - t->guard_len;
    return (t->stack_base && *sz) ? 0 : ENOSYS;
}

/* --- mutexes ------------------------------------------------------------ */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    if (!m) return EINVAL;
    m->futex = 0; m->owner = 0; m->depth = 0;
    m->type = a ? a->type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (!m) return EINVAL;
    if (__atomic_load_n(&m->futex, __ATOMIC_SEQ_CST) != 0) return EBUSY;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (!m) return EINVAL;
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        unsigned long me = pthread_self();
        if (m->owner == me) {
            if (m->type == PTHREAD_MUTEX_RECURSIVE) { m->depth++; return 0; }
            return EDEADLK;                       /* ERRORCHECK */
        }
        lock3(&m->futex);
        m->owner = me; m->depth = 1;
        return 0;
    }
    lock3(&m->futex);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    if (!m) return EINVAL;
    if (m->type == PTHREAD_MUTEX_RECURSIVE) {
        unsigned long me = pthread_self();
        if (m->owner == me) { m->depth++; return 0; }
        if (cas(&m->futex, 0, 1) != 0) return EBUSY;
        m->owner = me; m->depth = 1;
        return 0;
    }
    if (cas(&m->futex, 0, 1) != 0) return EBUSY;
    if (m->type != PTHREAD_MUTEX_NORMAL) { m->owner = pthread_self(); m->depth = 1; }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (!m) return EINVAL;
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != pthread_self()) return EPERM;
        if (--m->depth > 0) return 0;
        m->owner = 0;
    }
    unlock3(&m->futex);
    return 0;
}

/* Absolute deadline -> milliseconds from now, clamped at 0. `clk` is the clock
 * the caller's deadline is expressed in. */
static unsigned abs_to_ms(const struct timespec *abs, int clk)
{
    struct timespec now;
    long long ms;
    if (!abs) return 0;
    if (clock_gettime(clk, &now) != 0) return 0;
    ms = ((long long)abs->tv_sec - now.tv_sec) * 1000
       + ((long long)abs->tv_nsec - now.tv_nsec) / 1000000;
    if (ms <= 0) return 1;                 /* already due: one tick, never "forever" */
    if (ms > 0x7fffffffLL) ms = 0x7fffffffLL;
    return (unsigned)ms;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs)
{
    if (!m) return EINVAL;
    if (pthread_mutex_trylock(m) == 0) return 0;
    for (;;) {
        unsigned ms = abs_to_ms(abs, CLOCK_REALTIME);
        int c = xchg(&m->futex, 2);
        if (c == 0) {
            if (m->type != PTHREAD_MUTEX_NORMAL) { m->owner = pthread_self(); m->depth = 1; }
            return 0;
        }
        if (futex_wait(&m->futex, 2, ms) == FUTEX_E_TIMEDOUT) {
            /* Try once more before giving up: the deadline and the handoff can
             * genuinely race, and reporting ETIMEDOUT on a mutex we could have
             * had is a spurious failure a caller cannot distinguish from a real
             * one. */
            if (pthread_mutex_trylock(m) == 0) return 0;
            return ETIMEDOUT;
        }
    }
}

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{ if (!a) return EINVAL; a->type = PTHREAD_MUTEX_NORMAL; a->pshared = PTHREAD_PROCESS_PRIVATE; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (!a || type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    a->type = type;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{ if (!a || !type) return EINVAL; *type = a->type; return 0; }

/* --- condition variables ------------------------------------------------ */

/* A sequence counter and a futex, which is the smallest correct condvar.
 *
 * WHAT MAKES IT CORRECT: the waiter reads `seq` while it still holds the mutex,
 * and then waits for that exact value to CHANGE. A signal that lands in the
 * window between releasing the mutex and parking has already incremented seq,
 * so the futex's own value compare fails and the wait returns immediately
 * instead of sleeping on an event that already happened. That is the same
 * argument the kernel's wait queues make (c/kernel/core/wait.h rule 2), moved
 * into userland: the predicate is read under the lock the waker must take.
 *
 * WHAT IT COSTS, stated rather than hidden: there is no requeue operation, so
 * pthread_cond_broadcast wakes every waiter and they all then contend for the
 * mutex -- the classic thundering herd. Linux avoids it with FUTEX_REQUEUE.
 * Adding that here is a third futex op in c/kernel/sched/uthread.c and no
 * change to this file's structure; it is left undone because the alternative
 * would have been to leave the herd undocumented.
 */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{ if (!c) return EINVAL; c->seq = 0; c->clock = a ? a->clock : CLOCK_REALTIME; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { return c ? 0 : EINVAL; }

static int cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, unsigned ms)
{
    unsigned seq;
    int rc, err = 0;

    if (!c || !m) return EINVAL;
    seq = __atomic_load_n(&c->seq, __ATOMIC_SEQ_CST);

    rc = pthread_mutex_unlock(m);
    if (rc) return rc;

    if (futex_wait((volatile int *)&c->seq, (int)seq, ms) == FUTEX_E_TIMEDOUT)
        err = ETIMEDOUT;

    /* POSIX: the mutex is reacquired before returning, even on timeout and even
     * on error. A caller's `finally: mutex.release()` depends on it. */
    pthread_mutex_lock(m);
    return err;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{ return cond_wait_ms(c, m, 0); }

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abs)
{
    if (!c || !m) return EINVAL;
    if (!abs) return cond_wait_ms(c, m, 0);
    return cond_wait_ms(c, m, abs_to_ms(abs, c->clock));
}

int pthread_cond_signal(pthread_cond_t *c)
{
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    futex_wake((volatile int *)&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    futex_wake((volatile int *)&c->seq, INT_MAX);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *a)
{ if (!a) return EINVAL; a->clock = CLOCK_REALTIME; a->pshared = PTHREAD_PROCESS_PRIVATE; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clk)
{
    /* Both clocks are real here (CLOCK_MONOTONIC is SYS_MONOTONIC_MS), and this
     * matters: CPython sets CONDATTR_MONOTONIC precisely so a lock timeout is
     * not affected by someone setting the wall clock backwards. */
    if (!a || (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)) return EINVAL;
    a->clock = clk;
    return 0;
}
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clk)
{ if (!a || !clk) return EINVAL; *clk = a->clock; return 0; }

/* --- pthread_once ------------------------------------------------------- */

#define ONCE_NEW 0
#define ONCE_RUN 1
#define ONCE_DONE 2

int pthread_once(pthread_once_t *once, void (*init)(void))
{
    if (!once || !init) return EINVAL;
    for (;;) {
        int s = __atomic_load_n(once, __ATOMIC_SEQ_CST);
        if (s == ONCE_DONE) return 0;
        if (s == ONCE_NEW && cas(once, ONCE_NEW, ONCE_RUN) == ONCE_NEW) {
            init();
            __atomic_store_n(once, ONCE_DONE, __ATOMIC_SEQ_CST);
            futex_wake(once, INT_MAX);
            return 0;
        }
        /* Somebody else is running it. Park on the word itself: no extra state,
         * and the store above is the wake. */
        futex_wait(once, ONCE_RUN, 0);
    }
}

/* --- thread-specific data ------------------------------------------------
 *
 * Values live in the caller's TCB, so getspecific is a %fs load and an index --
 * no lock and no syscall, which is what a per-thread lookup on a hot path has
 * to be. Only the KEY table (is this key allocated, and what destroys it) is
 * global, and it is the only thing that needs the lock.
 * ------------------------------------------------------------------------- */

static struct { int used; void (*dtor)(void *); } keys[PTHREAD_KEYS_MAX];
static volatile int keys_lock;

int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
    if (!key) return EINVAL;
    if (tls_init_main() < 0) return ENOMEM;
    lock3(&keys_lock);
    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++)
        if (!keys[i].used) {
            keys[i].used = 1; keys[i].dtor = dtor;
            unlock3(&keys_lock);
            *key = i;
            return 0;
        }
    unlock3(&keys_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    lock3(&keys_lock);
    keys[key].used = 0; keys[key].dtor = 0;
    unlock3(&keys_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX || !tls_ready) return 0;
    return self_tcb()->keys[key];
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    if (tls_init_main() < 0) return ENOMEM;
    self_tcb()->keys[key] = (void *)(uintptr_t)value;
    return 0;
}

/* POSIX: run destructors repeatedly while any value is non-NULL, up to
 * PTHREAD_DESTRUCTOR_ITERATIONS passes -- a destructor is allowed to set
 * another key, and stopping after one pass would leak it. 4 is the POSIX
 * minimum and what glibc uses. */
static void run_key_destructors(struct tcb *t)
{
    for (int pass = 0; pass < 4; pass++) {
        int any = 0;
        for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
            void *v = t->keys[i];
            void (*d)(void *);
            if (!v) continue;
            lock3(&keys_lock);
            d = keys[i].used ? keys[i].dtor : 0;
            unlock3(&keys_lock);
            t->keys[i] = 0;                /* cleared BEFORE the call, per POSIX */
            if (d) { d(v); any = 1; }
        }
        if (!any) return;
    }
}

/* --- semaphores (see <semaphore.h>) ------------------------------------- */

int sem_init(sem_t *s, int pshared, unsigned value)
{
    if (!s) { errno = EINVAL; return -1; }
    if (pshared) { errno = ENOSYS; return -1; }   /* no shared memory on this machine */
    s->value = (int)value;
    return 0;
}
int sem_destroy(sem_t *s) { if (!s) { errno = EINVAL; return -1; } return 0; }

int sem_post(sem_t *s)
{
    if (!s) { errno = EINVAL; return -1; }
    __atomic_add_fetch(&s->value, 1, __ATOMIC_SEQ_CST);
    futex_wake(&s->value, 1);
    return 0;
}

int sem_trywait(sem_t *s)
{
    if (!s) { errno = EINVAL; return -1; }
    for (;;) {
        int v = __atomic_load_n(&s->value, __ATOMIC_SEQ_CST);
        if (v <= 0) { errno = EAGAIN; return -1; }
        if (cas(&s->value, v, v - 1) == v) return 0;
    }
}

static int sem_wait_ms(sem_t *s, unsigned ms, int timed)
{
    if (!s) { errno = EINVAL; return -1; }
    for (;;) {
        int v = __atomic_load_n(&s->value, __ATOMIC_SEQ_CST);
        if (v > 0) {
            if (cas(&s->value, v, v - 1) == v) return 0;
            continue;                      /* lost the race; re-read */
        }
        if (futex_wait(&s->value, 0, timed ? (ms ? ms : 1) : 0) == FUTEX_E_TIMEDOUT) {
            if (sem_trywait(s) == 0) return 0;
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

int sem_wait(sem_t *s) { return sem_wait_ms(s, 0, 0); }

int sem_timedwait(sem_t *s, const struct timespec *abs)
{ return sem_wait_ms(s, abs_to_ms(abs, CLOCK_REALTIME), 1); }

int sem_clockwait(sem_t *s, clockid_t clk, const struct timespec *abs)
{ return sem_wait_ms(s, abs_to_ms(abs, clk), 1); }

int sem_getvalue(sem_t *s, int *v)
{
    if (!s || !v) { errno = EINVAL; return -1; }
    *v = __atomic_load_n(&s->value, __ATOMIC_SEQ_CST);
    return 0;
}

/* --- the refusals ------------------------------------------------------- */

/* Not stubbed to success, and the difference matters. A caller that gets 0 from
 * pthread_cancel believes the thread is on its way out and will stop waiting
 * for it; a caller that gets ENOSYS knows to find another way. There is no
 * signal delivery in this kernel at all (see <signal.h>), so there is nothing
 * either of these could be built on -- not a missing wrapper, a missing
 * mechanism. */
int pthread_kill(pthread_t th, int sig)          { (void)th; (void)sig; return ENOSYS; }
int pthread_cancel(pthread_t th)                 { (void)th; return ENOSYS; }
int pthread_setcancelstate(int st, int *old)     { (void)st; if (old) *old = 0; return ENOSYS; }
int pthread_setcanceltype(int ty, int *old)      { (void)ty; if (old) *old = 0; return ENOSYS; }
void pthread_testcancel(void)                    { }

/* Succeeds, and that is the honest answer rather than the generous one: it
 * manipulates a mask nothing consults, and code that blocks signals around a
 * critical section is already correct on a system that has none. Same reasoning
 * as sigprocmask in c/apps/libc/src/signal.c, which this defers to. */
int pthread_sigmask(int how, const void *set, void *oldset)
{
    extern int sigprocmask(int, const unsigned long *, unsigned long *);
    return sigprocmask(how, (const unsigned long *)set, (unsigned long *)oldset) == 0 ? 0 : errno;
}
