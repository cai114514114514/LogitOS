#include <stdint.h>
#include <stddef.h>
#include "vma.h"
#include "mm.h"
#include "pcache.h"
#include "shm.h"
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

/* A slot, emptied. `file` matters as much as `used` does: a handle left behind
 * in a slot nobody is using is a reference that gets put a second time the next
 * time that slot is handed out, and a pcache file entry driven to zero
 * references while a mapping is still live purges pages that are in somebody's
 * page table. So there is exactly one way to empty a slot, and this is it. */
static inline void slot_clear(struct vma *v)
{ v->used = 0; v->file = -1; v->shm = -1; v->foff = 0; }

/* Handles are collected under vma_lock and released after it, because
 * pcache_file_put() can purge a file's pages, which takes the pcache lock and
 * then the frame allocator's. Lock order is BKL -> vma_lock -> pcache -> pmm,
 * and doing the put outside is what keeps vma_lock a leaf. */
#define VMA_PUTMAX VMA_MAXAREA
static void put_all(int *fh, int n)
{
    for (int i = 0; i < n; i++) if (fh[i] >= 0) pcache_file_put(fh[i]);
}

/* The same, for shm segment handles, and deliberately a SEPARATE array at every
 * call site rather than one tagged list. Every place below that collects a
 * `file` now collects a `shm` on the line beneath it, so the pair is visible in
 * one glance -- which is the only real defence against the bug this whole
 * arrangement exists to prevent, an area whose backing reference was moved for
 * one kind and forgotten for the other. shm_put can free a segment's frames, so
 * it is called after vma_lock is dropped for the reason pcache_file_put already
 * is: the order is BKL -> vma_lock -> {pcache, shm} -> pmm, and doing the put
 * outside is what keeps vma_lock a leaf. */
static void shm_put_all(int *sh, int n)
{
    for (int i = 0; i < n; i++) if (sh[i] >= 0) shm_put(sh[i]);
}

static struct space *find(uint64_t cr3)
{
    cr3 &= MM_PTE_ADDR;
    if (!cr3) return NULL;
    for (int i = 0; i < VMA_MAXSPACE; i++)
        if (spaces[i].cr3 == cr3) return &spaces[i];
    return NULL;
}

void vma_space_new(uint64_t cr3)
{
    cr3 &= MM_PTE_ADDR;
    if (!cr3) return;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    /* A CR3 is a physical frame address, and a freed space's frame can be
     * handed straight back out. Reclaim any stale slot with the same cr3 first
     * so a new space never inherits a dead one's areas. */
    int stale[VMA_PUTMAX], nstale = 0;
    int sstale[VMA_PUTMAX], nsstale = 0;
    for (int i = 0; i < VMA_MAXSPACE; i++)
        if (spaces[i].cr3 == cr3) {
            /* A dead space whose CR3 frame has been handed straight back out.
             * Its areas' file references go with it, or the handle leaks and
             * the file entry never releases its pages. */
            for (int j = 0; j < VMA_MAXAREA; j++) {
                if (spaces[i].v[j].used && spaces[i].v[j].file >= 0 && nstale < VMA_PUTMAX)
                    stale[nstale++] = spaces[i].v[j].file;
                if (spaces[i].v[j].used && spaces[i].v[j].shm >= 0 && nsstale < VMA_PUTMAX)
                    sstale[nsstale++] = spaces[i].v[j].shm;
            }
            spaces[i].cr3 = 0; spaces_live--;
        }
    for (int i = 0; i < VMA_MAXSPACE; i++)
        if (!spaces[i].cr3) {
            spaces[i].cr3 = cr3;
            for (int j = 0; j < VMA_MAXAREA; j++) slot_clear(&spaces[i].v[j]);
            spaces_live++;
            spin_unlock_irqrestore(&vma_lock, fl);
            put_all(stale, nstale);
            shm_put_all(sstale, nsstale);
            return;
        }
    space_overflow++;
    spin_unlock_irqrestore(&vma_lock, fl);
    put_all(stale, nstale);
    shm_put_all(sstale, nsstale);
    /* Not fatal: a space with no slot simply cannot mmap (vma_reserve returns
     * 0 -> ENOMEM to the app). Loud, because it means VMA_MAXSPACE is too
     * small for the workload, not that the workload is wrong. */
    kprintf("[mm] vma: no space slot for cr3 %p (%d live) -- mmap unavailable for it\n",
            (void *)cr3, spaces_live);
}

