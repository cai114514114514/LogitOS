#ifndef LOGIT_SCHED_H
#define LOGIT_SCHED_H

#include <stdint.h>
#include "spinlock.h"

/* Cooperative-capable, preemptive round-robin kernel thread scheduler. */

struct registers;   /* interrupts.h */
struct thread;      /* opaque outside sched.c */

/* Thread run states. A THREAD_BLOCKED thread is UNLINKED from the run ring: the
 * pick loop never visits it, so it consumes no scheduler time at all -- as
 * opposed to the pre-M27 model where every "wait" was a spin or a poll and the
 * waiter kept being dispatched only to re-test its condition. */
#define THREAD_READY    0
#define THREAD_BLOCKED  1

void sched_init(void);                                  /* adopt the boot context as "main" */
void thread_create(void (*entry)(void), const char *name);
int  thread_create_user(const char *name, uint64_t entry, uint64_t ustack, void *data, uint64_t cr3);
/* Create a child thread that resumes from the parent's int 0x80 frame `r`
 * (returning 0 in the child) in address space `cr3`. Used by fork(). */
int  thread_fork(const char *name, struct registers *r, void *data, uint64_t cr3);
void thread_exit(void);                                 /* end the current thread */
void schedule(void);                                    /* switch to the next ready thread */
void bkl_hlt_wait(void);                                /* blocking waits: drop BKL + hlt, re-acquire */
void thread_create_idle(int idx);                       /* SMP: build core `idx`'s idle thread */
void sched_become_idle(void);                           /* SMP: AP enters its idle loop (never returns) */
void sched_unlock_new_thread(void);                     /* new ring-3/fork first-run: drop g_sched_lock + BKL */
void kthread_bootstrap(void);                           /* kernel-thread first-run trampoline */

unsigned long sched_switches(void);                     /* total context switches so far */
void *sched_current_data(void);                         /* current thread's payload */
uint64_t sched_current_cr3(void);                        /* active thread address space */

/* ------------------------------------------------------------------------
 * M27 blocking core: park/unpark.
 *
 * These are the ONLY two calls the sleep primitives in c/kernel/core/wait.c sit
 * on. Everything else (waitqueue, mutex, semaphore, condvar, rwlock, workqueue)
 * is built out of them.
 *
 * THE LOST-WAKEUP RACE, and how it is closed
 * ------------------------------------------
 * The classic bug is: test the condition, find it false, and get preempted
 * before actually parking -- the waker sets the condition and issues its wake in
 * that window, finds nobody parked, and the sleeper then parks forever.
 *
 * sched_block_self_unlock() closes it by making "release the lock that guards
 * the condition" and "become unrunnable" ATOMIC with respect to any waker:
 *
 *   - the caller must already hold `outer` (via spin_lock_irqsave) and must have
 *     tested the condition UNDER `outer`;
 *   - we take g_sched_lock, mark ourselves THREAD_BLOCKED and unlink from the
 *     run ring, and only THEN release `outer`;
 *   - g_sched_lock stays held across context_switch and is released by the
 *     INCOMING thread, i.e. after our rsp has been saved.
 *
 * So a waker is in exactly one of two positions and never in between:
 *   (a) it got `outer` before us      -> it set the condition first, so our
 *                                        test under `outer` saw it and we never
 *                                        parked;
 *   (b) it got `outer` after us       -> we are already marked THREAD_BLOCKED
 *                                        and queued, its sched_wake() waits on
 *                                        g_sched_lock until our context_switch
 *                                        has completed, then relinks us.
 * There is no third case, so there is no window in which a wake can be observed
 * by neither path. The price is the contract: the condition MUST be evaluated
 * under the same lock the sleeper hands to sched_block_self_unlock(), the same
 * way pthread_cond_wait() demands the predicate be evaluated under its mutex.
 *
 * CONTEXT RULES
 *   - Callers must hold the BKL (it is dropped across the switch and re-acquired
 *     on resume, exactly as schedule() does). A BKL-free syscall must not block.
 *   - Never callable from interrupt context, and never with any spinlock held
 *     other than `outer`: the lock would stay held across the switch.
 *   - The idle thread never parks (the call degrades to a plain unlock).
 *   - Wakeups may be spurious. Every sleep site is a `while (!cond)` loop.
 */
struct thread *sched_current_thread(void);
void sched_block_self_unlock(spinlock_t *outer, uint64_t flags);
/* Same, but also wakes when timer_ticks() reaches `deadline`. Returns 1 if woken
 * by sched_wake(), 0 if the deadline expired. */
