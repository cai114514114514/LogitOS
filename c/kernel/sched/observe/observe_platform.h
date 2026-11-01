#ifndef LOGIT_OBSERVE_PLATFORM_H
#define LOGIT_OBSERVE_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include "../../../../include/abi/logit_sched_observe.h"

/* The hardware and kernel services the scheduler observer needs, gathered in
 * one place so that the observer's own logic (flight recorder, input ledger,
 * coalesced groups, run control) can be compiled and tested on a development
 * machine.  The kernel implements these in observe_platform.c; the host test
 * (tests/sched/observe/support.c) supplies fixtures with the same names and
 * nothing else, so every decision the observer makes runs as shipped code. */

/* Bit 9 of the x86 flags register: interrupts enabled
 * (Intel SDM 325384-093US, Vol. 3A section 2.3, system flags in EFLAGS). */
#define OBSERVE_EFLAGS_INTERRUPT_ENABLE (1u << 9)

/* Disable interrupts on this CPU and return the previous flags.  This is the
 * observer's own copy of the idea behind spin_lock_irqsave(), kept apart on
 * purpose: spin_lock_irqsave() is itself measured, and measuring the measurer
 * would recurse. */
uint64_t observe_irq_save(void);

/* Put the interrupt flag back exactly as observe_irq_save() found it.  The
 * caller may already have had interrupts off, and must not be made preemptible
 * by tracing. */
void observe_irq_restore(uint64_t saved_flags);

/* Kernel monotonic clock in nanoseconds.  Its cross-CPU clamp is not evidence
 * that the CPUs' TSCs agree, so the raw TSC is always recorded alongside. */
uint64_t observe_monotonic_ns(void);

/* Serialized TSC read: the raw counter, ordered against surrounding loads. */
uint64_t observe_read_tsc(void);

/* Index of the CPU executing this call.  Always asked afresh, because a thread
 * may migrate between two calls. */
unsigned observe_current_cpu(void);

/* Zeroed array allocation with overflow checking.  Only used from process
 * context when a run begins; interrupt paths never allocate. */
void *observe_allocate_zeroed(size_t count, size_t element_size);
void observe_free(void *allocation);

/* Access to the system-call caller's memory and identity.  The copy functions
 * return 0 on success. */
int observe_copy_from_caller(void *destination, uint64_t caller_address, size_t bytes);
int observe_copy_to_caller(uint64_t caller_address, const void *source, size_t bytes);
int observe_caller_range_writable(uint64_t caller_address, size_t bytes);
void observe_caller_identity(uint32_t *user_id, uint32_t *process_id);

/* Bit mask of the CPUs that are running; bit n stands for CPU n. */
uint32_t observe_online_cpu_mask(void);

/* A small spin lock for the observer's own state.  It deliberately bypasses
 * spin_lock_irqsave() (see observe_irq_save) and never records or prints, so
 * code holding it can be called from interrupt context.  Critical sections are
 * a few instructions long; nobody waits for anything while holding it. */
struct observe_spinlock {
    uint32_t held;
};

/* Take the lock with interrupts off, so an interrupt on this CPU cannot spin
 * forever on a lock its own interrupted code holds.  Returns the flags to pass
 * to observe_lock_release(). */
static inline uint64_t observe_lock_acquire(struct observe_spinlock *lock)
{
    uint64_t saved_flags = observe_irq_save();
    while (__atomic_exchange_n(&lock->held, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("" ::: "memory");
    }
    return saved_flags;
}

/* Drop the lock first, then restore interrupts, so no interrupt can arrive
 * between the two and find the lock still held by the code it interrupted. */
static inline void observe_lock_release(struct observe_spinlock *lock, uint64_t saved_flags)
{
    __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE);
    observe_irq_restore(saved_flags);
}

#ifdef SCHED_OBSERVE_GUEST
/* Test image only: the one kernel thread that runs the built-in diagnostic
 * program may pass kernel-owned buffers in its requests, so the program can
 * exercise the real system-call dispatcher without a user process.  Every other
 * caller still goes through the checked user-memory copy. */
void observe_platform_set_test_thread(int thread_id);
#endif

#ifdef SCHED_OBSERVE_HOST
/* Points in the observer's code where the host test can pause the running
 * operation and run another one, to reproduce a specific interleaving
 * deterministically (for example a reader meeting a half-written slot). */
enum observe_test_point {
    OBSERVE_POINT_WRITER_SLOT_MARKED_BUSY = 1,  /* slot version is odd; payload not yet written */
    OBSERVE_POINT_WRITER_HALF_WRITTEN,          /* about half of the payload words are written */
    OBSERVE_POINT_READER_VERSION_CHECKED,       /* reader saw a complete version; payload not yet copied */
    OBSERVE_POINT_READER_PAYLOAD_COPIED,        /* reader copied the payload; version not yet rechecked */
    OBSERVE_POINT_OUTCOME_BEFORE_DEADLINE_CHECK,/* settling an input, just before comparing deadlines */
    OBSERVE_POINT_IRQ_DISABLED_BEGUN,           /* interrupts-off timing has just started */
    OBSERVE_POINT_GROUP_NODE_RELEASED           /* one group node was taken off the release queue */
};
void observe_test_point(unsigned point);
#define OBSERVE_TEST_POINT(point) observe_test_point(point)
#else
#define OBSERVE_TEST_POINT(point) ((void)0)
#endif

/* Development-machine harnesses that include scheduler code without the
 * observer see these as plain no-ops, so they neither link nor measure it. */
#if __STDC_HOSTED__ && !defined(SCHED_OBSERVE_HOST)
#define observe_read_tsc() 0ull
#define observe_monotonic_ns() 0ull
#endif

#endif
