/* SPDX-License-Identifier: MIT */
#include "region_internal.h"
#include "buffer.h"
#include <stddef.h>
#include <stdlib.h>

int at_region_index(int64_t *out, int64_t length, int64_t index)
{
    if (index < 0) {
        index += length;
    }
    if (index < 0 || index >= length) {
        return AT_E_INDEX;
    }
    *out = index;
    return 0;
}

int at_region_range(int64_t length, int64_t start, int64_t stop)
{
    return start < 0 || stop < start || stop > length ? AT_E_INDEX : 0;
}

int at_region_new(AtRegion **out, int64_t length)
{
    if (*out) {
        return AT_E_RUNTIME;
    }
    if (length < 0 || length > AT_BUFFER_LIMIT) {
        return AT_E_VALUE;
    }
    if ((uint64_t)length > SIZE_MAX - sizeof(AtRegion)) {
        return AT_E_MEMORY;
    }

    /* Keep the former region/buffer zero-filled byte contract and size ceiling,
     * but make the allocation scope-owned. It must never enter the tracing
     * heap: a borrow is a lifetime obligation, not a guessed GC root. */
    AtRegion *owner = calloc(1, sizeof *owner + (size_t)length);
    if (!owner) {
        return AT_E_MEMORY;
    }
    owner->length = length;
    *out = owner;
    return 0;
}

int at_region_move(AtRegion **destination, AtRegion **source)
{
    if (destination == source || *destination || !*source) {
        return AT_E_RUNTIME;
    }
    AtRegion *owner = *source;
    if (owner->readers || owner->writer) {
        return AT_E_RUNTIME;
    }
    /* Consuming the source slot also makes its eventual scope cleanup a no-op.
     * There is no allocation or GC safepoint between validation and transfer. */
    *destination = owner;
    *source = NULL;
    return 0;
}

int at_region_release(AtRegion **slot)
{
    AtRegion *owner = *slot;
    if (!owner) {
        return 0;
    }
    if (owner->readers || owner->writer) {
        return AT_E_RUNTIME;
    }
    *slot = NULL;
    free(owner);
    return 0;
}

int at_region_length(int64_t *out, const AtRegion *owner)
{
    *out = 0;
    if (!owner) {
        return AT_E_RUNTIME;
    }
    *out = owner->length;
    return 0;
}

int at_region_read(int64_t *out, const AtRegion *owner, int64_t index)
{
    *out = 0;
    if (!owner || owner->writer) {
        return AT_E_RUNTIME;
    }
    int status = at_region_index(&index, owner->length, index);
    if (status) {
        return status;
    }
    *out = owner->data[index];
    return 0;
}

int at_region_address(unsigned char **out, AtRegion *owner, int64_t index, int32_t writable)
{
    *out = NULL;
    if (!owner || owner->writer || (writable && owner->readers)) {
        return AT_E_RUNTIME;
    }
    int status = at_region_index(&index, owner->length, index);
    if (status) {
        return status;
    }
    *out = &owner->data[index];
    return 0;
}

int at_region_write(AtRegion *owner, int64_t index, int64_t value)
{
    if (!owner || owner->readers || owner->writer) {
        return AT_E_RUNTIME;
    }
    int status = at_region_index(&index, owner->length, index);
    if (status) {
        return status;
    }
    if (value < 0 || value > 255) {
        return AT_E_VALUE;
    }
    owner->data[index] = (unsigned char)value;
    return 0;
}