int  sched_block_self_unlock_until(spinlock_t *outer, uint64_t flags, uint64_t deadline);
/* Make `t` runnable again. Safe from interrupt context and from any core.
 * Returns 1 if this call actually unparked the thread, 0 if it was not parked
 * (already runnable, or someone else's wake got there first). Takes only
 * g_sched_lock, so the lock order is always <caller's lock> -> g_sched_lock. */
int  sched_wake(struct thread *t);
/* Timer-driven deadline expiry. Called from the timer IRQ BEFORE the BKL is
 * acquired -- like timer_tick(), a sleeper's deadline must not be able to queue
 * behind a BKL held by a thread that is itself waiting for that deadline. */
void sched_timer_expire(void);

/* ------------------------------------------------------------------------
 * M30 threads: reaching a thread by ID, and the per-thread TLS pointer.
 *
 * `struct thread` is opaque outside sched.c and there was no way to name one
 * from anywhere else -- proc.c's kill path says exactly that, as the reason a
 * process parked on a wait queue could not be woken. An integer id can cross
 * the file boundary and cannot dangle, so these take one and do the lookup
 * INSIDE the lock; see the comment above by_id_locked() for why splitting that
 * into "find" then "use" would be a use-after-free.
 */
int  sched_wake_id(int tid);        /* 1 if this call unparked it, else 0 */
int  sched_thread_alive(int tid);
int  sched_current_tid(void);
uint64_t sched_current_fsbase(void);
/* Set the CALLING thread's %fs base (its thread-local storage pointer) in both
 * the descriptor and the MSR. See struct thread::fsbase in sched.c. */
void sched_set_fsbase(uint64_t fsbase);

/* Tell the scheduler that a mapping was removed from SOME address space, so
 * every core reloads CR3 on its way into the next thread. The lazy half of the
 * TLB shootdown; the long comment above g_tlb_gen in sched.c says why the eager
 * half (tlb_flush_all) is not sufficient on its own now that two threads can
 * share a cr3 and therefore skip the reload that used to clean up after it. */
void sched_tlb_gen_bump(void);
/* Resync THIS core immediately. Called from the syscall gate, which is where it
 * has to be: a core cannot obtain a recycled mapping without a syscall, and the
 * whole reuse-then-write sequence happens inside one pass of a create loop --
 * far inside a single timer tick, so the context-switch sites alone are orders
 * of magnitude too slow. See sched_tlb_gen_check() in sched.c. */
void sched_tlb_gen_check(void);

unsigned long sched_slices_of(struct thread *t);  /* dispatches: the sleep-vs-spin metric */
int  sched_thread_id(struct thread *t);
unsigned long sched_blocked_count(void);          /* threads currently parked */
/* Passes through bkl_hlt_wait(): the count of times some thread POLLED instead
 * of blocking. The number M27 exists to drive to zero; see sched.c. */
unsigned long sched_hlt_waits(void);

/* Walk the poll-pass attribution: slot i -> (return address, count), 0 when
 * exhausted. See the histogram in sched.c -- "something is still polling" is
 * not an actionable sentence, and this is what makes it one. */
int sched_hlt_who(int i, unsigned long *ra, unsigned long *n);

/* ------------------------------------------------------------------------
 * Per-thread CPU time + RLIMIT_CPU (SYS_RUSAGE). See the long comment on
 * SYS_RUSAGE in include/abi/logit_abi.h for the ABI and the design argument,
 * and the "RLIMIT_CPU ENFORCEMENT" comment above sched_cpu_tick_check() in
 * sched.c for why enforcement is a separate entry point from the accounting
 * one, called from a different file.
 */
uint64_t sched_cpu_ns_self(void);        /* the calling thread's own ns, folded to now */
uint64_t sched_cpu_ns_proc(void);        /* + every still-alive sibling thread's ns */
int      sched_cpu_limit_set(long seconds);   /* <0 = clear; -> 0, or -1 (no current thread) */
long     sched_cpu_limit_get_s(void);         /* -> seconds, or -1 = unlimited */
long     sched_rusage_syscall(long cmd, long a, long b);   /* SYS_RUSAGE dispatch */

/* Timer-tick RLIMIT_CPU check. Called from c/kernel/cpu/interrupts.c, in the
 * same non-nested/BKL-held window as ksig_tick(), BEFORE schedule() -- not
 * from inside schedule() itself; see the long comment above this function's
 * definition for why that placement is load-bearing and not a style choice.
 * Returns a pid to ksig_post(pid, LOGIT_SIGXCPU), or 0. */
int sched_cpu_tick_check(void);

#endif /* LOGIT_SCHED_H */
