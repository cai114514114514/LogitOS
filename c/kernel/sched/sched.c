#include <stdint.h>
#include <stddef.h>
#include "sched.h"
#include "kheap.h"
#include "gdt.h"
#include "vmm.h"
#include "interrupts.h"   /* struct registers (fork) */
#include "percpu.h"
#include "spinlock.h"
#include "kprintf.h"
#include "pit.h"          /* timer_ticks() -- deadline sleeps */
#include "work.h"         /* work_init(): the kworker thread (deferred work) */
#include "wait_selftest.h"
#include "kbench.h"       /* kbench_start(): what the kernel's primitives cost */

#define STACK_SIZE  16384
#define KSTACK_SIZE 32768

struct thread {
    uint64_t rsp;            /* saved stack pointer (must be first field) */
    struct thread *next;     /* circular ready ring */
    struct thread *prev;     /* circular ready ring (back-link, for O(1) removal) */
    void *stack;
    uint64_t kstack_top;     /* ring-0 stack top (TSS rsp0) for ring-3 threads */
    uint64_t cr3;            /* address space (PML4 phys); kernel space if 0 set at init */
    void *data;              /* opaque per-thread payload (the app) */
    char name[32];           /* owned copy -- callers pass stack/temp buffers
                              * (execve's `nm` is a stack array; a bare pointer
                              * dangles as soon as the caller returns) */
    int id;
    int alive;
    int running;             /* 1 while owned by some core (SMP: skip in pick) */
    void (*entry)(void);     /* kernel-thread trampoline target (kthread_bootstrap) */
    int is_idle;             /* per-core idle thread (off the shared ring) */

    /* --- M27 blocking core (see sched.h for the lost-wakeup argument) --- */
    int state;               /* THREAD_READY / THREAD_BLOCKED */
    int on_ring;             /* 1 while spliced into g_ring's circular list */
    unsigned long slices;    /* times this thread was DISPATCHED. The number that
                              * makes "it sleeps now" a measurement rather than a
                              * claim: a parked thread's slices do not move. */
    struct thread *tw_next;  /* deadline list (g_timer_wait) link */
    int    on_timer;         /* 1 while linked into the deadline list */
    uint64_t deadline;       /* timer_ticks() value at which to force a wake */
    int    timed_out;        /* set by sched_timer_expire when it did the waking */

    /* --- M30 threads ---------------------------------------------------
     * fsbase: this thread's IA32_FS_BASE, i.e. where its thread-local storage
     * lives. Kernel threads carry 0, which is both correct (they have none) and
     * useful (leaving a user TLS pointer loaded while ring 0 runs is exactly the
     * kind of stale state that turns one bug into two).
     *
     * THE INVARIANT: on every core, the hardware IA32_FS_BASE equals
     * this_cpu()->current->fsbase. It is maintained at the three places a core
     * changes `current` -- schedule(), block_self() and thread_exit() -- and at
     * sched_set_fsbase(), which writes both. Because the invariant holds for
     * `prev` on entry, comparing prev->fsbase with next->fsbase is enough to
     * decide whether the MSR needs writing, and it is correct across migration:
     * a core that has never run this thread before still knows what IT has
     * loaded, which is what the comparison is about.
     *
     * all_next: EVERY live thread, in creation order, whether or not it is on
     * the run ring. The ring is not a directory -- a parked thread is UNLINKED
     * from it (see ring_unlink) -- so a thread waiting on a futex is exactly the
     * thread the ring cannot find, and waking one by id is the whole job. */
    uint64_t fsbase;
    struct thread *all_next;
};

/* IA32_FS_BASE. See the SYS_SET_TLS note in include/abi/logit_abi.h for why
 * this and not `wrfsbase`. */
#define MSR_FS_BASE 0xC0000100u

static inline void fsbase_load(uint64_t v)
{
    __asm__ volatile ("wrmsr" :: "c"(MSR_FS_BASE),
                                 "a"((uint32_t)v),
                                 "d"((uint32_t)(v >> 32)));
}

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void ring3_bootstrap(void);     /* boot/enter_user.asm */
extern void fork_ret(void);            /* boot/enter_user.asm: pop a saved registers frame + iretq */

/* SMP: `current` is now per-CPU (this_cpu()->current). The single global run
 * queue is a circular doubly-linked ring anchored at g_ring; thread_create*
 * splice new threads next to g_ring. g_sched_lock protects the ring + flags. */
static struct thread *g_ring = NULL;
static int next_id = 0;
static volatile unsigned long switches = 0;
static struct thread *dead_threads = NULL;
static spinlock_t g_sched_lock = SPINLOCK_INIT;

/* M27: threads parked with a deadline (sched_block_self_unlock_until). Singly
 * linked, guarded by g_sched_lock; walked once per timer tick. Short by
 * construction -- only threads in a timed sleep are on it. */
static struct thread *g_timer_wait = NULL;
static volatile unsigned long g_blocked = 0;

/* M30: every live thread, on the ring or not. Guarded by g_sched_lock. */
static struct thread *g_all = NULL;

