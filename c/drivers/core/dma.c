/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dma.h"
#include "pmm.h"
#include "mmhost.h"
#include "mm.h"
#include "kheap.h"
#include "spinlock.h"
#include "kprintf.h"
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
static spinlock_t lock = SPINLOCK_INIT;
static struct dma_stats stats;
static uint64_t cookie;
static int power2(size_t n) { return n && !(n & (n - 1)); }
static int mask_ok(uint64_t n) { return n && !(n & (n + 1)); }
static int safe(enum dma_state s) { return s != DMA_DEVICE_OWNED && s != DMA_QUARANTINED; }
void dma_wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
void dma_rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
void dma_device_init(struct dma_device *d, const char *name, uint64_t mask)
{
    memset(d, 0, sizeof *d); d->name = name; d->mask = mask;
    d->alignment = 1; d->max_segment = UINT32_MAX; d->max_segments = 256;
    d->blocked = !mask_ok(mask); d->generation = 1;
}
static int constraints(struct dma_device *d)
{
    return d && !d->blocked && mask_ok(d->mask) && power2(d->alignment) &&
        (!d->boundary || power2(d->boundary)) && d->max_segment &&
        d->max_segments && d->max_segments <= DMA_MAX_MAPPING;
}
static struct dma_buffer *alloc_buffer(struct dma_device *d, size_t n, size_t a, size_t boundary, int streaming)
{
    if (!constraints(d) || !n || (!streaming && n > d->max_segment) || n > SIZE_MAX - 4095 ||
        (a && !power2(a)) || (boundary && !power2(boundary))) return NULL;
    if (a < 4096) a = 4096;
    if (a < d->alignment) a = d->alignment;
    if (!streaming && d->boundary && (!boundary || boundary > d->boundary)) boundary = d->boundary;
    struct dma_buffer *b = kmalloc(sizeof *b);
    if (!b) return NULL;
    memset(b, 0, sizeof *b);
    b->pages = (n + 4095) / 4096;
    b->phys = pmm_alloc_contig_masked(b->pages, d->mask, a, boundary);
    if (!b->phys) { kfree(b); uint64_t f=spin_lock_irqsave(&lock); stats.allocation_failures++; spin_unlock_irqrestore(&lock,f); return NULL; }
    b->cpu = mm_physmap_ptr(b->phys); b->dma.value = b->phys;
#ifdef DMA_NEG_CPU_ADDRESS
    b->dma.value = (uint64_t)(uintptr_t)b->cpu;
#endif
#ifdef DMA_NEG_TRUNCATE
    b->dma.value = (uint32_t)b->phys;
#endif
    b->size = n; b->dev = d; b->state = DMA_READY;
    memset(b->cpu, 0, b->pages * 4096);
    uint64_t f = spin_lock_irqsave(&lock);
    if (d->blocked) {
        spin_unlock_irqrestore(&lock,f);
        for(size_t i=0;i<b->pages;i++) pmm_free(b->phys+i*4096);
        kfree(b); return NULL;
    }
    int first = !d->buffers;
    b->next=d->buffers; d->buffers=b;
    stats.coherent_buffers++; stats.coherent_bytes+=b->pages*4096;
    stats.allocations++; if(b->phys>=UINT64_C(0x100000000)) stats.high_allocations++;
    if (b->phys+n-1>stats.max_dma) stats.max_dma=b->phys+n-1;
    spin_unlock_irqrestore(&lock,f);
    if(first) kprintf("[dma] alloc %s cpu=%p dma=%p bytes=%llu mask=%p\n",d->name,b->cpu,b->dma.value,(uint64_t)n,d->mask);
    return b;
}
struct dma_buffer *dma_alloc_coherent(struct dma_device *d, size_t n, size_t a, size_t boundary)
{ return alloc_buffer(d,n,a,boundary,0); }

int dma_free_coherent(struct dma_buffer *b)
{
    if (!b) return 0;
    uint64_t f=spin_lock_irqsave(&lock);
#ifndef DMA_NEG_EARLY_FREE
    if (!safe(b->state)) { spin_unlock_irqrestore(&lock,f); return -1; }
#endif
    struct dma_buffer **p=&b->dev->buffers;
    while(*p && *p!=b) p=&(*p)->next;
    if(!*p) {spin_unlock_irqrestore(&lock,f);return -1;}
    *p=b->next;stats.coherent_buffers--;stats.coherent_bytes-=b->pages*4096;
    spin_unlock_irqrestore(&lock,f);
    for(size_t i=0;i<b->pages;i++) pmm_free(b->phys+i*4096);
    kfree(b);return 0;
}

/* Alias arithmetic is only a fast path AFTER RAM provenance validation. The
 * generic path walks every VA page; a contiguous virtual span is not evidence
 * that any two physical frames are adjacent. User VAs are never accepted. */
