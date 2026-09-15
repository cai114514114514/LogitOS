/* SPDX-License-Identifier: MIT
 * Guest-only MM verification, linked exclusively by WIDEVERIFY=1. A bounded
 * private address space selects the high frames directly: filling 8 GiB merely
 * to make a CLOCK sweep find one high page would test host patience, not MM.
 * The actual fault, COW, cache, eviction and device swap code remain production
 * code. No production syscall exposes this test selector. */
#include "wide_memory_verify.h"
#include "dma.h"
#include "wide_dma_snapshot_wait.h"
#include "blkdev.h"
#include "mm.h"
#include "mmhost.h"
#include "pmm.h"
#include "vmm.h"
#include "vma.h"
#include "rmap.h"
#include "swap.h"
#include "reclaim.h"
#include "pcache.h"
#include "kprintf.h"
#include <stdint.h>
#include <stddef.h>
int reclaim_verify_evict(uint64_t phys);
void reclaim_late_init(void);
static int errors;
static void ck(int yes,const char *s) {
    kprintf("[widecheck] %s %s\n",yes?"PASS":"FAIL",s);
    if(!yes)errors++;
}
static uint64_t frame(uint64_t cr3,uint64_t va) {
    uint64_t *p=vmm_pte(cr3,va);return p&&(*p&1)?(*p&MM_PTE_ADDR):0;
}
static uint64_t pattern(unsigned n) { return 0x129a34b56c78d0efull^((uint64_t)n*0x102030405ull); }
/* Test images only: runners always attach PRIVATE disk copies. Save/restore a
 * 512 KiB tail extent of each DMA disk, so both root and swap backends move
 * actual high payloads. Never enable this selector in a production kernel. */
