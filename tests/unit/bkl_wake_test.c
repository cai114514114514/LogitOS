/* SPDX-License-Identifier: MIT
 * Model only CPU locks, predicate and ring/timer bookkeeping. The included
 * wake functions and pending protocol are extracted from production sched.c.
 * Two pthreads force signal publication AFTER a false predicate but BEFORE
 * block_self would change state; there is no timing-dependent sleep. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#define THREAD_READY 0
#define THREAD_BLOCKED 1
struct thread { int id,alive,state,wake_pending; struct thread *all_next; };
typedef pthread_mutex_t spinlock_t;
static spinlock_t g_sched_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t object_lock=PTHREAD_MUTEX_INITIALIZER;
static struct thread target={.id=7,.alive=1,.state=THREAD_READY};
static struct thread *g_all=&target;
static unsigned long g_blocked;
static atomic_int phase, condition;
static int checks,failures,refused_park,timer_removes,ring_links;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
static uint64_t spin_lock_irqsave(spinlock_t *l){pthread_mutex_lock(l);return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)f;pthread_mutex_unlock(l);}
static void timerlist_remove(struct thread *t){(void)t;timer_removes++;}
static void ring_link(struct thread *t){(void)t;ring_links++;}
#include "wake_production.inc"
static void *waiter(void *unused)
{
    (void)unused;pthread_mutex_lock(&object_lock);
    int observed=atomic_load(&condition);
    /* Deliberately keep this stale observation, just as the object predicate
     * has already returned false before entering block_self. */
    if(observed){pthread_mutex_unlock(&object_lock);return NULL;}
    atomic_store(&phase,1);
    while(atomic_load(&phase)<2)sched_yield();
    pthread_mutex_lock(&g_sched_lock);
    refused_park=wake_pending_take_locked(&target);
    if(!refused_park){target.state=THREAD_BLOCKED;g_blocked++;}
    pthread_mutex_unlock(&object_lock);
    pthread_mutex_unlock(&g_sched_lock);return NULL;
}
static void *poster(void *unused)
{
    (void)unused;while(atomic_load(&phase)<1)sched_yield();
    atomic_store(&condition,1);sched_wake_id(7);atomic_store(&phase,2);return NULL;
}
int main(void)
{
    pthread_t a,b;pthread_create(&a,NULL,waiter,NULL);pthread_create(&b,NULL,poster,NULL);
    pthread_join(a,NULL);pthread_join(b,NULL);
    CHECK(refused_park==1,"post between predicate check and park prevents sleeping");
    CHECK(target.state==THREAD_READY&&g_blocked==0,"async wake cannot strand the ready thread as blocked");
    CHECK(target.wake_pending==0,"park attempt consumes pending wake exactly once");
    pthread_mutex_lock(&g_sched_lock);int again=wake_pending_take_locked(&target);pthread_mutex_unlock(&g_sched_lock);
    CHECK(again==0,"consumed wake does not make future waits spin");
    target.state=THREAD_BLOCKED;g_blocked=1;
    CHECK(sched_wake_id(7)==1,"already blocked target is made runnable immediately");
    CHECK(target.state==THREAD_READY&&g_blocked==0&&timer_removes==1&&ring_links==1,"blocked wake balances timer run-ring and resource state");
    CHECK(target.wake_pending==0,"successful unpark does not leave an extra pending wake");
    sched_wake_id(7);sched_wake_id(7);pthread_mutex_lock(&g_sched_lock);
    int first=wake_pending_take_locked(&target),second=wake_pending_take_locked(&target);pthread_mutex_unlock(&g_sched_lock);
    CHECK(first==1&&second==0,"multiple pre-park events coalesce to one predicate recheck");
    CHECK(sched_wake_id(999)==0,"unknown thread id is harmless");
    target.alive=0;CHECK(sched_wake_id(7)==0&&target.wake_pending==0,"retired target cannot acquire new pending ownership");
    printf("BKL_WAKE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
