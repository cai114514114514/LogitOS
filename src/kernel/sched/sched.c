#include <stdint.h>
#include <stddef.h>
#include "sched.h"
#include "kheap.h"
#include "gdt.h"
#include "vmm.h"
#include "interrupts.h"   /* struct registers (fork) */
#include "percpu.h"
#include "spinlock.h"

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
    const char *name;
    int id;
    int alive;
    int running;             /* 1 while owned by some core (SMP: skip in pick) */
    void (*entry)(void);     /* kernel-thread trampoline target (kthread_bootstrap) */
    int is_idle;             /* per-core idle thread (off the shared ring) */
};

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

unsigned long sched_switches(void) { return switches; }
void *sched_current_data(void) { struct thread *t = this_cpu()->current; return t ? t->data : NULL; }
uint64_t sched_current_cr3(void) { struct thread *t = this_cpu()->current; return t ? t->cr3 : vmm_kernel_cr3(); }

void sched_init(void)
{
    struct thread *main = kmalloc(sizeof *main);
    main->rsp = 0;
    main->stack = NULL;
    main->kstack_top = 0;        /* the WM thread runs in ring 0; rsp0 unused */
    main->cr3 = vmm_kernel_cr3();/* the kernel/shared address space */
    main->data = NULL;
    main->name = "wm";
    main->id = next_id++;
    main->alive = 1;
    main->running = 1;
    main->entry = NULL;
    main->is_idle = 0;
    main->next = main;
    main->prev = main;
    g_ring = main;
    this_cpu()->current = main;     /* BSP (this_cpu falls back to g_cpus[0]) */

    /* The BSP gets a dedicated idle thread too (separate from the WM) so
     * thread_exit()/schedule() on core 0 always have a valid idle fallback. It
     * first runs via a hand-built kthread_bootstrap frame -> sched_become_idle. */
    struct thread *bi = kmalloc(sizeof *bi);
    bi->stack = kmalloc(STACK_SIZE);
    bi->rsp = 0;
    bi->kstack_top = 0;
    bi->cr3 = vmm_kernel_cr3();
    bi->data = NULL;
    bi->name = "idle0";
    bi->id = next_id++;
    bi->alive = 1;
    bi->running = 0;            /* not currently running (WM is core 0's current) */
    bi->entry = sched_become_idle;
    bi->is_idle = 1;
    bi->next = bi; bi->prev = bi;   /* off the shared ring */
    {
        uint64_t top = ((uint64_t)bi->stack + STACK_SIZE) & ~(uint64_t)0xF;
        uint64_t *sp = (uint64_t *)top;
        *--sp = (uint64_t)kthread_bootstrap;
        *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
        *--sp = 0x202;
        bi->rsp = (uint64_t)sp;
    }
    g_cpus[0].idle = bi;
}

extern void kthread_bootstrap(void);   /* sched.c: releases g_sched_lock, calls entry, exits */

