/* SPDX-License-Identifier: MIT */
/* Precise, nonmoving tracing heap. See native.h for the safepoint contract. */
#include "native.h"
#include <stdlib.h>

typedef struct AtAllocation {
    struct AtAllocation *next;
    struct AtAllocation *grey;
    size_t bytes;
    AtScan scan;
    int marked;

    /* max_align_t is absent in the small guest libc. A union gives the payload
     * the strongest alignment required by the language's present scalar types. */
    union {
        uint64_t integer;
        double real;
        void *pointer;
    } alignment;

    unsigned char data[];
} AtAllocation;

static AtRoot *roots;
static AtAllocation *allocations;
static AtAllocation *grey;
static size_t live_bytes;
static int64_t live_objects;
static size_t collect_at = 65536;

/* Allocations sorted by payload address, rebuilt once per collection so that
 * at_gc_mark() can binary-search instead of walking the allocation list.
 *
 * WHY THIS EXISTS, measured rather than assumed. at_gc_mark() has to resolve an
 * arbitrary address to the allocation CONTAINING it, because a substring points
 * inside its parent's text. The list walk that did that was O(live) per mark and
 * a collection marks every live object, so one collection cost O(live^2) and a
 * program's total cost grew quadratically in the objects it kept. Measured on
 * the host, 20,000 eight-byte strings: 1.3 ms when each is dropped immediately
 * (one object live) against 531 ms when they are appended to a list, and the
 * time rose ~4x for each doubling of N. The allocation path was never the cost;
 * retention was.
 *
 * The index is only valid during a collection: the sweep frees objects and the
 * mutator adds them, and both happen after the mark phase is finished with it. */
static AtAllocation **address_index;
static size_t address_index_count;
static size_t address_index_capacity;

void *at_gc_frame(void)
{
    return roots;
}

void at_gc_root(AtRoot *root, void *slot, AtScan scan)
{
    *root = (AtRoot){roots, slot, scan};
    roots = root;
}

void at_gc_restore(void *frame)
{
    /* Restoring is allocation-free. A returned value is rooted by its caller
     * before the next safepoint; collecting in this gap would lose the value. */
    roots = frame;
}

static int compare_by_address(const void *left, const void *right)
{
    uintptr_t a = (uintptr_t)(*(AtAllocation *const *)left)->data;
    uintptr_t b = (uintptr_t)(*(AtAllocation *const *)right)->data;
    return a < b ? -1 : a > b ? 1 : 0;
}

/* Returns 0 if the index could not be built; the mark path then falls back to
 * the list walk, which is slow but correct. A collection that cannot allocate
 * must still collect -- refusing here would turn memory pressure into a hang. */
static int build_address_index(void)
{
    size_t needed = (size_t)(live_objects > 0 ? live_objects : 0);
    if (needed > address_index_capacity) {
        size_t capacity = address_index_capacity ? address_index_capacity : 256;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                return 0;
            }
            capacity *= 2;
        }
        AtAllocation **grown = realloc(address_index, capacity * sizeof *grown);
        if (!grown) {
            return 0;
        }
        address_index = grown;
        address_index_capacity = capacity;
    }
    address_index_count = 0;
    for (AtAllocation *object = allocations; object; object = object->next) {
        if (address_index_count >= address_index_capacity) {
            return 0;                       /* live_objects disagreed with the list */
        }
        address_index[address_index_count++] = object;
    }
    qsort(address_index, address_index_count, sizeof *address_index, compare_by_address);
    return 1;
}

static void mark_object(AtAllocation *object)
{
    if (!object->marked) {
        object->marked = 1;
        object->grey = grey;
        grey = object;
    }
}

void at_gc_mark(void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;

    /* Substrings can point inside a text allocation, so an address has to be
     * resolved to the allocation that CONTAINS it rather than compared for
     * equality. Only generated text scanners supply such pointers; integers are
     * never treated as roots. */
    if (address_index_count) {
        /* Last allocation whose payload starts at or below the address. */
        size_t low = 0, high = address_index_count;
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if ((uintptr_t)address_index[middle]->data <= address) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low == 0) {
            return;
        }
        AtAllocation *object = address_index[low - 1];
        uintptr_t start = (uintptr_t)object->data;
        if (address - start < object->bytes) {
            mark_object(object);
        }
        return;
    }

    for (AtAllocation *object = allocations; object; object = object->next) {
        uintptr_t start = (uintptr_t)object->data;
        if (address >= start && address - start < object->bytes) {
            mark_object(object);
            return;
        }
    }
}

void at_gc_collect(void)
{
    /* Built before the first mark and torn down after the last one. Leaving it
     * live across the sweep would hand at_gc_mark() pointers to freed objects. */
    build_address_index();

    for (AtRoot *root = roots; root; root = root->previous) {
        root->scan(root->slot);
    }
    /* Pending exceptions outlive a callee's frame during explicit propagation. */
    at_exception_mark();
    while (grey) {
        AtAllocation *object = grey;
        grey = object->grey;
        if (object->scan) {
            object->scan(object->data);
        }
    }
    /* The sweep below frees objects, so the index must stop being consulted
     * before it starts. Emptying it also makes at_gc_mark() fall back to the
     * list walk if anything ever marks outside a collection. */
    address_index_count = 0;

    AtAllocation **link = &allocations;
    while (*link) {
        AtAllocation *object = *link;
        if (object->marked) {
            object->marked = 0;
            link = &object->next;
        } else {
            *link = object->next;
            live_bytes -= object->bytes;
            live_objects--;
            free(object);
        }
    }
    collect_at = live_bytes > (SIZE_MAX - 65536) / 2 ? SIZE_MAX : live_bytes * 2 + 65536;
}

int64_t at_gc_live_bytes(void)
{
    return (int64_t)live_bytes;
}

int64_t at_gc_live_objects(void)
{
    return live_objects;
}

int64_t at_gc_reclaim(void)
{
    /* Count allocations, not payload bytes. Native container backing objects
     * are independently traced, so exact counts need not match the old VM's
     * representation; the returned delta must match gc_stats before/after. */
    int64_t before = live_objects;
    at_gc_collect();
    return before - live_objects;
}

void *at_gc_allocate(size_t bytes, AtScan scan)
{
    if (bytes > SIZE_MAX - sizeof(AtAllocation) || bytes > INT64_MAX - live_bytes ||
        live_objects == INT64_MAX) {
        return NULL;
    }
    if (live_bytes + bytes >= collect_at) {
        at_gc_collect();
    }
    AtAllocation *object = calloc(1, sizeof(*object) + bytes);
    if (!object) {
        return NULL;
    }
    object->next = allocations;
    object->bytes = bytes;
    object->scan = scan;
    allocations = object;
    live_bytes += bytes;
    live_objects++;
    return object->data;
}
