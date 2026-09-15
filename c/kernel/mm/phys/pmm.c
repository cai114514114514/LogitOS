#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "rmap.h"
#include "reclaim.h"
#include "mmhost.h"
#include "spinlock.h"
#include "kprintf.h"
#include "../../../../include/weaksym.h"   /* the weak declarations below are an ELF idiom */

/* The out-of-memory recorder (c/kernel/mm/reclaim/oom.h). Weak for the reason given at
 * the same declaration in fault.c: this file is compiled by host harnesses with
 * no process table behind them. */
void oom_alloc_fail(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(oom_alloc_fail);

/* M25 P1/P2: the physical frame allocator is peeled out from under the BKL --
 * pmm_alloc/free/alloc_contig take their own lock so BKL-free paths on other
 * cores can allocate concurrently. irqsave: pmm is reachable from fault/IRQ
 * context (page-table fills) and must not be preempted mid-bitmap-scan. Lock
 * order: kheap_lock -> pmm_lock (grow() calls pmm_alloc_contig under kheap_lock);
 * pmm never takes kheap_lock, so the order never reverses. */
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* A bitmap physical frame allocator (1 bit per 4 KiB frame, 1 = used) with a
 * parallel REFERENCE COUNT per frame.
 *
 * Why the refcount table exists (and why one bit could not do it): under
 * copy-on-write fork two address spaces map the SAME frame, and the frame may
 * only return to the free pool when the last of them lets go. "Allocated /
 * not allocated" cannot express "referenced twice", so this is a change to the
 * allocator's data structure, not a layer on top of it.
 *
 * Shape: a flat uint16_t array indexed by frame number, parked immediately
 * after the allocation bitmap in the reserved region above the kernel image.
 * For the 512 MiB this kernel runs with that is 131072 frames -> 256 KiB, i.e.
 * 0.05% of RAM, for O(1) lookup with no allocation and no locking beyond the
 * one lock the allocator already had. A sparser structure (a hash of only the
 * shared frames) would save 128 KiB and cost a hash lookup on the path that
 * runs on every fork, every exit and every page fault; that is the wrong trade
 * at this scale. uint16_t rather than uint8_t: 255 references to one frame is
 * reachable (NPROC is 32 today, but a page-cache or a shared library maps one
 * frame into every space that touches it), 65535 is not.
 *
 * OVERFLOW IS DEFINED, NOT WRAPPED: at PMM_REF_MAX the count saturates and the
 * frame becomes PINNED -- pmm_ref() reports failure so the caller copies
 * instead of sharing, and pmm_free() on a saturated frame is a no-op. The
 * failure mode is therefore a bounded leak of one frame, never a premature
 * free (which would be silent corruption). Pinned frames are counted and
 * reported so the leak is visible rather than mysterious.
 *
 * INVARIANT, checked continuously: for every frame, "bitmap bit set" <=>
 * "refcount >= 1". Every mutation point re-checks the local form of it and
 * trips loudly (mm_bug) rather than continuing; pmm_audit() re-derives the
 * global counters from the table and is cheap enough to call from tests and
 * from the boot report.
 *
 * All usable RAM in our QEMU config sits below the identity-mapped first
 * 1 GiB, so a physical address can be used directly as a virtual address
 * (mm_p2v; see mmhost.h for why that is a function and not a cast).
 * Correction (2026-09-09): the old paragraph describes only the compatibility
 * zone now. pmm_alloc/contig/reserve still allocate below 1 GiB; the explicit
 * ANY APIs use high RAM through the direct map, never through its identity VA.
 * Firmware holes remain reserved, even when RAM exists above the 4 GiB hole. */

/* --- Multiboot2 structures (only what we need) --- */
#define MB2_TAG_MMAP      6
#define MMAP_AVAILABLE    1

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
};

void *memset(void *dst, int value, size_t n);   /* lib/string.c */

static uint8_t  *bitmap;
static uint8_t  *poison_bm;      /* 1 = this free frame currently holds valid poison */
static uint16_t *refcnt;
static uint8_t  *pincnt;         /* "do not move this frame right now"; see pmm.h */
static uint64_t  pins_live;      /* frames with pincnt > 0 */
/* 128 frames = 512 KiB. Sized by what it is for: the swap-in fault, which needs
 * one frame per page it brings back and runs in bursts when a process reads
 * back a working set it has just been evicted out of. 32 was the first guess
 * and was too small -- a read-back pass spent it in 32 faults and then started
 * killing the process. It is still a cushion and not a supply (the fault path
 * forces a reclaim pass when it empties, see reclaim_swapin), so there is
 * nothing to gain by making it large: this is 0.1% of a 512 MiB machine. */
static uint64_t  reserve_frames = 128;
static uint64_t  reserve_hits, alloc_fails;
static uint64_t total_frames;
static uint64_t used_frames;     /* frames with refcount >= 1 */
static uint64_t shared_frames;   /* frames with refcount >= 2 */
static uint64_t pinned_frames;   /* frames whose refcount saturated */
static uint64_t refs_total;      /* sum of all refcounts */
static uint64_t usable_bytes;
static uint64_t alloc_hint;      /* low-zone scan hint */
static uint64_t high_hint;       /* independent: high scans never strand the low hint */
static uint64_t low_zone_free, high_available;
static uint64_t high_allocs, high_max_phys;
static uint64_t physmap_pages, physmap_tables;
static int physmap_ready, physmap_low_ready;
static uint64_t bug_count;
static uint8_t *ram_bm;          /* immutable firmware AVAILABLE-page provenance */
static uint64_t mm_meta_bytes;   /* bitmap + poison bitmap + refcount table */
static int      poison_level = 1;