void vma_space_free(uint64_t cr3)
{
    int put[VMA_PUTMAX], np = 0;
    int sput[VMA_PUTMAX], nsp = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) {
        for (int j = 0; j < VMA_MAXAREA; j++) {
            if (s->v[j].used && s->v[j].file >= 0) put[np++] = s->v[j].file;
            if (s->v[j].used && s->v[j].shm  >= 0) sput[nsp++] = s->v[j].shm;
        }
        for (int j = 0; j < VMA_MAXAREA; j++) slot_clear(&s->v[j]);
        s->cr3 = 0; spaces_live--;
    }
    spin_unlock_irqrestore(&vma_lock, fl);
    put_all(put, np);
    shm_put_all(sput, nsp);
}

void vma_space_clear(uint64_t cr3)
{
    int put[VMA_PUTMAX], np = 0;
    int sput[VMA_PUTMAX], nsp = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) for (int j = 0; j < VMA_MAXAREA; j++) {
        if (s->v[j].used && s->v[j].file >= 0) put[np++] = s->v[j].file;
        /* execve: a shared segment does NOT survive the image being replaced.
         * The new program never asked for it and has no handle to it, so the
         * mapping goes with everything else in the old address space and the
         * segment lives on only for whoever else still holds it. */
        if (s->v[j].used && s->v[j].shm  >= 0) sput[nsp++] = s->v[j].shm;
        slot_clear(&s->v[j]);
    }
    spin_unlock_irqrestore(&vma_lock, fl);
    put_all(put, np);
    shm_put_all(sput, nsp);
}

int vma_space_clone(uint64_t dst_cr3, uint64_t src_cr3)
{
    int take[VMA_PUTMAX], nt = 0;
    int stake[VMA_PUTMAX], nst = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *d = find(dst_cr3), *s = find(src_cr3);
    int ret = 0;
    if (!d) ret = -1;
    else if (s) for (int j = 0; j < VMA_MAXAREA; j++) {
        d->v[j] = s->v[j];
        /* fork: the child's area is a SECOND reference to the same file. Its
         * PTEs are the parent's, shared copy-on-write by vmm_clone_user, so the
         * two spaces are already looking at ONE cached frame -- this is the
         * bookkeeping that stops the file entry, and with it those frames,
         * from being purged when the parent exits first. */
        if (d->v[j].used && d->v[j].file >= 0 && nt < VMA_PUTMAX) take[nt++] = d->v[j].file;
        /* fork: the child's area is a SECOND reference to the same SEGMENT, and
         * unlike the file case above the PTEs it describes are not
         * copy-on-write -- vmm_clone_user's VMM_PTE_SHM branch hands the child
         * the parent's entries verbatim, still writable. This reference is what
         * keeps the segment (and every frame in it) alive if the parent exits
         * first, which is the ordinary shape of a server that forks a worker
         * and then returns. */
        if (d->v[j].used && d->v[j].shm >= 0 && nst < VMA_PUTMAX) stake[nst++] = d->v[j].shm;
    }
    spin_unlock_irqrestore(&vma_lock, fl);
    for (int i = 0; i < nt; i++) pcache_file_ref(take[i]);
    for (int i = 0; i < nst; i++) shm_ref(stake[i]);
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

int vma_file_at(uint64_t cr3, uint64_t va, int *file, uint64_t *index, uint32_t *prot)
{
    int got = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s)
        for (int j = 0; j < VMA_MAXAREA; j++)
            if (s->v[j].used && va >= s->v[j].start && va < s->v[j].end) {
                if (s->v[j].file >= 0) {
                    if (file)  *file  = s->v[j].file;
                    if (index) *index = (s->v[j].foff + (va - s->v[j].start)) / 4096;
                    if (prot)  *prot  = s->v[j].prot;
                    got = 1;
                }
                break;
            }
    spin_unlock_irqrestore(&vma_lock, fl);
    return got;
}

