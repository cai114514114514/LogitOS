/* SPDX-License-Identifier: MIT
 * Actual PMM and heap: sparse firmware RAM above 4 GiB with a reserved hole,
 * not malloc pretending to be a physical allocator. CPU alias arithmetic on
 * small RAM is checked in the guest (MM_HOSTTEST has one offset alias). */
#define main old_physmap_main
#include "physmap_test.c"
#undef main
#include "kheap.h"
#define FAR_BYTES (64u*1024u*1024u)
static int fixture(void)
{
    void *a=mmap(NULL,HIGH_B+FAR_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(a==MAP_FAILED)return 0;
    mm_host_base=(uintptr_t)a;setup(1);
    /* Enlarge the last real firmware descriptor, then reinitialise PMM before
     * any heap allocation. setup's bootstrap metadata lies in the low zone. */
    struct entry *e=(void *)((char *)mm_p2v(INFO)+8+16);
    e[3].len=FAR_BYTES;memset(mm_p2v(ROOT),0,4096);pmm_init(INFO);return 1;
}
int main(void)
{
    if(!fixture())return 2;
    struct kheap_stats a,b,c;
    uint64_t low_before=pmm_low_zone_free_frames();
    void *p=kmalloc(64);check(p!=NULL,"ordinary allocation succeeds");if(!p)return 1;
    check(mm_v2p(p)>=HIGH_B,"HIGH_HEAP_ASSERT: ordinary arena uses above4g RAM");
    check(pmm_low_zone_free_frames()==low_before,"ordinary growth preserves scarce low pages");
    memset(p,0x42,64);kfree(p); /* leave a hot ordinary magazine */
    struct kheap_stats cached;kheap_get_stats(&cached);
    void *lo=kmalloc_low(64);check(lo!=NULL,"low allocation succeeds");if(!lo)return 1;
    check(mm_v2p(lo)<PMM_LOW_LIMIT,"LOW_DOMAIN_ASSERT: low allocation cannot pop high magazine");
    kheap_get_stats(&a);
    check(a.magazine_bytes==cached.magazine_bytes&&a.magazine_drains==cached.magazine_drains,
          "LOW_CACHE_ASSERT: low growth preserves ordinary CPU caches");
    void *lo2=kmalloc_low(8192);kheap_get_stats(&b);
    check(lo2&&mm_v2p(lo2)<PMM_LOW_LIMIT&&b.grows==a.grows,
          "SPLIT_DOMAIN_ASSERT: low remainder stays in its own reusable list");
    if(lo2)memset(lo2,0x73,8192);
    void *objects[128];
    for(unsigned i=0;i<128;i++){
        size_t n=513+i*173;objects[i]=kmalloc(n);
        check(objects[i]!=NULL,"mixed ordinary allocation succeeds");
        if(objects[i]){check(mm_v2p(objects[i])>=HIGH_B,"ordinary reuse cannot borrow low remainder");memset(objects[i],(int)i,n);}
    }
    for(unsigned i=0;i<128;i++)if(objects[i]){
        unsigned char *q=objects[i];int same=1;for(size_t j=0;j<513+i*173;j++)if(q[j]!=(unsigned char)i)same=0;
        check(same,"mixed payload bytes survive adjacent allocations");kfree(q);
    }
    if(lo2){check(((unsigned char *)lo2)[8191]==0x73,"ordinary churn preserves low payload");kfree(lo2);}
    kfree(lo);kheap_get_stats(&b);
    void *whole=kmalloc_low(4u*1024u*1024u-16);kheap_get_stats(&c);
    check(whole&&c.grows==b.grows&&mm_v2p(whole)<PMM_LOW_LIMIT,"low blocks coalesce to a complete arena");kfree(whole);
    /* A real impossible contiguous allocation drains ordinary magazines and
     * fails; free lists must remain usable, with both domains accounted. */
    check(!kmalloc(128u*1024u*1024u),"oversized contiguous request fails without consuming heap");
    kheap_get_stats(&c);
    check(c.live_bytes==0&&c.live_blocks==0&&c.magazine_bytes==0,"all live/cache owners return to baseline");
    check(c.free_bytes+16*c.grows==c.arena_bytes,"both domain arena headers and free payload balance");
    check(c.low_arena_bytes==4u*1024u*1024u&&c.far_arena_bytes==c.arena_bytes-c.low_arena_bytes,"separate arena accounting matches actual pages");
    check(!kmalloc(0)&&!kmalloc(SIZE_MAX)&&!kmalloc_low(SIZE_MAX),"zero and overflow fail in both domains");
    check(pmm_audit()==0,"PMM bitmap and references remain consistent");
    printf("HIGHHEAP_HOST checks=%d failures=%d arena=%llu low=%llu far=%llu max_phys=%llx\n",checks,fails,c.arena_bytes,c.low_arena_bytes,c.far_arena_bytes,c.max_phys);
    munmap((void *)(uintptr_t)mm_host_base,HIGH_B+FAR_BYTES);return fails?1:0;
}
