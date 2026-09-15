#ifndef LOGIT_GUI_SYNC_H
#define LOGIT_GUI_SYNC_H
#include <stdint.h>
/* Domain locks, never an entry lock. Hosted gates exercise ownership with
 * atomics; the kernel sleeps so disk work does not prevent unrelated work. */
#if __STDC_HOSTED__
struct gui_spin { unsigned locked; };
#define GUI_SPIN_INIT {0}
static inline void gui_spin_lock(struct gui_spin *s) {
    while (__atomic_exchange_n(&s->locked,1,__ATOMIC_ACQUIRE))
        while (__atomic_load_n(&s->locked,__ATOMIC_RELAXED)) {}
}
static inline void gui_spin_unlock(struct gui_spin *s) {
    __atomic_store_n(&s->locked,0,__ATOMIC_RELEASE);
}
struct gui_mutex { uintptr_t owner; unsigned depth; };
#define GUI_MUTEX_INIT {0,0}
static _Thread_local char gui_thread_token;
static inline void gui_mutex_lock(struct gui_mutex *m) {
    uintptr_t me=(uintptr_t)&gui_thread_token;
    if (__atomic_load_n(&m->owner,__ATOMIC_ACQUIRE)==me) {m->depth++;return;}
    uintptr_t zero=0;
    while (!__atomic_compare_exchange_n(&m->owner,&zero,me,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) zero=0;
    m->depth=1;
}
static inline void gui_mutex_unlock(struct gui_mutex *m) {
    if (--m->depth==0) __atomic_store_n(&m->owner,0,__ATOMIC_RELEASE);
}
#else
#include "spinlock.h"
#include "wait.h"
#include "sched.h"
struct gui_spin { spinlock_t lock; };
#define GUI_SPIN_INIT {SPINLOCK_INIT}
/* Never used by an ISR. Learning's timer only queues work; its table snapshot
 * does not allocate, invoke callbacks or access the disk while locked. */
static inline void gui_spin_lock(struct gui_spin *s) { spin_lock(&s->lock); }
static inline void gui_spin_unlock(struct gui_spin *s) { spin_unlock(&s->lock); }
struct gui_mutex { struct mutex wait; uintptr_t owner; unsigned depth; };
#define GUI_MUTEX_INIT {MUTEX_INIT,0,0}
static inline void gui_mutex_lock(struct gui_mutex *m) {
    uintptr_t me=(uintptr_t)sched_current_thread();
    if (!me) return; /* boot is single-threaded, before sched_init */
    if (__atomic_load_n(&m->owner,__ATOMIC_ACQUIRE)==me) {m->depth++;return;}
    mutex_lock(&m->wait);
    m->depth=1;
    __atomic_store_n(&m->owner,me,__ATOMIC_RELEASE);
}
static inline void gui_mutex_unlock(struct gui_mutex *m) {
    if (!sched_current_thread()) return;
    if (--m->depth==0) {
        __atomic_store_n(&m->owner,0,__ATOMIC_RELEASE);
        mutex_unlock(&m->wait);
    }
}
#endif
#endif