/* ==========================================================================
 * M30: THE LAZY HALF OF THE TLB SHOOTDOWN.
 *
 * c/kernel/cpu/tlb.c's tlb_flush_all() is the eager half, and on its own it is
 * NOT ENOUGH -- which was found by watching it fail rather than by reading it.
 * Two reasons, and they compound:
 *
 *   1. It GIVES UP SILENTLY. It spins a bounded number of times for the other
 *      cores' acknowledgements and then returns anyway, because an unbounded
 *      wait would deadlock against a core spinning for the BKL with IF=0. Its
 *      own comment says this is safe "since every current use pairs the
 *      shootdown with a CR3 reload".
 *
 *   2. THAT PAIRING DOES NOT HOLD FOR THREADS. schedule() reloads CR3 only when
 *      the incoming thread's address space DIFFERS from the outgoing one's --
 *      and two threads of one process have the SAME cr3. So the one thing that
 *      was supposed to clean up after an unacknowledged shootdown is precisely
 *      the thing threads skip.
 *
 * The observed failure: a thread stack was unmapped, the VMA allocator handed
 * the same range straight back to the next pthread_create, the creating thread
 * wrote the new thread's control block into a fresh frame, and the new thread
 * was dispatched onto a core still holding the old translation -- reading a
 * frame that had been freed. Reproducible at -smp 4, never at -smp 1.
 *
 * So: a global generation counter, bumped after any unmap that could leave a
 * stale entry on another core, and a per-core record of the generation that
 * core has flushed at. A core whose record is behind reloads CR3 on its way
 * into the next thread, whether or not the address space changed. That needs no
 * IPI, cannot be lost, and costs one comparison per context switch on a machine
 * where nothing is unmapping.
 *
 * The counter is GLOBAL rather than per-address-space, which over-flushes: an
 * unmap in one process makes every core reload once. Unmaps of this kind happen
 * when a thread ends, a CR3 reload is tens of cycles, and a per-space table
 * would need a lifetime rule for space ids. Cheap and obviously correct beat
 * precise and subtle here.
 *
 * WHAT IS STILL NOT COVERED, and it is the eager half's job: a thread ALREADY
 * RUNNING in ring 3 on another core, which takes no context switch and so never
 * consults this. tlb_flush_all() is what reaches that thread, and it is best
 * effort. Closing it completely means flushing inside munmap before the frames
 * go back to the PMM -- in c/kernel/mm/, not here.
 * ========================================================================== */
static volatile unsigned long g_tlb_gen;
static unsigned long g_cpu_tlb_gen[PERCPU_MAXCPU];

void sched_tlb_gen_bump(void)
{
    __atomic_add_fetch(&g_tlb_gen, 1, __ATOMIC_SEQ_CST);
}

/* Resync THIS core now, from anywhere -- no scheduler lock, no context switch.
 *
 * WHY THE SWITCH SITES ARE NOT ENOUGH, which took a second failure to see. The
 * first diagnosis had the stale entry on the READING side: a new thread
 * dispatched onto a core that still had the old translation. The switch sites
 * cover that. But the actual sequence is the other way round, and the stale
 * entry is on the WRITER:
 *
 *   1. a thread ends on core X; its stack is unmapped and the frames go back;
 *   2. the creating thread, on core Y, calls mmap and is handed the SAME range;
 *   3. it writes the new thread's control block -- and core Y still has the OLD
 *      translation cached, so THE WRITE DOES NOT FAULT. It lands in the frame
 *      that was just freed;
 *   4. the new thread reads the address through the real page tables, faults,
 *      gets a fresh zero page from do_anon, and finds a control block full of
 *      zeroes.
 *
 * That whole sequence happens inside one pass of a create loop -- far inside a
 * single 10 ms timer tick -- so a scheme that resyncs "at the next context
 * switch" is orders of magnitude too slow. But step 2 is a SYSCALL, and there
 * is no way to obtain a recycled address without one. So the syscall gate is
 * not an approximation of the right place, it is exactly the right place: a
 * core cannot act on a reused mapping without passing through it first.
 *
 * Cost when nothing has been unmapped: one relaxed load, one compare, one
 * not-taken branch. IF is already 0 on the int-0x80 path; saved and restored
 * anyway so this is callable from anywhere. */
void sched_tlb_gen_check(void)
{
    unsigned long gen = __atomic_load_n(&g_tlb_gen, __ATOMIC_ACQUIRE);
    uint64_t fl;
    int idx;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    idx = this_cpu()->index;
    /* Only this core ever writes its own slot, and only with IF off, so the
     * slot needs no lock -- unlike the switch-site path, which happens to hold
     * g_sched_lock for other reasons. */
    if (idx >= 0 && idx < PERCPU_MAXCPU && g_cpu_tlb_gen[idx] != gen) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
        g_cpu_tlb_gen[idx] = gen;
    }
    if (fl & 0x200) __asm__ volatile ("sti");
}

/* Called at every switch site with `reloaded` set if CR3 was just written
 * anyway (an address-space change already flushes). Caller holds g_sched_lock
 * with IF off, so the per-core slot needs no lock of its own. */
static inline void tlb_gen_sync(struct cpu *me, int reloaded)
{
    unsigned long gen = __atomic_load_n(&g_tlb_gen, __ATOMIC_ACQUIRE);
    int idx = me->index;
    if (idx < 0 || idx >= PERCPU_MAXCPU) return;
    if (g_cpu_tlb_gen[idx] == gen) return;
    if (!reloaded) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    g_cpu_tlb_gen[idx] = gen;
}

/* Both under g_sched_lock. all_unlink() runs in thread_exit() BEFORE the thread
 * joins dead_threads, so nothing can look up a struct that schedule() is about
 * to kfree -- which is the whole reason lookups go by INTEGER id and not by a
 * pointer somebody stashed. */
static void all_link(struct thread *t)   { t->all_next = g_all; g_all = t; }
static void all_unlink(struct thread *t)
{
    struct thread **pp = &g_all;
    while (*pp) { if (*pp == t) { *pp = t->all_next; break; } pp = &(*pp)->all_next; }
    t->all_next = NULL;
}

static void name_set(struct thread *t, const char *n)
{
    int i = 0;
    if (n) for (; i < 31 && n[i]; i++) t->name[i] = n[i];
    t->name[i] = 0;
}

/* Zero the M27 block-core fields. Every creation site calls this, so a new
 * field is added in exactly one place. */
static void block_fields_init(struct thread *t, int on_ring)
{
    t->state     = THREAD_READY;
    t->on_ring   = on_ring;
    t->slices    = 0;
    t->tw_next   = NULL;
    t->on_timer  = 0;
    t->deadline  = 0;
    t->timed_out = 0;
    t->fsbase    = 0;
    t->all_next  = NULL;
}

