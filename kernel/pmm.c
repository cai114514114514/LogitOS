#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

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

    /* Park the bitmap immediately after the kernel image. */
    bitmap = (uint8_t *)(((uint64_t)_kernel_end + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1));
    uint64_t bitmap_bytes = (total_frames + 7) / 8;

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

uint64_t pmm_alloc(void)
{
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bm_test(f)) {
            bm_set(f);
            used_frames++;
            return f * FRAME_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_contig(size_t n)
{
    if (n == 0)
        return 0;
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
                return start * FRAME_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free(uint64_t phys_addr)
{
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f < total_frames && bm_test(f)) {
        bm_clear(f);
        used_frames--;
    }
}

uint64_t pmm_total_bytes(void) { return usable_bytes; }
uint64_t pmm_free_bytes(void)  { return (total_frames - used_frames) * FRAME_SIZE; }
