/* SPDX-License-Identifier: MIT
 * Real PMM/VMM/AS owners with concurrent process roots. The only extra hook
 * pauses two absent-table observations if recursive kernel ownership is absent,
 * turning that negative control into a deterministic lost-publication race. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include "mm_common.h"
#include "mmhost.h"
#include "mmguard.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "reclaim.h"
#include "kprintf.h"
#define LOW 0xc0000000ull
#define HIGH 0xfffffe0000000000ull
#define RACE (LOW+0x1000000ull)
#define EXTRA 32
static _Thread_local int cpu;
static uint64_t kernel,space[2],owned[64],extra[EXTRA];
static int nowned;
static atomic_int ready,start,armed,absent,errors,published;
static atomic_uint flushes,lock_errors;
static uint64_t *race_pd;
int logit_lock_host_cpu(void){return cpu;}
int kheap_cpu_index(void){return cpu;}
void serial_putc(char c){(void)c;atomic_fetch_add(&lock_errors,1);}
void tlb_service(void){}
void tlb_flush_all(void){atomic_fetch_add(&flushes,1);}
void mm_host_table_absent(uint64_t *table,int idx,int user)
{
    if(user||!atomic_load(&armed)||table!=race_pd||idx!=8)return;
    struct mm_guard g=mm_kernel_guard_try();
    if(!g.held){
        atomic_fetch_add(&absent,1);
        while(atomic_load(&absent)<2)sched_yield();
    }
    mm_guard_end(&g);
}
static uint64_t page(void)
{
    uint64_t p=pmm_alloc();if(p){owned[nowned++]=p;memset(mm_p2v(p),(int)(p>>12),4096);}return p;
}
static uint64_t leaf(uint64_t cr3,uint64_t va)
{uint64_t *p=vmm_pte(cr3,va);return p?*p:0;}
static void launch(void)
{atomic_fetch_add(&ready,1);while(!atomic_load(&start))sched_yield();}
static void *mapper(void *arg)
{
    cpu=(int)(uintptr_t)arg;mm_host_cr3=space[cpu];launch();
    if(cpu==0)vmm_map_page(RACE,owned[2],VMM_WRITABLE);
    else vmm_map_page_in(space[1],RACE+4096,owned[3],VMM_WRITABLE);
    return NULL;
}
static void *churn_mapper(void *unused)
{
    (void)unused;cpu=1;mm_host_cr3=space[1];launch();
    for(int i=0;i<EXTRA;i++){
        vmm_map_page((uint64_t)(i+4)<<30,extra[i],VMM_WRITABLE);
        atomic_store(&published,i+1);sched_yield();
    }
    return NULL;
}
static void *churn_spaces(void *unused)
{
    (void)unused;cpu=2;mm_host_cr3=kernel;launch();
    for(int i=0;i<96;i++){
        uint64_t s=vmm_new_space();if(!s){atomic_fetch_add(&errors,1);continue;}
        {
            MM_KERNEL_GUARD;
            if((leaf(s,LOW)&MM_PTE_ADDR)!=owned[0]||(leaf(s,HIGH)&MM_PTE_ADDR)!=owned[1])atomic_fetch_add(&errors,1);
            int n=atomic_load(&published);
            for(int j=0;j<n;j++)if((leaf(s,(uint64_t)(j+4)<<30)&MM_PTE_ADDR)!=extra[j])atomic_fetch_add(&errors,1);
        }
        vmm_free_space(s);
    }
    return NULL;
}
static void pair(void *(*a)(void *),void *(*b)(void *))
{
    pthread_t t[2];atomic_store(&ready,0);atomic_store(&start,0);
    pthread_create(&t[0],NULL,a,(void *)(uintptr_t)0);pthread_create(&t[1],NULL,b,(void *)(uintptr_t)1);
    while(atomic_load(&ready)<2)sched_yield();atomic_store(&start,1);
    pthread_join(t[0],NULL);pthread_join(t[1],NULL);cpu=0;mm_host_cr3=kernel;
}
/* Fixture-owned supervisor mappings have no public unmap ABI. After every
 * process and worker is gone, remove only our test roots under their owner;
 * data pages stay separately owned so each is released exactly once. */