int vma_shm_at(uint64_t cr3, uint64_t va, int *shm, uint64_t *index, uint32_t *prot)
{
    int got = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s)
        for (int j = 0; j < VMA_MAXAREA; j++)
            if (s->v[j].used && va >= s->v[j].start && va < s->v[j].end) {
                if (s->v[j].shm >= 0) {
                    if (shm)   *shm   = s->v[j].shm;
                    if (index) *index = (s->v[j].foff + (va - s->v[j].start)) / 4096;
                    if (prot)  *prot  = s->v[j].prot;
                    got = 1;
                }
                break;
            }
    spin_unlock_irqrestore(&vma_lock, fl);
    return got;
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
    s->v[slot].file = -1;
    s->v[slot].shm  = -1;
    s->v[slot].foff = 0;
    s->v[slot].used = 1;
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    return got;
}

uint64_t vma_reserve_file(uint64_t cr3, uint64_t hint, uint64_t len, uint32_t prot,
                          int fh, uint64_t foff)
{
    if (fh < 0) return 0;
    /* Reserve first, then attach. vma_reserve does all of the arithmetic and
     * all of the overlap refusal; repeating any of it here would be a second
     * copy of exactly the code an mmap ABI is attacked through. */
    uint64_t base = vma_reserve(cr3, hint, len, prot);
    if (!base) return 0;

    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    int ok = 0;
    if (s)
        for (int j = 0; j < VMA_MAXAREA; j++)
            if (s->v[j].used && s->v[j].start == base) {
                s->v[j].file = fh;
                s->v[j].foff = foff;
                ok = 1;
                break;
            }
    spin_unlock_irqrestore(&vma_lock, fl);
    if (!ok) { vma_release(cr3, base, len); return 0; }
    pcache_file_ref(fh);
    return base;
}

/* Beside vma_reserve_file(), and deliberately its exact shape: reserve through
 * vma_reserve() so that ALL of the placement arithmetic and ALL of the overlap
 * refusal happen in one function, then attach the backing. Repeating any of
 * that here would be a second copy of the code an mmap ABI is attacked
 * through.
 *
 * WRITABLE IS ALLOWED HERE and refused in the file version, which is the one
 * real difference between the two and is worth stating rather than leaving to
 * be noticed. A writable FILE mapping would be a dirty page nothing can clean:
 * there is no writeback in this tree. A writable SEGMENT has nothing to write
 * back TO -- the frames ARE the storage, there is no second copy anywhere, and
 * a write is simply visible to everyone else mapping it. That is the entire
 * point, so the prot arrives from the caller untouched. */
uint64_t vma_reserve_shm(uint64_t cr3, uint64_t hint, uint64_t len, uint32_t prot,
                         int sh, uint64_t off)
{
    if (sh < 0) return 0;
    if (off & 0xFFF) return 0;              /* the fault path divides by 4096 */

    /* The range must lie inside the segment. Checked HERE, before the area
     * exists, because the alternative is a mapping whose upper pages fault
     * forever against a segment that ends below them -- do_shm() would decline,
     * the process would die, and the address it died on would be one the kernel
     * had told it it could have. */
    uint64_t need = (len + 0xFFF) & ~(uint64_t)0xFFF;
    if (!len || need < len) return 0;
    uint64_t pages = shm_pages(sh);
    if (!pages) return 0;                                   /* not a live segment */
    if (off / 4096 + need / 4096 > pages) return 0;

    uint64_t base = vma_reserve(cr3, hint, len, prot);
    if (!base) return 0;

    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    int ok = 0;
    if (s)
        for (int j = 0; j < VMA_MAXAREA; j++)
            if (s->v[j].used && s->v[j].start == base) {
                s->v[j].shm  = sh;
                s->v[j].foff = off;
                ok = 1;
                break;
            }
    spin_unlock_irqrestore(&vma_lock, fl);
    if (!ok) { vma_release(cr3, base, len); return 0; }
    /* The AREA's own reference, taken after the area exists and never before:
     * the caller keeps the one it came in with, exactly as vma_reserve_file()
     * promises for a pcache handle. */
    shm_ref(sh);
    return base;
}

