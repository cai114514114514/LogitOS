/* ISO C11 thread adapters. The only shape pthread cannot represent directly
 * is an int-returning start routine, so thrd_create owns one tiny trampoline;
 * synchronization and TSS remain the exact pthread objects underneath. */
#include <threads.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>

static int thread_result(int rc)
{
    if (rc == 0) return thrd_success;
    if (rc == ETIMEDOUT) return thrd_timedout;
    if (rc == EBUSY) return thrd_busy;
    if (rc == ENOMEM || rc == EAGAIN) return thrd_nomem;
    return thrd_error;
}

struct start_call { thrd_start_t func; void *arg; };

static void *start_thunk(void *opaque)
{
    struct start_call call = *(struct start_call *)opaque;
    free(opaque);
    return (void *)(intptr_t)call.func(call.arg);
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    if (!thr || !func) return thrd_error;
    struct start_call *call = malloc(sizeof *call);
    if (!call) return thrd_nomem;
    call->func = func;
    call->arg = arg;
    int rc = pthread_create(thr, NULL, start_thunk, call);
    if (rc) free(call);
    return thread_result(rc);
}

int thrd_equal(thrd_t lhs, thrd_t rhs) { return pthread_equal(lhs, rhs); }
thrd_t thrd_current(void) { return pthread_self(); }
int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{ return nanosleep(duration, remaining) == 0 ? 0 : -1; }
void thrd_yield(void) { (void)sched_yield(); }
void thrd_exit(int res) { pthread_exit((void *)(intptr_t)res); }
int thrd_detach(thrd_t thr) { return thread_result(pthread_detach(thr)); }

int thrd_join(thrd_t thr, int *res)
{
    void *value = NULL;
    int rc = pthread_join(thr, &value);
    if (!rc && res) *res = (int)(intptr_t)value;
    return thread_result(rc);
}

int mtx_init(mtx_t *mutex, int type)
{
    if (!mutex || (type & ~(mtx_recursive | mtx_timed))) return thrd_error;
    pthread_mutexattr_t attr;
    int rc = pthread_mutexattr_init(&attr);
    if (!rc && (type & mtx_recursive))
        rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (!rc) rc = pthread_mutex_init(mutex, &attr);
    (void)pthread_mutexattr_destroy(&attr);
    return thread_result(rc);
}

void mtx_destroy(mtx_t *mutex) { (void)pthread_mutex_destroy(mutex); }
int mtx_lock(mtx_t *mutex) { return thread_result(pthread_mutex_lock(mutex)); }
int mtx_timedlock(mtx_t *mutex, const struct timespec *time_point)
{ return thread_result(pthread_mutex_timedlock(mutex, time_point)); }
int mtx_trylock(mtx_t *mutex) { return thread_result(pthread_mutex_trylock(mutex)); }
int mtx_unlock(mtx_t *mutex) { return thread_result(pthread_mutex_unlock(mutex)); }

int cnd_init(cnd_t *cond) { return thread_result(pthread_cond_init(cond, NULL)); }
void cnd_destroy(cnd_t *cond) { (void)pthread_cond_destroy(cond); }
int cnd_signal(cnd_t *cond) { return thread_result(pthread_cond_signal(cond)); }
int cnd_broadcast(cnd_t *cond) { return thread_result(pthread_cond_broadcast(cond)); }
int cnd_wait(cnd_t *cond, mtx_t *mutex)
{ return thread_result(pthread_cond_wait(cond, mutex)); }
int cnd_timedwait(cnd_t *cond, mtx_t *mutex,
                  const struct timespec *time_point)
{ return thread_result(pthread_cond_timedwait(cond, mutex, time_point)); }

int tss_create(tss_t *key, tss_dtor_t destructor)
{ return thread_result(pthread_key_create(key, destructor)); }
void tss_delete(tss_t key) { (void)pthread_key_delete(key); }
void *tss_get(tss_t key) { return pthread_getspecific(key); }
int tss_set(tss_t key, void *value)
{ return thread_result(pthread_setspecific(key, value)); }

void call_once(once_flag *flag, void (*func)(void))
{ (void)pthread_once(flag, func); }