static void free_tables(uint64_t phys,int levels)
{
    uint64_t *t=mm_p2v(phys);
    if(levels>1)for(int i=0;i<512;i++)if((t[i]&0x81)==1)free_tables(t[i]&MM_PTE_ADDR,levels-1);
    pmm_free(phys);
}
static void cleanup(void)
{
    mm_host_cr3=kernel;for(int i=0;i<2;i++)vmm_free_space(space[i]);
    {
        MM_KERNEL_GUARD;uint64_t *pml4=mm_p2v(kernel),*pdpt=mm_p2v(pml4[0]&MM_PTE_ADDR);
        for(int q=3;q<5+EXTRA;q++)if(pdpt[q]&1){free_tables(pdpt[q]&MM_PTE_ADDR,2);pdpt[q]=0;}
        for(unsigned l=(unsigned)((HIGH>>39)&511);l<512;l++)if(pml4[l]&1){free_tables(pml4[l]&MM_PTE_ADDR,3);pml4[l]=0;}
    }
    for(int i=0;i<nowned;i++)pmm_free(owned[i]);
}
static uint64_t shared_root(uint64_t cr3,uint64_t va)
{
    uint64_t *root=mm_p2v(cr3),entry=root[(va>>39)&511];
    if(((va>>39)&511)==0 && (entry&1))entry=((uint64_t *)mm_p2v(entry&MM_PTE_ADDR))[(va>>30)&511];
    return entry;
}
/* Exhaust the actual PMM normal zone, then return exactly enough frames to
 * fail at each intermediate allocation. No fake allocation result: the PMM
 * reserve rule and its failure counter prove next_table reached a real OOM. */