/* --- run-ring membership. Both callers hold g_sched_lock. ---
 *
 * A parked thread is genuinely removed from the ring rather than skipped in the
 * pick loop, with one guard: a thread that is the ring's SOLE member stays
 * linked (marked BLOCKED, so the pick loop skips it anyway). That keeps g_ring
 * non-NULL unconditionally, which thread_create/thread_fork rely on to splice. */
static void ring_unlink(struct thread *t)
{
    if (!t->on_ring || t->next == t) return;    /* off-ring, or the sole member */
    if (g_ring == t) g_ring = t->next;
    t->prev->next = t->next;
    t->next->prev = t->prev;
    t->next = t;
    t->prev = t;
    t->on_ring = 0;
}

static void ring_link(struct thread *t)
{
    if (t->on_ring || !g_ring) return;
    t->next = g_ring->next;
    t->prev = g_ring;
    t->next->prev = t;
    g_ring->next = t;
    t->on_ring = 1;
}

/* Remove `t` from the deadline list. Caller holds g_sched_lock. */
static void timerlist_remove(struct thread *t)
{
    struct thread **pp = &g_timer_wait;
    if (!t->on_timer) return;
    while (*pp) {
        if (*pp == t) { *pp = t->tw_next; break; }
        pp = &(*pp)->tw_next;
    }
    t->tw_next  = NULL;
    t->on_timer = 0;
}

unsigned long sched_switches(void) { return switches; }
void *sched_current_data(void) { struct thread *t = this_cpu()->current; return t ? t->data : NULL; }
uint64_t sched_current_cr3(void) { struct thread *t = this_cpu()->current; return t ? t->cr3 : vmm_kernel_cr3(); }

void sched_init(void)
{
    struct thread *main = kmalloc(sizeof *main);
    if (!main) { kprintf("[sched] OOM creating boot thread\n"); return; }
    main->rsp = 0;
    main->stack = NULL;
    main->kstack_top = 0;        /* the WM thread runs in ring 0; rsp0 unused */
    main->cr3 = vmm_kernel_cr3();/* the kernel/shared address space */
    main->data = NULL;
    name_set(main, "wm");
    main->id = next_id++;
    main->alive = 1;
    main->running = 1;
    main->entry = NULL;
    main->is_idle = 0;
    main->next = main;
    main->prev = main;
    block_fields_init(main, 1);
    g_ring = main;
    all_link(main);
    this_cpu()->current = main;     /* BSP (this_cpu falls back to g_cpus[0]) */

    /* The BSP gets a dedicated idle thread too (separate from the WM) so
     * thread_exit()/schedule() on core 0 always have a valid idle fallback. It
     * first runs via a hand-built kthread_bootstrap frame -> sched_become_idle. */
    struct thread *bi = kmalloc(sizeof *bi);
    if (!bi) { kprintf("[sched] OOM creating BSP idle thread\n"); return; }
    bi->stack = kmalloc(STACK_SIZE);
    if (!bi->stack) { kprintf("[sched] OOM creating BSP idle stack\n"); kfree(bi); return; }
    bi->rsp = 0;
    bi->kstack_top = 0;
    bi->cr3 = vmm_kernel_cr3();
    bi->data = NULL;
    name_set(bi, "idle0");
    bi->id = next_id++;
    bi->alive = 1;
    bi->running = 0;            /* not currently running (WM is core 0's current) */
    bi->entry = sched_become_idle;
    bi->is_idle = 1;
    bi->next = bi; bi->prev = bi;   /* off the shared ring */
    block_fields_init(bi, 0);
    {
        /* The -8 is the ABI; see the long note in thread_create() below. This
         * is the site that actually stopped the machine booting: the idle
         * thread is what every core falls into, so a misaligned frame here is
         * reproducible AT IDLE with nothing else running -- which is exactly
         * how it was reported, from three lines independently, as a #GP inside
         * kprintf's own va_start. Fixing thread_create() and not this one
         * fixed the threads nobody was running. */
        uint64_t top = (((uint64_t)bi->stack + STACK_SIZE) & ~(uint64_t)0xF) - 8;
        uint64_t *sp = (uint64_t *)top;
        *--sp = (uint64_t)kthread_bootstrap;
        *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
        *--sp = 0x002;   /* RFLAGS: IF=0 (reserved bit1 only). A first-run thread MUST
                      * start with interrupts OFF: its bootstrap still holds
                      * g_sched_lock, and a timer IRQ there would acquire g_bkl in
                      * reverse order (g_sched_lock->g_bkl) -> ABBA deadlock. The
                      * bootstrap re-enables IF at its own controlled point (kthread:
                      * sti after taking g_bkl; ring3: the iretq's RFLAGS). */
        bi->rsp = (uint64_t)sp;
    }
    all_link(bi);
    g_cpus[0].idle = bi;

    /* M27: the run ring exists, so kernel threads can be created. Start the
     * deferred-work thread (the context an interrupt hands blocking work to),
     * then the boot-time wait/wake self-test. Both are ordinary ring-0 threads
     * spliced onto the ring; the caller (wm_run) continues immediately. */
    work_init();
    wait_selftest_start();
    /* Arms the BKL/entry counters NOW (so the accounted window starts at the
     * first kernel entry, which is where the contention is) and spawns the
     * benchmark thread, which sleeps until the desktop is live before it
     * measures anything. */
    kbench_start();
}

extern void kthread_bootstrap(void);   /* sched.c: releases g_sched_lock, calls entry, exits */

