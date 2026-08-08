#ifndef _PTHREAD_H
#define _PTHREAD_H

/* POSIX threads on LogitOS -- READ THIS BEFORE RELYING ON ANY OF IT.
 *
 * This header used to be eleven lines: a `typedef int pthread_mutex_t` and five
 * inline functions whose entire body was `return 0`. That was honest at the
 * time -- QuickJS takes one mutex to allocate class ids and the machine was
 * single-threaded, so a lock that does nothing is a correct lock. It is not
 * honest any more, because there are now real threads underneath (M30,
 * c/kernel/sched/uthread.c) and a mutex that returns 0 would be a data race.
 *
 * WHAT IS REAL:
 *   pthread_create/join/detach/self/equal/exit, and attributes far enough to
 *   set a stack size; mutexes (normal + recursive) and condition variables,
 *   both built on SYS_FUTEX so the uncontended path is one atomic instruction
 *   and NO syscall -- which matters more here than on Linux, because every
 *   syscall that is not BKL-free serialises the whole machine; pthread_once;
 *   pthread_key_* thread-specific data; sem_* (see <semaphore.h>);
 *   and `__thread`, which needs the linker script (see below).
 *
 * WHAT IS REFUSED, with ENOSYS, rather than stubbed to success:
 *   pthread_kill, pthread_cancel, pthread_setcancelstate/type, and
 *   pthread_testcancel. This kernel has no signal delivery at all -- see the
 *   long note in <signal.h> -- so there is no substrate under either one.
 *   Returning 0 from pthread_cancel would tell a caller its thread is stopping
 *   when nothing will ever stop it, and that is a worse answer than "no".
 *   pthread_sigmask succeeds because there is nothing that could fail: it
 *   manipulates a mask that nothing consults, exactly as sigprocmask does.
 *
 * WHAT IS NOT HERE AT ALL: pthread_rwlock_*, pthread_barrier_*, pthread_spin_*,
 * priority inheritance, and process-shared anything. None is needed by the
 * program this was built for and each would be an untested primitive; the futex
 * they would all sit on is in place, so adding one is a page of code in
 * c/apps/libc/src/pthread.c and no kernel change.
 *
 * `__thread` NEEDS THE LINKER SCRIPT. Link with
 *     -T c/apps/libc/logit_tls.ld
 * or thread-local variables have nowhere to live -- see that file for why the
 * bounds cannot be discovered any other way here. Without it, the pthread
 * runtime still works (pthread_getspecific and friends are stored in the TCB,
 * not in `__thread`), and the first TLS setup prints a diagnostic on stderr
 * rather than letting `__thread` writes land in the control block.
 *
 * fork() FROM A THREADED PROCESS is POSIX's rule, not a local quirk: only the
 * calling thread exists in the child, and a lock another thread held is locked
 * forever there. Async-signal-safe calls only, until exec -- and note that exec
 * from a MULTI-threaded process is refused by the kernel (it cannot stop the
 * other threads safely), so the usable shape is fork+exec from one thread.
 */

#include <stddef.h>
#include <time.h>
#include <sched.h>

/* A tid, which is what the kernel deals in. An integer identity is also what
 * CPython wants: it prints thread ids and defines PYTHREAD_INVALID_THREAD_ID as
 * (pthread_t)-1, both of which need a scalar. */
typedef unsigned long pthread_t;
typedef unsigned int  pthread_key_t;

#define PTHREAD_KEYS_MAX      64
#define PTHREAD_STACK_MIN     (16u * 1024)
#define PTHREAD_THREADS_MAX   64

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

#define PTHREAD_ONCE_INIT 0

typedef struct { size_t stacksize; int detachstate; int guardsize; } pthread_attr_t;
typedef struct { int type; int pshared; } pthread_mutexattr_t;
typedef struct { int clock; int pshared; } pthread_condattr_t;

/* The futex word is the WHOLE mutex. 0 = free, 1 = held with no waiter, 2 =
 * held and at least one thread is parked -- Drepper's three-state lock, which
 * is the design that makes the uncontended case a single compare-exchange with
 * no syscall in it. `owner` and `depth` exist only for RECURSIVE mutexes and
 * are untouched by a normal one. */
typedef struct {
    volatile int  futex;
    int           type;
    unsigned long owner;
    int           depth;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }

/* A sequence counter, and nothing else. Every waiter reads it before releasing
 * the mutex and waits for it to CHANGE, so a signal that lands in the window
 * between the read and the park is not lost -- the futex compare fails and the
 * wait returns immediately. See pthread_cond_wait() for what this costs
 * (no requeue: a broadcast wakes everyone, and they contend for the mutex). */
typedef struct { volatile unsigned seq; int clock; } pthread_cond_t;
#define PTHREAD_COND_INITIALIZER { 0, 0 }

typedef volatile int pthread_once_t;

/* --- threads ----------------------------------------------------------- */
int  pthread_create(pthread_t *th, const pthread_attr_t *attr,
                    void *(*start)(void *), void *arg);
int  pthread_join(pthread_t th, void **retval);
int  pthread_detach(pthread_t th);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);

int  pthread_attr_init(pthread_attr_t *a);
int  pthread_attr_destroy(pthread_attr_t *a);
int  pthread_attr_setstacksize(pthread_attr_t *a, size_t sz);
int  pthread_attr_getstacksize(const pthread_attr_t *a, size_t *sz);
int  pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int  pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int  pthread_attr_setscope(pthread_attr_t *a, int scope);
int  pthread_attr_setguardsize(pthread_attr_t *a, size_t sz);
int  pthread_getattr_np(pthread_t th, pthread_attr_t *a);
int  pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *sz);

/* --- mutexes ----------------------------------------------------------- */
int  pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int  pthread_mutex_destroy(pthread_mutex_t *m);
int  pthread_mutex_lock(pthread_mutex_t *m);
int  pthread_mutex_trylock(pthread_mutex_t *m);
int  pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs);
int  pthread_mutex_unlock(pthread_mutex_t *m);
int  pthread_mutexattr_init(pthread_mutexattr_t *a);
int  pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int  pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int  pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);

/* --- condition variables ------------------------------------------------ */
int  pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int  pthread_cond_destroy(pthread_cond_t *c);
int  pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int  pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *abs);
int  pthread_cond_signal(pthread_cond_t *c);
int  pthread_cond_broadcast(pthread_cond_t *c);
int  pthread_condattr_init(pthread_condattr_t *a);
int  pthread_condattr_destroy(pthread_condattr_t *a);
int  pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clk);
int  pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clk);

/* --- once + thread-specific data ---------------------------------------- */
int  pthread_once(pthread_once_t *once, void (*init)(void));
int  pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int  pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int  pthread_setspecific(pthread_key_t key, const void *value);

/* --- refused, and they say so ------------------------------------------- */
int  pthread_kill(pthread_t th, int sig);            /* ENOSYS: no signals here */
int  pthread_cancel(pthread_t th);                   /* ENOSYS: nothing to cancel with */
int  pthread_setcancelstate(int state, int *old);    /* ENOSYS */
int  pthread_setcanceltype(int type, int *old);      /* ENOSYS */
void pthread_testcancel(void);                       /* no-op: never a cancel point */
int  pthread_sigmask(int how, const void *set, void *oldset);

#endif /* _PTHREAD_H */