static void partial_allocation(void)
{
    static const struct { uint64_t va; unsigned budget; } cases[]={
        {HIGH+(3ull<<39),0}, {HIGH+(1ull<<39),1},
        {HIGH+(2ull<<39),2}, {(uint64_t)(4+EXTRA)<<30,1}
    };
    uint64_t *pressure=malloc((size_t)pmm_total_frames()*sizeof(*pressure));
    mm_ok(pressure!=NULL,"host pressure inventory is available");
    if(!pressure)return;
    for(unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);c++){
        size_t n=0;uint64_t p;
        while((p=pmm_alloc())!=0)pressure[n++]=p;
        mm_ok(n>=cases[c].budget,"real PMM exhaustion leaves enough held frames for failure budget");
        for(unsigned j=0;j<cases[c].budget;j++)pmm_free(pressure[--n]);
        uint64_t failures=pmm_alloc_failures();unsigned f=atomic_load(&flushes);
        mm_host_cr3=space[0];vmm_map_page(cases[c].va,owned[0],VMM_WRITABLE);
        mm_ok(pmm_alloc_failures()>failures,"kernel table construction reaches an actual PMM allocation failure");
        uint64_t canonical=shared_root(kernel,cases[c].va);
        for(int i=0;i<2;i++){
            mm_eqi(shared_root(space[i],cases[c].va),canonical,"partial kernel table allocation failure publishes valid shared roots");
            mm_ok(!(leaf(space[i],cases[c].va)&1),"partial allocation never publishes a present data leaf");
        }
        mm_eqi(atomic_load(&flushes)-f,cases[c].budget?1:0,"partial shared root publication confirms exactly one shootdown");
        while(n)pmm_free(pressure[--n]);
        vmm_map_page(cases[c].va,owned[0],VMM_WRITABLE);
        for(int i=0;i<2;i++)mm_eqi(leaf(space[i],cases[c].va)&MM_PTE_ADDR,owned[0],"kernel mapping retry after allocation failure reaches existing address spaces");
    }
    free(pressure);
}
int main(void)
{
    mm_log_quiet(1);mm_sim_init(128);kernel=mm_sim_kernel_space();vmm_kernel_cr3();reclaim_set_enabled(0);
    uint64_t baseline=pmm_free_frames();space[0]=vmm_new_space();space[1]=vmm_new_space();
    mm_ok(space[0]&&space[1],"two address spaces predate shared kernel publication");
    for(int i=0;i<4;i++)mm_ok(page()!=0,"fixture owns physical kernel data page");
    mm_host_cr3=space[0];vmm_map_page(LOW,owned[0],VMM_WRITABLE);
    vmm_map_page_in(space[1],HIGH,owned[1],VMM_WRITABLE);
    for(int i=0;i<2;i++){
        mm_eqi(leaf(space[i],LOW)&MM_PTE_ADDR,owned[0],"new low supervisor root visible in both existing address spaces");
        mm_eqi(leaf(space[i],HIGH)&MM_PTE_ADDR,owned[1],"new high supervisor root visible in both existing address spaces");
        mm_ok(!(leaf(space[i],LOW)&VMM_USER)&&!(leaf(space[i],HIGH)&VMM_USER),"published mappings remain supervisor-only");
        uint64_t lp=pmm_alloc(),wp=pmm_alloc();
        vmm_map_page_in(space[i],MM_USER_BASE,lp,VMM_USER|VMM_WRITABLE);
        vmm_map_page_in(space[i],MM_USER_WIDE_BASE,wp,VMM_USER|VMM_WRITABLE);
        vmm_map_page_in(space[i],MM_USER_BASE,owned[0],VMM_WRITABLE);
        vmm_map_page_in(space[i],MM_USER_WIDE_BASE,owned[1],VMM_WRITABLE);
        mm_eqi(leaf(space[i],MM_USER_BASE)&MM_PTE_ADDR,lp,"supervisor map cannot overwrite private legacy user root");
        mm_eqi(leaf(space[i],MM_USER_WIDE_BASE)&MM_PTE_ADDR,wp,"supervisor map cannot overwrite private wide user root");
    }
    uint64_t *kpml4=mm_p2v(kernel),*kpdpt=mm_p2v(kpml4[0]&MM_PTE_ADDR);race_pd=mm_p2v(kpdpt[3]&MM_PTE_ADDR);
    uint64_t before=pmm_free_frames();atomic_store(&armed,1);pair(mapper,mapper);atomic_store(&armed,0);
    for(int i=0;i<2;i++){
        mm_ok((leaf(space[i],RACE)&MM_PTE_ADDR)==owned[2]&&(leaf(space[i],RACE+4096)&MM_PTE_ADDR)==owned[3],"concurrent kernel table publication preserves both leaf mappings");
    }
    mm_eqi(before-pmm_free_frames(),1,"shared absent PT is allocated exactly once across two AS callers");
    if(mm_fails){cleanup();mm_eqi(pmm_free_frames(),baseline,"all shared page tables and data ownership return to baseline");mm_sim_done();return mm_summary("bkl-kernel-map");}
    partial_allocation();
    uint64_t block=pmm_alloc_contig(4);mm_ok(block!=0,"contiguous physical range for batched mapping");
    for(int i=0;i<4;i++)owned[nowned++]=block+(uint64_t)i*4096;
    unsigned f=atomic_load(&flushes);mm_host_cr3=space[0];vmm_map_range(LOW+0x2000000,block,4*4096,VMM_WRITABLE);
    mm_eqi(atomic_load(&flushes)-f,1,"kernel map range batches four page changes into one shootdown");
    f=atomic_load(&flushes);vmm_map_range(LOW+0x2000000,block,4*4096,VMM_WRITABLE);
    mm_eqi(atomic_load(&flushes),f,"identical supervisor mappings do not broadcast a redundant shootdown");
    vmm_map_range(LOW+0x2000000,block,4*4096,VMM_WRITABLE|VMM_NOCACHE);
    mm_eqi(atomic_load(&flushes)-f,1,"kernel permission changes also batch shootdown confirmation");
    struct mm_guard kg=mm_kernel_guard_start(),ug=mm_guard_try(space[0]);
    mm_ok(kg.held&&ug.held&&kg.slot!=ug.slot,"shared kernel owner occupies a slot outside the user AS hash");
    mm_guard_end(&ug);mm_guard_end(&kg);
    for(int i=0;i<EXTRA;i++)extra[i]=page();
    pair(churn_mapper,churn_spaces);
    mm_eqi(atomic_load(&errors),0,"kernel roots stay valid across concurrent create publish retire and free");
    for(int i=0;i<2;i++)for(int j=0;j<EXTRA;j++)
        mm_eqi(leaf(space[i],(uint64_t)(j+4)<<30)&MM_PTE_ADDR,extra[j],"existing AS receives every root published during process churn");
    for(int i=0;i<nowned;i++)mm_eqi(pmm_refcount(owned[i]),1,"shared kernel frame retains exactly its original owner");
    cleanup();mm_eqi(pmm_free_frames(),baseline,"all shared page tables and data ownership return to baseline");
    mm_eqi(pmm_bugs(),0,"no PMM reference or double-free violations during kernel-root publication");
    mm_eqi(atomic_load(&lock_errors),0,"production ticket locks preserve CPU ownership");
    mm_sim_done();return mm_summary("bkl-kernel-map");
}