void thread_create(void (*entry)(void), const char *name)
{
    struct thread *t = kmalloc(sizeof *t);
    t->stack = kmalloc(STACK_SIZE);
    t->kstack_top = 0;
    t->cr3 = vmm_kernel_cr3();
    t->data = NULL;
    t->name = name;
    t->id = next_id++;
    t->alive = 1;
    t->running = 0;
    t->entry = entry;
    t->is_idle = 0;

    /* First switch lands in kthread_bootstrap, which drops g_sched_lock then
     * calls entry() (kernel threads keep the BKL while in kernel code). */
    uint64_t top = ((uint64_t)t->stack + STACK_SIZE) & ~(uint64_t)0xF;
    uint64_t *sp = (uint64_t *)top;
    *--sp = (uint64_t)kthread_bootstrap;  /* ret target (drops g_sched_lock, calls entry) */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0x202;
    t->rsp = (uint64_t)sp;

    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->next = g_ring->next;
    t->prev = g_ring;
    t->next->prev = t;
    g_ring->next = t;
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
    t->name = name;
    t->id = next_id++;
    t->data = data;
    t->alive = 1;
    t->running = 0;
    t->entry = NULL;
    t->is_idle = 0;
    t->cr3 = cr3 ? cr3 : vmm_kernel_cr3();
    t->kstack_top = ((uint64_t)ks + KSTACK_SIZE) & ~(uint64_t)0xF;

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
    *--sp = 0x202;                       /* rflags */
    t->rsp = (uint64_t)sp;

    uint64_t f = spin_lock_irqsave(&g_sched_lock);
    t->next = g_ring->next;
    t->prev = g_ring;
    t->next->prev = t;
    g_ring->next = t;
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
    t->name = name;
    t->id = next_id++;
    t->data = data;
    t->alive = 1;
    t->running = 0;
    t->entry = NULL;
    t->is_idle = 0;
    t->cr3 = cr3 ? cr3 : vmm_kernel_cr3();
    t->kstack_top = ((uint64_t)ks + KSTACK_SIZE) & ~(uint64_t)0xF;

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
    struct thread *anchor = (prev->is_idle ? g_ring : prev);
    struct thread *next = NULL, *s = anchor->next, *start = s;
    do {
        if (s != prev && s->alive && !s->running && !s->is_idle) { next = s; break; }
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
        me->current = next;
        switches++;
        if (next->kstack_top)
            percpu_tss_set_rsp0(next->kstack_top);
        if (next->cr3 && next->cr3 != prev->cr3)
            vmm_switch(next->cr3);          /* enter the next thread's address space */

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

    /* Pick a runnable successor (skip running-on-other-core / idle). */
    struct thread *next = dead->next;
    while (next != dead && (next->running || next->is_idle)) next = next->next;
    if (next == dead) next = me->idle;            /* nothing runnable: go idle */

    if (next == dead) {                           /* truly nothing: stop this core */
        spin_unlock_irqrestore(&g_sched_lock, flags);
        for (;;) __asm__ volatile ("hlt");
    }

    /* O(1) unlink the dead thread from the shared ring (idle is off-ring). */
    if (!dead->is_idle) {
        dead->prev->next = dead->next;
        dead->next->prev = dead->prev;
    }
    dead->running = 0;
    dead->alive = 0;
    dead->next = dead_threads;
    dead_threads = dead;

    next->running = 1;
    me->current = next;
    switches++;
    if (next->kstack_top)
        percpu_tss_set_rsp0(next->kstack_top);
    if (next->cr3 && next->cr3 != dead->cr3)
        vmm_switch(next->cr3);          /* leave the dying app's space */
    /* Drop the BKL before the switch (the dead thread never resumes to release it);
     * the incoming thread re-acquires it (schedule tail / kthread_bootstrap) or
     * runs ring3 without it (ring3_bootstrap/fork_ret). Same model as schedule(). */
    me->in_kernel = 0;
    spin_unlock(&g_bkl);
    context_switch(&me->exit_discard, next->rsp);   /* per-cpu discard, NOT static */
    /* unreachable: the dead thread never resumes. */
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

/* Create core `idx`'s off-ring idle thread and make it that core's current. The
 * idle thread does NOT get a hand-built startup frame: its "context" is the
 * bring-up call stack that runs sched_become_idle() below. Its rsp is filled when
 * it is first switched away (inside the idle loop's schedule()). */
void thread_create_idle(int idx)
{
    struct thread *t = kmalloc(sizeof *t);
    t->stack = NULL;        /* runs on the core's bring-up stack, not a kmalloc one */
    t->rsp = 0;
    t->kstack_top = 0;
    t->cr3 = vmm_kernel_cr3();
    t->data = NULL;
    t->name = "idle";
    t->id = next_id++;
    t->alive = 1;
    t->running = 1;          /* idle is always "running" on its core */
    t->entry = NULL;
    t->is_idle = 1;
    t->next = t;            /* off the shared ring */
    t->prev = t;

    g_cpus[idx].idle = t;
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
