/* SPDX-License-Identifier: MIT
 * Actual tlb.c and spinlock.c. CPU identity, CR3 reload, IPI delivery and halt
 * are host seams. Three simultaneous initiators each perform eight refreshes;
 * CPU 3 delays its first flush while the other initiators ACK from spin_lock.
 * Extra/nested IPI calls must not manufacture extra acknowledgements. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include "tlb.h"
#include "percpu.h"
#define CPUS 4
#define INITIATORS 3
#define PASSES 8
#define GENERATIONS (INITIATORS*PASSES+2)
struct cpu g_cpus[PERCPU_MAXCPU];
static _Thread_local int cpu;
static _Thread_local unsigned epoch;
static int ncpu=1,timeout_mode;
static atomic_uint next_epoch,active_publishers,max_publishers;
static atomic_uint desired[CPUS],completed[CPUS],per_epoch[GENERATIONS];
static atomic_int delay_entered,release_delay,stop,finished,returned,halted;
static atomic_int local_flushes,remote_flushes,nested_ipis,issued_ipis;
static atomic_int early_returns,duplicate_flushes,console_calls,protocol_errors;
static char diagnostic[512];static unsigned diag_size;
struct cpu *this_cpu(void){return &g_cpus[cpu];}
int smp_cpu_count(void){return ncpu;}
int logit_lock_host_cpu(void){return cpu;}
void serial_putc(char c){(void)c;atomic_fetch_add(&console_calls,1);}
int lapic_send_ipi(uint32_t apic,uint8_t vec)
{
    if(apic>=CPUS||vec!=240)atomic_fetch_add(&protocol_errors,1);
    atomic_fetch_add(&issued_ipis,1);
    return 0;
}
void tlb_host_pause(void){sched_yield();}
void tlb_host_begin(int me,int peers)
{
    (void)me;
    if(peers!=CPUS-1)atomic_fetch_add(&protocol_errors,1);
    unsigned active=atomic_fetch_add(&active_publishers,1)+1;
    if(active>atomic_load(&max_publishers))atomic_store(&max_publishers,active);
    if(active!=1){
        /* Abort the negative run at the actual overlapping publication, before
         * its intentionally corrupted ACK state can create a misleading hang. */
        puts("FAIL simultaneous shootdown publishers never overlap shared ACK ownership");fflush(stdout);_Exit(1);
    }
    epoch=atomic_fetch_add(&next_epoch,1)+1;
    for(int i=0;i<CPUS;i++)if(i!=cpu)atomic_store(&desired[i],epoch);
}
void tlb_host_flush_self(int request)
{
    if(!request){atomic_fetch_add(&local_flushes,1);return;}
    unsigned generation=atomic_load(&desired[cpu]);
    /* Poll has claimed the flag, then an IPI arrives before the reload/ACK.
     * This nested call exercises the production atomic request claim. */
    atomic_fetch_add(&nested_ipis,1);tlb_ipi();
    if(cpu==3&&generation==1){
        atomic_store(&delay_entered,1);
        while(!atomic_load(&release_delay))sched_yield();
    }
    if(!generation||generation>=GENERATIONS){atomic_fetch_add(&protocol_errors,1);return;}
    if(atomic_exchange(&completed[cpu],generation)>=generation)atomic_fetch_add(&duplicate_flushes,1);
    atomic_fetch_add(&per_epoch[generation],1);atomic_fetch_add(&remote_flushes,1);
}
void tlb_host_end(int me,int ack)
{
    if(ack!=CPUS-1||atomic_load(&per_epoch[epoch])!=CPUS-1)atomic_fetch_add(&early_returns,1);
    for(int i=0;i<CPUS;i++)if(i!=me&&atomic_load(&completed[i])<epoch)atomic_fetch_add(&early_returns,1);
    atomic_fetch_sub(&active_publishers,1);
}
void tlb_host_emergency_putc(char c)
{
    if(diag_size+1<sizeof diagnostic){diagnostic[diag_size++]=c;diagnostic[diag_size]=0;}
}
_Noreturn void tlb_host_failstop(void)
{
    atomic_fetch_add(&halted,1);pthread_exit((void *)(uintptr_t)1);
}
static void *run_cpu(void *arg)
{
    cpu=(int)(uintptr_t)arg;
    if(timeout_mode){tlb_flush_all();atomic_fetch_add(&returned,1);return NULL;}
    if(cpu<INITIATORS){
        if(cpu)while(!atomic_load(&next_epoch))sched_yield();
        for(int i=0;i<PASSES;i++){tlb_flush_all();atomic_fetch_add(&returned,1);}
        atomic_fetch_add(&finished,1);
    }
    while(!atomic_load(&stop)){tlb_ipi();tlb_service();tlb_ipi();sched_yield();}
    return NULL;
}
static int checks,failures;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
int main(int argc,char **argv)
{
    timeout_mode=argc>1&&!strcmp(argv[1],"timeout");
    for(int i=0;i<CPUS;i++){g_cpus[i].index=i;g_cpus[i].lapic_id=(uint32_t)i;}
    if(timeout_mode){
        ncpu=CPUS;pthread_t t;pthread_create(&t,NULL,run_cpu,NULL);void *result=NULL;pthread_join(t,&result);
        CHECK(atomic_load(&halted)==1&&atomic_load(&returned)==0&&result==(void *)(uintptr_t)1,"timeout retains page ownership until fail-stop");
        CHECK(tlb_late_count()==CPUS-1,"timeout accounts for every missing CPU acknowledgement");
        CHECK(strstr(diagnostic,"[tlb] FATAL shootdown timeout cpu=0x00000000")&&strstr(diagnostic," ack=0x00000000 want=0x00000003"),"timeout diagnostic identifies sender and ACK deficit");
        CHECK(strstr(diagnostic,"unclaimed=0x0000000e; page ownership retained"),"timeout diagnostic records outstanding request mask");
        CHECK(atomic_load(&console_calls)==0,"fail-stop diagnostics never acquire console or serial locks");
        CHECK(atomic_load(&active_publishers)==1,"failed sender retains transaction ownership");
        printf("BKL_TLB_TIMEOUT checks=%d failures=%d\n",checks,failures);return failures?1:0;
    }
    tlb_flush_all();
    CHECK(atomic_load(&local_flushes)==1&&atomic_load(&remote_flushes)==0,"uniprocessor refresh completes with a local CR3 flush");
    CHECK(atomic_load(&issued_ipis)==0&&atomic_load(&active_publishers)==0,"uniprocessor refresh creates no remote transaction");
    atomic_store(&local_flushes,0);ncpu=CPUS;pthread_t threads[CPUS];
    for(int i=0;i<CPUS;i++)if(pthread_create(&threads[i],NULL,run_cpu,(void *)(uintptr_t)i))return 2;
    while(!atomic_load(&delay_entered)||atomic_load(&completed[1])<1||atomic_load(&completed[2])<1)sched_yield();
    CHECK(atomic_load(&returned)==0,"sender cannot return while delayed target still owns a stale translation");
    CHECK(atomic_load(&active_publishers)==1&&atomic_load(&next_epoch)==1,"other initiators service ACKs while queued behind sender lock");
    atomic_store(&release_delay,1);
    while(atomic_load(&finished)<INITIATORS)sched_yield();
    atomic_store(&stop,1);for(int i=0;i<CPUS;i++)pthread_join(threads[i],NULL);
    unsigned transactions=INITIATORS*PASSES;
    CHECK(atomic_load(&next_epoch)==transactions&&atomic_load(&returned)==(int)transactions,"all simultaneous initiators complete every refresh transaction");
    CHECK(atomic_load(&max_publishers)==1&&atomic_load(&active_publishers)==0,"sender lock serializes ACK resets and returns ownership exactly once");
    CHECK(atomic_load(&early_returns)==0,"every transaction returns only after all target generations flush");
    CHECK(atomic_load(&remote_flushes)==(int)(transactions*(CPUS-1)),"each target flush contributes exactly one ACK per transaction");
    CHECK(atomic_load(&nested_ipis)==atomic_load(&remote_flushes)&&atomic_load(&duplicate_flushes)==0,"nested and repeated IPIs cannot acknowledge the same request twice");
    CHECK(atomic_load(&local_flushes)==(int)transactions&&atomic_load(&issued_ipis)==(int)(transactions*(CPUS-1)),"initiators flush locally and publish every remote IPI");
    CHECK(tlb_late_count()==0&&atomic_load(&halted)==0,"delayed acknowledgement completes without timeout");
    CHECK(atomic_load(&console_calls)==0&&atomic_load(&protocol_errors)==0,"production ticket ownership and host hardware seams stay consistent");
    printf("BKL_TLB transactions=%u remote_acks=%d checks=%d failures=%d\n",transactions,atomic_load(&remote_flushes),checks,failures);return failures?1:0;
}
