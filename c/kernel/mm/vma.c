#include <stdint.h>
#include <stddef.h>
#include "vma.h"
#include "mm.h"
#include "spinlock.h"
#include "kprintf.h"

/* See vma.h. One lock for the whole table: the critical sections are a scan of
 * at most VMA_MAXAREA entries and never call out (no pmm, no kheap), so there
 * is nothing under it to deadlock against. Lock order is
 * BKL -> vma_lock -> pmm_lock; nothing takes them the other way round.
 * irqsave because the page-fault path reads this table. */
static spinlock_t vma_lock = SPINLOCK_INIT;

struct space {
    uint64_t cr3;                 /* 0 = slot free */
    struct vma v[VMA_MAXAREA];
};

static struct space spaces[VMA_MAXSPACE];
static int spaces_live;
static int space_overflow;        /* spaces that could not get a slot */

static struct space *find(uint64_t cr3)
{
    cr3 &= ~(uint64_t)0xFFF;
    if (!cr3) return NULL;
    for (int i = 0; i < VMA_MAXSPACE; i++)
        if (spaces[i].cr3 == cr3) return &spaces[i];
    return NULL;
}

void vma_space_new(uint64_t cr3)
{
    cr3 &= ~(uint64_t)0xFFF;
    if (!cr3) return;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    /* A CR3 is a physical frame address, and a freed space's frame can be
     * handed straight back out. Reclaim any stale slot with the same cr3 first
     * so a new space never inherits a dead one's areas. */
    for (int i = 0; i < VMA_MAXSPACE; i++)
        if (spaces[i].cr3 == cr3) { spaces[i].cr3 = 0; spaces_live--; }
    for (int i = 0; i < VMA_MAXSPACE; i++)
        if (!spaces[i].cr3) {
            spaces[i].cr3 = cr3;
            for (int j = 0; j < VMA_MAXAREA; j++) spaces[i].v[j].used = 0;
            spaces_live++;
            spin_unlock_irqrestore(&vma_lock, fl);
            return;
        }
    space_overflow++;
    spin_unlock_irqrestore(&vma_lock, fl);
    /* Not fatal: a space with no slot simply cannot mmap (vma_reserve returns
     * 0 -> ENOMEM to the app). Loud, because it means VMA_MAXSPACE is too
     * small for the workload, not that the workload is wrong. */
    kprintf("[mm] vma: no space slot for cr3 %p (%d live) -- mmap unavailable for it\n",
            (void *)cr3, spaces_live);
}

void vma_space_free(uint64_t cr3)
{
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) { s->cr3 = 0; spaces_live--; }
    spin_unlock_irqrestore(&vma_lock, fl);
}

void vma_space_clear(uint64_t cr3)
{
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) for (int j = 0; j < VMA_MAXAREA; j++) s->v[j].used = 0;
    spin_unlock_irqrestore(&vma_lock, fl);
}

int vma_space_clone(uint64_t dst_cr3, uint64_t src_cr3)
{
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *d = find(dst_cr3), *s = find(src_cr3);
    int ret = 0;
    if (!d) ret = -1;
    else if (s) for (int j = 0; j < VMA_MAXAREA; j++) d->v[j] = s->v[j];
    spin_unlock_irqrestore(&vma_lock, fl);
    return ret;
}

uint32_t vma_prot_at(uint64_t cr3, uint64_t va)
{
    uint32_t p = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s)
        for (int j = 0; j < VMA_MAXAREA; j++)
            if (s->v[j].used && va >= s->v[j].start && va < s->v[j].end) { p = s->v[j].prot; break; }
    spin_unlock_irqrestore(&vma_lock, fl);
    return p;
}

/* Page-aligned range arithmetic. Every overflow case is written out because
 * this is the arithmetic an mmap ABI is attacked through: a length that wraps
 * turns "map 16 bytes" into "map from here to the end of the address space". */
int vma_range(uint64_t addr, uint64_t len, uint64_t *out_start, uint64_t *out_end)
{
    if (len == 0) return -1;
    if (len > MM_USER_END) return -1;                 /* cannot exceed the whole region */
    uint64_t start = addr & ~(uint64_t)0xFFF;
    if (addr > ~(uint64_t)0 - len) return -1;         /* addr + len wraps */
    uint64_t end = addr + len;
    if (end > ~(uint64_t)0 - 0xFFF) return -1;        /* rounding up wraps */
    end = (end + 0xFFF) & ~(uint64_t)0xFFF;
    if (end <= start) return -1;
    if (start < MM_USER_BASE || end > MM_USER_END) return -1;
    *out_start = start;
    *out_end = end;
    return 0;
}

