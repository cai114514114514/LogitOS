/* SPDX-License-Identifier: MIT
 * Production kheap + production ticket locks, with only CPU identity and
 * contiguous RAM supplied by the host. The 8 workers verify payloads before
 * ownership transfer/free; PMM refusal forces the actual magazine drain. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "kheap.h"
#include "pmm.h"
#define CPUS 8
#define BATCH 32
#define ROUNDS 32
#define HOST_RAM (64u*1024u*1024u)
static _Thread_local int cpu;
uint64_t mm_host_base,mm_host_kend;
_Thread_local uint64_t mm_host_cr3;
static uint64_t next_phys=4096;
static unsigned pmm_calls,pmm_failures;
static int refuse_growth;
static atomic_int corrupt,lock_errors;
static unsigned iterations=10000;
int kheap_cpu_index(void){return cpu;}
int logit_lock_host_cpu(void){return cpu;}
void tlb_service(void){}
void serial_putc(char c){(void)c;atomic_fetch_add(&lock_errors,1);}
void kprintf(const char *fmt,...){(void)fmt;}
uint64_t pmm_alloc_contig(size_t frames)
{
    pmm_calls++;
    if(refuse_growth||frames>HOST_RAM/4096||next_phys+frames*4096>HOST_RAM){pmm_failures++;return 0;}
    uint64_t result=next_phys;next_phys+=frames*4096;return result;
}
/* This fixture supplies one small RAM arena; address-zone preference belongs
 * to the real-PMM highheap fixture. Both paths retain failure injection. */
uint64_t pmm_alloc_contig_masked(size_t frames,uint64_t mask,size_t align,size_t boundary)
{ (void)mask;(void)align;(void)boundary;return pmm_alloc_contig(frames); }
struct barrier {pthread_mutex_t lock;pthread_cond_t cv;unsigned entered,generation;};
static struct barrier gate={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0};
static void barrier(void)
{
    pthread_mutex_lock(&gate.lock);unsigned seen=gate.generation;
    if(++gate.entered==CPUS){gate.entered=0;gate.generation++;pthread_cond_broadcast(&gate.cv);}
    else while(seen==gate.generation)pthread_cond_wait(&gate.cv,&gate.lock);
    pthread_mutex_unlock(&gate.lock);
}
static uint64_t nanos(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;}
static unsigned char pattern(unsigned who,unsigned round,unsigned slot){return (unsigned char)(1+who*29+round*13+slot*7);}
static void fill(void *p,size_t size,unsigned char value){if(p)memset(p,value,size);else atomic_fetch_add(&corrupt,1);}
static void verify(void *p,size_t size,unsigned char value)
{
    if(!p)return;
    const unsigned char *b=p;
    for(size_t i=0;i<size;i++)if(b[i]!=value){atomic_fetch_add(&corrupt,1);break;}
}
static void *handoff[CPUS][BATCH][2];
static void *worker(void *arg)
{
    cpu=(int)(uintptr_t)arg;barrier();
    for(unsigned i=0;i<iterations;i++){
        void *a=kmalloc(64),*b=kmalloc(512);unsigned char p=pattern(cpu,i,0);
        fill(a,64,p);fill(b,512,(unsigned char)(p^0x5a));
        verify(a,64,p);verify(b,512,(unsigned char)(p^0x5a));kfree(a);kfree(b);
    }
    for(unsigned round=0;round<ROUNDS;round++){
        for(unsigned j=0;j<BATCH;j++){
            handoff[cpu][j][0]=kmalloc(64);handoff[cpu][j][1]=kmalloc(512);
            fill(handoff[cpu][j][0],64,pattern(cpu,round,j));
            fill(handoff[cpu][j][1],512,pattern(cpu,round,j)^0x5a);
        }
        barrier();int from=(cpu+CPUS-1)%CPUS;
        for(unsigned j=0;j<BATCH;j++){
            verify(handoff[from][j][0],64,pattern(from,round,j));
            verify(handoff[from][j][1],512,pattern(from,round,j)^0x5a);
            kfree(handoff[from][j][0]);kfree(handoff[from][j][1]);
        }
        barrier();
    }
    return NULL;
}
static int checks,failures;
#define CHECK(c,s) do{checks++;if(!(c)){failures++;printf("FAIL %s\n",s);}}while(0)
int main(int argc,char **argv)
{
    if(argc>1){unsigned long n=strtoul(argv[1],NULL,10);if(n>0&&n<=10000000)iterations=(unsigned)n;}
    void *ram=aligned_alloc(4096,HOST_RAM);if(!ram)return 2;mm_host_base=(uint64_t)(uintptr_t)ram;
    pthread_t threads[CPUS];uint64_t t0=nanos();
    for(int i=0;i<CPUS;i++)if(pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i))return 2;
    for(int i=0;i<CPUS;i++)pthread_join(threads[i],NULL);
    uint64_t elapsed=nanos()-t0;