int vma_reserve_fixed(uint64_t cr3, uint64_t start, uint64_t len, uint32_t prot)
{
    uint64_t a, b;
    if (vma_range(start, len, &a, &b) < 0) return -1;   /* also bounds it to the user region */

    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    int ret = -1;
    if (!s) goto out;
    if (overlaps(s, a, b)) goto out;                    /* never evict: see the header */
    for (int j = 0; j < VMA_MAXAREA; j++)
        if (!s->v[j].used) {
            s->v[j].start = a;
            s->v[j].end   = b;
            s->v[j].prot  = prot ? prot : VMA_READ;
            s->v[j].file  = -1;
            s->v[j].shm   = -1;
            s->v[j].foff  = 0;
            s->v[j].used  = 1;
            ret = 0;
            break;
        }
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    return ret;
}

/* THE LOADER'S FILE MAPPING, and it exists because vma_reserve_file() cannot
 * do this job: that one delegates placement to vma_reserve(), which only ever
 * hands out addresses inside [MM_MMAP_BASE, MM_MMAP_TOP) and honours a hint
 * only if the hint is inside that window too. Every program on this machine
 * links at 0x40000000..0x50000000, so a hint at 0x45000000 is silently ignored
 * and the area lands at 0x60000000 -- a file mapping of the right bytes at the
 * wrong address, which is a worse outcome than no mapping at all.
 *
 * So this is vma_reserve_fixed() plus two fields plus one reference, and it
 * lives HERE rather than in mmsys.c for the reason mmsys.c states about
 * itself: that file is the userland face, where "the argument validation for
 * an mmap ABI belongs next to the code that enforces those bounds". This has
 * no ABI and no user argument -- elf.c is the only caller and its arguments
 * came from a program header the loader already validated. What it does keep
 * is vma_range()'s bounds (so a mapping still cannot leave the private user
 * region) and overlaps()'s refusal (so it still cannot take an area away from
 * anything), because those two are what make a fixed reservation safe at all.
 *
 * Returns 0, or -1 for a bad range / no free slot / an occupied range / a
 * handle the cache has forgotten. A -1 here is NOT fatal to a load: elf.c
 * copies the run eagerly instead, which is the behaviour that was there before
 * this function existed. */
int vma_reserve_file_fixed(uint64_t cr3, uint64_t start, uint64_t len,
                           uint32_t prot, int fh, uint64_t foff)
{
    if (fh < 0) return -1;
    /* A file mapping is never writable: there is no writeback in this line and
     * no private-file COW fault case, so a writable file PTE would be a dirty
     * page nothing can ever clean. mmsys.c refuses this out loud for the mmap
     * ABI; refusing it here too means the invariant is a property of the
     * mechanism rather than of the one caller that currently respects it. */
    if (prot & VMA_WRITE) return -1;
    if (foff & 0xFFF) return -1;              /* the fault path divides by 4096 */

    uint64_t a, b;
    if (vma_range(start, len, &a, &b) < 0) return -1;
    if (a != start) return -1;                /* the loader's ranges are already
                                               * page aligned; a rounded one would
                                               * shift every file page index by
                                               * the rounding, silently */

    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    int ret = -1;
    if (!s) goto out;
    if (overlaps(s, a, b)) goto out;
    for (int j = 0; j < VMA_MAXAREA; j++)
        if (!s->v[j].used) {
            s->v[j].start = a;
            s->v[j].end   = b;
            s->v[j].prot  = prot ? prot : VMA_READ;
            s->v[j].file  = fh;
            s->v[j].foff  = foff;
            s->v[j].used  = 1;
            ret = 0;
            break;
        }
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    /* Outside the lock, like every other pcache call in this file: lock order
     * is BKL -> vma_lock -> pcache -> pmm and vma_lock stays a leaf. */
    if (ret == 0) pcache_file_ref(fh);
    return ret;
}