static uint64_t resolve(void *ptr)
{
#ifdef DMA_HOSTTEST
    extern uint64_t dma_host_resolve(void *);
    uint64_t p=dma_host_resolve(ptr);
    return p!=MM_PHYS_INVALID && pmm_is_ram(p,1) ? p : MM_PHYS_INVALID;
#else
    uint64_t va=(uint64_t)(uintptr_t)ptr;
    if(mm_user_addr(va) || (va>=UINT64_C(0x800000000000) && va<UINT64_C(0xffff800000000000))) return MM_PHYS_INVALID;
    uint64_t p=mm_v2p(ptr);
    if(p!=MM_PHYS_INVALID) {
        if(p>=PMM_LOW_LIMIT && !pmm_physmap_ready()) return MM_PHYS_INVALID;
        return pmm_is_ram(p,1) ? p : MM_PHYS_INVALID;
    }
    uint64_t tab=mm_read_cr3()&MM_PTE_ADDR;
    int user=1;
    for(int shift=39;shift>=12;shift-=9) {
        if(!pmm_is_ram(tab,4096)) return MM_PHYS_INVALID;
        uint64_t e=((uint64_t*)mm_p2v(tab))[(va>>shift)&511];
        if(!(e&1))return MM_PHYS_INVALID;
        user &= !!(e&4);
        if(shift==12 || ((shift==30||shift==21)&&(e&128))) {
            uint64_t offmask=(UINT64_C(1)<<shift)-1;
            p=(e&MM_PTE_ADDR&~offmask)+(va&offmask);
            return !user && pmm_is_ram(p,1) ? p : MM_PHYS_INVALID;
        }
        tab=e&MM_PTE_ADDR;
    }
    return MM_PHYS_INVALID;
#endif
}
static int add_segment(struct dma_mapping *m,uint64_t p,size_t n)
{
    struct dma_device *d=m->dev;
    while(n) {
        if(p>d->mask || n-1>d->mask-p || p&(d->alignment-1))return -1;
        size_t take=n;
        if(take>d->max_segment)take=d->max_segment;
        if(d->boundary) {size_t left=d->boundary-(p&(d->boundary-1));if(take>left)take=left;}
        if(m->nsegments) {
            struct dma_segment *s=&m->segments[m->nsegments-1];
            if(s->addr.value+s->len==p && s->len<=d->max_segment-take &&
               (!d->boundary || s->addr.value/d->boundary==(p+take-1)/d->boundary)) {
                s->len+=take;p+=take;n-=take;continue;
            }
        }
        if(m->nsegments==d->max_segments)return -1;
        m->segments[m->nsegments++]=(struct dma_segment){{p},take};p+=take;n-=take;
    }
    return 0;
}
static void unpin(struct dma_mapping *m)
{
    for(size_t i=0;i<m->npages;i++) {pmm_unpin(m->phys_pages[i]);pmm_free(m->phys_pages[i]);}
}
struct dma_mapping *dma_map_kernel(struct dma_device *d,void *cpu,size_t n,enum dma_direction dir,unsigned flags)
{
    uintptr_t va=(uintptr_t)cpu;
    if(!constraints(d)||!cpu||!n||n>DMA_MAX_MAPPING||n-1>UINTPTR_MAX-va||
       dir>DMA_BIDIRECTIONAL||dir<DMA_TO_DEVICE||flags&~DMA_MAP_CONTIGUOUS)return NULL;
    struct dma_mapping *m=kmalloc(sizeof *m);if(!m)return NULL;memset(m,0,sizeof *m);
    m->dev=d;m->cpu=cpu;m->size=n;m->direction=dir;m->state=DMA_READY;
    size_t pages=((va&4095)+n+4095)/4096;
    m->phys_pages=kmalloc(pages*sizeof(uint64_t));
    m->segments=kmalloc(d->max_segments*sizeof(struct dma_segment));
    if(!m->phys_pages||!m->segments)goto fail;
    int direct=1;size_t remaining=n;
    for(size_t i=0;i<pages;i++) {
        size_t off=i?0:va&4095;
        uint64_t p=resolve((void*)((va&~(uintptr_t)4095)+i*4096));
        if(p==MM_PHYS_INVALID || (p&4095) || !pmm_is_ram(p,4096)||pmm_ref(p))goto fail;
        pmm_pin(p);m->phys_pages[m->npages++]=p;
        size_t take=4096-off;if(take>remaining)take=remaining;
        if(direct&&add_segment(m,p+off,take))direct=0;
        remaining-=take;
    }
    if((flags&DMA_MAP_CONTIGUOUS)&&m->nsegments!=1)direct=0;
    if(!direct) {
        m->nsegments=0;m->bounced=1;
        /* A bounce is a private allocation, not one hardware segment. Split it
         * below so a device boundary/segment limit does not incorrectly limit
         * the TOTAL mapping length (e.g. two 4 KiB segments in an 8 KiB map). */
        m->bounce=alloc_buffer(d,n,d->alignment,(flags&DMA_MAP_CONTIGUOUS)?d->boundary:0,1);
        if(!m->bounce||add_segment(m,m->bounce->phys,n)||
           ((flags&DMA_MAP_CONTIGUOUS)&&m->nsegments!=1))goto fail;
    }
    uint64_t f=spin_lock_irqsave(&lock);
    if(d->blocked){spin_unlock_irqrestore(&lock,f);goto fail;}
    int high = !m->bounced && m->segments[0].addr.value >= UINT64_C(0x100000000);
    int first = !d->logged_mapping || (high && !d->logged_high_mapping);
    d->logged_mapping = 1; if(high)d->logged_high_mapping = 1;
    m->next=d->mappings;d->mappings=m;stats.active_mappings++;stats.pinned_pages+=m->npages;
    if(m->bounced)stats.bounce_bytes+=n;else stats.direct_bytes+=n;
    spin_unlock_irqrestore(&lock,f);
    if(first) kprintf("[dma] map %s cpu=%p dma=%p bytes=%llu sg=%llu bounce=%d\n",d->name,cpu,m->segments[0].addr.value,(uint64_t)n,(uint64_t)m->nsegments,m->bounced);
    return m;
fail:
    if(m->bounce)dma_free_coherent(m->bounce);
    unpin(m);kfree(m->phys_pages);kfree(m->segments);kfree(m);return NULL;
}
dma_addr_t dma_mapping_addr(const struct dma_mapping *m,size_t off)
{
    if(m&&off<m->size) for(size_t i=0;i<m->nsegments;i++) {
        if(off<m->segments[i].len)return dma_addr_add(m->segments[i].addr,off);
        off-=m->segments[i].len;
    }
    return (dma_addr_t){UINT64_MAX};
}
static void sync_device(struct dma_mapping *m)
{
    if(m->bounce && m->direction!=DMA_FROM_DEVICE)memcpy(m->bounce->cpu,m->cpu,m->size);
    dma_wmb();
}
static void sync_cpu(struct dma_mapping *m,size_t valid)
{
    dma_rmb();
#ifndef DMA_NEG_DIRECTION
    if(m->bounce && m->direction!=DMA_TO_DEVICE)
#else
    if(m->bounce)
#endif
        memcpy(m->cpu,m->bounce->cpu,valid);
    m->valid=valid;
}
int dma_sync_for_device(struct dma_mapping *m)
{
    if(!m)return -1;uint64_t f=spin_lock_irqsave(&lock);
    if(m->dev->blocked||!safe(m->state)){spin_unlock_irqrestore(&lock,f);return -1;}
    sync_device(m);spin_unlock_irqrestore(&lock,f);return 0;
}
int dma_sync_for_cpu(struct dma_mapping *m,size_t valid)
{
    if(!m||valid>m->size)return -1;uint64_t f=spin_lock_irqsave(&lock);
    if(m->state!=DMA_COMPLETED){spin_unlock_irqrestore(&lock,f);return -1;}
    sync_cpu(m,valid);spin_unlock_irqrestore(&lock,f);return 0;
}
uint64_t dma_buffer_submit(struct dma_buffer *b)
{
    if(!b)return 0;uint64_t f=spin_lock_irqsave(&lock),t=0;
    if(!b->dev->blocked&&safe(b->state)){b->state=DMA_DEVICE_OWNED;b->token=t=++cookie;dma_wmb();}
    spin_unlock_irqrestore(&lock,f);return t;
}
int dma_buffer_complete(struct dma_buffer *b,uint64_t t)
{
    if(!b)return -1;uint64_t f=spin_lock_irqsave(&lock);int rc=-1;
    if(t&&b->token==t&&b->state==DMA_DEVICE_OWNED){dma_rmb();b->state=DMA_COMPLETED;rc=0;}
    else stats.rejected_completions++;
    spin_unlock_irqrestore(&lock,f);return rc;
}
uint64_t dma_mapping_submit(struct dma_mapping *m)
{
    if(!m)return 0;uint64_t f=spin_lock_irqsave(&lock),t=0;
    if(!m->dev->blocked&&safe(m->state)) {
        sync_device(m);m->valid=0;m->state=DMA_DEVICE_OWNED;m->token=t=++cookie;
        if(m->bounce){m->bounce->state=DMA_DEVICE_OWNED;m->bounce->token=t;}
    }
    spin_unlock_irqrestore(&lock,f);return t;
}
int dma_mapping_complete(struct dma_mapping *m,uint64_t t,size_t valid)
{
    if(!m)return -1;uint64_t f=spin_lock_irqsave(&lock);int rc=-1;
    if(t&&m->token==t&&m->state==DMA_DEVICE_OWNED&&valid<=m->size) {
        sync_cpu(m,valid);m->state=DMA_COMPLETED;
        if(!m->bounced) {
            stats.completed_direct_bytes+=valid;
            size_t left=valid;
            for(size_t i=0;i<m->nsegments && left;i++) {
                size_t n=m->segments[i].len;if(n>left)n=left;
                uint64_t start=m->segments[i].addr.value,end=start+n;
                if(end>UINT64_C(0x100000000))
                    stats.completed_high_bytes+=end-(start>UINT64_C(0x100000000)?start:UINT64_C(0x100000000));
                left-=n;
            }
        }
        if(m->bounce)m->bounce->state=DMA_COMPLETED;
        rc=0;
    } else stats.rejected_completions++;
    spin_unlock_irqrestore(&lock,f);return rc;
}
int dma_unmap(struct dma_mapping *m)
{
    if(!m)return 0;uint64_t f=spin_lock_irqsave(&lock);
#ifndef DMA_NEG_EARLY_FREE
    if(!safe(m->state)){spin_unlock_irqrestore(&lock,f);return -1;}
#endif
    struct dma_mapping **p=&m->dev->mappings;
    while(*p&&*p!=m)p=&(*p)->next;
    if(!*p){spin_unlock_irqrestore(&lock,f);return -1;}
    *p=m->next;stats.active_mappings--;stats.pinned_pages-=m->npages;
    if(m->bounced)stats.bounce_bytes-=m->size;else stats.direct_bytes-=m->size;
    spin_unlock_irqrestore(&lock,f);
    if(m->bounce)dma_free_coherent(m->bounce);
    unpin(m);kfree(m->phys_pages);kfree(m->segments);kfree(m);return 0;
}
void dma_device_quarantine(struct dma_device *d)
{
    if(!d)return;uint64_t f=spin_lock_irqsave(&lock);d->blocked=1;d->generation++;
    for(struct dma_buffer *b=d->buffers;b;b=b->next)if(b->state==DMA_DEVICE_OWNED){b->state=DMA_QUARANTINED;stats.quarantined_buffers++;stats.quarantined_bytes+=b->pages*4096;}
    for(struct dma_mapping *m=d->mappings;m;m=m->next)if(m->state==DMA_DEVICE_OWNED){m->state=DMA_QUARANTINED;stats.quarantined_mappings++;if(!m->bounced)stats.quarantined_bytes+=m->size;}
    spin_unlock_irqrestore(&lock,f);
}
void dma_device_quiesced(struct dma_device *d)
{
    if(!d)return;uint64_t f=spin_lock_irqsave(&lock);d->blocked=1;d->generation++;
    for(struct dma_buffer *b=d->buffers;b;b=b->next){
        if(b->state==DMA_QUARANTINED){stats.quarantined_buffers--;stats.quarantined_bytes-=b->pages*4096;}
        b->state=DMA_QUIESCED;b->token=0;
    }
    for(struct dma_mapping *m=d->mappings;m;m=m->next){
        if(m->state==DMA_QUARANTINED){stats.quarantined_mappings--;if(!m->bounced)stats.quarantined_bytes-=m->size;}
        m->state=DMA_QUIESCED;m->token=0;
    }
    dma_rmb();spin_unlock_irqrestore(&lock,f);
}
int dma_device_resume(struct dma_device *d)
{
    if(!d)return -1;uint64_t f=spin_lock_irqsave(&lock);
    for(struct dma_buffer *b=d->buffers;b;b=b->next)if(!safe(b->state)){spin_unlock_irqrestore(&lock,f);return -1;}
    for(struct dma_mapping *m=d->mappings;m;m=m->next)if(!safe(m->state)){spin_unlock_irqrestore(&lock,f);return -1;}
    d->blocked=!mask_ok(d->mask);int rc=d->blocked?-1:0;spin_unlock_irqrestore(&lock,f);return rc;
}
void dma_get_stats(struct dma_stats *s)
{uint64_t f=spin_lock_irqsave(&lock);*s=stats;spin_unlock_irqrestore(&lock,f);}
void dma_report(const char *tag)
{
    struct dma_stats s;dma_get_stats(&s);
    kprintf("[dma] %s coherent=%llu bytes=%llu mappings=%llu pins=%llu direct=%llu bounce=%llu quarantine=%llu/%llu/%llu high=%llu max=%p stale=%llu completed_direct=%llu completed_high=%llu\n",tag,s.coherent_buffers,s.coherent_bytes,s.active_mappings,s.pinned_pages,s.direct_bytes,s.bounce_bytes,s.quarantined_buffers,s.quarantined_mappings,s.quarantined_bytes,s.high_allocations,s.max_dma,s.rejected_completions,s.completed_direct_bytes,s.completed_high_bytes);
}