void thread_create(void (*entry)(void), const char *name)
{
    struct thread *t = kmalloc(sizeof *t);
    if (!t) return;
    t->stack = kmalloc(STACK_SIZE);
    if (!t->stack) { kfree(t); return; }
    t->kstack_top = 0;
    t->cr3 = vmm_kernel_cr3();
    t->data = NULL;
    name_set(t, name);
    t->id = next_id++;
    t->alive = 1;
    t->running = 0;
    t->entry = entry;
    t->is_idle = 0;
    block_fields_init(t, 1);

    /* First switch lands in kthread_bootstrap, which drops g_sched_lock then
     * calls entry() (kernel threads keep the BKL while in kernel code).
     *
     * The -8 is the ABI, not a fudge. SysV requires RSP to be 16-byte aligned
     * at a `call`, so a function ENTERED normally sees RSP ≡ 8 (mod 16) -- the
     * return address the call pushed. This frame is entered by `ret` instead,
     * which pops rather than pushes, so starting from a 16-aligned top left
     * kthread_bootstrap running with RSP ≡ 0: misaligned by exactly 8 against
     * what every function it calls was compiled to assume.
     *
     * That was invisible until M15 built the kernel with -msse2. Any callee
     * with a 16-byte-aligned SSE spill then faults on its own prologue --
     * `movaps %xmm0,-0x30(%rbp)` takes a #GP -- and `kprintf` is one of them,
     * so the thread dies before it can report that it died. Found by the USB
     * line, which hit it at rip=0x11cae8 and then reproduced it with USB
     * absent entirely, from an unrelated self-test thread. Every kernel thread
     * created here has been one aligned spill away from this since M15.
     *
     * ring3_bootstrap and fork_ret are unaffected: both are assembly and end in
     * iretq, so no C prologue ever runs on the misaligned frame. */
    uint64_t top = (((uint64_t)t->stack + STACK_SIZE) & ~(uint64_t)0xF) - 8;
    uint64_t *sp = (uint64_t *)top;
    *--sp = (uint64_t)kthread_bootstrap;  /* ret target (drops g_sched_lock, calls entry) */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0x002;   /* RFLAGS: IF=0 (reserved bit1 only). A first-run thread MUST
                      * start with interrupts OFF: its bootstrap still holds
                      * g_sched_lock, and a timer IRQ there would acquire g_bkl in
                      * reverse order (g_sched_lock->g_bkl) -> ABBA deadlock. The
                      * bootstrap re-enables IF at its own controlled point (kthread:
                      * sti after taking g_bkl; ring3: the iretq's RFLAGS). */
    t->rsp = (uint64_t)sp;

    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->next = g_ring->next;
    t->prev = g_ring;
    t->next->prev = t;
    g_ring->next = t;
    all_link(t);
    spin_unlock_irqrestore(&g_sched_lock, f);
}

/* Create a ring-3 process thread: first switch drops to `entry` in user mode
 * on `ustack`, with its own kernel stack for traps. */
int thread_create_user(const char *name, uint64_t entry, uint64_t ustack, void *data, uint64_t cr3)
{
    struct thread *t = kmalloc(sizeof *t);
    if (!t) return -1;
    uint8_t *ks = kmalloc(KSTACK_SIZE);
    if (!ks) { kfree(t); return -1; }
    t->stack = ks;
    name_set(t, name);
    t->id = next_id++;
    t->data = data;
    t->alive = 1;
    t->running = 0;
    t->entry = NULL;
    t->is_idle = 0;
    t->cr3 = cr3 ? cr3 : vmm_kernel_cr3();
    t->kstack_top = ((uint64_t)ks + KSTACK_SIZE) & ~(uint64_t)0xF;
    block_fields_init(t, 1);

    /* Hand-built kernel frame: context_switch "returns" into ring3_bootstrap
     * with entry in r15 and the user stack in r14. (ring3_bootstrap releases
     * g_sched_lock + the BKL before iretq -- see enter_user.asm.) */
    uint64_t *sp = (uint64_t *)t->kstack_top;
    *--sp = (uint64_t)ring3_bootstrap;   /* ret target */
    *--sp = 0;                           /* rbp */
    *--sp = 0;                           /* rbx */
    *--sp = 0;                           /* r12 */
    *--sp = 0;                           /* r13 */
    *--sp = ustack;                      /* r14 */
    *--sp = entry;                       /* r15 */
    *--sp = 0x002;   /* RFLAGS: IF=0 (reserved bit1 only). A first-run thread MUST
                      * start with interrupts OFF: its bootstrap still holds
                      * g_sched_lock, and a timer IRQ there would acquire g_bkl in
                      * reverse order (g_sched_lock->g_bkl) -> ABBA deadlock. The
                      * bootstrap re-enables IF at its own controlled point (kthread:
                      * sti after taking g_bkl; ring3: the iretq's RFLAGS). */                       /* rflags */
    t->rsp = (uint64_t)sp;

    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->next = g_ring->next;
    t->prev = g_ring;
    t->next->prev = t;
    g_ring->next = t;
    all_link(t);
    spin_unlock_irqrestore(&g_sched_lock, f);
    return t->id;
}

/* fork(): build a child thread that, when first scheduled, resumes in ring 3
 * exactly where the parent took the int 0x80 -- but returning 0. We copy the
 * parent's interrupt-return frame to the top of the child's kernel stack and
 * lay a context_switch frame below it whose `ret` lands in fork_ret, which pops
 * that registers frame and iretq's into user mode. */