#ifdef KHEAP_MEASURE_ONLY
    /* This identical byte workload also runs against archived allocator source
     * whose stats struct predates magazines. No absent fields are inspected. */
    CHECK(atomic_load(&corrupt)==0,"measured workload preserves every payload byte");
    CHECK(atomic_load(&lock_errors)==0,"measured workload keeps ticket lock ownership");
    printf("KHEAP_MEASURE cpus=%d iterations=%u wall_ns=%llu checks=%d failures=%d\n",CPUS,iterations,(unsigned long long)elapsed,checks,failures);
    free(ram);return failures?1:0;
#else
    struct kheap_stats cached,drained,final;kheap_get_stats(&cached);
    CHECK(atomic_load(&corrupt)==0,"eight CPU allocation and cross-CPU free preserve every payload byte");
    CHECK(atomic_load(&lock_errors)==0,"production ticket locks preserve CPU release ownership");
    CHECK(cached.magazine_cpus==CPUS,"magazine_cpus records eight independent active CPU caches");
    CHECK(cached.magazine_hits>=2ull*CPUS*(iterations-1),"small allocations reuse warmed CPU magazines");
    CHECK(cached.magazine_bytes==CPUS*BATCH*(64ull+512ull),"cached byte total equals all eight bounded magazines");
    CHECK(cached.live_bytes==cached.magazine_bytes,"quiescent heap live accounting contains only cached blocks");
    void *held=kmalloc(4096);fill(held,4096,0xa7);unsigned calls=pmm_calls;
    refuse_growth=1;void *oversize=kmalloc(128ull*1024*1024);kheap_get_stats(&drained);
    CHECK(!oversize&&pmm_calls==calls+1&&pmm_failures==1,"forced PMM refusal reaches bounded allocator failure");
    CHECK(drained.magazine_drains>cached.magazine_drains&&drained.magazine_bytes==0,"failed large allocation drains all CPU cache ownership");
    CHECK(drained.live_bytes==4096&&drained.live_blocks==1,"drain removes cached blocks from live resource totals");
    verify(held,4096,0xa7);CHECK(atomic_load(&corrupt)==0,"drain preserves concurrently retained large payload");
    kfree(held);calls=pmm_calls;size_t whole=4u*1024u*1024u-16;void *large=kmalloc(whole);
    CHECK(large&&pmm_calls==calls,"drained blocks coalesce into full arena despite PMM refusal");
    fill(large,whole,0x6b);verify(large,whole,0x6b);kfree(large);kheap_get_stats(&final);
    CHECK(atomic_load(&corrupt)==0,"coalesced large allocation preserves written content");
    CHECK(final.live_bytes==0&&final.live_blocks==0&&final.magazine_bytes==0,"all caller and magazine resources return to baseline");
    CHECK(final.free_bytes+16*final.grows==final.arena_bytes,"all arena bytes balance as free payload and arena headers");
    printf("BKL_KHEAP cpus=%d iterations=%u wall_ns=%llu hits=%llu puts=%llu cached_bytes=%llu drains=%llu arena=%llu live=%llu checks=%d failures=%d\n",CPUS,iterations,(unsigned long long)elapsed,cached.magazine_hits,cached.magazine_puts,cached.magazine_bytes,final.magazine_drains,final.arena_bytes,final.live_bytes,checks,failures);
    free(ram);return failures?1:0;
#endif
}