int vma_release(uint64_t cr3, uint64_t addr, uint64_t len)
{
    uint64_t start, end;
    if (vma_range(addr, len, &start, &end) < 0) return -1;

    int put[VMA_PUTMAX], np = 0;
    int take[VMA_PUTMAX], nt = 0;
    int sput[VMA_PUTMAX], nsp = 0;
    int stake[VMA_PUTMAX], nst = 0;
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

    /* File references move with the AREAS, so the three shapes below have three
     * different answers and each one is a bug if it is the other two:
     *   whole area removed  -> put the reference it held;
     *   area split in two   -> the far half is a NEW area over the same file,
     *                          so take one more;
     *   trimmed at an end   -> still one area, so nothing moves -- but `foff`
     *                          has to follow a front trim or every page of the
     *                          remainder reads from the wrong file offset. */
    int touched = 0;
    for (int j = 0; j < VMA_MAXAREA; j++) {
        if (!s->v[j].used) continue;
        uint64_t vs = s->v[j].start, ve = s->v[j].end;
        if (end <= vs || ve <= start) continue;          /* disjoint */
        touched = 1;
        if (start <= vs && end >= ve) {                  /* whole area goes */
            if (s->v[j].file >= 0) put[np++] = s->v[j].file;
            if (s->v[j].shm  >= 0) sput[nsp++] = s->v[j].shm;
            slot_clear(&s->v[j]);
        } else if (start > vs && end < ve) {             /* punched out of the middle: split */
            int slot = -1;
            for (int k = 0; k < VMA_MAXAREA; k++) if (!s->v[k].used) { slot = k; break; }
            if (slot < 0) { ret = -1; goto out; }        /* pre-checked above; belt and braces */
            s->v[slot] = s->v[j];
            s->v[j].end = start;
            s->v[slot].start = end;
            s->v[slot].foff += end - vs;
            s->v[slot].used = 1;
            if (s->v[slot].file >= 0 && nt < VMA_PUTMAX) take[nt++] = s->v[slot].file;
            /* The far half is a NEW area over the SAME segment, so it needs a
             * reference of its own -- and `foff` above already advanced by
             * `end - vs`, which is the segment offset for exactly the same
             * reason it is the file offset: both count bytes from the start of
             * whatever backs the area. */
            if (s->v[slot].shm >= 0 && nst < VMA_PUTMAX) stake[nst++] = s->v[slot].shm;
        } else if (start <= vs) {                        /* trimmed at the front */
            s->v[j].foff += end - vs;
            s->v[j].start = end;
        } else {                                         /* trimmed at the back */
            s->v[j].end = start;
        }
    }
    ret = touched ? 0 : -1;
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    for (int i = 0; i < nt; i++) pcache_file_ref(take[i]);
    for (int i = 0; i < nst; i++) shm_ref(stake[i]);
    put_all(put, np);
    shm_put_all(sput, nsp);
    return ret;
}

/* Split the area containing `b` so that `b` becomes an area boundary. A no-op
 * if `b` already is one (or is in no area). Returns 0, or -1 if a split was
 * needed and no slot was free -- which the caller has already ruled out, so a
 * -1 here is belt and braces exactly as vma_release's second slot check is.
 *
 * The far half is a NEW area over the same file, so it takes a reference, and
 * its `foff` advances by the bytes that stayed behind -- the same three-line
 * rule vma_release's split obeys, and getting the foff wrong would make every
 * page of the remainder read from the wrong offset in the file, silently.
 * Called with vma_lock held; the ref is taken by the caller after the unlock. */
static int split_at(struct space *s, uint64_t b, int *take, int *nt)
{
    for (int j = 0; j < VMA_MAXAREA; j++) {
        if (!s->v[j].used) continue;
        if (b <= s->v[j].start || b >= s->v[j].end) continue;
        int slot = -1;
        for (int k = 0; k < VMA_MAXAREA; k++) if (!s->v[k].used) { slot = k; break; }
        if (slot < 0) return -1;
        s->v[slot] = s->v[j];
        s->v[slot].start = b;
        s->v[slot].foff += b - s->v[j].start;
        s->v[slot].used = 1;
        s->v[j].end = b;
        if (s->v[slot].file >= 0 && *nt < VMA_PUTMAX) take[(*nt)++] = s->v[slot].file;
        return 0;
    }
    return 0;
}