int thread_fork(const char *name, struct registers *pr, void *data, uint64_t cr3)
{
    struct thread *t = kmalloc(sizeof *t);
    if (!t) return -1;
    uint8_t *ks = kmalloc(KSTACK_SIZE);
    if (!ks) { kfree(t); return -1; }
    t->stack = ks;
    name_set(t, name);
    t->id = next_id++;
    t->data = data;
    t->alive = 1;
    t->running = 0;
    t->entry = NULL;
    t->is_idle = 0;
    t->cr3 = cr3 ? cr3 : vmm_kernel_cr3();
    t->kstack_top = ((uint64_t)ks + KSTACK_SIZE) & ~(uint64_t)0xF;
    block_fields_init(t, 1);
    /* M30: the child inherits the forking thread's TLS pointer. It has to. fork
     * clones the address space, so the TLS block is at the SAME user address in
     * the child, and the child resumes at the parent's instruction -- which,
     * for anything linked against mini-libc, is inside a function whose next
     * `%fs:`-relative access assumes the pointer is still there. Leaving it 0
     * (what block_fields_init sets) makes fork() from a program that has ever
     * called pthread_self() or malloc() fault in the child, on the first TLS
     * read, with nothing in the trace to say why. */
    {
        struct thread *me = this_cpu()->current;
        t->fsbase = me ? me->fsbase : 0;
    }

    /* Copy of the parent's full register frame at the top of the child kstack,
     * with rax = 0 (the value fork returns in the child). */
    struct registers *cr = (struct registers *)(t->kstack_top - sizeof(struct registers));
    *cr = *pr;
    cr->rax = 0;

    /* context_switch restore frame just below it: popfq; pop r15..rbp; ret. */
    uint64_t *sp = (uint64_t *)cr;
    *--sp = (uint64_t)fork_ret;          /* ret target */
    *--sp = 0;                           /* rbp */
    *--sp = 0;                           /* rbx */
    *--sp = 0;                           /* r12 */
    *--sp = 0;                           /* r13 */
    *--sp = 0;                           /* r14 */
    *--sp = 0;                           /* r15 */
    *--sp = 0x002;                       /* rflags (IF restored by iretq from cr->rflags) */
    t->rsp = (uint64_t)sp;

    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->next = g_ring->next;
    t->prev = g_ring;
    t->next->prev = t;
    g_ring->next = t;
    all_link(t);
    spin_unlock_irqrestore(&g_sched_lock, f);
    return t->id;
}

/* SMP scheduler (BKL model). schedule() runs with the BKL held by this core; the
 * BKL is NOT touched here -- it is held across schedule() and released by the
 * IRQ/syscall unwind (interrupt_handler) or by a new thread's first-run
 * trampoline. g_sched_lock protects the ring + flags; the "incoming" thread
 * releases g_sched_lock at its own post-context_switch point (each thread brackets
 * its own IF via its saved `flags`). noinline so `flags` lives in each thread's
 * own frame. */
__attribute__((noinline)) void schedule(void)
{
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);

    while (dead_threads) {
        struct thread *t = dead_threads;
        dead_threads = t->next;
        if (t->stack) kfree(t->stack);
        kfree(t);
    }

    struct cpu *me = this_cpu();
    struct thread *prev = me->current;
    if (!prev) {                         /* core not yet adopted into the scheduler */
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return;
    }

    /* Pick the next runnable thread from the SHARED ring (g_ring), skipping ones
     * running on another core, dead ones, and idle threads (idle is per-core, off
     * the shared ring). We always traverse g_ring -- not prev->next -- because an
     * idle prev is off-ring and couldn't otherwise reach shared-ring threads.
     * Start the scan one past prev when prev is on the ring (round-robin); else
     * from g_ring's successor. */
    /* An anchor that is no longer ON the ring (it parked and was unlinked) has
     * next==itself, so scanning from it would see nothing: fall back to g_ring. */
    struct thread *anchor = (prev->is_idle || !prev->on_ring) ? g_ring : prev;
    struct thread *next = NULL, *s = anchor->next, *start = s;
    do {
        if (s != prev && s->alive && !s->running && !s->is_idle &&
            s->state == THREAD_READY) { next = s; break; }
        s = s->next;
    } while (s != start);

    if (!next) {
        /* Nothing else runnable. An idle/non-running core goes to its idle thread;
         * a running thread (e.g. the WM) just stays on. */
        next = (prev->is_idle || prev->running == 0) ? me->idle : prev;
    }

    if (next && next != prev) {
        if (!prev->is_idle) prev->running = 0;
        next->running = 1;
        next->slices++;
        me->current = next;
        switches++;
        if (next->kstack_top)
            percpu_tss_set_rsp0(next->kstack_top);
        {   /* M30: force a CR3 reload if this core is behind on unmaps -- see
             * the g_tlb_gen block above. Two threads of one process share a
             * cr3, so the reload below does NOT happen for them, and that is
             * exactly the case that needs it. */
            int reloaded = 0;
            if (next->cr3 && next->cr3 != prev->cr3) {
                vmm_switch(next->cr3);      /* enter the next thread's address space */
                reloaded = 1;
            }
            tlb_gen_sync(me, reloaded);
        }
        /* M30: the TLS pointer. Note this is NOT covered by the cr3 check above
         * and must not be folded into it -- two threads of one process have the
         * SAME cr3 and DIFFERENT TLS, which is the entire case this exists for. */
        if (next->fsbase != prev->fsbase) fsbase_load(next->fsbase);

        /* BKL hand-off (drop-before / re-acquire-after). The CORE drops the BKL
         * just before the switch and the INCOMING thread re-acquires it just
         * after -- so the BKL is never double-released (an existing thread's old
         * IRQ frame would otherwise release it once, and the new thread's
         * trampoline a second time). g_sched_lock is held across context_switch
         * (released by the incoming thread). IF stays OFF across the whole window
         * so a timer can't fire while the core holds neither the BKL nor a valid
         * in_kernel state. Lock order: we release g_sched_lock BEFORE re-acquiring
         * the BKL on the incoming side, so BKL is never taken while holding the
         * inner lock (no AB-BA with interrupt_handler's BKL->sched order). */
        me->in_kernel = 0;
        spin_unlock(&g_bkl);
        context_switch(&prev->rsp, next->rsp);
        /* === RESUME AS THE INCOMING THREAD (existing) === g_sched_lock held, BKL not. */
        me = this_cpu();
        spin_unlock(&g_sched_lock);              /* release inner; IF still off */
        spin_lock(&g_bkl);                       /* re-enter the kernel (correct order) */
        me->in_kernel = 1;
        if (flags & 0x200) __asm__ volatile ("sti");
    } else {
        /* NO switch this time -- but this is still the per-tick visit every
         * core makes through schedule(), and it is what gives the generation
         * counter its bound. A core running one thread flat out never changes
         * `current`, so without this line it would never notice an unmap at
         * all. With it, every core is at most one timer tick behind. */
        tlb_gen_sync(me, 0);
        spin_unlock_irqrestore(&g_sched_lock, flags);
    }
}

