#include <stdint.h>
#include <stddef.h>
#include "sched.h"
#include "kheap.h"

#define STACK_SIZE 16384

struct thread {
    uint64_t rsp;            /* saved stack pointer (must be first field) */
    struct thread *next;     /* circular ready ring */
    void *stack;
    const char *name;
    int id;
};

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

static struct thread *current = NULL;
static int next_id = 0;
static volatile unsigned long switches = 0;

unsigned long sched_switches(void) { return switches; }

void sched_init(void)
{
    struct thread *main = kmalloc(sizeof *main);
    main->rsp   = 0;             /* filled in on the first switch away */
    main->stack = NULL;
    main->name  = "main";
    main->id    = next_id++;
    main->next  = main;          /* a ring of one */
    current = main;
}

void thread_create(void (*entry)(void), const char *name)
{
    struct thread *t = kmalloc(sizeof *t);
    t->stack = kmalloc(STACK_SIZE);
    t->name  = name;
    t->id    = next_id++;

    /* Hand-build a stack frame that context_switch will "return" into:
     * [rflags][r15][r14][r13][r12][rbx][rbp][entry]  (low -> high). */
    uint64_t top = ((uint64_t)t->stack + STACK_SIZE) & ~(uint64_t)0xF;
    uint64_t *sp = (uint64_t *)top;
    *--sp = (uint64_t)entry;     /* ret target */
    *--sp = 0;                   /* rbp */
    *--sp = 0;                   /* rbx */
    *--sp = 0;                   /* r12 */
    *--sp = 0;                   /* r13 */
    *--sp = 0;                   /* r14 */
    *--sp = 0;                   /* r15 */
    *--sp = 0x202;               /* rflags: IF set + reserved bit */
    t->rsp = (uint64_t)sp;

    /* Splice into the ring just after the current thread. */
    t->next = current->next;
    current->next = t;
}

void schedule(void)
{
    if (!current || current->next == current)
        return;                  /* nothing else runnable */

    struct thread *prev = current;
    struct thread *next = current->next;
    current = next;
    switches++;
    context_switch(&prev->rsp, next->rsp);
}