static inline void bm_set(uint64_t f)   { bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void bm_clear(uint64_t f) { bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline int  bm_test(uint64_t f)  { return bitmap[f >> 3] & (1u << (f & 7)); }

static inline void pz_set(uint64_t f)   { poison_bm[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void pz_clear(uint64_t f) { poison_bm[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline int  pz_test(uint64_t f)  { return poison_bm[f >> 3] & (1u << (f & 7)); }

/* An invariant violation. Loud, counted, and non-fatal: the allocator refuses
 * the operation and keeps running, because halting the machine inside the
 * frame allocator (with pmm_lock held and IF=0) turns one process's bug into a
 * dead machine. `bug_count` is what a test asserts on. */
static void mm_bug(const char *what, uint64_t frame)
{
    bug_count++;
    kprintf("[mm] BUG: %s (frame %d, phys %p, refcount %d)\n",
            what, (int)frame, (void *)(frame * FRAME_SIZE),
            (int)(refcnt && frame < total_frames ? refcnt[frame] : 0));
}

/* ---------------------------------------------------------------- poison --
 * A freed frame is filled with a per-frame pattern; the pattern is verified
 * when the frame is handed back out. A use-after-free stops being "corruption
 * somewhere, someday" and becomes "the frame you just allocated was written to
 * after it was freed", reported at the allocation, with the frame number.
 *
 * Level 1 (the default, always on) touches only the first PMM_POISON_HEAD
 * bytes = one cache line, which is where a stale pointer's victim almost
 * always writes first (a freelist header, a struct's first fields, the start
 * of a memset). It costs one cache line of stores per free and one compare per
 * alloc, so it is affordable to leave on in a shipping kernel. Level 2 covers
 * the whole frame and is for tests and for bisecting a live corruption.
 *
 * Poisoning also means a freed frame no longer carries the previous owner's
 * data into the next process (pmm_alloc's callers are not all required to
 * zero), which is a small confidentiality win on top. */
static uint64_t poison_word(uint64_t frame, uint64_t idx)
{
    return 0xFEEDFACEDEADBEEFull ^ (frame * 0x9E3779B97F4A7C15ull) ^ idx;
}

static uint64_t poison_bytes(void)
{
    if (poison_level >= 2) return FRAME_SIZE;
    if (poison_level == 1) return PMM_POISON_HEAD;
    return 0;
}

static void poison_fill(uint64_t frame)
{
    uint64_t n = poison_bytes();
    if (!n) { pz_clear(frame); return; }
    uint64_t *p = (uint64_t *)mm_p2v(frame * FRAME_SIZE);
    for (uint64_t i = 0; i < n / 8; i++)
        p[i] = poison_word(frame, i);
    pz_set(frame);
}

static void poison_check(uint64_t frame)
{
    if (!pz_test(frame))
        return;                     /* never poisoned (boot-time release, or poison off) */
    pz_clear(frame);
    uint64_t n = poison_bytes();
    /* n is the CURRENT level's extent, and the frame was filled at whatever the
     * level was when it was freed. Raising the level therefore used to compare
     * bytes that were never written and report every previously-freed frame as
     * a use-after-free -- an accusation of corruption produced by turning the
     * corruption detector up, which is the worst possible failure mode for a
     * guardrail. pmm_set_poison() now invalidates the whole poison map on a
     * change, so by the time control reaches here the fill and the check agree
     * by construction. The `!n` case (level dropped to 0) is kept because the
     * map is not consulted at all then. */
    if (!n) return;
    uint64_t *p = (uint64_t *)mm_p2v(frame * FRAME_SIZE);
    for (uint64_t i = 0; i < n / 8; i++) {
        if (p[i] != poison_word(frame, i)) {
            mm_bug("use-after-free: freed frame was written to", frame);
            kprintf("[mm]      at byte %d: %p, expected %p\n",
                    (int)(i * 8), (void *)p[i], (void *)poison_word(frame, i));
            return;                 /* one report per frame is enough */
        }
    }
}

/* ------------------------------------------------------------ boot setup -- */

static void reserve(uint64_t base, uint64_t len)
{
    uint64_t start = base / FRAME_SIZE;
    uint64_t end   = (base + len + FRAME_SIZE - 1) / FRAME_SIZE;
    for (uint64_t f = start; f < end && f < total_frames; f++) {
        if (!bm_test(f)) { bm_set(f); used_frames++; if (f < PMM_LOW_LIMIT / FRAME_SIZE) low_zone_free--; }
        if (refcnt[f] == 0) { refcnt[f] = 1; refs_total++; }
        pz_clear(f);
    }
}

static void release(uint64_t base, uint64_t len)
{
    uint64_t start = (base + FRAME_SIZE - 1) / FRAME_SIZE;  /* round up  */
    uint64_t end   = (base + len) / FRAME_SIZE;             /* round down */
    for (uint64_t f = start; f < end && f < total_frames; f++) {
        if (bm_test(f)) { bm_clear(f); used_frames--; if (f < PMM_LOW_LIMIT / FRAME_SIZE) low_zone_free++; }
        if (refcnt[f]) { refs_total -= refcnt[f]; refcnt[f] = 0; }
        pz_clear(f);
    }
}

/* The physmap builder lives in this translation unit so every existing host
 * PMM suite continues to link the real boot allocator. It runs before APs,
 * rmap and reclaim exist, using only low-zone pages for its own tables. */
#define PMM_PTE_ADDR 0x000ffffffffff000ull
#define PMM_PTE_P    1ull
#define PMM_PTE_W    2ull
#define PMM_PTE_U    4ull
#define PMM_PTE_PS   128ull
#define PMM_PTE_NX   (1ull << 63)

static int direct_nx_enabled(void)
{
#ifdef MM_HOSTTEST
    /* Host tests inspect actual constructed PTEs with NX both enabled and
     * disabled; neither variant executes a privileged host instruction. */
#ifdef PHYSMAP_TEST_NO_NX
    return 0;
#else
    return 1;
#endif
#else
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080u));
    (void)hi;
    return (lo & (1u << 11)) != 0;   /* EFER.NXE was decided by boot/long.asm */
#endif
}

static uint64_t *direct_next(uint64_t *table, unsigned idx)
{
    uint64_t e = table[idx];
    if (!(e & PMM_PTE_P)) {
        uint64_t f = pmm_alloc_contig(1);  /* never invokes reclaim at bootstrap */
        if (!f) return NULL;
        memset(mm_p2v(f), 0, FRAME_SIZE);
        table[idx] = f | PMM_PTE_P | PMM_PTE_W; /* USER deliberately absent */
        physmap_tables++;
        e = table[idx];
    }
    if ((e & (PMM_PTE_PS | PMM_PTE_U)) || (e & PMM_PTE_ADDR) >= PMM_LOW_LIMIT)
        return NULL;                    /* cannot adopt an incompatible shared subtree */
    return mm_p2v(e & PMM_PTE_ADDR);
}

static int direct_map_range(uint64_t start, uint64_t end, uint64_t flags)
{
    uint64_t root = mm_read_cr3() & PMM_PTE_ADDR;
    if (!root || root >= PMM_LOW_LIMIT) return -1;
    uint64_t *pml4 = mm_p2v(root);
    while (start < end) {
        uint64_t va = PHYSMAP_BASE + start;
        uint64_t *pdpt = direct_next(pml4, (unsigned)((va >> 39) & 511));
        if (!pdpt) return -1;
        uint64_t *pd = direct_next(pdpt, (unsigned)((va >> 30) & 511));
        if (!pd) return -1;
        unsigned di = (unsigned)((va >> 21) & 511);
        /* Only a fully RAM-backed, aligned 2 MiB interval is a large leaf.
         * Boundary pages stay 4 KiB, so alignment never maps a firmware hole. */
        if (!(start & 0x1fffffull) && end - start >= 0x200000ull && !pd[di]) {
            pd[di] = start | flags | PMM_PTE_PS;
            start += 0x200000ull;
            physmap_pages += 512;
        } else {
            if ((pd[di] & (PMM_PTE_P | PMM_PTE_PS)) == (PMM_PTE_P | PMM_PTE_PS)) {
                /* An overlapping AVAILABLE descriptor can name a page already
                 * covered by a large leaf; it adds no mapping and no capacity. */
                if ((pd[di] & PMM_PTE_ADDR & ~0x1fffffull) != (start & ~0x1fffffull))
                    return -1;
            } else {
                uint64_t *pt = direct_next(pd, di);
                if (!pt) return -1;
                unsigned ti = (unsigned)((va >> 12) & 511);
                if (!(pt[ti] & PMM_PTE_P)) { pt[ti] = start | flags; physmap_pages++; }
                else if (pt[ti] != (start | flags)) return -1;
            }
            start += FRAME_SIZE;
        }
    }
    return 0;
}

static int mmap_entry_range(const struct mb2_mmap_entry *me, uint64_t *start, uint64_t *end)
{
    if (me->type != MMAP_AVAILABLE || !me->len || me->addr >= PHYSMAP_SIZE ||
        me->len > PHYSMAP_SIZE - me->addr) return 0;
    *start = (me->addr + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1);
    *end = (me->addr + me->len) & ~(uint64_t)(FRAME_SIZE - 1);
    return *start < *end;
}

/* UEFI emits separate descriptors for reclaimed Loader/BootServices and
 * conventional RAM. A 16 GiB guest needs roughly 15 MiB of flat metadata;
 * requiring ONE descriptor immediately after the kernel rejected a usable
 * 16 GiB OVMF boot. Cover a run by adjacent AVAILABLE descriptors, independent
 * of their order, but never bridge an unreported/reserved page. */
static int metadata_available(struct mb2_tag_mmap *map, uint64_t base, uint64_t bytes)
{
    uint8_t *end = (uint8_t *)map + map->size;
    uint64_t cursor = base, limit = base + bytes;
    while (cursor < limit) {
        uint64_t next = cursor;
        for (uint8_t *p = (uint8_t *)map->entries; p + sizeof(struct mb2_mmap_entry) <= end; ) {
            uint64_t a, b;
            if (mmap_entry_range((struct mb2_mmap_entry *)p, &a, &b) && a <= cursor && b > next)
                next = b;
            if (map->entry_size > (uint64_t)(end - p)) break;
            p += map->entry_size;
        }
        if (next == cursor) return 0;
#ifdef PMM_META_SINGLE_RANGE
        return next >= limit; /* negative: the old one-descriptor assumption */
#endif
        cursor = next;
    }
    return 1;
}

static uint64_t metadata_place(struct mb2_tag_mmap *map, uint64_t floor, uint64_t bytes,
                               uint64_t info, uint64_t info_size)
{
    uint8_t *end = (uint8_t *)map + map->size;
    uint64_t best = 0;
    for (uint8_t *p = (uint8_t *)map->entries; p + sizeof(struct mb2_mmap_entry) <= end; ) {
        uint64_t a, b;
        if (mmap_entry_range((struct mb2_mmap_entry *)p, &a, &b)) {
            uint64_t candidate = a > floor ? a : floor;
#ifdef PMM_META_FIXED_AFTER_KERNEL
            candidate = floor; /* negative: refuse firmware holes after kernel */
#endif
            if (candidate < PMM_LOW_LIMIT && bytes <= PMM_LOW_LIMIT - candidate) {
                if (info < candidate + bytes && candidate < info + info_size)
                    candidate = (info + info_size + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1);
                if (candidate < PMM_LOW_LIMIT && bytes <= PMM_LOW_LIMIT - candidate &&
                    (!best || candidate < best) && metadata_available(map, candidate, bytes))
                    best = candidate;
            }
        }
        if (map->entry_size > (uint64_t)(end - p)) break;
        p += map->entry_size;
    }
    return best;
}


int pmm_is_ram(uint64_t phys, size_t bytes)
{
    if (!ram_bm || !bytes || phys >= PHYSMAP_SIZE ||
        bytes > PHYSMAP_SIZE - phys) return 0;
    uint64_t first = phys / FRAME_SIZE;
    uint64_t last = (phys + bytes - 1) / FRAME_SIZE;
    if (last >= total_frames) return 0;
    for (uint64_t f = first; f <= last; f++)
        if (!(ram_bm[f >> 3] & (1u << (f & 7)))) return 0;
    return 1;
}

void pmm_init(uint64_t mb_info_addr)
{
    /* Retain the low boot hand-off contract on both BIOS and UEFI. Metadata
     * and map-building tables must fit in actually AVAILABLE, identity-mapped
     * RAM BEFORE a high frame can be accessed. Failure leaves no allocatable
     * pool and is printed, rather than pretending a RAM-size clamp succeeded. */
    total_frames = used_frames = refs_total = usable_bytes = low_zone_free = 0;
    high_available = high_allocs = high_max_phys = high_hint = alloc_hint = 0;
    physmap_ready = physmap_low_ready = 0; physmap_pages = physmap_tables = mm_meta_bytes = 0;
    reserve_hits = alloc_fails = shared_frames = pinned_frames = pins_live = bug_count = 0;
    bitmap = poison_bm = pincnt = ram_bm = NULL; refcnt = NULL;
    if (mb_info_addr >= PMM_LOW_LIMIT - 8) {
        kprintf("[physmap] boot info outside low RAM; PMM disabled\n"); return;
    }
    uint32_t total_size = *(volatile uint32_t *)mm_p2v(mb_info_addr);
    if (total_size < 16 || total_size > PMM_LOW_LIMIT - mb_info_addr) {
        kprintf("[physmap] invalid boot info extent; PMM disabled\n"); return;
    }
    uint8_t *p = mm_p2v(mb_info_addr + 8);
    uint8_t *end = (uint8_t *)mm_p2v(mb_info_addr) + total_size;
    struct mb2_tag_mmap *mmap = NULL;
    uint64_t highest = 0;
    while (p + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (!tag->type) break;
        if (tag->size < sizeof(*tag) || tag->size > (uint64_t)(end - p)) break;
        if (tag->type == MB2_TAG_MMAP && tag->size >= sizeof(struct mb2_tag_mmap)) {
            struct mb2_tag_mmap *mt = (struct mb2_tag_mmap *)p;
            if (mt->entry_size >= sizeof(struct mb2_mmap_entry)) mmap = mt;
        }
        uint64_t step = ((uint64_t)tag->size + 7) & ~7ull;
        if (step > (uint64_t)(end - p)) break;
        p += step;
    }
    if (!mmap) { kprintf("[physmap] no usable memory map; PMM disabled\n"); return; }
    uint8_t *mend = (uint8_t *)mmap + mmap->size;
    for (uint8_t *e = (uint8_t *)mmap->entries; e + sizeof(struct mb2_mmap_entry) <= mend; ) {
        struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
        uint64_t a, b;
        if (mmap_entry_range(me, &a, &b) && b > highest) highest = b;
        if (mmap->entry_size > (uint64_t)(mend - e)) break;
        e += mmap->entry_size;
    }
    uint64_t frames = highest / FRAME_SIZE;
    uint64_t base = (mm_kernel_end_phys() + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1);
    uint64_t bm_bytes = (((frames + 7) / 8) + 7) & ~7ull;
    uint64_t rc_bytes = frames * sizeof(uint16_t);
    uint64_t pin_bytes = (frames + 7) & ~7ull;
    uint64_t meta = bm_bytes * 3 + rc_bytes + pin_bytes;
    uint64_t kernel_reserved = base;
    base = frames ? metadata_place(mmap, base, meta, mb_info_addr, total_size) : 0;
    if (!base) {
        kprintf("[physmap] PMM metadata does not fit unoccupied low RAM; PMM disabled\n"); return;
    }
    kprintf("[physmap] metadata base=%p bytes=%llu kernel_end=%p\n", (void *)base, meta, (void *)kernel_reserved);
    total_frames = frames;
    mm_meta_bytes = meta;
    bitmap = mm_p2v(base);
    poison_bm = mm_p2v(base + bm_bytes);
    refcnt = mm_p2v(base + bm_bytes * 2);
    pincnt = mm_p2v(base + bm_bytes * 2 + rc_bytes);
    ram_bm = mm_p2v(base + bm_bytes * 2 + rc_bytes + pin_bytes);
    memset(ram_bm, 0, bm_bytes);
    memset(bitmap, 0xff, bm_bytes);
    memset(poison_bm, 0, bm_bytes);
    memset(pincnt, 0, pin_bytes);
    used_frames = refs_total = frames;
    for (uint64_t f = 0; f < frames; f++) refcnt[f] = 1;

    /* Release LOW RAM first. High pages remain reserved until EVERY high RAM
     * interval has a complete supervisor mapping. An OOM/negative control in
     * the builder therefore degrades to a low-only boot, never a high pointer
     * into a half-built map. Low DMA users keep their explicit allocation zone. */
    for (uint8_t *e = (uint8_t *)mmap->entries; e + sizeof(struct mb2_mmap_entry) <= mend; ) {
        struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
        uint64_t a, b;
        if (mmap_entry_range(me, &a, &b)) {
            usable_bytes += b - a;
            for (uint64_t f = a / FRAME_SIZE; f < b / FRAME_SIZE; f++)
                ram_bm[f >> 3] |= (uint8_t)(1u << (f & 7));
            if (a < PMM_LOW_LIMIT) release(a, (b < PMM_LOW_LIMIT ? b : PMM_LOW_LIMIT) - a);
        }
        if (mmap->entry_size > (uint64_t)(mend - e)) break;
        e += mmap->entry_size;
    }
    reserve(0, kernel_reserved);
    reserve(base, meta); /* relocated metadata must not consume all intervening RAM */
    reserve(mb_info_addr, total_size);

    uint64_t flags = PMM_PTE_P | PMM_PTE_W | (direct_nx_enabled() ? PMM_PTE_NX : 0);
    /* Low aliases must be complete even in the high-map negative control.
     * Legacy host fixtures have no hardware CR3 and use their arena directly;
     * a nonzero simulated CR3 exercises this exact production builder. */
    int low_ok = 1;
#ifdef MM_HOSTTEST
    if (mm_read_cr3())
#endif
    for (uint8_t *e = (uint8_t *)mmap->entries; e + sizeof(struct mb2_mmap_entry) <= mend; ) {
        uint64_t a, b;
        if (mmap_entry_range((struct mb2_mmap_entry *)e, &a, &b) && a < PMM_LOW_LIMIT &&
            direct_map_range(a, b < PMM_LOW_LIMIT ? b : PMM_LOW_LIMIT, flags)) low_ok = 0;
        if (mmap->entry_size > (uint64_t)(mend - e)) break;
        e += mmap->entry_size;
    }
    if (!low_ok) {
        /* Exposing a half-built low alias to DMA would be unsafe. Keep no
         * allocatable pages; callers receive OOM instead of unmapped memory. */
        kprintf("[physmap] low RAM alias failed; PMM disabled\n");
        total_frames = used_frames = refs_total = usable_bytes = low_zone_free = 0;
        bitmap = poison_bm = pincnt = ram_bm = NULL; refcnt = NULL;
        return;
    }
    if (physmap_pages) mm_write_cr3(mm_read_cr3());
    physmap_low_ready = 1; /* publish only the completed, flushed low alias */
    int map_ok = 1;
    for (uint8_t *e = (uint8_t *)mmap->entries; e + sizeof(struct mb2_mmap_entry) <= mend; ) {
        uint64_t a, b;
        if (mmap_entry_range((struct mb2_mmap_entry *)e, &a, &b) && b > PMM_LOW_LIMIT) {
            if (a < PMM_LOW_LIMIT) a = PMM_LOW_LIMIT;
#ifdef PHYS_MAP_DISABLE_HIGH
            /* Negative control: keep high frames reserved instead of making
             * the high-address workload accidentally pass through low RAM. */
            map_ok = 0;
#else
            if (direct_map_range(a, b, flags)) map_ok = 0;
#endif
        }
        if (mmap->entry_size > (uint64_t)(mend - e)) break;
        e += mmap->entry_size;
    }
    (void)flags;
    (void)direct_map_range;  /* retain identical build surface for the control */
    if (map_ok) {
        if (physmap_pages) mm_write_cr3(mm_read_cr3());
        for (uint8_t *e = (uint8_t *)mmap->entries; e + sizeof(struct mb2_mmap_entry) <= mend; ) {
            uint64_t a, b;
            if (mmap_entry_range((struct mb2_mmap_entry *)e, &a, &b) && b > PMM_LOW_LIMIT) {
                if (a < PMM_LOW_LIMIT) a = PMM_LOW_LIMIT;
                release(a, b - a);
            }
            if (mmap->entry_size > (uint64_t)(mend - e)) break;
            e += mmap->entry_size;
        }
        high_available = total_frames - used_frames - low_zone_free;
        /* Prefer payload RAM above 4 GiB when the physical map reaches it.
         * This preserves scarce low DMA RAM and exercises full-width physical
         * addresses during ordinary large-memory boots. scan_zone still wraps
         * through 1..4 GiB, including when every descriptor above 4 GiB is
         * reserved. Small machines start at 1 GiB; the legacy low allocator's
         * cursor and immediate free/reuse behavior are unchanged. */
        const uint64_t far_first = 0x100000000ull / FRAME_SIZE;
        high_hint = total_frames > far_first ? far_first : PMM_LOW_LIMIT / FRAME_SIZE;
        physmap_ready = 1;
    }
    kprintf("[physmap] %s: base=%p high_pages=%llu table_pages=%llu low_free=%llu NX=%d\n",
            map_ok ? "ready" : "HIGH DISABLED", (void *)PHYSMAP_BASE,
            (unsigned long long)high_available, (unsigned long long)physmap_tables,
            (unsigned long long)low_zone_free, direct_nx_enabled());
    pmm_report("boot");
    rmap_init(total_frames);
}

/* --------------------------------------------------------- allocate/free -- */

/* Scan the bitmap a 64-bit word at a time (Linux find_next_zero_bit style):
 * a fully-used word (all ones) is skipped with one branch, and the first free
 * bit inside a partial word is located with __builtin_ctzll(~word).  Resumes
 * from alloc_hint so successive allocs don't rescan exhausted low frames, and
 * wraps back to frame 0 once before giving up so no free frame is missed. */
/* ------------------------------------------------------- the leak trace --
 * A leak is a TREND, and a trend needs samples nobody has to remember to take.
 * `pmm_free_frames()` answers "how much is free right now", which is useless on
 * its own: memory legitimately goes up and down as apps open and close, and the
 * question is whether it comes back.
 *
 * So the allocator keeps its own low-water mark and says so, once, every time
 * free memory reaches a new all-time low a full step (1 MiB) below the last one
 * it announced. That gives a self-triggering, self-throttling time series with
 * exactly the shape the answer needs:
 *
 *   a system that is merely BUSY  -> the line stops appearing once the workload
 *                                    has been round once; the low-water mark is
 *                                    reached and never beaten.
 *   a system that is LEAKING      -> the line keeps appearing forever, and the
 *                                    interval between two of them is the leak
 *                                    rate, in MiB per whatever happened between.
 *
 * At most total/step lines can ever be printed in a boot (511 at 1 MiB over 511
 * MiB), and the check is two comparisons on the allocation path. */
static uint64_t low_free = ~(uint64_t)0;   /* lowest free-frame count seen */
static uint64_t low_step = 256;            /* announce each new 1 MiB low */
static uint64_t low_announced = ~(uint64_t)0;

void pmm_watch_step(uint64_t frames) { low_step = frames ? frames : 1; }

uint64_t pmm_low_free_frames(void)
{
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t v = (low_free == ~(uint64_t)0) ? total_frames - used_frames : low_free;
    spin_unlock_irqrestore(&pmm_lock, fl);
    return v;
}

/* Called with pmm_lock held. kprintf under the lock is what mm_bug already
 * does; pmm_report() must NOT be used here because it takes the lock itself. */
static void watch_low(void)
{
    uint64_t free_now = total_frames - used_frames;
    if (free_now >= low_free) return;
    low_free = free_now;
    if (low_announced != ~(uint64_t)0 && free_now + low_step > low_announced) return;
    low_announced = free_now;
    kprintf("[mm] low: %d frames free (%d MiB), %d used, %d shared, %d bugs\n",
            (int)free_now, (int)(free_now * FRAME_SIZE / (1024 * 1024)),
            (int)used_frames, (int)shared_frames, (int)bug_count);
}

static uint64_t take_frame(uint64_t cand)
{
    /* Continuous invariant: a frame the bitmap calls free must have no
     * references. If it does, the two structures disagree and handing this
     * frame out would give one frame two owners. */
    if (refcnt[cand] != 0)
        mm_bug("allocating a frame that still has references", cand);
    poison_check(cand);
    bm_set(cand);
    refcnt[cand] = 1;
    used_frames++;
    refs_total++;
    if (cand < PMM_LOW_LIMIT / FRAME_SIZE) low_zone_free--;
    else {
        high_allocs++;
        if (cand * FRAME_SIZE > high_max_phys) high_max_phys = cand * FRAME_SIZE;
    }
    watch_low();
    return cand * FRAME_SIZE;
}

/* The allocator proper. `allow_reserve` is what separates an ordinary request
 * from the swap-in fault's: see pmm.h on why the last few frames are kept back. */
static uint64_t scan_zone(uint64_t first, uint64_t last, uint64_t *hint)
{
    uint64_t start = *hint;
    if (start < first || start >= last) start = first;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t from = pass ? first : start;
        uint64_t to = pass ? start : last;
        for (uint64_t f = from; f < to; ) {
            uint64_t base = f & ~63ull;
            uint64_t word = *(uint64_t *)(bitmap + (base >> 3));
            unsigned skip = (unsigned)(f - base);
            if (skip) word |= (1ull << skip) - 1;
            if (word == UINT64_MAX) { f = base + 64; continue; }
            uint64_t cand = base + (uint64_t)__builtin_ctzll(~word);
            if (cand >= to) break;
            *hint = cand;  /* preserve immediate free/reuse, including poison checks */
            return take_frame(cand);
        }
    }
    *hint = first;
    return 0;
}

/* Reserve both a GLOBAL fault cushion and a LOW-zone cushion. Otherwise free
 * high pages would conceal low-zone exhaustion from old page-table/DMA users.
 * The explicit reserve APIs may use either cushion to complete a fault. */
static uint64_t alloc_locked(int allow_reserve, int any)
{
    if (!allow_reserve && total_frames - used_frames <= reserve_frames) return 0;
    uint64_t boundary = PMM_LOW_LIMIT / FRAME_SIZE;
    if (any && physmap_ready && total_frames > boundary) {
        uint64_t f = scan_zone(boundary, total_frames, &high_hint);
        if (f) return f;
    }
    if (!allow_reserve && low_zone_free <= reserve_frames) return 0;
    return scan_zone(0, total_frames < boundary ? total_frames : boundary, &alloc_hint);
}

/* THE PRESSURE POINT. Every frame in the system is handed out here, so this is
 * where "we are running low" is noticed and where reclaim is given the chance
 * to do something about it. The call is made BEFORE pmm_lock is taken -- a
 * reclaim pass calls pmm_free() and pmm_refcount(), which take that lock, and a
 * spinlock this kernel uses is not recursive. */
static uint64_t alloc_normal(int any)
{
    if (total_frames == 0)
        return 0;

    reclaim_on_alloc();

    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t ret = alloc_locked(0, any);
    if (!ret) alloc_fails++;
    spin_unlock_irqrestore(&pmm_lock, fl);
    /* OUTSIDE THE LOCK -- oom_alloc_fail() calls back into pmm_free_frames(),
     * which takes it, and this kernel's spinlocks are not recursive.
     *
     * It RECORDS and does not kill, which is the one asymmetry worth explaining
     * here rather than in oom.c: this function is the bottom of every allocation
     * in the kernel, including speculative ones that handle a 0 perfectly well
     * (vmm.c's next_table, reclaim's own probes) and ones made from interrupt
     * context or from the BKL-free syscall, where the reverse map may not be
     * walked at all. The two callers that MEAN it -- a user page fault and
     * kmalloc -- ask for a victim one layer up, where the failure is known to be
     * terminal. What this adds is the moment: `alloc_fails` was already counted
     * and mm_report() already printed the total, but nothing said a word at the
     * instant the machine first ran out, so the first refusal of a run was
     * invisible until somebody thought to ask for a report. */
    if (!ret && LOGIT_HAVE(oom_alloc_fail)) oom_alloc_fail();
    return ret;
}

uint64_t pmm_alloc(void)     { return alloc_normal(0); }
uint64_t pmm_alloc_any(void) { return alloc_normal(1); }
/* For bounded leaf-lock callers. Honor the normal reserve floor, but never
 * reclaim, wait for a device or call OOM policy while another spinlock is held. */
uint64_t pmm_alloc_any_nowait(void)
{
    uint64_t f=spin_lock_irqsave(&pmm_lock);
    uint64_t page=alloc_locked(0,1);
    if (!page) alloc_fails++;
    spin_unlock_irqrestore(&pmm_lock,f);
    return page;
}


/* The swap-in fault's allocation, and the only caller allowed past the reserve.
 * Deliberately does NOT trigger a reclaim pass: this is called from inside the
 * fault that reclaim's own eviction created, and the machine is better served
 * by finishing that fault than by starting another sweep underneath it. */
static uint64_t alloc_reserve(int any)
{
    if (total_frames == 0) return 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t before = total_frames - used_frames;
    uint64_t ret = alloc_locked(1, any);
    if (ret && before <= reserve_frames) reserve_hits++;
    if (!ret) alloc_fails++;
    spin_unlock_irqrestore(&pmm_lock, fl);
    return ret;
}

uint64_t pmm_alloc_reserve(void)     { return alloc_reserve(0); }
uint64_t pmm_alloc_reserve_any(void) { return alloc_reserve(1); }

void pmm_set_reserve(uint64_t frames)
{
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    reserve_frames = frames;
    spin_unlock_irqrestore(&pmm_lock, fl);
}


/* Scan complete aligned candidates under the PMM lock. A failed candidate
 * advances past its first occupied frame; no frame is claimed until the full
 * range is known free. Zone cuts avoid crossing the legacy alias boundary. */
static uint64_t masked_zone(uint64_t first, uint64_t last, uint64_t bytes,
                            uint64_t align, uint64_t boundary)
{
    uint64_t p = (first + align - 1) & ~(align - 1);
    while (p < last && bytes <= last - p) {
        if (boundary && (p & (boundary - 1)) > boundary - bytes) {
            uint64_t next = (p & ~(boundary - 1)) + boundary;
            p = (next + align - 1) & ~(align - 1);
            continue;
        }
        uint64_t f = p / FRAME_SIZE, end = f + bytes / FRAME_SIZE, busy = f;
        while (busy < end && !bm_test(busy)) busy++;
        if (busy == end) {
            for (; f < end; f++) take_frame(f);
            return p;
        }
        p = ((busy + 1) * FRAME_SIZE + align - 1) & ~(align - 1);
    }
    return 0;
}

uint64_t pmm_alloc_contig_masked(size_t pages, uint64_t mask,
                               size_t align, size_t boundary)
{
    if (!pages || pages > PHYSMAP_SIZE / FRAME_SIZE ||
        (mask != UINT64_MAX && (mask & (mask + 1))) ||
        (align && (align & (align - 1))) ||
        (boundary && (boundary & (boundary - 1)))) return 0;
    uint64_t bytes = (uint64_t)pages * FRAME_SIZE;
    uint64_t alignment = align < FRAME_SIZE ? FRAME_SIZE : align;
    if (alignment > PHYSMAP_SIZE || (boundary && bytes > boundary) ||
        bytes - 1 > mask) return 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t last = total_frames * FRAME_SIZE;
    if (mask < last) last = mask + 1;
    uint64_t ret = 0;
    const uint64_t far = 0x100000000ull;
    if (last > far) ret = masked_zone(far, last, bytes, alignment, boundary);
    if (!ret && last > PMM_LOW_LIMIT)
        ret = masked_zone(PMM_LOW_LIMIT, last < far ? last : far, bytes, alignment, boundary);
    if (!ret) ret = masked_zone(FRAME_SIZE, last < PMM_LOW_LIMIT ? last : PMM_LOW_LIMIT,
                                bytes, alignment, boundary);
    if (!ret) alloc_fails++;
    spin_unlock_irqrestore(&pmm_lock, fl);
    return ret;
}

uint64_t pmm_alloc_contig(size_t n)
{
    if (n == 0)
        return 0;
    uint64_t ret = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t run = 0, start = 0;
    for (uint64_t f = 0; f < total_frames && f < PMM_LOW_LIMIT / FRAME_SIZE; f++) {
        if (!bm_test(f)) {
            if (run == 0)
                start = f;
            if (++run == n) {
                for (uint64_t i = start; i < start + n; i++)
                    take_frame(i);
                ret = start * FRAME_SIZE;
                goto out;
            }
        } else {
            run = 0;
        }
    }
out:
    spin_unlock_irqrestore(&pmm_lock, fl);
    return ret;
}

void pmm_free(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (f >= total_frames) { spin_unlock_irqrestore(&pmm_lock, fl); return; }

    if (!bm_test(f) || refcnt[f] == 0) {
        /* Double free, or a free of a frame that was never allocated. Under the
         * old one-bit allocator this silently "worked" and handed the frame to
         * two owners; now it is refused and reported. */
        mm_bug("free of a frame that is not allocated (double free?)", f);
        spin_unlock_irqrestore(&pmm_lock, fl);
        return;
    }
    if (refcnt[f] == PMM_REF_MAX) {
        /* Saturated: this frame is pinned for the life of the boot. Dropping a
         * reference we can no longer count would eventually free a frame that
         * is still mapped, so we deliberately leak it instead. */
        spin_unlock_irqrestore(&pmm_lock, fl);
        return;
    }

    refcnt[f]--;
    refs_total--;
    if (refcnt[f] == 1)
        shared_frames--;            /* was 2, no longer shared */
    if (refcnt[f] == 0) {
        poison_fill(f);             /* before the bit clears: still exclusively ours */
        bm_clear(f);
        used_frames--;
        if (f < PMM_LOW_LIMIT / FRAME_SIZE) low_zone_free++;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
}

int pmm_ref(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    int ret = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (f >= total_frames) { ret = -1; goto out; }
    if (!bm_test(f) || refcnt[f] == 0) {
        mm_bug("reference taken on a frame that is not allocated", f);
        ret = -1;
        goto out;
    }
    if (refcnt[f] >= PMM_REF_MAX) {
        ret = -1;                   /* already pinned (counted once, below); caller must copy */
        goto out;
    }
    refcnt[f]++;
    refs_total++;
    if (refcnt[f] == 2)
        shared_frames++;
    if (refcnt[f] == PMM_REF_MAX) {   /* counted exactly once: on the transition */
        pinned_frames++;
        kprintf("[mm] frame %d refcount saturated at %d -- pinned for this boot\n",
                (int)f, (int)PMM_REF_MAX);
    }
out:
    spin_unlock_irqrestore(&pmm_lock, fl);
    return ret;
}

unsigned pmm_refcount(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    unsigned r = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (f < total_frames && refcnt)
        r = refcnt[f];
    spin_unlock_irqrestore(&pmm_lock, fl);
    return r;
}

/* --------------------------------------------------------------- pinning --
 * See pmm.h. A pin is checked by reclaim and by nothing else, so the cost of
 * being wrong in the safe direction (an extra pin) is one frame that will not
 * be evicted, and the cost of being wrong in the other direction is a page
 * evicted out from under a kernel pointer. Hence the saturating count and the
 * loud complaint on an unbalanced unpin. */
void pmm_pin(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (f < total_frames && pincnt) {
        if (pincnt[f] == 0) pins_live++;
        if (pincnt[f] < 255) pincnt[f]++;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
}

void pmm_unpin(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (f < total_frames && pincnt) {
        if (pincnt[f] == 0) {
            spin_unlock_irqrestore(&pmm_lock, fl);
            mm_bug("unpin of a frame that was not pinned", f);
            return;
        }
        if (pincnt[f] == 255) {
            /* Saturated: the count can no longer be trusted downwards, so the
             * pin is permanent. One frame, deliberately, rather than an
             * eviction while somebody still holds it. */
            spin_unlock_irqrestore(&pmm_lock, fl);
            return;
        }
        if (--pincnt[f] == 0) pins_live--;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
}

unsigned pmm_pincount(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    unsigned r = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (f < total_frames && pincnt) r = pincnt[f];
    spin_unlock_irqrestore(&pmm_lock, fl);
    return r;
}

void pmm_set_poison(int level)
{
    int want = (level < 0) ? 0 : (level > 2 ? 2 : level);
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    if (want != poison_level) {
        /* Every currently-free frame holds a pattern of the OLD extent. Under
         * the new one it would be compared against bytes nobody wrote, so drop
         * the validity map: those frames are simply "not poisoned" until they
         * are freed again. One 16 KiB memset on a call that happens a handful
         * of times in a boot, in exchange for the detector never lying. */
        poison_level = want;
        if (poison_bm && total_frames)
            memset(poison_bm, 0, (size_t)((((total_frames + 7) / 8) + 7) & ~(uint64_t)7));
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
}

int pmm_poison_level(void) { return poison_level; }

/* ------------------------------------------------------------ accounting -- */

uint64_t pmm_total_bytes(void) { return usable_bytes; }   /* set once at boot; no lock needed */

/* M25 P3: read the counters under pmm_lock -- they're written by the BKL-free
 * allocator path (kmalloc->grow->pmm_alloc_contig), so a BKL-covered reader
 * (sysinfo) is NOT serialized against them by the BKL. */
#define PMM_STAT(name, expr) \
    uint64_t name(void) { uint64_t fl = spin_lock_irqsave(&pmm_lock); \
                          uint64_t v = (expr); spin_unlock_irqrestore(&pmm_lock, fl); return v; }

PMM_STAT(pmm_free_bytes,     (total_frames - used_frames) * FRAME_SIZE)
PMM_STAT(pmm_total_frames,   total_frames)
PMM_STAT(pmm_used_frames,    used_frames)
PMM_STAT(pmm_free_frames,    total_frames - used_frames)
PMM_STAT(pmm_shared_frames,  shared_frames)
PMM_STAT(pmm_pinned_frames,  pinned_frames)
PMM_STAT(pmm_refs_total,     refs_total)
PMM_STAT(pmm_bugs,           bug_count)
PMM_STAT(pmm_pins_live,      pins_live)
PMM_STAT(pmm_reserve,        reserve_frames)
PMM_STAT(pmm_reserve_hits,   reserve_hits)
PMM_STAT(pmm_alloc_failures, alloc_fails)
PMM_STAT(pmm_high_allocations, high_allocs)
PMM_STAT(pmm_high_max_phys, high_max_phys)
PMM_STAT(pmm_high_free_frames, total_frames - used_frames - low_zone_free)
PMM_STAT(pmm_high_live_frames, high_available - (total_frames - used_frames - low_zone_free))
PMM_STAT(pmm_low_zone_free_frames, low_zone_free)
uint64_t pmm_physmap_pages(void)       { return physmap_pages; }
uint64_t pmm_physmap_table_pages(void) { return physmap_tables; }
int pmm_physmap_ready(void)           { return physmap_ready; }
int pmm_physmap_low_ready(void)       { return physmap_low_ready; }

int pmm_audit(void)
{
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t used = 0, shared = 0, pinned = 0, refs = 0, mismatch = 0, low_free_check = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        unsigned rc = refcnt[f];
        int bit = bm_test(f) ? 1 : 0;
        if ((rc >= 1) != (bit == 1)) {
            if (mismatch < 8)
                kprintf("[mm] AUDIT: frame %d bitmap=%d refcount=%d disagree\n",
                        (int)f, bit, (int)rc);
            mismatch++;
        }
        if (!rc && f < PMM_LOW_LIMIT / FRAME_SIZE) low_free_check++;
        if (rc >= 1) used++;
        if (rc >= 2) shared++;
        if (rc == PMM_REF_MAX) pinned++;
        refs += rc;
    }
    int errs = 0;
    if (low_free_check != low_zone_free) {
        kprintf("[mm] AUDIT: low-zone free count disagrees\n"); errs++;
    }
    if (mismatch) { kprintf("[mm] AUDIT: %d frames where the bitmap and the refcount table disagree\n",
                            (int)mismatch); errs++; }
    if (used != used_frames)     { kprintf("[mm] AUDIT: used %d, counter says %d\n",
                                           (int)used, (int)used_frames); errs++; }
    if (shared != shared_frames) { kprintf("[mm] AUDIT: shared %d, counter says %d\n",
                                           (int)shared, (int)shared_frames); errs++; }
    if (pinned != pinned_frames) { kprintf("[mm] AUDIT: pinned %d, counter says %d\n",
                                           (int)pinned, (int)pinned_frames); errs++; }
    if (refs != refs_total)      { kprintf("[mm] AUDIT: refs %d, counter says %d\n",
                                           (int)refs, (int)refs_total); errs++; }
    spin_unlock_irqrestore(&pmm_lock, fl);
    return errs;
}

/* One line, the way the trust store prints "130 roots, 0 skipped". Every number
 * a memory bug is diagnosed with, in the order you need them:
 *   total   frames of usable RAM
 *   free    frames nobody references
 *   used    frames with >= 1 reference
 *   shared  used frames with >= 2 references (i.e. copy-on-write savings)
 *   refs    sum of all references; refs - used is how many mappings the shared
 *           frames saved, so (refs - used) * 4 KiB is memory NOT copied
 *   pinned  frames whose count saturated and can never be freed (a known leak)
 *   bugs    invariant violations detected since boot; must stay 0 */
void pmm_report(const char *tag)
{
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t total = total_frames, used = used_frames, shared = shared_frames;
    uint64_t refs = refs_total, pinned = pinned_frames, bugs = bug_count;
    uint64_t meta = mm_meta_bytes;
    int plevel = poison_level;
    spin_unlock_irqrestore(&pmm_lock, fl);

    kprintf("[mm] %s: %d frames total (%d MiB), %d free, %d used, %d shared, "
            "%d refs (+%d saved = %d KiB), %d pinned, %d bugs\n",
            tag ? tag : "-", (int)total, (int)(total * FRAME_SIZE / (1024 * 1024)),
            (int)(total - used), (int)used, (int)shared,
            (int)refs, (int)(refs - used), (int)((refs - used) * FRAME_SIZE / 1024),
            (int)pinned, (int)bugs);
    kprintf("[physmap] %s: high_allocs=%llu high_live=%llu high_max=%p high_free=%llu low_free=%llu\n",
            tag ? tag : "-", (unsigned long long)pmm_high_allocations(),
            (unsigned long long)pmm_high_live_frames(), (void *)pmm_high_max_phys(),
            (unsigned long long)pmm_high_free_frames(), (unsigned long long)pmm_low_zone_free_frames());
    kprintf("[mm] %s: metadata %d KiB (bitmap + poison map + %d-byte refcounts + pins), poison level %d\n",
            tag ? tag : "-", (int)(meta / 1024), (int)sizeof(uint16_t), plevel);
    kprintf("[mm] %s: %d frames pinned against reclaim, %d reserve frames "
            "(%d dips), %d allocations refused\n",
            tag ? tag : "-", (int)pmm_pins_live(), (int)pmm_reserve(),
            (int)pmm_reserve_hits(), (int)pmm_alloc_failures());
}
