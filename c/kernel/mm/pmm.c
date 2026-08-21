#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "rmap.h"
#include "reclaim.h"
#include "mmhost.h"
#include "spinlock.h"
#include "kprintf.h"

/* The out-of-memory recorder (c/kernel/mm/oom.h). Weak for the reason given at
 * the same declaration in fault.c: this file is compiled by host harnesses with
 * no process table behind them. */
void oom_alloc_fail(void) __attribute__((weak));

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
 * (mm_p2v; see mmhost.h for why that is a function and not a cast). */

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
static uint64_t alloc_hint;      /* frame to resume scanning from */
static uint64_t bug_count;
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
        if (!bm_test(f)) { bm_set(f); used_frames++; }
        if (refcnt[f] == 0) { refcnt[f] = 1; refs_total++; }
        pz_clear(f);
    }
}

static void release(uint64_t base, uint64_t len)
{
    uint64_t start = (base + FRAME_SIZE - 1) / FRAME_SIZE;  /* round up  */
    uint64_t end   = (base + len) / FRAME_SIZE;             /* round down */
    for (uint64_t f = start; f < end && f < total_frames; f++) {
        if (bm_test(f)) { bm_clear(f); used_frames--; }
        if (refcnt[f]) { refs_total -= refcnt[f]; refcnt[f] = 0; }
        pz_clear(f);
    }
}

void pmm_init(uint64_t mb_info_addr)
{
    uint32_t total_size = *(volatile uint32_t *)mm_p2v(mb_info_addr);
    uint8_t *p = (uint8_t *)mm_p2v(mb_info_addr + 8);   /* skip total_size + reserved */
    uint8_t *end = (uint8_t *)mm_p2v(mb_info_addr + total_size);

    struct mb2_tag_mmap *mmap = NULL;
    uint64_t highest = 0;

    /* First pass: find the memory map tag and the highest usable address. */
    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0 || tag->size == 0)
            break;                                   /* end tag (size 0 would stall the walk) */
        if (tag->type == MB2_TAG_MMAP) {
            mmap = (struct mb2_tag_mmap *)tag;
            if (mmap->entry_size >= sizeof(struct mb2_mmap_entry)) {   /* entry_size 0 would stall the walk */
                uint8_t *e = (uint8_t *)mmap->entries;
                uint8_t *mend = p + mmap->size;
                for (; e < mend; e += mmap->entry_size) {
                    struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
                    if (me->type == MMAP_AVAILABLE && me->addr + me->len > highest)
                        highest = me->addr + me->len;
                }
            }
        }
        p += (tag->size + 7) & ~7u;                  /* tags are 8-byte aligned */
    }

    total_frames = highest / FRAME_SIZE;

    /* Park the metadata immediately after the kernel image: the allocation
     * bitmap, the poison-validity bitmap, then the refcount table. The two
     * bitmaps' byte counts are rounded up to an 8-byte multiple so pmm_alloc's
     * word-at-a-time scan can always read a full uint64_t without running past
     * the bitmap; the extra padding bits cover frames beyond total_frames and
     * stay marked used. */
    uint64_t base = (mm_kernel_end_phys() + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1);
    uint64_t bm_bytes = (((total_frames + 7) / 8) + 7) & ~(uint64_t)7;
    uint64_t rc_bytes = total_frames * sizeof(uint16_t);
    uint64_t pin_bytes = (total_frames + 7) & ~(uint64_t)7;   /* one byte per frame */

    bitmap    = (uint8_t *)mm_p2v(base);
    poison_bm = (uint8_t *)mm_p2v(base + bm_bytes);
    refcnt    = (uint16_t *)mm_p2v(base + bm_bytes * 2);
    pincnt    = (uint8_t *)mm_p2v(base + bm_bytes * 2 + rc_bytes);
    mm_meta_bytes = bm_bytes * 2 + rc_bytes + pin_bytes;

    memset(bitmap, 0xFF, bm_bytes);                  /* everything used... */
    memset(poison_bm, 0, bm_bytes);
    memset(pincnt, 0, pin_bytes);
    used_frames = total_frames;
    shared_frames = pinned_frames = 0;
    refs_total = total_frames;
    for (uint64_t f = 0; f < total_frames; f++)      /* ...with exactly one reference each */
        refcnt[f] = 1;

    /* ...then free what firmware says is available. */
    if (mmap && mmap->entry_size >= sizeof(struct mb2_mmap_entry)) {
        uint8_t *e = (uint8_t *)mmap->entries;
        uint8_t *mend = (uint8_t *)mmap + mmap->size;
        for (; e < mend; e += mmap->entry_size) {
            struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
            if (me->type == MMAP_AVAILABLE) {
                release(me->addr, me->len);
                usable_bytes += me->len;
            }
        }
    }

    /* Re-reserve low memory + kernel + the metadata itself, and the info block. */
    reserve(0, base + mm_meta_bytes);
    reserve(mb_info_addr, total_size);

    pmm_report("boot");

    /* The reverse map's tables come out of the pool that was just built, while
     * it is empty and a contiguous run of a few hundred frames is certain to be
     * there. It has to be here rather than later for a reason that is easy to
     * get wrong: reclaim must work when memory is exhausted, so every structure
     * it needs has to exist before anything can exhaust it. */
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
    watch_low();
    return cand * FRAME_SIZE;
}

