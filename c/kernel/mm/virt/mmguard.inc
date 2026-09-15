/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Kept in vmm's translation unit so the existing real-MM host harnesses link
 * the SAME lifetime/locking implementation, without hand-copied source lists. */
#define MM_LOCK_COUNT 256u
#ifdef MM_HOSTTEST
static _Thread_local unsigned mm_host_owner;
struct mm_op_lock { uintptr_t owner; unsigned depth; };
static struct mm_op_lock mm_op_locks[MM_LOCK_COUNT + 1];
static uintptr_t mm_owner(void) { return (uintptr_t)&mm_host_owner; }
#else
#include "wait.h"
#include "percpu.h"
struct mm_op_lock { struct waitq q; uintptr_t owner; unsigned depth; };
static struct mm_op_lock mm_op_locks[MM_LOCK_COUNT + 1] = {
    [0 ... MM_LOCK_COUNT] = { WAITQ_INIT, 0, 0 }
};
static uintptr_t mm_owner(void)
{
    struct thread *t = sched_current_thread();
    return t ? (uintptr_t)t : (uintptr_t)(this_cpu()->index + 1);
}
#endif
unsigned mm_guard_index(uint64_t cr3)
{
#ifdef MM_GLOBAL_AS_LOCK
    (void)cr3; return 0; /* negative control: emulate global MM serialization */
#else
    uint64_t page = (cr3 & MM_PTE_ADDR) >> 12;
    return (unsigned)(page ^ (page >> 8)) & (MM_LOCK_COUNT - 1);
#endif
}
static struct mm_guard mm_guard_take_slot(unsigned slot, int wait)
{
    struct mm_guard g = { slot, 0 };
#ifdef MM_NO_AS_LOCK
    if (slot < MM_LOCK_COUNT) return g;
#endif
#ifdef MM_NO_KERNEL_MAP_LOCK
    if (slot == MM_LOCK_COUNT) return g;
#endif
    struct mm_op_lock *l = &mm_op_locks[g.slot];
    uintptr_t owner = mm_owner();
#ifdef MM_HOSTTEST
    for (;;) {
        uintptr_t who = __atomic_load_n(&l->owner, __ATOMIC_ACQUIRE);
        if (who == owner) { l->depth++; g.held=1; return g; }
        uintptr_t zero = 0;
        if (!who && __atomic_compare_exchange_n(&l->owner,&zero,owner,0,
                                     __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
            l->depth=1; g.held=1; return g;
        }
        if (!wait) return g;
        __asm__ volatile ("" ::: "memory");
    }
#else
    uint64_t f = spin_lock_irqsave(&l->q.lock);
    while (l->owner && l->owner != owner) {
        if (!wait) { spin_unlock_irqrestore(&l->q.lock,f); return g; }
        struct waiter w;
        waitq_enqueue(&l->q,&w);
        sched_block_self_unlock(&l->q.lock,f);
        f=spin_lock_irqsave(&l->q.lock);
        waitq_dequeue(&l->q,&w);
    }
    l->owner=owner; l->depth++; g.held=1;
    spin_unlock_irqrestore(&l->q.lock,f);
    return g;
#endif
}
struct mm_guard mm_guard_start(uint64_t cr3) { return mm_guard_take_slot(mm_guard_index(cr3),1); }
struct mm_guard mm_guard_try(uint64_t cr3) { return mm_guard_take_slot(mm_guard_index(cr3),0); }
struct mm_guard mm_kernel_guard_start(void) { return mm_guard_take_slot(MM_LOCK_COUNT,1); }
struct mm_guard mm_kernel_guard_try(void) { return mm_guard_take_slot(MM_LOCK_COUNT,0); }
void mm_guard_end(struct mm_guard *g)
{
    if (!g || !g->held) return;
    struct mm_op_lock *l=&mm_op_locks[g->slot];
#ifdef MM_HOSTTEST
    if (!--l->depth) __atomic_store_n(&l->owner,0,__ATOMIC_RELEASE);
#else
    uint64_t f=spin_lock_irqsave(&l->q.lock);
    int wake = !--l->depth;
    if (wake) l->owner=0;
    spin_unlock_irqrestore(&l->q.lock,f);
    if (wake) waitq_wake_all(&l->q);
#endif
    g->held=0;
}
/* The AS operation lock orders retire with remote copies. A queued remote
 * reader must recheck liveness after acquiring, before dereferencing CR3.
 * Entries are atomic because different address-space locks publish in parallel.
 * The process/ptrace lifetime lock still identifies WHICH process owns a CR3.
 * Correction: publish/retire now also take the dedicated kernel-map owner so
 * shared-root propagation can safely enumerate physical roots. Atomic reads
 * remain necessary for mm_space_live callers that own only their AS guard. */
static uint64_t mm_live_spaces[256];
int mm_space_live(uint64_t cr3)
{
    if (!cr3) return 0;
    for (unsigned i=0;i<256;i++)
        if (__atomic_load_n(&mm_live_spaces[i],__ATOMIC_ACQUIRE)==cr3) return 1;
    return 0;
}
int mm_space_publish(uint64_t cr3)
{
    MM_KERNEL_GUARD;
    if (mm_space_live(cr3)) return 1;
    for (unsigned i=0;i<256;i++) {
        uint64_t zero=0;
        if (__atomic_compare_exchange_n(&mm_live_spaces[i],&zero,cr3,0,
                                         __ATOMIC_RELEASE,__ATOMIC_RELAXED)) return 1;
    }
    return 0;
}
void mm_space_retire(uint64_t cr3)
{
    MM_KERNEL_GUARD;
    for (unsigned i=0;i<256;i++) {
        uint64_t old=cr3;
        if (__atomic_compare_exchange_n(&mm_live_spaces[i],&old,0,0,
                                         __ATOMIC_RELEASE,__ATOMIC_RELAXED)) return;
    }
}
