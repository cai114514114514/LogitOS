#ifndef _THREADS_H
#define _THREADS_H

/* ISO C11 threads, backed by LogitOS's real pthread/futex implementation.
 * Keeping the public types ABI-identical to their pthread counterparts lets
 * the adapter stay allocation-free except for thrd_create's int-return shim. */
#include <pthread.h>
#include <time.h>

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef pthread_key_t tss_t;
typedef pthread_once_t once_flag;

typedef int  (*thrd_start_t)(void *);
typedef void (*tss_dtor_t)(void *);

#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#define TSS_DTOR_ITERATIONS 4

enum {
    thrd_success = 0,
    thrd_nomem,
    thrd_timedout,
    thrd_busy,
    thrd_error
};

enum {
    mtx_plain = 0,
    mtx_recursive = 1,
    mtx_timed = 2
};

int    thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int    thrd_equal(thrd_t lhs, thrd_t rhs);
thrd_t thrd_current(void);
int    thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void   thrd_yield(void);
void   thrd_exit(int res) __attribute__((noreturn));
int    thrd_detach(thrd_t thr);
int    thrd_join(thrd_t thr, int *res);

int  mtx_init(mtx_t *mutex, int type);
void mtx_destroy(mtx_t *mutex);
int  mtx_lock(mtx_t *mutex);
int  mtx_timedlock(mtx_t *mutex, const struct timespec *time_point);
int  mtx_trylock(mtx_t *mutex);
int  mtx_unlock(mtx_t *mutex);

int  cnd_init(cnd_t *cond);
void cnd_destroy(cnd_t *cond);
int  cnd_signal(cnd_t *cond);
int  cnd_broadcast(cnd_t *cond);
int  cnd_wait(cnd_t *cond, mtx_t *mutex);
int  cnd_timedwait(cnd_t *cond, mtx_t *mutex,
                   const struct timespec *time_point);

int   tss_create(tss_t *key, tss_dtor_t destructor);
void  tss_delete(tss_t key);
void *tss_get(tss_t key);
int   tss_set(tss_t key, void *value);

void call_once(once_flag *flag, void (*func)(void));

#endif /* _THREADS_H */
