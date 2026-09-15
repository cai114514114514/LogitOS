/* SPDX-License-Identifier: MIT
 * Actual oom_task_reap_dead with pthread lock/AS lifetime seams. Reuse is
 * simulated only in the MM fixture: no guest addresses or payloads are used.
 * Barriers force both sides of the snapshot/guard/revalidation boundary. */
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <errno.h>
#include "mmguard.h"
#define NPROC 1
#define PROC_FREE 0
#define PROC_ZOMBIE 1
#define PROC_RUNNING 2
struct proc { int state,pid; uint64_t cr3; };
static struct proc procs[NPROC];
typedef pthread_mutex_t spinlock_t;
static spinlock_t g_proc_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t as_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_int phase;
static int scenario, generation, freed_generation, frees, reuse_blocked;
static int checks, failures;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
static void wait_phase(int p){while(atomic_load(&phase)<p)sched_yield();}
static uint64_t spin_lock_irqsave(spinlock_t *l){pthread_mutex_lock(l);return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)f;pthread_mutex_unlock(l);}
struct mm_guard mm_guard_try(uint64_t cr3)
{
    (void)cr3;
    if(scenario==1){atomic_store(&phase,1);wait_phase(2);}
    int got=pthread_mutex_trylock(&as_lock)==0;
    if(scenario==3)atomic_store(&phase,2);
    return (struct mm_guard){.held=got};
}
void mm_guard_end(struct mm_guard *g){if(g->held){g->held=0;pthread_mutex_unlock(&as_lock);}}
int mm_space_live(uint64_t cr3){return cr3==0x1000;}
static void vmm_free_user(uint64_t cr3)
{
    (void)cr3;
    if(scenario==2){atomic_store(&phase,1);wait_phase(2);}
    frees++;freed_generation=generation;
}
#include "reap_production.inc"
static void replace_process(void)
{
    pthread_mutex_lock(&g_proc_lock);
    procs[0]=(struct proc){.state=PROC_FREE};
    pthread_mutex_unlock(&g_proc_lock);
    pthread_mutex_lock(&as_lock);generation=2;
    pthread_mutex_lock(&g_proc_lock);
    procs[0]=(struct proc){.state=PROC_RUNNING,.pid=202,.cr3=0x1000};
    pthread_mutex_unlock(&g_proc_lock);pthread_mutex_unlock(&as_lock);
}
static void *normal_reaper(void *unused)
{
    (void)unused;
    if(scenario==1){wait_phase(1);replace_process();atomic_store(&phase,2);}
    else if(scenario==2){
        wait_phase(1);
        pthread_mutex_lock(&g_proc_lock);procs[0]=(struct proc){.state=PROC_FREE};pthread_mutex_unlock(&g_proc_lock);
        int r=pthread_mutex_trylock(&as_lock);reuse_blocked=r==EBUSY;if(r==0)pthread_mutex_unlock(&as_lock);
        atomic_store(&phase,2);replace_process();
    }else{
        pthread_mutex_lock(&as_lock);atomic_store(&phase,1);wait_phase(2);pthread_mutex_unlock(&as_lock);
    }
    return NULL;
}
static void reset(int mode)
{
    scenario=mode;generation=1;freed_generation=frees=reuse_blocked=0;atomic_store(&phase,0);
    procs[0]=(struct proc){.state=PROC_ZOMBIE,.pid=101,.cr3=0x1000};
}
int main(void)
{
    pthread_t worker;int count;
    reset(1);pthread_create(&worker,NULL,normal_reaper,NULL);count=oom_task_reap_dead();pthread_join(worker,NULL);
    CHECK(count==0&&frees==0,"reap rejects recycled CR3 after snapshot before guard");
    CHECK(procs[0].pid==202&&freed_generation!=2,"replacement address space retains all user pages");
    reset(2);pthread_create(&worker,NULL,normal_reaper,NULL);count=oom_task_reap_dead();pthread_join(worker,NULL);
    CHECK(count==1&&frees==1&&freed_generation==1,"validated zombie is reclaimed under original AS ownership");
    CHECK(reuse_blocked==1&&generation==2,"normal reaper cannot recycle CR3 until OOM guard drops");
    reset(3);pthread_create(&worker,NULL,normal_reaper,NULL);wait_phase(1);count=oom_task_reap_dead();pthread_join(worker,NULL);
    CHECK(count==0&&frees==0,"busy remote AS is skipped without waiting while OOM owns caller AS");
    reset(0);count=oom_task_reap_dead();
    CHECK(count==1&&frees==1,"uncontended zombie user pages are reclaimed");
    procs[0].state=PROC_RUNNING;count=oom_task_reap_dead();
    CHECK(count==0&&frees==1,"live process cannot enter zombie reclamation");
    CHECK(pthread_mutex_trylock(&as_lock)==0,"all early and success paths return AS ownership");pthread_mutex_unlock(&as_lock);
    printf("BKL_REAP checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