static void dma_disk_verify(void) {
    const size_t bytes=512*1024;
    struct dma_device owner;dma_device_init(&owner,"dma-guest-data",DMA_MASK_64);
    struct dma_buffer *saved=dma_alloc_coherent(&owner,bytes,4096,0);
    struct dma_buffer *write=dma_alloc_coherent(&owner,bytes+4096,4096,0);
    struct dma_buffer *read=dma_alloc_coherent(&owner,bytes+4096,4096,0);
    ck(saved&&write&&read,"DMA guest owns independent high data buffers");
    if(!saved||!write||!read)goto done;
    /* Deliberately unaligned CPU/physical offsets exercise multi-page PRPs,
     * rather than only the especially easy one-aligned-page case. */
    unsigned char *w=(unsigned char*)write->cpu+37,*r=(unsigned char*)read->cpu+123;
    ck((uint64_t)(uintptr_t)w!=write->dma.value+37,"DMA CPU alias differs even in low RAM");
    for(size_t j=0;j<bytes;j++)w[j]=(unsigned char)((j*131u)^(j>>8)^0x5a);
    for(int i=0;i<blk_count();i++) {
        struct blkdev *d=blk_at(i);
        if(d->parent||d->nsectors<2048||(d->name[0]=='a'&&d->name[1]=='t'))continue;
        uint64_t lba=d->nsectors-bytes/512;
        struct dma_stats before,after,raw_before,raw_after;
        dma_get_stats(&raw_before);
        int before_idle=wide_dma_idle_snapshot(&before,timer_ms()+1000);
        ck(before_idle,"backend baseline becomes idle within deadline");
        int ok=blk_dev_read(d,lba,bytes/512,saved->cpu)==0;
        ck(ok,"save private test disk tail before DMA write");if(!ok)continue;
        ok=blk_dev_write(d,lba,bytes/512,w)==0&&blk_dev_flush(d)==0;
        for(size_t j=0;j<bytes;j++)r[j]=0xa5;
        ok=ok&&blk_dev_read(d,lba,bytes/512,r)==0;
        for(size_t j=0;j<bytes;j++)if(r[j]!=w[j])ok=0;
        ck(ok,"device 512 KiB cross-page DMA read/write byte equality");
        int restored=blk_dev_write(d,lba,bytes/512,saved->cpu)==0&&blk_dev_flush(d)==0;
        restored=restored&&blk_dev_read(d,lba,bytes/512,r)==0;
        for(size_t j=0;j<bytes;j++)if(r[j]!=((unsigned char*)saved->cpu)[j])restored=0;
        ck(restored,"private disk original bytes restored exactly");
        dma_get_stats(&raw_after);
        /* This is a global snapshot, so retain both sides when an unrelated
         * background request overlaps the per-backend assertion. A later
         * zero count is not evidence that this immediate comparison passed. */
        kprintf("[dmacheck] resources backend=%s mappings=%llu/%llu pins=%llu/%llu direct=%llu/%llu bounce=%llu/%llu\n",
                d->name,raw_before.active_mappings,raw_after.active_mappings,
                raw_before.pinned_pages,raw_after.pinned_pages,raw_before.direct_bytes,
                raw_after.direct_bytes,raw_before.bounce_bytes,raw_after.bounce_bytes);
        int after_idle=wide_dma_idle_snapshot(&after,timer_ms()+1000);
        if(pmm_total_bytes()>(1ull<<32))ck(after.completed_high_bytes>=before.completed_high_bytes+2*bytes,"backend completed high physical data transfer");
        ck(before_idle&&after_idle&&before.active_mappings==after.active_mappings&&before.pinned_pages==after.pinned_pages&&before.bounce_bytes==after.bounce_bytes,"backend per-request resources return to baseline");
        kprintf("[dmacheck] backend=%s cpu=%p dma=%p bytes=%llu completed_high=%llu result=%s\n",d->name,w,write->dma.value+37,(uint64_t)bytes,after.completed_high_bytes-before.completed_high_bytes,ok&&restored?"PASS":"FAIL");
    }
done:
    dma_free_coherent(saved);dma_free_coherent(write);dma_free_coherent(read);
}
int wide_memory_verify(void) {
    errors=0;
    kprintf("[widecheck] capacity usable=%llu high_free_pages=%llu\n",
            pmm_total_bytes(), pmm_high_free_frames());
    struct dma_stats dma_before,dma_after;
    int dma_before_idle=wide_dma_idle_snapshot(&dma_before,timer_ms()+1000);
    int dma_after_idle;
    ck(dma_before_idle,"global DMA baseline becomes idle within deadline");
    reclaim_late_init();
    ck(swap_ready(),"dedicated swap device present");
    int enabled=reclaim_enabled();reclaim_set_enabled(0);
    dma_disk_verify();
    uint64_t slots=swap_slots_used(),before=pmm_used_frames();
    uint64_t a=vmm_new_space(),b=0;
    const uint64_t va=MM_USER_WIDE_BASE+(1ull<<39)-4096;
    ck(a!=0,"private address space");
    if(!a)goto end;
    ck(!mm_user_addr(0x90000000ull)&&!mm_user_addr(0xffff800000000000ull),"MMIO gap and physmap excluded from user windows");
    ck(vma_reserve_fixed(a,va,8192,VMA_READ|VMA_WRITE)==0,"reservation crosses PML4 boundary");
    ck(mm_fault_in(a,va,6)&&mm_fault_in(a,va+4096,6),"anonymous high-VA faults");
    uint64_t f=frame(a,va);
    ck(f!=0,"anonymous page has a real frame");
    if(!f)goto cleanup;
    kprintf("[widecheck] physical=%p high_allocs=%p high_max=%p\n",(void*)f,(void*)pmm_high_allocations(),(void*)pmm_high_max_phys());
    if(pmm_total_bytes()>(1ull<<32))ck(f>=(1ull<<32),"anonymous physical page above 4 GiB");
    else if(pmm_total_bytes()>(1ull<<30))ck(f>=(1ull<<30),"anonymous physical page above 1 GiB");
    uint64_t *data=mm_p2v(f);
    for(unsigned i=0;i<512;i++)data[i]=pattern(i);
    data=mm_p2v(frame(a,va+4096));data[0]=0xace123;
    b=vmm_new_space();
    ck(b&&vmm_clone_user(b,a)==0,"clone both high PML4 subtrees");
    if(!b)goto cleanup;
    ck(frame(b,va)==f&&frame(b,va+4096)==frame(a,va+4096),"COW initially shares both high pages");
    ck(mm_fault_in(b,va,7),"child high-page write triggers COW");
    uint64_t cf=frame(b,va);
    ck(cf&&cf!=f,"COW allocates a distinct physical page");
    if(!cf)goto cleanup;
    data=mm_p2v(cf);int same=1;for(unsigned i=0;i<512;i++)if(data[i]!=pattern(i))same=0;
    ck(same,"COW copies all high physical bytes");data[0]=0xbeef;
    ck(((uint64_t*)mm_p2v(f))[0]==pattern(0),"child write leaves parent bytes intact");
    vmm_free_space(b);b=0;
    ck(vma_protect(a,va,4096,VMA_READ)==0,"high mapping protects read-only");
    vmm_protect_range_in(a,va,4096,VMA_READ);
    ck(vmm_user_range_ok(a,(void*)va,8,0)&&!vmm_user_range_ok(a,(void*)va,8,1),"read-only permissions reach usercopy");
    if(swap_ready()) {
        uint64_t wr=swap_writes(),rd=swap_reads();
        ck(reclaim_verify_evict(f)==1,"production reclaim evicts selected high page");
        uint64_t *pte=vmm_pte(a,va);
        ck(pte&&vmm_pte_is_swap(*pte),"eviction installs a swap PTE");
        ck(mm_fault_in(a,va,4),"high-VA swap fault restores page");
        uint64_t restored=frame(a,va);same=restored!=0;
        if(restored){data=mm_p2v(restored);for(unsigned i=0;i<512;i++)if(data[i]!=pattern(i))same=0;}
        ck(same&&swap_writes()>wr&&swap_reads()>rd,"device swap roundtrip preserves full page");
    }
cleanup:
    if(b)vmm_free_space(b);
    if(a)vmm_free_space(a);
    ck(swap_slots_used()==slots,"all private swap slots reclaimed");
    ck(pmm_used_frames()==before,"private address spaces return every frame");
    /* The disk-backed cache can retain a pre-existing entry; test its bytes and
     * high frame identity independently from the private-space accounting. */
    int fh=pcache_file_open("/docs/readme.txt");
    ck(fh>=0,"open real on-disk cache fixture");
    if(fh>=0){
        uint64_t pf=pcache_get_ref(fh,0);
        ck(pf!=0,"page cache reads through mapped high CPU alias");
        if(pf){
            volatile unsigned char x=((unsigned char*)mm_p2v(pf))[0];
            ck(x!=0,"on-disk page data is readable");
            if(pmm_total_bytes()>(1ull<<32))ck(pf>=(1ull<<32),"page-cache physical page above 4 GiB");
        }
        if(pf)pmm_free(pf);
        pcache_file_put(fh);
    }
    ck(pmm_audit()==0&&rmap_audit()==0,"allocator and reverse-map accounting");
end:
    dma_after_idle=wide_dma_idle_snapshot(&dma_after,timer_ms()+1000);
    ck(dma_before_idle && dma_after_idle &&
       dma_after.active_mappings==dma_before.active_mappings &&
       dma_after.pinned_pages==dma_before.pinned_pages &&
       dma_after.bounce_bytes==dma_before.bounce_bytes &&
       dma_after.direct_bytes==dma_before.direct_bytes &&
       dma_after.quarantined_bytes==dma_before.quarantined_bytes,
       "DMA mappings pins and bounce resources restore baseline");
    if(pmm_total_bytes()>(1ull<<32))
        ck(dma_after.completed_high_bytes>=dma_before.completed_high_bytes+8192,
           "device completed high-page direct write and read");
    dma_report("wide-memory");
    reclaim_set_enabled(enabled);
    kprintf("[widecheck] RESULT failures=%d\n",errors);
    return errors? -1:0;
}
