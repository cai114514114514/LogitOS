#include <stdint.h>
#include <stddef.h>
#include "rmap.h"
#include "pmm.h"
#include "mm.h"
#include "mmhost.h"
#include "spinlock.h"
#include "kprintf.h"

/* See rmap.h for the argument. This file is the mechanics. */

void *memset(void *, int, size_t);

static spinlock_t rmap_lock = SPINLOCK_INIT;

/* Three parallel arrays instead of an array of structs: a node is then exactly
 * 12 bytes with no padding, which on a 512 MiB machine is 750 KiB saved over
 * the 16-byte struct the compiler would produce. Chains are short (1 for a
 * private page, 2-4 for a forked one), so the extra cache lines a parallel
 * layout costs on a walk are not the thing to optimise for here; the fixed
 * footprint is. */
static uint32_t *rm_head;        /* frame -> first node, or RMAP_NIL */
static uint32_t *rm_next;        /* node  -> next node, or RMAP_NIL */
/* Correction: the original 12-byte node accounting above was for one 1 GiB
 * user window. VPNs are now absolute 64-bit values (16-byte total node arrays),
 * so mappings beyond 16 TiB cannot alias low VPNs during reclaim. CR3 remains
 * a 32-bit frame number because page tables are allocated from low memory. */
static uint64_t *rm_vpn;         /* node -> va >> 12, both user windows */
static uint32_t *rm_cr3;         /* node  -> cr3 >> 12 */
static uint8_t  *rm_incomp;      /* 1 bit per frame: chain is not the whole truth */

static uint64_t rm_frames;
static uint64_t rm_total, rm_used, rm_peak;
static uint32_t rm_free_list = RMAP_NIL;
static uint64_t rm_overflow, rm_incomplete_frames, rm_bug;
static int      rm_ready;

static inline void ic_set(uint64_t f) { __atomic_fetch_or(&rm_incomp[f >> 3],(uint8_t)(1u << (f & 7)),__ATOMIC_RELEASE); }
static inline int  ic_test(uint64_t f) { return __atomic_load_n(&rm_incomp[f >> 3],__ATOMIC_ACQUIRE) & (1u << (f & 7)); }

static void rmap_bug(const char *what, uint64_t phys, uint64_t cr3, uint64_t va)
{
    rm_bug++;
    kprintf("[rmap] BUG: %s (phys %p, cr3 %p, va %p)\n",
            what, (void *)phys, (void *)cr3, (void *)va);
}

uint64_t rmap_nodes_total(void)      { return rm_total; }
uint64_t rmap_nodes_used(void)       { return rm_used; }
uint64_t rmap_nodes_peak(void)       { return rm_peak; }
uint64_t rmap_overflows(void)        { return rm_overflow; }
uint64_t rmap_frames_incomplete(void){ return rm_incomplete_frames; }
uint64_t rmap_bugs(void)             { return rm_bug; }
int      rmap_ready(void)            { return rm_ready; }

int rmap_init(uint64_t total_frames)
{
    if (rm_ready || total_frames == 0) return rm_ready ? 0 : -1;

    /* 1.5 nodes per frame: PTEs outnumber frames whenever anything is shared,
     * and a fork of a large process is the case that makes it so. See rmap.h. */
    uint64_t nodes = total_frames + total_frames / 2;
    uint64_t head_b = total_frames * 4;
    uint64_t node_b = nodes * 4;
    uint64_t ic_b   = ((total_frames + 7) / 8 + 7) & ~(uint64_t)7;
    uint64_t bytes  = head_b + node_b * 4 + ic_b + 7; /* align the 64-bit VPN array */
    uint64_t frames = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;

    uint64_t base = pmm_alloc_contig((size_t)frames);
    if (!base) {
        /* Correct degradation: no reverse map means no reclaim, and the kernel
         * behaves exactly as it did before this line existed. Loud, because a
         * silently disabled reclaim path is indistinguishable from a broken
         * one the day memory runs out. */
        kprintf("[rmap] init FAILED: no %d contiguous frames for the reverse map "
                "-- reclaim is DISABLED for this boot\n", (int)frames);
        return -1;
    }

    uint8_t *p = (uint8_t *)mm_p2v(base);
    rm_head   = (uint32_t *)p;                     p += head_b;
    rm_next   = (uint32_t *)p;                     p += node_b;
    rm_cr3    = (uint32_t *)p;                     p += node_b;
    p = (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7);
    rm_vpn    = (uint64_t *)p;                     p += node_b * 2;
    rm_incomp = (uint8_t *)p;

    memset(rm_head, 0xFF, (size_t)head_b);         /* RMAP_NIL everywhere */
    memset(rm_incomp, 0, (size_t)ic_b);

    /* Thread every node onto the free list, in order, so the first allocations
     * are sequential in memory. */
    for (uint64_t i = 0; i < nodes; i++)
        rm_next[i] = (i + 1 < nodes) ? (uint32_t)(i + 1) : RMAP_NIL;
    rm_free_list = 0;

    rm_frames = total_frames;
    rm_total  = nodes;
    rm_used   = rm_peak = 0;
    rm_ready  = 1;

    kprintf("[rmap] %d frames, %d nodes (%d KiB total: %d KiB heads + %d KiB nodes)\n",
            (int)total_frames, (int)nodes, (int)(bytes / 1024),
            (int)(head_b / 1024), (int)(node_b * 4 / 1024));
    return 0;
}

