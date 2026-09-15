/* SPDX-License-Identifier: MIT
 * Test-only syscall body: exercise real heap/PMM/page tables/stacks and DMA.
 * Ordinary images do not contain this selector or the pressure workload. */
#include "kheap.h"
#include "pmm.h"
#include "mmhost.h"
#include "sched.h"
#include "percpu.h"
#include "ktime.h"
#include "tlb.h"
#include "kprintf.h"
#include "dma.h"
#include "panic.h"
#include <stdint.h>
static unsigned seen;
#define CHECK(x,s) do { int ok=!!(x);kprintf("[highheap] %s %s\n",ok?"PASS":"FAIL",s);if(!ok)errors++; } while(0)
static uint64_t word(size_t j,unsigned block) { return 0x67a18923cafed005ull^((uint64_t)j*0x102030407ull)^block; }
static int mapped_kernel(uint64_t va)
{
    uint64_t pa=mm_read_cr3();int user=1;
    for(int shift=39;shift>=12;shift-=9){
        uint64_t e=((uint64_t *)mm_p2v(pa))[(va>>shift)&511];
        if(!(e&1))return 0;if(!(e&4))user=0;
        if(shift==12||(e&128))return !user&&(e&2)&&(e&(1ull<<63));
        pa=e&0x000ffffffffff000ull;
    }
    return 0;
}
static int capacity(void)
{
    int errors=0;
    /* >1 GiB of actual heap payload on 8 GiB machines, all words written and
     * checked. Small machines perform the same operation with four arenas. */
    unsigned count=pmm_total_bytes()>(4ull<<30)?20:4;
    size_t bytes=(count==20?(64u<<20):(4u<<20))-16;
    uint64_t *blocks[20]={0};unsigned allocated=0;uint64_t total=0;
    uint64_t low_before=pmm_low_zone_free_frames(),start=time_mono_raw_ns();
    struct kheap_stats before,peak,after;kheap_get_stats(&before);
    for(unsigned i=0;i<count;i++){
        blocks[i]=kmalloc(bytes);if(!blocks[i])break;allocated++;
        CHECK((uintptr_t)blocks[i]>=PHYSMAP_BASE,"ALIAS_ASSERT ordinary heap uses supervisor alias");
        uint64_t phys=mm_v2p(blocks[i]);
        if(count==20)CHECK(phys>=(1ull<<32),"HIGH_HEAP_ASSERT pressure payload above4g");
        CHECK(pmm_is_ram(phys,bytes)&&pmm_refcount(phys)==1,"heap owns real RAM pages");
        for(size_t j=0;j<bytes/8;j++)blocks[i][j]=word(j,i);
        total+=bytes;
    }
    kheap_get_stats(&peak);
    for(unsigned i=0;i<allocated;i++){
        int same=1;for(size_t j=0;j<bytes/8;j++)if(blocks[i][j]!=word(j,i))same=0;
        CHECK(same,"pressure verifies every payload word");kfree(blocks[i]);
    }
    kheap_get_stats(&after);
    CHECK(allocated==count,"entire requested heap capacity allocated and released");
    if(count==20){
        CHECK(total>(1ull<<30),"more than1g heap payload actually written and checked");
        /* IRQ/UI activity can allocate a few low page tables concurrently;
         * it cannot explain a 1.25 GiB heap quietly spending the low zone. */
        CHECK(pmm_low_zone_free_frames()+1024>=low_before,"large heap growth preserves low zone");
    }
    CHECK(peak.live_bytes>=before.live_bytes+total-(1u<<20),"peak heap accounting includes pressure payload");
    CHECK(after.live_bytes<before.live_bytes+(8u<<20),"pressure owners released within background activity bound");
    CHECK(after.arena_bytes==peak.arena_bytes,"freed arenas remain reusable heap ownership");
    kprintf("[highheap-capacity] bytes=%llu words=%llu allocations=%u arena=%llu low=%llu above4g=%llu live_before=%llu live_after=%llu ms=%llu\n",
            total,total/8,allocated,after.arena_bytes,after.low_arena_bytes,after.far_arena_bytes,
            before.live_bytes,after.live_bytes,(time_mono_raw_ns()-start)/1000000);
    CHECK(pmm_audit()==0,"PMM audit after heap pressure");return errors?-1:0;
}
long highheap_verify(long phase)
{
    if(phase==0)return capacity();
    if(phase!=1)return -1;
    int errors=0;unsigned cpu=(unsigned)this_cpu()->index;
    __atomic_fetch_or(&seen,1u<<cpu,__ATOMIC_ACQ_REL);
    uint64_t end=time_mono_raw_ns()+3000000000ull;
    while(__builtin_popcount(__atomic_load_n(&seen,__ATOMIC_ACQUIRE))<4 && time_mono_raw_ns()<end){tlb_service();__asm__ volatile("pause");}
    CHECK(__builtin_popcount(__atomic_load_n(&seen,__ATOMIC_ACQUIRE))==4,"four CPUs enter heap workload concurrently");
    uint64_t rsp,rbp;__asm__ volatile("mov %%rsp,%0;mov %%rbp,%1":"=r"(rsp),"=r"(rbp));
    CHECK(rsp>=PHYSMAP_BASE,"ALIAS_ASSERT actual syscall stack uses supervisor alias");
    CHECK(mapped_kernel(rsp),"active process maps high stack supervisor writable NX");
    uint64_t frames[8];CHECK(backtrace(rbp,rsp,frames,8)>=2,"real high stack frame-chain backtrace");
    uint64_t cr3=mm_read_cr3();struct thread *self=sched_current_thread();
    unsigned char *p=kmalloc(12301);void *lo=kmalloc_low(64);
    CHECK(p&&lo,"concurrent ordinary and low objects allocated");
    if(p&&lo){
        CHECK((uintptr_t)p>=PHYSMAP_BASE&&(uintptr_t)lo<PMM_LOW_LIMIT,"LOW_DOMAIN_ASSERT independent allocation domains");
        if(pmm_total_bytes()>(4ull<<30))CHECK(mm_v2p(p)>=(1ull<<32),"HIGH_HEAP_ASSERT ordinary concurrent object above4g");
        for(unsigned i=0;i<12301;i++)p[i]=(unsigned char)(i*31+cpu);
        for(unsigned i=0;i<32;i++)schedule();
        int same=1;for(unsigned i=0;i<12301;i++)if(p[i]!=(unsigned char)(i*31+cpu))same=0;
        CHECK(same&&self==sched_current_thread()&&cr3==mm_read_cr3(),"high stack and payload survive32 context switches");
        struct dma_device dev;dma_device_init(&dev,"heap-stream",DMA_MASK_64);
        struct dma_mapping *m=dma_map_kernel(&dev,p+37,8192,DMA_BIDIRECTIONAL,0);
        CHECK(m&&!m->bounced,"ordinary heap streams through real DMA mapper");
        if(m){
            CHECK(dma_addr_value(dma_mapping_addr(m,0))==mm_v2p(p+37),"heap DMA device address is physical not CPU pointer");
            uint64_t ticket=dma_mapping_submit(m);
            CHECK(ticket&&dma_mapping_complete(m,ticket,8192)==0&&dma_unmap(m)==0,"heap DMA mapping completes and releases pins");
        }
    }
    kfree(p);kfree(lo);
    kprintf("[highheap-cpu] cpu=%u stack=%p phys=%p failures=%d\n",cpu,(void *)rsp,(void *)mm_v2p((void *)rsp),errors);
    return errors?-1:0;
}