static int overlaps(struct space *s, uint64_t a, uint64_t b)
{
    for (int j = 0; j < VMA_MAXAREA; j++)
        if (s->v[j].used && a < s->v[j].end && s->v[j].start < b) return 1;
    return 0;
}

uint64_t vma_reserve(uint64_t cr3, uint64_t hint, uint64_t len, uint32_t prot)
{
    uint64_t need = (len + 0xFFF) & ~(uint64_t)0xFFF;
    if (len == 0 || need < len) return 0;                 /* zero, or the round-up wrapped */
    if (need > MM_MMAP_TOP - MM_MMAP_BASE) return 0;

    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    uint64_t got = 0;
    if (!s) goto out;

    int slot = -1;
    for (int j = 0; j < VMA_MAXAREA; j++) if (!s->v[j].used) { slot = j; break; }
    if (slot < 0) goto out;

    /* An explicit hint is honoured only if it is inside the mmap window, page
     * aligned and free -- never by evicting an existing area, which is how
     * MAP_FIXED becomes a way to unmap someone else's memory. */
    if (hint) {
        uint64_t h = hint & ~(uint64_t)0xFFF;
        if (h >= MM_MMAP_BASE && h + need <= MM_MMAP_TOP && !overlaps(s, h, h + need))
            got = h;
    }
    if (!got) {
        /* First fit from the base up. VMA_MAXAREA is 16, so the O(n^2) scan is
         * 256 comparisons worst case -- cheaper than keeping a sorted list
         * correct through splits. */
        for (uint64_t a = MM_MMAP_BASE; a + need <= MM_MMAP_TOP; ) {
            uint64_t clash_end = 0;
            for (int j = 0; j < VMA_MAXAREA; j++)
                if (s->v[j].used && a < s->v[j].end && s->v[j].start < a + need)
                    if (s->v[j].end > clash_end) clash_end = s->v[j].end;
            if (!clash_end) { got = a; break; }
            a = clash_end;
        }
    }
    if (!got) goto out;

    s->v[slot].start = got;
    s->v[slot].end = got + need;
    s->v[slot].prot = prot ? prot : VMA_READ;
    s->v[slot].used = 1;
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    return got;
}

int vma_release(uint64_t cr3, uint64_t addr, uint64_t len)
{
    uint64_t start, end;
    if (vma_range(addr, len, &start, &end) < 0) return -1;

    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    int ret = -1;
    if (!s) goto out;

    /* Decide BEFORE mutating anything. A release that punches a hole out of the
     * middle of an area needs a spare slot to hold the far half; if there is
     * none the whole call must fail having changed NOTHING, not fail halfway
     * with some areas already trimmed. Areas never overlap, so at most one can
     * strictly contain the range, and one spare slot is always enough. */
    int need_split = 0;
    for (int j = 0; j < VMA_MAXAREA; j++)
        if (s->v[j].used && start > s->v[j].start && end < s->v[j].end) need_split = 1;
    if (need_split) {
        int spare = 0;
        for (int k = 0; k < VMA_MAXAREA; k++) if (!s->v[k].used) { spare = 1; break; }
        if (!spare) goto out;                            /* ret is still -1 */
    }

    int touched = 0;
    for (int j = 0; j < VMA_MAXAREA; j++) {
        if (!s->v[j].used) continue;
        uint64_t vs = s->v[j].start, ve = s->v[j].end;
        if (end <= vs || ve <= start) continue;          /* disjoint */
        touched = 1;
        if (start <= vs && end >= ve) {                  /* whole area goes */
            s->v[j].used = 0;
        } else if (start > vs && end < ve) {             /* punched out of the middle: split */
            int slot = -1;
            for (int k = 0; k < VMA_MAXAREA; k++) if (!s->v[k].used) { slot = k; break; }
            if (slot < 0) { ret = -1; goto out; }        /* pre-checked above; belt and braces */
            s->v[slot] = s->v[j];
            s->v[j].end = start;
            s->v[slot].start = end;
            s->v[slot].used = 1;
        } else if (start <= vs) {                        /* trimmed at the front */
            s->v[j].start = end;
        } else {                                         /* trimmed at the back */
            s->v[j].end = start;
        }
    }
    ret = touched ? 0 : -1;
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    return ret;
}

uint64_t vma_reserved_bytes(uint64_t cr3)
{
    uint64_t n = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) for (int j = 0; j < VMA_MAXAREA; j++)
        if (s->v[j].used) n += s->v[j].end - s->v[j].start;
    spin_unlock_irqrestore(&vma_lock, fl);
    return n;
}

int vma_count(uint64_t cr3)
{
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) for (int j = 0; j < VMA_MAXAREA; j++) if (s->v[j].used) n++;
    spin_unlock_irqrestore(&vma_lock, fl);
    return n;
}

int vma_spaces_live(void) { return spaces_live; }