/* --- the two hooks ------------------------------------------------------- */

/* Both are called from vmm.c/fault.c on every user PTE install and teardown, so
 * they carry their own applicability tests rather than making five call sites
 * repeat them. `phys` out of range, `va` outside the private user region, or no
 * reverse map at all: nothing to record, and nothing recorded means the frame
 * is not a reclaim candidate, which is the safe direction. */
static int usable(uint64_t phys, uint64_t va, uint64_t *frame, uint64_t *vpn)
{
    if (!rm_ready) return 0;
    if (!mm_user_addr(va)) return 0;
    uint64_t f = phys / FRAME_SIZE;
    if (f == 0 || f >= rm_frames) return 0;
    *frame = f;
    *vpn = va >> 12;
    return 1;
}

void rmap_add(uint64_t phys, uint64_t cr3, uint64_t va)
{
    uint64_t f, vpn;
    if (!usable(phys, va, &f, &vpn)) return;
    uint32_t c = (uint32_t)((cr3 & MM_PTE_ADDR) >> 12);

    uint64_t fl = spin_lock_irqsave(&rmap_lock);

    /* An install over an entry we already hold. vmm.c removes the old mapping
     * before installing a new one, so reaching here means two adds without a
     * remove between them -- a leak of one node per repeat, and a chain that
     * would unmap the same PTE twice during eviction. Refuse and report. */
    for (uint32_t n = rm_head[f]; n != RMAP_NIL; n = rm_next[n])
        if (rm_vpn[n] == vpn && rm_cr3[n] == c) {
            spin_unlock_irqrestore(&rmap_lock, fl);
            rmap_bug("duplicate mapping recorded", phys, cr3, va);
            return;
        }

    if (rm_free_list == RMAP_NIL) {
        /* No node. The chain for this frame is now incomplete FOREVER: we can
         * never again claim to know every PTE that maps it, so it must never be
         * evicted. Marking is cheap; the alternative is silent corruption. */
        if (!ic_test(f)) { ic_set(f); rm_incomplete_frames++; }
        rm_overflow++;
        spin_unlock_irqrestore(&rmap_lock, fl);
        return;
    }

    uint32_t n = rm_free_list;
    rm_free_list = rm_next[n];
    rm_vpn[n] = vpn;
    rm_cr3[n] = c;
    rm_next[n] = rm_head[f];
    __atomic_store_n(&rm_head[f],n,__ATOMIC_RELEASE);
    if (++rm_used > rm_peak) rm_peak = rm_used;

    spin_unlock_irqrestore(&rmap_lock, fl);
}

void rmap_remove(uint64_t phys, uint64_t cr3, uint64_t va)
{
    uint64_t f, vpn;
    if (!usable(phys, va, &f, &vpn)) return;
    uint32_t c = (uint32_t)((cr3 & MM_PTE_ADDR) >> 12);

    uint64_t fl = spin_lock_irqsave(&rmap_lock);
    uint32_t prev = RMAP_NIL;
    for (uint32_t n = rm_head[f]; n != RMAP_NIL; prev = n, n = rm_next[n]) {
        if (rm_vpn[n] != vpn || rm_cr3[n] != c) continue;
        if (prev == RMAP_NIL) __atomic_store_n(&rm_head[f],rm_next[n],__ATOMIC_RELEASE);
        else                  rm_next[prev] = rm_next[n];
        rm_next[n] = rm_free_list;
        rm_free_list = n;
        rm_used--;
        spin_unlock_irqrestore(&rmap_lock, fl);
        return;
    }
    spin_unlock_irqrestore(&rmap_lock, fl);

    /* Not found. Only legitimate when the frame was already flagged incomplete
     * (the entry was one the pool could not hold). Anything else means a PTE
     * was torn down that we never saw installed, which is the half of the
     * bookkeeping that would leave a stale chain behind. */
    if (!ic_test(f))
        rmap_bug("removing a mapping that was never recorded", phys, cr3, va);
}

unsigned rmap_count(uint64_t phys)
{
    if (!rm_ready) return 0;
    uint64_t f = phys / FRAME_SIZE;
    if (f == 0 || f >= rm_frames) return 0;
    unsigned n = 0;
    uint64_t fl = spin_lock_irqsave(&rmap_lock);
    for (uint32_t i = rm_head[f]; i != RMAP_NIL; i = rm_next[i]) n++;
    spin_unlock_irqrestore(&rmap_lock, fl);
    return n;
}

int rmap_mapped(uint64_t phys)
{
    if (!rm_ready) return 0;
    uint64_t f = phys / FRAME_SIZE;
    if (f == 0 || f >= rm_frames) return 0;
    return __atomic_load_n(&rm_head[f],__ATOMIC_ACQUIRE) != RMAP_NIL;      /* deliberately unlocked; see rmap.h */
}

