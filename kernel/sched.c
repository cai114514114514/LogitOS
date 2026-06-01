#include <stdint.h>
#include <stddef.h>
#include "sched.h"
#include "kheap.h"
#include "gdt.h"
#include "vmm.h"

#define STACK_SIZE  16384
#define KSTACK_SIZE 16384

struct thread {
    uint64_t rsp;            /* saved stack pointer (must be first field) */
    struct thread *next;     /* circular ready ring */
    void *stack;
    uint64_t kstack_top;     /* ring-0 stack top (TSS rsp0) for ring-3 threads */
    uint64_t cr3;            /* address space (PML4 phys); kernel space if 0 set at init */
    void *data;              /* opaque per-thread payload (the app) */
    const char *name;
    int id;
    int alive;
};

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void ring3_bootstrap(void);     /* boot/enter_user.asm */

static struct thread *current = NULL;
static int next_id = 0;
static volatile unsigned long switches = 0;

unsigned long sched_switches(void) { return switches; }
void *sched_current_data(void) { return current ? current->data : NULL; }

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
    main->next = main;
    current = main;
}

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

    uint64_t top = ((uint64_t)t->stack + STACK_SIZE) & ~(uint64_t)0xF;
    uint64_t *sp = (uint64_t *)top;
    *--sp = (uint64_t)entry;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    *--sp = 0x202;
    t->rsp = (uint64_t)sp;

    t->next = current->next;
    current->next = t;
}

/* Create a ring-3 process thread: first switch drops to `entry` in user mode
 * on `ustack`, with its own kernel stack for traps. */
int thread_create_user(const char *name, uint64_t entry, uint64_t ustack, void *data, uint64_t cr3)
{
    struct thread *t = kmalloc(sizeof *t);
    uint8_t *ks = kmalloc(KSTACK_SIZE);
    t->stack = ks;
    t->name = name;
    t->id = next_id++;
    t->data = data;
    t->alive = 1;
    t->cr3 = cr3 ? cr3 : vmm_kernel_cr3();
    t->kstack_top = ((uint64_t)ks + KSTACK_SIZE) & ~(uint64_t)0xF;

    /* Hand-built kernel frame: context_switch "returns" into ring3_bootstrap
     * with entry in r15 and the user stack in r14. */
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

    t->next = current->next;
    current->next = t;
    return t->id;
}

void schedule(void)
{
    /* schedule() runs both cooperatively (IF=1) and from the timer IRQ (IF=0).
     * It MUST be atomic: if a timer tick re-entered it mid-switch it would save
     * the wrong stack into the wrong thread and corrupt the scheduler. Disable
     * interrupts across the critical section, then restore the caller's IF. */
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");

    if (current && current->next != current) {
        struct thread *prev = current;
        struct thread *next = current->next;
        current = next;
        switches++;
        if (next->kstack_top)
            tss_set_rsp0(next->kstack_top);
        if (next->cr3 && next->cr3 != prev->cr3)
            vmm_switch(next->cr3);          /* enter the next thread's address space */
        context_switch(&prev->rsp, next->rsp);
    }

    if (flags & 0x200)          /* restore IF only if the caller had it set */
        __asm__ volatile ("sti");
}

/* Remove the current thread from the ring and never return. */
void thread_exit(void)
{
    __asm__ volatile ("cli");      /* atomic: never reschedule a half-removed ring */
    static uint64_t discard;
    struct thread *dead = current;
    struct thread *next = current->next;

    if (next == dead)            /* nothing else to run; just stop */
        for (;;) __asm__ volatile ("hlt");

    struct thread *prev = dead;
    while (prev->next != dead)
        prev = prev->next;
    prev->next = next;

    current = next;
    switches++;
    dead->alive = 0;
    if (next->kstack_top)
        tss_set_rsp0(next->kstack_top);
    if (next->cr3 && next->cr3 != dead->cr3)
        vmm_switch(next->cr3);          /* leave the dying app's space */
    context_switch(&discard, next->rsp);
}
