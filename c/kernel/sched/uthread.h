#ifndef LOGIT_UTHREAD_H
#define LOGIT_UTHREAD_H

#include <stdint.h>

/* ===========================================================================
 * M30 -- ring-3 threads. Two execution flows in ONE address space.
 *
 * WHAT WAS ALREADY HERE, and what was missing. c/kernel/sched/sched.c has been
 * able to build a ring-3 thread since M6 (thread_create_user: its own kernel
 * stack, its own TSS rsp0 set at every dispatch) and M25 made the scheduler
 * per-core. What did not exist was a DOOR from ring 3 -- SYS_SPAWN 63 makes a
 * PROCESS, with its own address space -- and the per-thread lifecycle a
 * threading library needs: an exit that ends one flow without tearing down the
 * process, a join that delivers the return value, a detach that releases the
 * stack with nobody waiting, and somewhere to put the thread-local storage
 * pointer.
 *
 * A THREAD IS A struct thread WHOSE ->data POINTS AT AN EXISTING struct proc.
 * That one sentence is the whole model. pid, cwd, the fd table and the address
 * space are shared because they are literally the same object, so every syscall
 * in the tree that resolves its caller through proc_current() -- which is all of
 * them -- keeps working with no change. What is per-thread is what struct thread
 * already held (kernel stack, saved rsp, run state) plus the two things added
 * for this: the %fs base, and the descriptor below.
 *
 * WHY A DESCRIPTOR SEPARATE FROM struct thread. Join needs a place to put the
 * return value that OUTLIVES the thread -- a joiner may not arrive until long
 * after the thread is gone, and schedule() kfree()s the struct thread on its
 * next pass through the dead list. So the exit value, the detach bit and the
 * stack to release live here, in a table with its own lifetime, and struct
 * thread is referred to only by integer id.
 *
 * THE BIG KERNEL LOCK, said out loud. Two ring-3 threads of one process, both
 * entering the kernel, is exactly the case the BKL serialises -- so two threads
 * making syscalls do NOT run in parallel, and the concurrency proof for this
 * work (c/apps/coreutils/thrtest.c) measures RING-3 work, which does run in
 * parallel, not syscall throughput, which does not. That is a property of the
 * BKL and not of threads; it is the same wall SYS_KHEAP_STRESS was made
 * BKL-free to get past, and the same fix would work here.
 *
 * WHAT IS DELIBERATELY REFUSED, rather than stubbed to success:
 *   - pthread_kill and cancellation. There is no signal delivery in this kernel
 *     (c/apps/libc/include/signal.h is explicit about it), so there is no
 *     substrate under either. They return ENOSYS.
 *   - execve from a multi-threaded process. POSIX says the other threads
 *     vanish; doing that safely means stopping threads that may be anywhere,
 *     which is the same problem proc_kill() documents and does not solve. The
 *     syscall gate refuses it with THR_E_INVAL instead of producing a process
 *     whose surviving threads run in an address space that was replaced under
 *     them.
 *   - fork from a multi-threaded process is ALLOWED and behaves as POSIX
 *     specifies: only the calling thread exists in the child. See proc_fork().
 * =========================================================================== */

struct proc;

/* Lazily adopt the calling thread as its process's first thread. Called from
 * every entry point below, so no launch path (wm_launch, proc_spawn, execve)
 * needs to know threads exist. Returns the tid, or -1 if there is no proc. */
int  uthread_self(void);

/* Live (running) threads of `pid`. A thread that has exited and is waiting to
 * be joined is not live -- it holds a descriptor, not a core. */
int  uthread_proc_live(int pid);

/* Release the CALLING thread's descriptor: hand the return value to any joiner,
 * unmap the stack the kernel owns, and free the descriptor outright if the
 * thread was detached. Idempotent -- proc_exit() and SYS_THREAD_EXIT both reach
 * it and either may be first. */
void uthread_release_self(uint64_t retval);

/* Mark every thread of `pid` for exit with `code` and wake the parked ones, so
 * that a thread which called exit() takes its siblings with it. The siblings
 * die at their next kernel entry (uthread_exit_check, from the syscall gate),
 * exactly as a SYS_KILL victim does and for the identical reason: tearing a
 * thread down from another thread's context would pull the stack out from under
 * whatever it is doing. */
int  uthread_proc_kill(int pid, int code);   /* -> the EFFECTIVE exit code: the
                                              * first caller's, not necessarily
                                              * this caller's */

/* Free every descriptor belonging to `pid`. Called once, by the last thread
 * out, from proc_exit()'s teardown branch. */
void uthread_proc_reap(int pid);

/* The syscall-gate check for "my process is exiting". Gated by a global counter
 * so the cost on a machine where nothing is exiting is one load and one
 * not-taken branch -- the same discipline as proc_kill_armed(). Does not return
 * if the caller is a doomed thread. */
int  uthread_exit_armed(void);
void uthread_exit_check(void);

/* SYS_THREAD_* / SYS_FUTEX. Forwarded whole from c/kernel/exec/syscall.c for
 * the reason mm_syscall() and proc_syscall() give: which argument is a user
 * pointer and what it means are facts about threads, and they belong beside the
 * table that knows them. */
long uthread_syscall(long num, long a, long b, long c);

#endif /* LOGIT_UTHREAD_H */