int rmap_incomplete(uint64_t phys)
{
    if (!rm_ready) return 1;            /* no map = we know nothing = never evict */
    uint64_t f = phys / FRAME_SIZE;
    if (f == 0 || f >= rm_frames) return 1;
    return ic_test(f) ? 1 : 0;
}

/* --- iteration ----------------------------------------------------------- */

int rmap_snapshot(uint64_t phys,uint64_t *cr3,uint64_t *va,int cap)
{
    uint64_t frame=phys/FRAME_SIZE;
    if (!rm_ready || !frame || frame>=rm_frames) return 0;
    uint64_t f=spin_lock_irqsave(&rmap_lock);
    int count=0;
    for (uint32_t n=rm_head[frame];n!=RMAP_NIL;n=rm_next[n]) {
        if (count==cap) { count=-1; break; }
        cr3[count]=(uint64_t)rm_cr3[n]<<12;
        va[count]=rm_vpn[n]<<12; count++;
    }
    spin_unlock_irqrestore(&rmap_lock,f);
    return count;
}
void rmap_begin(struct rmap_iter *it,uint64_t phys)
{ it->phys=phys; it->last_cr3=it->last_va=0; it->ended=0; }
int rmap_next(struct rmap_iter *it,uint64_t *cr3,uint64_t *va)
{
    uint64_t frame=it->phys/FRAME_SIZE;
    if (it->ended || !rm_ready || !frame || frame>=rm_frames) return 0;
    uint64_t f=spin_lock_irqsave(&rmap_lock);
    uint64_t bc=~0ull,bv=~0ull; int found=0;
    for (uint32_t n=rm_head[frame];n!=RMAP_NIL;n=rm_next[n]) {
        uint64_t c=(uint64_t)rm_cr3[n]<<12,v=rm_vpn[n]<<12;
        if ((c>it->last_cr3 || (c==it->last_cr3 && v>it->last_va)) &&
            (!found || c<bc || (c==bc && v<bv))) { bc=c; bv=v; found=1; }
    }
    spin_unlock_irqrestore(&rmap_lock,f);
    if (!found) { it->ended=1; return 0; }
    it->last_cr3=bc; it->last_va=bv;
    if (cr3) *cr3=bc; if (va) *va=bv; return 1;
}

/* --- the check ----------------------------------------------------------- */

int rmap_audit(void)
{
    if (!rm_ready) return 0;
    uint64_t fl = spin_lock_irqsave(&rmap_lock);

    uint64_t counted = 0, over = 0, orphan = 0, chains = 0;
    for (uint64_t f = 0; f < rm_frames; f++) {
        unsigned n = 0;
        for (uint32_t i = rm_head[f]; i != RMAP_NIL; i = rm_next[i]) n++;
        if (!n) continue;
        chains++;
        counted += n;
        /* Two independent structures, the same number. The rmap may know FEWER
         * PTEs than pmm counts references (the kernel holds references that are
         * not user PTEs -- a page-table frame, a kheap arena). It may never know
         * MORE: that would mean a PTE exists whose reference pmm never took, and
         * freeing the frame would then free it out from under a live mapping. */
        unsigned rc = pmm_refcount(f * FRAME_SIZE);
        if (rc == 0) {
            if (orphan < 8)
                kprintf("[rmap] AUDIT: frame %d has %d mappings but is not allocated\n",
                        (int)f, n);
            orphan++;
        } else if (n > rc) {
            if (over < 8)
                kprintf("[rmap] AUDIT: frame %d mapped by %d PTEs, refcount is only %d\n",
                        (int)f, n, rc);
            over++;
        }
    }

    /* The free list plus the live nodes must be every node there is. A node
     * that is on neither has leaked; one on both is about to be handed out
     * twice. */
    uint64_t freen = 0;
    for (uint32_t i = rm_free_list; i != RMAP_NIL && freen <= rm_total; i = rm_next[i]) freen++;

    int errs = 0;
    if (orphan)  { kprintf("[rmap] AUDIT: %d frames mapped but not allocated\n", (int)orphan); errs++; }
    if (over)    { kprintf("[rmap] AUDIT: %d frames with more mappings than references\n", (int)over); errs++; }
    if (counted != rm_used) {
        kprintf("[rmap] AUDIT: %d nodes on chains, counter says %d\n", (int)counted, (int)rm_used);
        errs++;
    }
    if (freen + rm_used != rm_total) {
        kprintf("[rmap] AUDIT: %d free + %d used != %d nodes\n",
                (int)freen, (int)rm_used, (int)rm_total);
        errs++;
    }
    spin_unlock_irqrestore(&rmap_lock, fl);
    (void)chains;
    return errs;
}

void rmap_report(const char *tag)
{
    if (!rm_ready) {
        kprintf("[rmap] %s: NOT READY -- no reverse map, reclaim disabled\n", tag ? tag : "-");
        return;
    }
    kprintf("[rmap] %s: %d/%d nodes used (peak %d), %d overflows, "
            "%d frames incomplete, %d bugs\n",
            tag ? tag : "-", (int)rm_used, (int)rm_total, (int)rm_peak,
            (int)rm_overflow, (int)rm_incomplete_frames, (int)rm_bug);
}
