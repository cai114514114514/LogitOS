/* Real DMA core + real PMM over sparse RAM. The resolver seam models only CPU
 * page-table translation; allocation, SG, pinning and lifetimes are production. */
#define main physmap_baseline_main
#include "physmap_test.c"
#undef main
#include "dma.h"
static void *scatter;
static uint64_t scatter_pages[3];
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
uint64_t dma_host_resolve(void *p)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)scatter;
    if(scatter&&a>=b&&a-b<12288)return scatter_pages[(a-b)/4096]+((a-b)&4095);
    return mm_v2p(p);
}
#define MUST(x,msg) do {int ok=!!(x);check(ok,msg);if(!ok)return 1;}while(0)
int main(void)
{
    void *arena=mmap(NULL,ARENA_END,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    MUST(arena!=MAP_FAILED,"reserve sparse physical memory");
    mm_host_base=(uintptr_t)arena;setup(1);
    struct dma_device d,limited;dma_device_init(&d,"host64",DMA_MASK_64);
    dma_device_init(&limited,"host32",DMA_MASK_32);
    uint64_t baseline=pmm_free_frames();
    limited.max_segment=2048;
    check(!dma_alloc_coherent(&limited,4096,4096,0),"coherent allocation enforces device segment length");
    limited.max_segment=UINT32_MAX;
    struct dma_buffer *b=dma_alloc_coherent(&d,12288,65536,65536);
    MUST(b,"coherent allocation");
    MUST((uintptr_t)b->cpu!=dma_addr_value(b->dma),"CPU_POINTER_ASSERT: CPU alias differs from DMA address");
    MUST(dma_addr_value(b->dma)==b->phys&&b->phys>=HIGH_B,"HIGH_ADDRESS_ASSERT: no truncation beyond 4GiB");
    check(!(b->phys&65535),"alignment respected");
    memset(b->cpu,0x41,b->size);
    struct dma_mapping *m=dma_map_kernel(&d,(char*)b->cpu+37,8192,DMA_BIDIRECTIONAL,0);
    MUST(m&&!m->bounced,"cross-page offset direct mapping");
    check(m->npages==3&&dma_mapping_addr(m,4096).value==b->phys+4133,"offset resolves across pages");
    uint64_t t=dma_mapping_submit(m);MUST(t,"submit direct");
#ifndef DMA_NEG_EARLY_FREE
    check(dma_unmap(m)==-1,"owned mapping cannot unmap");
#else
    MUST(dma_unmap(m)==-1,"EARLY_FREE_ASSERT: device owned mapping retained");
#endif
    check(dma_mapping_complete(m,t+1,8192)==-1,"old completion rejected");
    check(dma_mapping_complete(m,t,8193)==-1,"invalid length rejected");
    memset(mm_p2v(dma_mapping_addr(m,0).value),0x6b,8192);
    check(dma_mapping_complete(m,t,8192)==0&&((unsigned char*)b->cpu)[37]==0x6b,"direct device bytes reach CPU alias");
    check(dma_unmap(m)==0,"completed mapping releases");
    unsigned pin_before=pmm_pincount(b->phys);
    struct dma_mapping *overlap1=dma_map_kernel(&d,b->cpu,4096,DMA_FROM_DEVICE,0);
    struct dma_mapping *overlap2=dma_map_kernel(&d,b->cpu,4096,DMA_FROM_DEVICE,0);
    MUST(overlap1&&overlap2,"independent overlapping mappings");
    check(pmm_pincount(b->phys)==pin_before+2,"overlap pins are counted");
    dma_unmap(overlap1);check(pmm_pincount(b->phys)==pin_before+1,"first unmap retains second pin");
    dma_unmap(overlap2);check(pmm_pincount(b->phys)==pin_before,"last unmap releases final pin");
    struct dma_mapping *one=dma_map_kernel(&limited,b->cpu,4096,DMA_TO_DEVICE,0);
    struct dma_mapping *two=dma_map_kernel(&limited,(char*)b->cpu+4096,4096,DMA_FROM_DEVICE,0);
    MUST(one&&two&&one->bounced&&two->bounced,"32-bit device bounces high pages");
    check(one->bounce!=two->bounce&&one->bounce->phys!=two->bounce->phys,"concurrent bounce ownership independent");
    check(one->segments[0].addr.value+4095<=DMA_MASK_32,"full 32-bit extent fits");
    t=dma_mapping_submit(one);check(!memcmp(one->bounce->cpu,b->cpu,4096),"TO copy at submit");
    memset(one->bounce->cpu,0x22,4096);
    check(!dma_mapping_complete(one,t,4096),"TO completion");
    MUST(((unsigned char*)b->cpu)[0]==0x41,"DIRECTION_ASSERT: TO completion cannot overwrite original");
    memset((char*)b->cpu+4096,0x77,4096);
    t=dma_mapping_submit(two);memset(two->bounce->cpu,0x18,4096);
    check(!dma_mapping_complete(two,t,19),"FROM completion actual length");
    check(((unsigned char*)b->cpu)[4096]==0x18&&((unsigned char*)b->cpu)[4115]==0x77,"copy back only valid prefix");
    dma_unmap(one);dma_unmap(two);
    limited.max_segment=1024;limited.boundary=4096;limited.max_segments=8;
    m=dma_map_kernel(&limited,b->cpu,8192,DMA_TO_DEVICE,0);
    MUST(m&&m->bounced&&m->nsegments==8,"bounce splits to obey per-segment length and boundary");
    for(size_t i=0;i<m->nsegments;i++)check(m->segments[i].len<=1024&&(m->segments[i].addr.value&4095)+m->segments[i].len<=4096,"each bounced SG extent obeys boundary");
    dma_unmap(m);limited.max_segment=UINT32_MAX;limited.boundary=0;limited.max_segments=256;

    limited.max_segment=1024;limited.boundary=4096;limited.max_segments=7;
    uint64_t fail_free=pmm_free_frames(),fail_refs=pmm_refcount(b->phys),fail_pins=pmm_pincount(b->phys);
    struct dma_stats fail_before,fail_after;dma_get_stats(&fail_before);
    check(!dma_map_kernel(&limited,b->cpu,8192,DMA_TO_DEVICE,0),"insufficient SG capacity refuses bounce");
    limited.max_segments=8;
    check(!dma_map_kernel(&limited,b->cpu,8192,DMA_TO_DEVICE,DMA_MAP_CONTIGUOUS),"CONTIG cannot cross required boundary");
    limited.boundary=0;
    check(!dma_map_kernel(&limited,b->cpu,8192,DMA_TO_DEVICE,DMA_MAP_CONTIGUOUS),"CONTIG cannot exceed one segment");
    check(!dma_map_kernel(&d,(void*)(UINTPTR_MAX-2),8,DMA_TO_DEVICE,0),"CPU extent wrap rejected before translation");
    check(!dma_alloc_coherent(&d,SIZE_MAX,4096,0)&&!dma_alloc_coherent(&d,4096,(size_t)1<<63,0),"rounded size and impossible alignment rejected");
    dma_get_stats(&fail_after);
    check(pmm_free_frames()==fail_free&&pmm_refcount(b->phys)==fail_refs&&pmm_pincount(b->phys)==fail_pins&&
          fail_before.coherent_buffers==fail_after.coherent_buffers&&fail_before.active_mappings==fail_after.active_mappings&&
          fail_before.pinned_pages==fail_after.pinned_pages&&!limited.buffers&&!limited.mappings,
          "failed bounce leaves no frames refs pins or list entries");
    limited.max_segment=UINT32_MAX;limited.boundary=0;limited.max_segments=256;
    scatter=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    MUST(scatter!=MAP_FAILED,"scattered VA fixture");
    for(int i=0;i<3;i++)scatter_pages[i]=b->phys+(2-i)*4096;
    m=dma_map_kernel(&d,(char*)scatter+13,8192,DMA_BIDIRECTIONAL,0);
    MUST(m&&!m->bounced&&m->nsegments==3,"noncontiguous kernel pages form real SG");
    check(dma_mapping_addr(m,4083).value==b->phys+4096,"second page is translated independently");dma_unmap(m);
    uint64_t refs_before=pmm_refcount(scatter_pages[0]);
    uint64_t save_page=scatter_pages[1];scatter_pages[1]=0x80000000;
    check(!dma_map_kernel(&d,scatter,8192,DMA_FROM_DEVICE,0),"partial page resolution failure refuses mapping");
    check(pmm_refcount(scatter_pages[0])==refs_before&&!pmm_pincount(scatter_pages[0]),"failed partial mapping releases preceding pins and refs");
    scatter_pages[1]=save_page;
    d.max_segments=1;
    m=dma_map_kernel(&d,(char*)scatter+13,8192,DMA_BIDIRECTIONAL,DMA_MAP_CONTIGUOUS);
    MUST(m&&m->bounced,"SG constraint uses contiguous independent bounce");
    memset((char*)scatter+13,0x34,8192);t=dma_mapping_submit(m);
    check(((unsigned char*)m->bounce->cpu)[0]==0x34,"bidirectional submit copy");
    memset(m->bounce->cpu,0x56,8192);dma_mapping_complete(m,t,8192);
    check(((unsigned char*)scatter)[13]==0x56,"bidirectional completion copy");dma_unmap(m);d.max_segments=256;
    m=dma_map_kernel(&d,b->cpu,4096,DMA_FROM_DEVICE,0);MUST(m,"quarantine fixture");
    t=dma_mapping_submit(m);dma_device_quarantine(&d);
    check(dma_unmap(m)==-1&&dma_mapping_complete(m,t,1)==-1,"quarantine retains DMA and rejects late completion");
    check(!dma_alloc_coherent(&d,4096,0,0)&&dma_device_resume(&d)==-1,"quarantine blocks submissions and premature resume");
    struct dma_stats s;dma_get_stats(&s);check(s.quarantined_mappings==1,"quarantine counted");
    dma_device_quiesced(&d);check(dma_mapping_complete(m,t,1)==-1,"completion after stop is stale");
    check(!dma_unmap(m)&&!dma_device_resume(&d),"acknowledged stop releases mapping");
    check(!dma_map_kernel(&d,(void*)(mm_host_base+0x80000000),4096,DMA_TO_DEVICE,0),"MMIO holes refused");
    check(!dma_map_kernel(&d,b->cpu,DMA_MAX_MAPPING+1,DMA_TO_DEVICE,0),"mapping allocation bounded");
    check(!dma_alloc_coherent(&limited,65537,4096,65536),"allocation failure preserves state");
    dma_free_coherent(b);dma_get_stats(&s);
    check(!s.coherent_buffers&&!s.active_mappings&&!s.pinned_pages&&!s.bounce_bytes&&!s.direct_bytes&&!s.quarantined_mappings,"all live DMA resource counts restore baseline");
    check(pmm_free_frames()==baseline&&!pmm_audit(),"PMM baseline restored");
    munmap(scatter,12288);scatter=NULL;
    setup(0);dma_device_init(&d,"low-only",DMA_MASK_64);
    b=dma_alloc_coherent(&d,4096,0,0);MUST(b,"low memory DMA allocation");
    check(b->phys<LOW_END&&(uintptr_t)b->cpu!=b->dma.value,"low memory still uses distinct CPU and DMA addresses");
    dma_free_coherent(b);munmap(arena,ARENA_END);
    printf("dma-core: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
