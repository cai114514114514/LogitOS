/* SPDX-License-Identifier: MIT
 * Included after production synchronous-signal storage and ksig_fault. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int host_tid;
static struct proc host_proc={.pid=123};
static atomic_int ready_a, ready_b, consumed_b;
static struct ksig_fault_record a_record,b_record;
static int checks,failures;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
uint64_t spin_lock_irqsave(spinlock_t *l){(void)l;pthread_mutex_lock(&host_lock);return 0;}
void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)l;(void)f;pthread_mutex_unlock(&host_lock);}
int sched_current_tid(void){return host_tid;}
struct proc *proc_current(void){return &host_proc;}
static void *fault_a(void *unused)
{
    (void)unused;host_tid=20;ksig_fault(LOGIT_SIGSEGV,0xaaa,1,14);atomic_store(&ready_a,1);
    while(!atomic_load(&consumed_b))sched_yield();
    uint64_t f=spin_lock_irqsave(&g_sig_lock);ksig_take_fault_locked(&a_record);spin_unlock_irqrestore(&g_sig_lock,f);return NULL;
}
static void *fault_b(void *unused)
{
    (void)unused;host_tid=21;while(!atomic_load(&ready_a))sched_yield();
    ksig_fault(LOGIT_SIGSEGV,0xbbb,2,14);atomic_store(&ready_b,1);
    uint64_t f=spin_lock_irqsave(&g_sig_lock);ksig_take_fault_locked(&b_record);spin_unlock_irqrestore(&g_sig_lock,f);
    atomic_store(&consumed_b,1);return NULL;
}
int main(void)
{
    g_sig[0].pid=123;g_sig[0].handler[LOGIT_SIGSEGV]=0x50000100;
    pthread_t a,b;pthread_create(&a,NULL,fault_a,NULL);pthread_create(&b,NULL,fault_b,NULL);
    pthread_join(a,NULL);pthread_join(b,NULL);
    CHECK(a_record.tid==20&&b_record.tid==21,"fault delivery stays on originating thread");
    CHECK(a_record.cr2==0xaaa&&b_record.cr2==0xbbb,"concurrent fault addresses stay independent");
    CHECK(a_record.err==1&&b_record.err==2,"concurrent error codes stay independent");
    CHECK(g_sig[0].pending==0,"synchronous fault cannot be consumed as process-directed signal");
    CHECK(g_sig_armed==0,"all synchronous pending resources return to baseline");
    host_tid=20;ksig_fault(LOGIT_SIGSEGV,0xccc,3,14);fault_clear_locked(123);
    CHECK(g_sig_armed==0,"process teardown clears unconsumed fault resources");
    printf("BKL_SIGNAL checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
