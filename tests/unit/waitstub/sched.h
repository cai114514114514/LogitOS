/* Host-test stub for the scheduler's park/unpark pair.
 *
 * The include_next below is load-bearing: this directory is first on the include
 * path, so glibc's <pthread.h> resolves its own `#include <sched.h>` to THIS
 * file. Pulling the real one through first keeps cpu_set_t and friends defined;
 * without it the whole host build collapses inside pthread.h.
 *
 * The stub deliberately has NO safety net of its own -- no wakeup token, no
 * "wake arrived early" counter. A wakeup delivered to a thread that has not yet
 * parked is simply LOST here. That is the point: every lost-wakeup result in
 * wait_test.c is then a statement about wait.c's own ordering, not about
 * something the harness quietly absorbed.
 *
 * The one thing the stub DOES reproduce faithfully is the kernel's ordering
 * contract (see sched.h's block_self comment): the caller's lock is released
 * only after the thread is marked parked and while the per-thread lock a waker
 * must take is still held. Building with -DWAIT_NEGCTRL inverts exactly that one
 * ordering and nothing else.
 */
#include_next <sched.h>

#ifndef WAITSTUB_SCHED_H
#define WAITSTUB_SCHED_H

#include <stdint.h>
#include "spinlock.h"

#define THREAD_READY    0
#define THREAD_BLOCKED  1

struct thread;

struct thread *sched_current_thread(void);
void sched_block_self_unlock(spinlock_t *outer, uint64_t flags);
int  sched_block_self_unlock_until(spinlock_t *outer, uint64_t flags, uint64_t deadline);
int  sched_wake(struct thread *t);
unsigned long sched_slices_of(struct thread *t);

#endif
