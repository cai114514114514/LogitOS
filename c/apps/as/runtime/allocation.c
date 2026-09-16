/* SPDX-License-Identifier: MIT */
#include "allocation.h"
#include "capability.h"
#include "exception.h"
#include <stddef.h>
#include <stdlib.h>

typedef struct AtManualAllocation {
    struct AtManualAllocation *next;

    /* Ptr currently admits integer scalars only. Match their natural alignment
     * on supported targets without depending on max_align_t in guest libc. */
    union {
        uint64_t integer;
        double real;
        void *pointer;
    } alignment;

    unsigned char data[];
} AtManualAllocation;

static AtManualAllocation *allocations;

int at_manual_alloc(uint64_t *out, int64_t bytes)
{
    *out = 0;
    if (!at_caps_have(AS_CAP_RAW)) {
        return AT_E_PERMISSION;
    }
    if (bytes <= 0) {
        return AT_E_VALUE;
    }
    if ((uint64_t)bytes > SIZE_MAX - sizeof(AtManualAllocation)) {
        return AT_E_MEMORY;
    }

    /* One allocation means failure cannot leave a payload without its registry
     * entry. calloc also preserves alloc's zero-initialized byte contract. This
     * is deliberately not at_gc_allocate: collection cannot release manual
     * memory, and raw pointers do not keep managed objects alive. */
    AtManualAllocation *allocation = calloc(1, sizeof *allocation + (size_t)bytes);
    if (!allocation) {
        return AT_E_MEMORY;
    }
    allocation->next = allocations;
    allocations = allocation;
    *out = (uint64_t)(uintptr_t)allocation->data;
    return 0;
}

int at_manual_dealloc(uint64_t address)
{
    if (!at_caps_have(AS_CAP_RAW)) {
        return AT_E_PERMISSION;
    }

    /* Search our entries, never a header inferred from the supplied address.
     * Buffer storage, an interior pointer, and an already removed allocation
     * must fail without passing an unowned address to libc free. A raw pointer
     * cannot distinguish a stale alias from a later allocation at the same
     * address: lifetime correctness still belongs to the unsafe caller. */
    AtManualAllocation **link = &allocations;
    while (*link) {
        AtManualAllocation *allocation = *link;
        if ((uint64_t)(uintptr_t)allocation->data == address) {
            *link = allocation->next;
            free(allocation);
            return 0;
        }
        link = &allocation->next;
    }
    return AT_E_VALUE;
}
