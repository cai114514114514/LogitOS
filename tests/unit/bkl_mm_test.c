/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <stdatomic.h>
#include "mm_common.h"
#include "pmm.h"
#include "kprintf.h"
#include "mm.h"
#include "vmm.h"
#include "vma.h"
#include "mmhost.h"
#include "mmguard.h"
#include "pcache.h"
#include "reclaim.h"
#include "spinlock.h"
#define N 8
#define STEPS 300
static _Thread_local int cpu;
int logit_lock_host_cpu(void) { return cpu; }
void serial_putc(char c) { fputc(c,stderr); }
void tlb_service(void) { }
static atomic_int start, ready, errors, inside, peak;
static uint64_t space[N];
static spinlock_t ticket=SPINLOCK_INIT;
static unsigned ticket_sum;
static void launch(void) {
 atomic_fetch_add(&ready,1);
 while(!atomic_load(&start)) sched_yield();
}
static void *ticket_worker(void *arg) {
 cpu=(int)(uintptr_t)arg; launch();
 for(int i=0;i<STEPS*10;i++) {
  uint64_t f=spin_lock_irqsave(&ticket);
  unsigned x=ticket_sum;
  if((i&15)==0) sched_yield();
  ticket_sum=x+1;
  spin_unlock_irqrestore(&ticket,f);
 }
 return 0;
}
static void *copy_worker(void *arg) {
 cpu=(int)(uintptr_t)arg; mm_host_cr3=space[0]; launch();
 for(int i=0;i<STEPS;i++) {
  MM_GUARD(space[0]);
  uint64_t f=0;
  if(vmm_pin_user_page(space[0],MM_USER_WIDE_BASE,1,&f)) { atomic_fetch_add(&errors,1);continue; }
  uint64_t *p=mm_p2v(f),value=*p;
  sched_yield();
  *p=value+1;
  pmm_unpin(f);pmm_free(f);
 }
 return 0;
}
static void *parallel_worker(void *arg) {
 cpu=(int)(uintptr_t)arg; mm_host_cr3=space[cpu]; launch();
 MM_GUARD(space[cpu]);
 int n=atomic_fetch_add(&inside,1)+1;
 int old=atomic_load(&peak);
 while(old<n&&!atomic_compare_exchange_weak(&peak,&old,n)){}
 /* All address spaces must enter before anyone leaves. Bounded so a global
  * serialization mutant fails the overlap assertion rather than hanging. */
 for(unsigned i=0;i<100000&&atomic_load(&peak)<N;i++) sched_yield();
 uint64_t a=vma_reserve(space[cpu],MM_USER_WIDE_BASE,8ull<<30,VMA_READ|VMA_WRITE);
 if(a!=MM_USER_WIDE_BASE) atomic_fetch_add(&errors,1);
 for(unsigned i=0;i<12;i++) if(!mm_fault_in(space[cpu],a+i*(512ull<<20),6)) atomic_fetch_add(&errors,1);
 atomic_fetch_sub(&inside,1);
 return 0;
}
static void run(void *(*worker)(void *)) {
 pthread_t t[N];atomic_store(&ready,0);atomic_store(&start,0);
 for(int i=0;i<N;i++) if(pthread_create(&t[i],0,worker,(void *)(uintptr_t)i)) abort();
 while(atomic_load(&ready)<N) sched_yield();
 atomic_store(&start,1);
 for(int i=0;i<N;i++) pthread_join(t[i],0);
 cpu=0;
}
static atomic_int fs_pause, fs_entered, fs_release, fs_value=0x6d;
static uint64_t flight_frame;
static int flight_fh;
static int fs_stat(const char *p,uint64_t*d,uint64_t*i,uint64_t*s)
{ (void)p;*d=1;*i=7;*s=4096;return 0; }
static long fs_read(const char*p,uint64_t off,void*d,uint64_t n)
{ (void)p;(void)off;memset(d,atomic_load(&fs_value),n);
  if(atomic_load(&fs_pause)) {
    atomic_store(&fs_entered,1);
    while(!atomic_load(&fs_release)) sched_yield();
  }
  return (long)n; }
static void *flight_reader(void *unused)
{ (void)unused;cpu=1;flight_frame=pcache_get_ref(flight_fh,0);return 0; }
static struct pcache_ops fsops={.stat=fs_stat,.read=fs_read};
int main(void) {
 mm_log_quiet(1);mm_sim_init(128);mm_sim_kernel_space();vmm_kernel_cr3();pcache_init(pmm_total_frames());
 reclaim_set_enabled(0);
 run(ticket_worker);mm_eqi(ticket_sum,N*STEPS*10,"real ticket lock exact increment count");
 mm_eqi(ticket.ticket,ticket.serving,"ticket queue drains without lost release");
 uint64_t baseline=pmm_free_frames();
 for(int i=0;i<N;i++) {
  space[i]=vmm_new_space();mm_ok(space[i]!=0,"new address space");
  for(int j=0;j<i;j++) mm_ok(mm_guard_index(space[i])!=mm_guard_index(space[j]),"fixture uses distinct AS guard slots");
 }
 run(parallel_worker);
 mm_eqi(atomic_load(&peak),N,"independent address spaces execute concurrently");
 mm_eqi(atomic_load(&errors),0,"parallel 8 GiB sparse reservations and faults");
 uint64_t *e=vmm_pte(space[0],MM_USER_WIDE_BASE);mm_ok(e&&(*e&1),"shared-AS test page present");
 if(e&&(*e&1)) { memset(mm_p2v(*e&MM_PTE_ADDR),0,4096);run(copy_worker);
  mm_eqi(*(uint64_t*)mm_p2v(*vmm_pte(space[0],MM_USER_WIDE_BASE)&MM_PTE_ADDR),N*STEPS,"guarded physical usercopy has no lost updates"); }
 mm_eqi(atomic_load(&errors),0,"pinned copy faults all succeed");
 for(int i=0;i<N;i++) vmm_free_space(space[i]);
 mm_eqi(pmm_free_frames(),baseline,"all sparse pages and page tables recovered");
 pcache_set_ops(&fsops);
 int fh=pcache_file_open("/held");mm_ok(fh>=0,"cache handle acquired");
 uint64_t f=pcache_get_ref(fh,0);mm_ok(f!=0,"cache page acquired");
 pcache_invalidate_file(fh);
 mm_eqi(pmm_refcount(f),1,"cache invalidation preserves caller reference");
 if(pmm_refcount(f)) { mm_ok(*(unsigned char*)mm_p2v(f)==0x6d,"held cache bytes survive invalidation");pmm_free(f); }
 /* A read already holding old bytes must not reinsert them AFTER a write's
  * invalidation completed. The current reader may keep its own snapshot. */
 flight_fh=fh;atomic_store(&fs_pause,1);
 pthread_t reader;pthread_create(&reader,0,flight_reader,0);
 while(!atomic_load(&fs_entered)) sched_yield();
 atomic_store(&fs_value,0x7e);pcache_invalidate_file(fh);
 atomic_store(&fs_release,1);pthread_join(reader,0);
 atomic_store(&fs_pause,0);
 if(flight_frame && pmm_refcount(flight_frame)) pmm_free(flight_frame);
 uint64_t fresh=pcache_get_ref(fh,0);
 mm_ok(fresh && *(unsigned char*)mm_p2v(fresh)==0x7e,"in-flight old read cannot refill invalidated cache");
 if(fresh && pmm_refcount(fresh)) pmm_free(fresh);
 pcache_file_put(fh);
 mm_eqi(pmm_bugs(),0,"no PMM ownership violations");
 mm_sim_done();return mm_summary("bkl-mm");
}