/* Remove the current thread from the ring and never return. Reached from
 * proc_exit inside interrupt_handler holding the BKL; the BKL is released by the
 * INCOMING thread's unwind, never by the dead thread. */
void thread_exit(void)
{
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    struct cpu *me = this_cpu();
    struct thread *dead = me->current;

    /* Pick a runnable successor (skip running-on-other-core / idle / parked). */
    struct thread *next = dead->next;
    while (next != dead && (next->running || next->is_idle ||
                            next->state != THREAD_READY)) next = next->next;
    if (next == dead) next = me->idle;            /* nothing runnable: go idle */

    if (next == dead) {                           /* truly nothing: stop this core */
        spin_unlock_irqrestore(&g_sched_lock, flags);
        for (;;) __asm__ volatile ("hlt");
    }

    /* O(1) unlink the dead thread from the shared ring (idle is off-ring). */
    if (!dead->is_idle && dead->on_ring) {
        dead->prev->next = dead->next;
        dead->next->prev = dead->prev;
        if (g_ring == dead && dead->next != dead) g_ring = dead->next;
    }
    dead->on_ring = 0;
    timerlist_remove(dead);
    dead->running = 0;
    dead->alive = 0;
    all_unlink(dead);
    dead->next = dead_threads;
    dead_threads = dead;

    next->running = 1;
    next->slices++;
    me->current = next;
    switches++;
    if (next->kstack_top)
        percpu_tss_set_rsp0(next->kstack_top);
    {
        int reloaded = 0;
        if (next->cr3 && next->cr3 != dead->cr3) {
            vmm_switch(next->cr3);      /* leave the dying app's space */
            reloaded = 1;
        }
        tlb_gen_sync(me, reloaded);
    }
    if (next->fsbase != dead->fsbase) fsbase_load(next->fsbase);
    /* Drop the BKL before the switch (the dead thread never resumes to release it);
     * the incoming thread re-acquires it (schedule tail / kthread_bootstrap) or
     * runs ring3 without it (ring3_bootstrap/fork_ret). Same model as schedule(). */
    me->in_kernel = 0;
    spin_unlock(&g_bkl);
    context_switch(&me->exit_discard, next->rsp);   /* per-cpu discard, NOT static */
    /* unreachable: the dead thread never resumes. */
}

/* ==========================================================================
 * M27 blocking core: park / unpark / deadline expiry.
 * The lost-wakeup argument lives in sched.h next to the declarations -- read it
 * there before changing anything below.
 * ========================================================================== */

struct thread *sched_current_thread(void) { return this_cpu()->current; }
int sched_thread_id(struct thread *t) { return t ? t->id : -1; }
unsigned long sched_slices_of(struct thread *t) { return t ? t->slices : 0; }
unsigned long sched_blocked_count(void) { return g_blocked; }

/* Park the caller until sched_wake(), releasing `outer` atomically w.r.t. wakers.
 *
 * Preconditions (all of them, every time):
 *   - `outer` is held by this core via spin_lock_irqsave(), `flags` is its result;
 *   - the condition being waited on was tested under `outer`;
 *   - this core holds the BKL (dropped across the switch, re-taken on resume);
 *   - no other spinlock is held.
 *
 * Returns with `outer` NOT held and IF restored from `flags`. Wakeups may be
 * spurious -- callers loop.
 *
 * `deadline` != 0 additionally arms a timer wake; *timed_out (if non-NULL) is
 * set to 1 when the deadline, not a wake, is what made us runnable. */
static void block_self(spinlock_t *outer, uint64_t flags, uint64_t deadline, int *timed_out)
{
    spin_lock(&g_sched_lock);                 /* IF is already 0 (outer was irqsave) */

    struct cpu *me = this_cpu();
    struct thread *self = me->current;

    /* Degrade to a plain unlock rather than wedge a core: the idle thread has no
     * successor to hand to, and a core not yet adopted has no `current`. Callers
     * are `while (!cond)` loops, so returning early is always safe -- it just
     * costs a spin. */
    if (!self || self->is_idle || !me->idle) {
        spin_unlock(&g_sched_lock);
        spin_unlock_irqrestore(outer, flags);
        return;
    }

    /* Pick a successor from the shared ring (same rules as schedule()). */
    struct thread *anchor = self->on_ring ? self : g_ring;
    struct thread *next = NULL, *s = anchor->next, *start = s;
    do {
        if (s != self && s->alive && !s->running && !s->is_idle &&
            s->state == THREAD_READY) { next = s; break; }
        s = s->next;
    } while (s != start);
    if (!next) next = me->idle;
    if (next == self) {                       /* cannot happen (idle != self) */
        spin_unlock(&g_sched_lock);
        spin_unlock_irqrestore(outer, flags);
        return;
    }

    self->state     = THREAD_BLOCKED;
    self->running   = 0;
    self->timed_out = 0;
    ring_unlink(self);
    g_blocked++;
    if (deadline) {
        self->deadline = deadline;
        self->tw_next  = g_timer_wait;
        g_timer_wait   = self;
        self->on_timer = 1;
    }

    /* THE ORDERING THAT CLOSES THE LOST WAKEUP: `outer` is released only now --
     * after we are marked BLOCKED and off the ring, and while g_sched_lock is
     * still held. A waker that acquires `outer` from here on must then wait on
     * g_sched_lock (released by the INCOMING thread, i.e. after context_switch
     * has saved our rsp), so it can only ever observe a fully parked thread. */
    spin_unlock(outer);

    next->running = 1;
    next->slices++;
    me->current = next;
    switches++;
    if (next->kstack_top)
        percpu_tss_set_rsp0(next->kstack_top);
    {
        int reloaded = 0;
        if (next->cr3 && next->cr3 != self->cr3) {
            vmm_switch(next->cr3);
            reloaded = 1;
        }
        tlb_gen_sync(me, reloaded);
    }
    if (next->fsbase != self->fsbase) fsbase_load(next->fsbase);

    /* Same BKL hand-off as schedule(): drop before the switch, the incoming
     * thread re-acquires after. IF stays 0 across the whole window. */
    me->in_kernel = 0;
    spin_unlock(&g_bkl);
    context_switch(&self->rsp, next->rsp);
    /* === RESUMED (someone called sched_wake / the deadline fired) ===
     * g_sched_lock is held, the BKL is not. */
    me = this_cpu();
    timerlist_remove(self);                   /* no-op if the deadline already popped us */
    if (timed_out) *timed_out = self->timed_out;
    spin_unlock(&g_sched_lock);
    spin_lock(&g_bkl);
    me->in_kernel = 1;
    if (flags & 0x200) __asm__ volatile ("sti");
}

