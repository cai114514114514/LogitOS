/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_IO_DOMAIN_H
#define LOGIT_IO_DOMAIN_H
/* Thread-owned sleeping mutex for a service object whose operation can wait.
 * Recursion is by task, never CPU: a waiting task may migrate. IRQ callers
 * must use their device spinlock instead of entering a sleeping owner. */
#if __STDC_HOSTED__
#include <pthread.h>
struct io_domain { pthread_mutex_t lock; void *owner; unsigned depth; };
#define IO_DOMAIN_INIT { PTHREAD_MUTEX_INITIALIZER, 0, 0 }
static inline void *io_domain_identity(void)
{ static _Thread_local char token; return &token; }
#else
/* Bare name: INCDIRS carries every directory under c/, and a relative
 * spelling only survives until the target moves -- wait.h went from
 * kernel/core/ to kernel/sync/ on 2026-09-15 and this was the one include
 * in the tree that pointed at the old path. */
#include "wait.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/cpu/smp/percpu.h"
struct io_domain { struct mutex lock; void *owner; unsigned depth; };
#define IO_DOMAIN_INIT { MUTEX_INIT, 0, 0 }
static inline void *io_domain_identity(void)
{ struct thread *t = sched_current_thread(); return t ? (void *)t : (void *)this_cpu(); }
#endif
static inline void io_domain_enter(struct io_domain *d)
{
    void *me = io_domain_identity();
    if (__atomic_load_n(&d->owner, __ATOMIC_ACQUIRE) == me) { d->depth++; return; }
#if __STDC_HOSTED__
    pthread_mutex_lock(&d->lock);
#else
    mutex_lock(&d->lock);
#endif
    d->depth = 1;
    __atomic_store_n(&d->owner, me, __ATOMIC_RELEASE);
}
static inline void io_domain_leave(struct io_domain *d)
{
    if (--d->depth) return;
    __atomic_store_n(&d->owner, 0, __ATOMIC_RELEASE);
#if __STDC_HOSTED__
    pthread_mutex_unlock(&d->lock);
#else
    mutex_unlock(&d->lock);
#endif
}
struct io_domain_guard { struct io_domain *domain; };
static inline struct io_domain_guard io_domain_take(struct io_domain *d)
{ io_domain_enter(d); return (struct io_domain_guard){d}; }
static inline void io_domain_drop(struct io_domain_guard *g)
{ if (g->domain) { io_domain_leave(g->domain); g->domain = 0; } }
#define IO_DOMAIN_GUARD(d) struct io_domain_guard domain_guard_ \
    __attribute__((cleanup(io_domain_drop))) = io_domain_take(d)
#endif