int vma_protect(uint64_t cr3, uint64_t addr, uint64_t len, uint32_t prot)
{
    uint64_t start, end;
    if (vma_range(addr, len, &start, &end) < 0) return VMA_E_RANGE;

    int take[VMA_PUTMAX], nt = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    int ret = VMA_E_NOMEM;
    if (!s) goto out;

    /* ---- DECIDE EVERYTHING FIRST, MUTATE NOTHING ------------------------
     * Three refusals, all computed before the first write, because a partially
     * applied mprotect is worse than a refused one: the caller is told the
     * protection did not change and half of it did. */

    /* (1) the range must be RESERVED end to end. POSIX calls this ENOMEM and
     * it is the check that makes mprotect safe to hand a computed address:
     * without it, protecting a range that runs off the end of a mapping would
     * succeed and change less than it said. Areas never overlap, so walking
     * forward from `start` and jumping to each covering area's end either
     * reaches `end` or finds a hole. */
#ifndef VMA_PROTECT_NO_COVERAGE
    {
        uint64_t a = start;
        while (a < end) {
            uint64_t next = 0;
            for (int j = 0; j < VMA_MAXAREA; j++)
                if (s->v[j].used && a >= s->v[j].start && a < s->v[j].end) {
                    next = s->v[j].end; break;
                }
            if (!next) goto out;                 /* a hole: ret is VMA_E_NOMEM */
            a = next;
        }
    }
#else
    /* NEGATIVE CONTROL (tests/unit/mm_run.sh): the coverage check removed, and
     * removed rather than inverted because THE PLAUSIBLE WRONG VERSION IS THE
     * ONE THAT LOOKS FINE. Without it, mprotect over a range that runs past the
     * end of a mapping reports success and changes only the part that was
     * mapped -- every assertion about the pages that ARE mapped still passes,
     * and the caller is told the boundary it asked for exists. */
#endif

    /* (2) a file-backed area may never become writable. Checked over every area
     * the range touches, not only the first. */
    if (prot & VMA_WRITE)
        for (int j = 0; j < VMA_MAXAREA; j++)
            if (s->v[j].used && s->v[j].file >= 0 &&
                start < s->v[j].end && s->v[j].start < end) { ret = VMA_E_ACCES; goto out; }

    /* (3) the splits. At most two: one at each edge of the range, each in a
     * different area (or the same one, when the range sits strictly inside a
     * single area -- which is the case that needs both). Count what is
     * actually needed rather than reserving two unconditionally, because
     * VMA_MAXAREA is 16 and the common case (protecting a whole area) needs
     * none. */
    {
        int need = 0, spare = 0;
        for (int j = 0; j < VMA_MAXAREA; j++) {
            if (!s->v[j].used) { spare++; continue; }
            if (start > s->v[j].start && start < s->v[j].end) need++;
            if (end   > s->v[j].start && end   < s->v[j].end) need++;
        }
        if (spare < need) goto out;              /* ret is VMA_E_NOMEM */
    }

    /* ---- now it cannot fail --------------------------------------------- */
    /* Belt and braces: the slot pre-check above makes a -1 here unreachable.
     * If it were reached, the areas would be split and NO protection would have
     * changed -- the split is invisible from outside (same coverage, same
     * prots) and its file reference is taken on the way out like every other
     * path, so "the call failed and nothing about the space changed" still
     * holds for anything a caller can observe. */
    if (split_at(s, start, take, &nt) < 0) goto out;
    if (split_at(s, end,   take, &nt) < 0) goto out;

    /* After both splits every area is entirely inside the range or entirely
     * outside it, so this is a straight assignment and no area is half-changed.
     * NOT floored to VMA_READ: see the header -- prot 0 is the guard page. */
    for (int j = 0; j < VMA_MAXAREA; j++)
        if (s->v[j].used && s->v[j].start >= start && s->v[j].end <= end)
            s->v[j].prot = prot;
    ret = 0;
out:
    spin_unlock_irqrestore(&vma_lock, fl);
    /* Outside the lock, like every other pcache call here: BKL -> vma_lock ->
     * pcache -> pmm, and vma_lock stays a leaf. A split that happened before a
     * later step failed still owns its reference, so this runs on every path. */
    for (int i = 0; i < nt; i++) pcache_file_ref(take[i]);
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

/* THE TABLE, COPIED OUT, ascending by start.
 *
 * Added for c/kernel/exec/coredump.c, which has to describe a dying process's
 * whole address space and had nothing to ask: this file exports vma_count()
 * and vma_prot_at()/vma_file_at()/vma_shm_at(), all of which answer about ONE
 * address, so enumerating an address space meant probing every page of a 1 GiB
 * region -- 262,144 four-level walks to recover a table of at most 32 rows that
 * is sitting right here.
 *
 * A COPY rather than a borrowed pointer, and the lock is the reason: every
 * other reader of `spaces` holds vma_lock for the length of its answer, and a
 * caller handed `&s->v[0]` would be reading a live table with no lock at all --
 * from the fault path, where the process it describes is being torn down. 32 x
 * 40 bytes is a memcpy; the caller's buffer is the caller's problem.
 *
 * Returns how many areas EXIST, which may be more than `max` (nothing on this
 * machine can produce more than VMA_MAXAREA, but a caller sizing its buffer at
 * something smaller has to be able to tell "that was all" from "there was more"
 * -- the same contract vma_count() would give it and the same one getgroups(2)
 * gives). Areas are written in ascending `start` order; the slot array is not
 * sorted, so this sorts, which costs 32^2 comparisons at most and saves every
 * consumer from having to. */
int vma_snapshot(uint64_t cr3, struct vma *out, int max)
{
    if (!out || max < 0) return 0;
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) for (int j = 0; j < VMA_MAXAREA; j++) {
        if (!s->v[j].used) continue;
        int stored = n < max ? n : max;
        int k = stored;
        if (k == max) {
            /* Full. Keep the `max` LOWEST addresses rather than the first
             * `max` slots visited: the slot array is not in address order, so
             * "the first ones I met" would hand back an arbitrary subset while
             * looking like a prefix. Only reachable if a caller passes a `max`
             * below VMA_MAXAREA -- nothing here does, and a truncation rule
             * that only exists for a caller that does not exist yet is exactly
             * where a silently-wrong answer waits. */
            if (max == 0 || out[max - 1].start <= s->v[j].start) { n++; continue; }
            k = max - 1;
        }
        while (k > 0 && out[k - 1].start > s->v[j].start) { out[k] = out[k - 1]; k--; }
        out[k] = s->v[j];
        n++;
    }
    spin_unlock_irqrestore(&vma_lock, fl);
    return n;
}