void sched_block_self_unlock(spinlock_t *outer, uint64_t flags)
{
    block_self(outer, flags, 0, NULL);
}

int sched_block_self_unlock_until(spinlock_t *outer, uint64_t flags, uint64_t deadline)
{
    int to = 0;
    block_self(outer, flags, deadline ? deadline : 1, &to);
    return !to;
}

/* Caller holds g_sched_lock. */
static int wake_locked(struct thread *t)
{
    if (!t || !t->alive || t->state != THREAD_BLOCKED) return 0;
    t->state = THREAD_READY;
    timerlist_remove(t);
    ring_link(t);
    if (g_blocked) g_blocked--;
    return 1;
}

/* Make `t` runnable. Callable from any core and from interrupt context; takes
 * only g_sched_lock, so the lock order is always <caller's lock> -> g_sched_lock
 * and never the reverse. Returns 1 iff this call did the unparking. */
int sched_wake(struct thread *t)
{
    int did;
    if (!t) return 0;
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    did = wake_locked(t);
    spin_unlock_irqrestore(&g_sched_lock, f);
    return did;
}

/* ==========================================================================
 * M30: reaching a thread by id.
 *
 * sched_wake() takes a `struct thread *`, and nothing outside this file can
 * obtain one -- proc.c says so in the comment above proc_kill(), which is why a
 * killed process parked on a wait queue was left un-woken. A THREAD id is the
 * handle userland already has (SYS_THREAD_CREATE returns one), so the lookup and
 * the wake happen inside ONE hold of g_sched_lock. That is not a convenience: a
 * two-step "find the pointer, then wake it" is a use-after-free, because
 * schedule() kfree()s an exited thread's struct on the next pass through the
 * dead list, and nothing stops that pass from landing between the two steps.
 * ========================================================================== */

/* Caller holds g_sched_lock. Walks g_all, not the ring: the thread being looked
 * for is usually PARKED, and a parked thread is off the ring. */
static struct thread *by_id_locked(int id)
{
    for (struct thread *t = g_all; t; t = t->all_next)
        if (t->id == id && t->alive) return t;
    return NULL;
}

int sched_wake_id(int id)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    int did = wake_locked(by_id_locked(id));
    spin_unlock_irqrestore(&g_sched_lock, f);
    return did;
}

int sched_thread_alive(int id)
{
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    int yes = by_id_locked(id) != NULL;
    spin_unlock_irqrestore(&g_sched_lock, f);
    return yes;
}

int sched_current_tid(void)
{
    struct thread *t = this_cpu()->current;
    return t ? t->id : -1;
}

uint64_t sched_current_fsbase(void)
{
    struct thread *t = this_cpu()->current;
    return t ? t->fsbase : 0;
}

/* Set the CALLING thread's TLS pointer. Writes the descriptor and the MSR
 * together, with interrupts off, so the two can never be seen to disagree --
 * the invariant documented on struct thread::fsbase is "hardware equals
 * current->fsbase", and a preemption between the two stores would break it in
 * the one direction nothing later repairs (the switch back compares fsbase
 * fields, finds them equal, and leaves the wrong value loaded). */
void sched_set_fsbase(uint64_t v)
{
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    struct thread *t = this_cpu()->current;
    if (t) t->fsbase = v;
    fsbase_load(v);
    if (fl & 0x200) __asm__ volatile ("sti");
}

/* Deadline expiry, driven from the timer IRQ BEFORE the BKL is acquired (see
 * interrupts.c). Touches only g_sched_lock, never the BKL: a sleeper's deadline
 * must not be able to queue behind a BKL held by a thread that is itself waiting
 * for that deadline -- the same circular wait that made timer_tick() move ahead
 * of the BKL acquire. */
void sched_timer_expire(void)
{
    uint64_t now;
    /* Lock-free early-out: this runs on every tick and the list is empty almost
     * always. A stale read costs at most one tick of latency. */
    if (!__atomic_load_n(&g_timer_wait, __ATOMIC_RELAXED)) return;
    now = timer_ticks();
    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    struct thread **pp = &g_timer_wait;
    while (*pp) {
        struct thread *t = *pp;
        if (t->state == THREAD_BLOCKED && (int64_t)(now - t->deadline) >= 0) {
            *pp = t->tw_next;
            t->tw_next   = NULL;
            t->on_timer  = 0;
            t->timed_out = 1;
            t->state     = THREAD_READY;
            ring_link(t);
            if (g_blocked) g_blocked--;
            continue;
        }
        pp = &t->tw_next;
    }
    spin_unlock_irqrestore(&g_sched_lock, f);
}

/* First-run helpers ------------------------------------------------------- */

