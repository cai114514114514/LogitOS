#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "spinlock.h"

/* M25 P1/P2: the physical frame allocator is peeled out from under the BKL --
 * pmm_alloc/free/alloc_contig take their own lock so BKL-free paths on other
 * cores can allocate concurrently. irqsave: pmm is reachable from fault/IRQ
 * context (page-table fills) and must not be preempted mid-bitmap-scan. Lock
 * order: kheap_lock -> pmm_lock (grow() calls pmm_alloc_contig under kheap_lock);
 * pmm never takes kheap_lock, so the order never reverses. */
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* A bitmap physical frame allocator (1 bit per 4 KiB frame, 1 = used).
 * All usable RAM in our QEMU config sits below the identity-mapped first
 * 1 GiB, so a physical address can be used directly as a virtual address. */

extern char _kernel_end[];      /* provided by linker.ld */

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

static uint8_t *bitmap;
static uint64_t total_frames;
static uint64_t used_frames;
static uint64_t usable_bytes;
static uint64_t alloc_hint;      /* frame to resume scanning from */

static inline void bm_set(uint64_t f)   { bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void bm_clear(uint64_t f) { bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline int  bm_test(uint64_t f)  { return bitmap[f >> 3] & (1u << (f & 7)); }

static void reserve(uint64_t base, uint64_t len)
{
    uint64_t start = base / FRAME_SIZE;
    uint64_t end   = (base + len + FRAME_SIZE - 1) / FRAME_SIZE;
    for (uint64_t f = start; f < end && f < total_frames; f++)
        if (!bm_test(f)) { bm_set(f); used_frames++; }
}

static void release(uint64_t base, uint64_t len)
{
    uint64_t start = (base + FRAME_SIZE - 1) / FRAME_SIZE;  /* round up  */
    uint64_t end   = (base + len) / FRAME_SIZE;             /* round down */
    for (uint64_t f = start; f < end && f < total_frames; f++)
        if (bm_test(f)) { bm_clear(f); used_frames--; }
}

void pmm_init(uint64_t mb_info_addr)
{
    uint32_t total_size = *(volatile uint32_t *)mb_info_addr;
    uint8_t *p = (uint8_t *)(mb_info_addr + 8);     /* skip total_size + reserved */
    uint8_t *end = (uint8_t *)(mb_info_addr + total_size);

    struct mb2_tag_mmap *mmap = NULL;
    uint64_t highest = 0;

    /* First pass: find the memory map tag and the highest usable address. */
    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0)
            break;                                   /* end tag */
        if (tag->type == MB2_TAG_MMAP) {
            mmap = (struct mb2_tag_mmap *)tag;
            uint8_t *e = (uint8_t *)mmap->entries;
            uint8_t *mend = p + mmap->size;
            for (; e < mend; e += mmap->entry_size) {
                struct mb2_mmap_entry *me = (struct mb2_mmap_entry *)e;
                if (me->type == MMAP_AVAILABLE && me->addr + me->len > highest)
                    highest = me->addr + me->len;
            }
        }
        p += (tag->size + 7) & ~7u;                  /* tags are 8-byte aligned */
    }

    total_frames = highest / FRAME_SIZE;

    /* Park the bitmap immediately after the kernel image.  Round the byte
     * count up to an 8-byte multiple so pmm_alloc's word-at-a-time scan can
     * always read a full uint64_t without running past the bitmap; the extra
     * padding bits cover frames beyond total_frames and stay marked used. */
    bitmap = (uint8_t *)(((uint64_t)_kernel_end + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1));
    uint64_t bitmap_bytes = (((total_frames + 7) / 8) + 7) & ~(uint64_t)7;

    memset(bitmap, 0xFF, bitmap_bytes);              /* everything used... */
    used_frames = total_frames;

    /* ...then free what firmware says is available. */
    if (mmap) {
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

    /* Re-reserve low memory + kernel + the bitmap itself, and the info block. */
    reserve(0, (uint64_t)bitmap + bitmap_bytes);
    reserve(mb_info_addr, total_size);
}

/* Scan the bitmap a 64-bit word at a time (Linux find_next_zero_bit style):
 * a fully-used word (all ones) is skipped with one branch, and the first free
 * bit inside a partial word is located with __builtin_ctzll(~word).  Resumes
 * from alloc_hint so successive allocs don't rescan exhausted low frames, and
 * wraps back to frame 0 once before giving up so no free frame is missed. */
uint64_t pmm_alloc(void)
{
    if (total_frames == 0)
        return 0;

    uint64_t ret = 0;
    uint64_t fl = spin_lock_irqsave(&pmm_lock);

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
            bm_set(cand);
            used_frames++;
            alloc_hint = cand;
            ret = cand * FRAME_SIZE;
            goto out;
        }
    }

    alloc_hint = 0;     /* wrap-around for the next attempt */
out:
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
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bm_test(f)) {
            if (run == 0)
                start = f;
            if (++run == n) {
                for (uint64_t i = start; i < start + n; i++) {
                    bm_set(i);
                    used_frames++;
                }
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
    if (f < total_frames && bm_test(f)) {
        bm_clear(f);
        used_frames--;
    }
    spin_unlock_irqrestore(&pmm_lock, fl);
}

uint64_t pmm_total_bytes(void) { return usable_bytes; }
uint64_t pmm_free_bytes(void)  { return (total_frames - used_frames) * FRAME_SIZE; }