int vma_spaces_live(void) { return spaces_live; }

/* The i'th LIVE area of `cr3`, by value.
 *
 * Added for /proc/<pid>/maps (c/fs/procfs_src.c), which was the first caller
 * in the tree that wanted to SEE an address space rather than ask a question
 * about one address in it. Everything else here answers "what is at this va" --
 * vma_prot_at, vma_file_at, vma_shm_at -- and none of them can enumerate, so
 * until now the shape of a process's address space was known to this file and
 * to nothing else.
 *
 * BY VALUE, not by pointer, for the reason every accessor in this file is:
 * `spaces[]` is mutated under vma_lock from the fault path, and a pointer
 * handed out here would be read after the lock is dropped. A copy of 40 bytes
 * costs nothing against that.
 *
 * `i` INDEXES LIVE AREAS, not slots. A slot index would make the caller's loop
 * depend on VMA_MAXAREA and on the free-slot layout, so an munmap between two
 * calls would silently renumber everything after the hole -- and a maps file
 * that skips an area because a slot went free is exactly the kind of wrong
 * that reads as correct. Two calls with an intervening change can still show a
 * seam; procfs's latch (c/fs/procfs.h, point 4) is what bounds that to one
 * read pass. Returns 1 if there is an i'th area, 0 otherwise. */
int vma_nth(uint64_t cr3, int i, struct vma *out)
{
    if (!out || i < 0) return 0;
    int n = 0, got = 0;
    uint64_t fl = spin_lock_irqsave(&vma_lock);
    struct space *s = find(cr3);
    if (s) for (int j = 0; j < VMA_MAXAREA; j++) {
        if (!s->v[j].used) continue;
        if (n++ != i) continue;
        *out = s->v[j];
        got = 1;
        break;
    }
    spin_unlock_irqrestore(&vma_lock, fl);
    return got;
}