/* Incoming ring-3 / forked child first run: release g_sched_lock (held across the
 * context_switch by schedule/thread_exit). The BKL was ALREADY dropped by
 * schedule()/thread_exit() before the switch, and this thread goes to ring 3 (no
 * BKL needed), so we must NOT touch the BKL here. The incoming thread inherits IF
 * from its iretq frame. Called as the FIRST instruction of ring3_bootstrap and
 * fork_ret (enter_user.asm). */
void sched_unlock_new_thread(void)
{
    /* DIAG: one line per ring-3/forked thread at its first dispatch -- proves a
     * forked child (e.g. the terminal's /bin/sh) actually got a time slice.
     *
     * RATE-LIMITED SINCE M30, and the reason is a measurement rather than
     * tidiness: a thread is now something a program creates in a loop, and the
     * leak check runs thousands of create/join cycles. At one serial line each
     * -- serial being a byte-at-a-time device -- the diagnostic became the
     * dominant cost of the thing it was watching. The first 64 keep every case
     * this line was written for (boot, the shell, the terminal's child), and
     * every 512th after that keeps a trail without setting the pace. */
    struct thread *t = this_cpu()->current;
    if (t && (t->id < 64 || (t->id % 512) == 0))
        kprintf("[sched] first-run tid %d (%s)\n", t->id, t->name[0] ? t->name : "?");
    spin_unlock(&g_sched_lock);
}

/* Incoming kernel thread first run (drop-before/re-acquire-after model): the BKL
 * was dropped by schedule() before the switch, so re-acquire it (kernel threads
 * run holding the BKL), release g_sched_lock, then call entry(). entry() never
 * returns; if it does, thread_exit() reaps the thread. */
__attribute__((noreturn)) void kthread_bootstrap(void)
{
    struct cpu *me = this_cpu();
    spin_unlock(&g_sched_lock);
    spin_lock(&g_bkl);
    me->in_kernel = 1;
    void (*fn)(void) = me->current->entry;
    __asm__ volatile ("sti");      /* kernel threads run with IF on */
    fn();
    thread_exit();
    for (;;) __asm__ volatile ("hlt");
}

/* Block briefly WITHOUT hogging the BKL: drop it, idle until the next interrupt
 * (timer 100Hz at worst), re-acquire. Use this in blocking-wait loops
 * (pipe_read/pipe_write/proc_waitpid): a bare schedule() there is a no-op when
 * every other thread is currently running on its own core (next==prev), so the
 * loop spins holding the BKL with IF=0 and wedges the whole machine -- the
 * "Terminal black frame + frozen clock" deadlock: with enough apps closed,
 * sh's blocking pipe_read on an all-cores-busy system never releases the BKL.
 * Same release/re-acquire protocol as the WM idle loop and tty_read: BOTH
 * windows must run with IF=0 or a nested IRQ re-acquires the BKL this core
 * still holds (self-deadlock). */
/* Every call is one pass of a POLL: a thread that had nothing to do was
 * re-dispatched anyway, took the global lock, re-tested a condition and halted.
 * Counted because that is the number the M27 blocking core exists to drive to
 * zero -- the wait-queue line measured a 200 ms wait going from 117 dispatches
 * to 0, and this counter is the same measurement for the rest of the kernel.
 * A build where the pipe and the waitpid still polled through here reaches the
 * thousands over a shell session; one where they park does not. */
unsigned long g_bkl_hlt_waits;
unsigned long sched_hlt_waits(void) { return g_bkl_hlt_waits; }

void bkl_hlt_wait(void)
{
    g_bkl_hlt_waits++;
    __asm__ volatile ("cli");
    this_cpu()->in_kernel = 0;
    spin_unlock(&g_bkl);
    __asm__ volatile ("sti\n\thlt\n\tcli");
    spin_lock(&g_bkl);
    this_cpu()->in_kernel = 1;
}

/* Create core `idx`'s off-ring idle thread and make it that core's current. The
 * idle thread does NOT get a hand-built startup frame: its "context" is the
 * bring-up call stack that runs sched_become_idle() below. Its rsp is filled when
 * it is first switched away (inside the idle loop's schedule()). */
void thread_create_idle(int idx)
{
    struct thread *t = kmalloc(sizeof *t);
    if (!t) { kprintf("[sched] OOM creating idle thread for core %d\n", idx); return; }
    t->stack = NULL;        /* runs on the core's bring-up stack, not a kmalloc one */
    t->rsp = 0;
    t->kstack_top = 0;
    t->cr3 = vmm_kernel_cr3();
    t->data = NULL;
    name_set(t, "idle");
    t->id = next_id++;
    t->alive = 1;
    t->running = 1;          /* idle is always "running" on its core */
    t->entry = NULL;
    t->is_idle = 1;
    t->next = t;            /* off the shared ring */
    t->prev = t;
    block_fields_init(t, 0);

    g_cpus[idx].idle = t;
    all_link(t);
    g_cpus[idx].current = t;
}

/* Become this core's idle thread: arrives holding the BKL (in_kernel=1). Loop:
 * drop the BKL, hlt until an IRQ, re-take it, reschedule. Never returns. Called
 * directly from ap_entry (AP) or the BSP idle bootstrap so the idle thread's
 * context is that stack (a context_switch away saves it into the idle's rsp).
 * A timer IRQ on this idle core (non-nested, since in_kernel was cleared) acquires
 * the BKL and may pick a runnable thread. */
__attribute__((noreturn)) void sched_become_idle(void)
{
    for (;;) {
        /* cli FIRST so the in_kernel=0 .. spin_unlock window runs with IF=0: a
         * nested IRQ there reads nested=0 and tries to re-acquire the BKL this core
         * still holds -> self-deadlock. (The trailing cli of `sti; hlt; cli` covers
         * later iterations, but the first entry arrives with IF=1.) */
        __asm__ volatile ("cli");
        this_cpu()->in_kernel = 0;
        spin_unlock(&g_bkl);
        __asm__ volatile ("sti; hlt; cli");
        spin_lock(&g_bkl);
        this_cpu()->in_kernel = 1;
        schedule();
    }
}
