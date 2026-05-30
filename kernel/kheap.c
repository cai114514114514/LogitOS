#include <stdint.h>
#include <stddef.h>
#include "kheap.h"
#include "pmm.h"

/* A small allocator: bump-allocate within a contiguous arena of physical
 * frames, and keep a first-fit free list of returned blocks for reuse. No
 * coalescing — adequate for kernel bookkeeping and easy to reason about. */

#define ALIGN16(x)    (((x) + 15) & ~((size_t)15))
#define ARENA_FRAMES  1024                 /* 4 MiB per arena */

struct header {
    size_t size;            /* payload size */
    struct header *next;    /* only meaningful while on the free list */
};

static struct header *free_list = NULL;
static uint8_t *brk = NULL;
static size_t   brk_left = 0;

static int grow(size_t need)
{
    size_t frames = ARENA_FRAMES;
    while (frames * FRAME_SIZE < need)
        frames *= 2;

    uint64_t phys = pmm_alloc_contig(frames);
    if (!phys)
        return 0;

    brk = (uint8_t *)phys;                 /* identity-mapped */
    brk_left = frames * FRAME_SIZE;
    return 1;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    size = ALIGN16(size);

    /* Reuse a freed block if one is big enough. */
    for (struct header **pp = &free_list; *pp; pp = &(*pp)->next) {
        if ((*pp)->size >= size) {
            struct header *b = *pp;
            *pp = b->next;
            return (void *)(b + 1);
        }
    }

    /* Otherwise bump-allocate, growing the arena if needed. */
    size_t total = sizeof(struct header) + size;
    if (brk_left < total && !grow(total))
        return NULL;

    struct header *h = (struct header *)brk;
    h->size = size;
    h->next = NULL;
    brk += total;
    brk_left -= total;
    return (void *)(h + 1);
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct header *h = (struct header *)ptr - 1;
    h->next = free_list;
    free_list = h;
}
