#include <stddef.h>

/* A correct, compact allocator over a static arena: 16-byte-aligned blocks each
 * prefixed by a 16-byte header {size, used}. first-fit; free() just marks; the
 * malloc scan coalesces physically-adjacent free blocks (no footer needed). */

#define ARENA_SIZE (24u * 1024u * 1024u)   /* 24 MiB JS heap */
#define HDR 16u

static unsigned char arena[ARENA_SIZE] __attribute__((aligned(16)));
static int inited;

struct hdr { size_t size; size_t used; };   /* 16 bytes, keeps payload 16-aligned */

static void heap_init(void)
{
    struct hdr *h = (struct hdr *)arena;
    h->size = ARENA_SIZE - 2 * HDR; /* one big free block, ending AT the sentinel */
    h->used = 0;
    struct hdr *end = (struct hdr *)(arena + ARENA_SIZE - HDR);
    end->size = 0;                  /* sentinel: size 0 marks the end */
    end->used = 1;
    inited = 1;
}

static size_t align16(size_t n) { return (n + 15u) & ~(size_t)15u; }

void *malloc(size_t n)
{
    if (!inited) heap_init();
    if (n == 0) n = 1;
    size_t need = align16(n);
    unsigned char *aend = arena + ARENA_SIZE;

    for (struct hdr *h = (struct hdr *)arena; h->size; ) {
        /* Defensive: a sane block's `next` stays inside the arena. If a header
         * was corrupted (e.g. clobbered), bail to NULL rather than walking off
         * into an unmapped page and faulting -- an allocator must never fault. */
        unsigned char *np = (unsigned char *)h + HDR + h->size;
        if (np <= (unsigned char *)h || np + HDR > aend) return NULL;
        struct hdr *next = (struct hdr *)np;
        if (!h->used) {
            /* coalesce following free blocks */
            while ((unsigned char *)next < aend && next->size && !next->used) {
                unsigned char *nn = (unsigned char *)next + HDR + next->size;
                if (nn <= (unsigned char *)next || nn > aend) break;
                h->size += HDR + next->size;
                next = (struct hdr *)((unsigned char *)h + HDR + h->size);
            }
            if (h->size >= need) {
                if (h->size >= need + HDR + 16u) {      /* split */
                    struct hdr *nb = (struct hdr *)((unsigned char *)h + HDR + need);
                    nb->size = h->size - need - HDR;
                    nb->used = 0;
                    h->size = need;
                }
                h->used = 1;
                return (unsigned char *)h + HDR;
            }
        }
        h = next;
    }
    return NULL;     /* out of arena */
}

void free(void *p)
{
    if (!p) return;
    if ((unsigned char *)p < arena + HDR || (unsigned char *)p >= arena + ARENA_SIZE)
        return;                     /* not ours: never scribble outside the arena */
    struct hdr *h = (struct hdr *)((unsigned char *)p - HDR);
    h->used = 0;
}

size_t malloc_usable_size(void *p)
{
    if (!p) return 0;
    if ((unsigned char *)p < arena + HDR || (unsigned char *)p >= arena + ARENA_SIZE)
        return 0;                   /* not ours */
    struct hdr *h = (struct hdr *)((unsigned char *)p - HDR);
    return h->size;
}

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    struct hdr *h = (struct hdr *)((unsigned char *)p - HDR);
    if (h->size >= align16(n)) return p;               /* shrink/fits in place */
    void *np = malloc(n);
    if (np) { memcpy(np, p, h->size); free(p); }
    return np;
}

void *calloc(size_t a, size_t b)
{
    size_t n = a * b;
    if (a && n / a != b) return NULL;                  /* overflow */
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
