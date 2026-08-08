#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

/* POSIX unnamed semaphores, over the same SYS_FUTEX everything else in
 * <pthread.h> sits on. The counter IS the futex word, so sem_post on a
 * semaphore nobody is waiting on is one atomic add plus one syscall that finds
 * an empty queue, and sem_wait on a positive count is one compare-exchange and
 * no syscall.
 *
 * WHY THIS EXISTS AT ALL when mutex+cond would do: CPython's
 * Python/thread_pthread.h prefers sem_t for PyThread_acquire_lock when
 * _POSIX_SEMAPHORES is defined, and falls back to a mutex+condvar pair
 * otherwise. Both paths work here; the semaphore one is a smaller object and
 * one fewer wake per acquire.
 *
 * NAMED semaphores (sem_open/sem_close/sem_unlink) are NOT here. They are
 * inter-PROCESS objects living in a filesystem namespace, and this machine has
 * no shared memory between address spaces at all -- there is no MAP_SHARED and
 * no shm (see c/kernel/mm/mmsys.c). sem_init with pshared != 0 fails with
 * ENOSYS for the same reason, rather than quietly giving back a semaphore that
 * only works within one process.
 */

#include <time.h>

typedef struct { volatile int value; } sem_t;

#define SEM_VALUE_MAX 0x7fffffff

int sem_init(sem_t *s, int pshared, unsigned value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abstime);
int sem_clockwait(sem_t *s, clockid_t clk, const struct timespec *abstime);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);

#endif /* _SEMAPHORE_H */