/* The allocator proper. `allow_reserve` is what separates an ordinary request
 * from the swap-in fault's: see pmm.h on why the last few frames are kept back. */
static uint64_t alloc_locked(int allow_reserve)
{
    uint64_t ret = 0;

    if (!allow_reserve && total_frames - used_frames <= reserve_frames)
        return 0;                     /* the reserve is not for ordinary callers */

    uint64_t start = alloc_hint;
    if (start >= total_frames)
        start = 0;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t from = (pass == 0) ? start : 0;
        uint64_t to   = (pass == 0) ? total_frames : start;

        /* Walk word-aligned 64-frame blocks within [from, to). */
        for (uint64_t f = from; f < to; ) {
            uint64_t byte_idx = f >> 3;
            uint64_t word = *(uint64_t *)(bitmap + (byte_idx & ~(uint64_t)7));
            uint64_t base = (byte_idx & ~(uint64_t)7) << 3;   /* first frame in word */

            if (word == 0xFFFFFFFFFFFFFFFFull) {
                /* All 64 frames used: jump to the next aligned word. */
                f = base + 64;
                continue;
            }

            /* At least one free bit; find the first free frame >= f. */
            uint64_t bit = (uint64_t)__builtin_ctzll(~word);
            uint64_t cand = base + bit;
            while (cand < f) {
                /* First free bit is before our scan start: mask it out. */
                word |= (1ull << (cand - base));
                if (word == 0xFFFFFFFFFFFFFFFFull)
                    break;
                bit = (uint64_t)__builtin_ctzll(~word);
                cand = base + bit;
            }
            if (word == 0xFFFFFFFFFFFFFFFFull) {
                f = base + 64;
                continue;
            }
            if (cand >= to) {
                f = base + 64;
                continue;
            }
            alloc_hint = cand;
            ret = take_frame(cand);
            return ret;
        }
    }

    alloc_hint = 0;     /* wrap-around for the next attempt */
    return ret;
}

/* THE PRESSURE POINT. Every frame in the system is handed out here, so this is
 * where "we are running low" is noticed and where reclaim is given the chance
 * to do something about it. The call is made BEFORE pmm_lock is taken -- a
 * reclaim pass calls pmm_free() and pmm_refcount(), which take that lock, and a
 * spinlock this kernel uses is not recursive. */
uint64_t pmm_alloc(void)
{
    if (total_frames == 0)
        return 0;

    reclaim_on_alloc();

    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t ret = alloc_locked(0);
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
    if (!ret && oom_alloc_fail) oom_alloc_fail();
    return ret;
}

/* The swap-in fault's allocation, and the only caller allowed past the reserve.
 * Deliberately does NOT trigger a reclaim pass: this is called from inside the
 * fault that reclaim's own eviction created, and the machine is better served
 * by finishing that fault than by starting another sweep underneath it. */
uint64_t pmm_alloc_reserve(void)
{
    if (total_frames == 0) return 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t before = total_frames - used_frames;
    uint64_t ret = alloc_locked(1);
    if (ret && before <= reserve_frames) reserve_hits++;
    if (!ret) alloc_fails++;
    spin_unlock_irqrestore(&pmm_lock, fl);
    return ret;
}

void pmm_set_reserve(uint64_t frames)
{
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    reserve_frames = frames;
    spin_unlock_irqrestore(&pmm_lock, fl);
}

uint64_t pmm_alloc_contig(size_t n)
{
    if (n == 0)
        return 0;
    uint64_t ret = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t run = 0, start = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
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

int pmm_audit(void)
{
    uint64_t fl = spin_lock_irqsave(&pmm_lock);
    uint64_t used = 0, shared = 0, pinned = 0, refs = 0, mismatch = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        unsigned rc = refcnt[f];
        int bit = bm_test(f) ? 1 : 0;
        if ((rc >= 1) != (bit == 1)) {
            if (mismatch < 8)
                kprintf("[mm] AUDIT: frame %d bitmap=%d refcount=%d disagree\n",
                        (int)f, bit, (int)rc);
            mismatch++;
        }
        if (rc >= 1) used++;
        if (rc >= 2) shared++;
        if (rc == PMM_REF_MAX) pinned++;
        refs += rc;
    }
    int errs = 0;
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
    kprintf("[mm] %s: metadata %d KiB (bitmap + poison map + %d-byte refcounts + pins), poison level %d\n",
            tag ? tag : "-", (int)(meta / 1024), (int)sizeof(uint16_t), plevel);
    kprintf("[mm] %s: %d frames pinned against reclaim, %d reserve frames "
            "(%d dips), %d allocations refused\n",
            tag ? tag : "-", (int)pmm_pins_live(), (int)pmm_reserve(),
            (int)pmm_reserve_hits(), (int)pmm_alloc_failures());
}
