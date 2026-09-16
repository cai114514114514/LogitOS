/* SPDX-License-Identifier: MIT */
#include "region_internal.h"
#include <stdlib.h>

static AtRegionBorrow *new_borrow(AtRegion *owner, AtRegionBorrow *parent, int64_t start,
                                  int64_t length, int32_t writable)
{
    /* An opaque heap record keeps C layout out of generated stack descriptors.
     * Every borrow currently pays for this small allocation and explicit drop;
     * future stack placement must preserve the same parent/child obligations. */
    AtRegionBorrow *borrow = calloc(1, sizeof *borrow);
    if (borrow) {
        borrow->owner = owner;
        borrow->parent = parent;
        borrow->start = start;
        borrow->length = length;
        borrow->writable = writable;
    }
    return borrow;
}

int at_region_borrow(AtRegionBorrow **out, AtRegion *owner, int64_t start, int64_t stop,
                     int32_t writable)
{
    if (*out || !owner || (writable != 0 && writable != 1)) {
        return AT_E_RUNTIME;
    }
    int status = at_region_range(owner->length, start, stop);
    if (status) {
        return status;
    }
    if (owner->writer || (writable && owner->readers)) {
        return AT_E_RUNTIME;
    }
    if (!writable && owner->readers == INT64_MAX) {
        return AT_E_MEMORY;
    }
    AtRegionBorrow *borrow = new_borrow(owner, NULL, start, stop - start, writable);
    if (!borrow) {
        return AT_E_MEMORY;
    }
    /* Publish only after successful allocation. A failed lease must not leave
     * a phantom reader/writer preventing the owner from ever being released. */
    if (writable) {
        owner->writer = 1;
    } else {
        owner->readers++;
    }
    *out = borrow;
    return 0;
}

int at_region_reborrow(AtRegionBorrow **out, AtRegionBorrow *parent, int64_t start, int64_t stop,
                       int32_t writable)
{
    if (*out || !parent || (writable != 0 && writable != 1)) {
        return AT_E_RUNTIME;
    }
    int status = at_region_range(parent->length, start, stop);
    if (status) {
        return status;
    }
    if ((writable && (!parent->writable || parent->children)) || parent->mutable_child) {
        return AT_E_RUNTIME;
    }
    if (parent->children == INT64_MAX) {
        return AT_E_MEMORY;
    }
    /* Ranges were checked against the parent's extent, itself within owner.
     * Their sum cannot overflow or point outside the original allocation. */
    AtRegionBorrow *borrow =
        new_borrow(parent->owner, parent, parent->start + start, stop - start, writable);
    if (!borrow) {
        return AT_E_MEMORY;
    }
    parent->children++;
    parent->mutable_child = writable;
    *out = borrow;
    return 0;
}

int at_region_borrow_release(AtRegionBorrow **slot)
{
    AtRegionBorrow *borrow = *slot;
    if (!borrow) {
        return 0;
    }
    if (borrow->children) {
        return AT_E_RUNTIME;
    }
    if (borrow->parent) {
        borrow->parent->children--;
        if (borrow->writable) {
            borrow->parent->mutable_child = 0;
        }
    } else if (borrow->writable) {
        borrow->owner->writer = 0;
    } else {
        borrow->owner->readers--;
    }
    *slot = NULL;
    free(borrow);
    return 0;
}

int at_region_borrow_length(int64_t *out, const AtRegionBorrow *borrow)
{
    *out = 0;
    if (!borrow) {
        return AT_E_RUNTIME;
    }
    *out = borrow->length;
    return 0;
}

unsigned char *at_region_borrow_data(AtRegionBorrow *borrow)
{
    return borrow->owner->data + borrow->start;
}

static int borrow_access(const AtRegionBorrow *borrow, int64_t *index, int write)
{
    if (!borrow || (write && !borrow->writable) || (borrow->writable && borrow->children)) {
        return AT_E_RUNTIME;
    }
    return at_region_index(index, borrow->length, *index);
}

int at_region_borrow_read(int64_t *out, const AtRegionBorrow *borrow, int64_t index)
{
    *out = 0;
    int status = borrow_access(borrow, &index, 0);
    if (status) {
        return status;
    }
    *out = borrow->owner->data[borrow->start + index];
    return 0;
}

int at_region_borrow_write(AtRegionBorrow *borrow, int64_t index, int64_t value)
{
    int status = borrow_access(borrow, &index, 1);
    if (status) {
        return status;
    }
    if (value < 0 || value > 255) {
        return AT_E_VALUE;
    }
    borrow->owner->data[borrow->start + index] = (unsigned char)value;
    return 0;
}
