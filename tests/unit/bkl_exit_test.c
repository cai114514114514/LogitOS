/* SPDX-License-Identifier: MIT
 * Included after the exact production uthread table and release function.
 * pthreads model CPUs; the controlled unmap blocks are MM lifecycle barriers,
 * not an implementation of unmapping. The production exit election is real. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int host_tid;
static struct proc host_proc;
static atomic_int entered, allow_a, allow_b;
static int result_a, result_b, checks, failures;
static atomic_int detached;
#define CHECK(c,s) do { checks++; if (!(c)) { failures++; printf("FAIL %s\n",s); } } while(0)
uint64_t spin_lock_irqsave(spinlock_t *l) { (void)l; pthread_mutex_lock(&host_lock); return 0; }
void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)l; (void)f; pthread_mutex_unlock(&host_lock); }
int sched_current_tid(void) { return host_tid; }
struct proc *proc_current(void) { return &host_proc; }
void sched_tlb_gen_bump(void) {}
void sched_detach_current_proc(void *p) { (void)p; atomic_fetch_add(&detached, 1); }
int waitq_wake_all(struct waitq *q) { (void)q; return 0; }
long mm_syscall(long op, long va, long len, long ignored)
{
    (void)op; (void)va; (void)len; (void)ignored;
    atomic_fetch_add(&entered, 1);
    atomic_int *permit = host_tid == 10 ? &allow_a : &allow_b;
    while (!atomic_load(permit)) sched_yield();
    return 0;
}
static void *leave(void *arg)
{
    host_tid = (int)(uintptr_t)arg;
    int rc = uthread_release_self(0);
    if (host_tid == 10) result_a=rc; else result_b=rc;
    return NULL;
}
int main(void)
{
    host_proc.pid=123;
    g_ut[0]=(struct uthread){ .state=UT_LIVE, .tid=10, .pid=123, .detached=1, .stack_base=0x50000000, .stack_len=4096 };
    g_ut[1]=(struct uthread){ .state=UT_LIVE, .tid=11, .pid=123, .detached=1, .stack_base=0x50001000, .stack_len=4096 };
    pthread_t a,b;
    pthread_create(&a,NULL,leave,(void *)(uintptr_t)10);
    pthread_create(&b,NULL,leave,(void *)(uintptr_t)11);
    while(atomic_load(&entered)<2) sched_yield();
    CHECK(ut_proc_count_locked(123,1)==2,"both unmapping threads retain address-space ownership");
    atomic_store(&allow_a,1);pthread_join(a,NULL);
    CHECK(result_a==0,"first departing sibling cannot tear down while another unmaps");
    CHECK(atomic_load(&detached)==1,"departing sibling detaches CR3 before last-thread election can proceed");
    CHECK(host_proc.teardown==0,"no teardown owner while another stack is in use");
    atomic_store(&allow_b,1);pthread_join(b,NULL);
    CHECK(result_b==1,"last departing sibling owns teardown");
    CHECK(host_proc.teardown==12,"last exit records a durable unique owner");
    CHECK(ut_proc_count_locked(123,1)==0,"all stack owners leave before teardown");
    CHECK(g_reaped==2,"each detached descriptor is reclaimed once");
    host_tid=10;CHECK(uthread_release_self(0)==0,"an old departing sibling cannot reacquire teardown");
    host_tid=11;CHECK(uthread_release_self(0)==1,"elected owner can enter proc_exit teardown exactly once");
    printf("BKL_EXIT checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
