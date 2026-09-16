/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_REGION_INTERNAL_H
#define AS_NATIVE_REGION_INTERNAL_H
#include "region.h"
#include "exception.h"

/* Neither allocation contains GC references. Owner lifetime and loan lifetime
 * are controlled by generated cleanup, independently of collector safepoints. */
struct AtRegion {
    int64_t length;
    int64_t readers;
    int32_t writer;
    unsigned char data[];
};

struct AtRegionBorrow {
    AtRegion *owner;
    struct AtRegionBorrow *parent;
    int64_t start;
    int64_t length;
    int64_t children;
    int32_t writable;
    int32_t mutable_child;
};

/* Normalize a single index without overflow, including INT64_MIN. */
int at_region_index(int64_t *out, int64_t length, int64_t index);
int at_region_range(int64_t length, int64_t start, int64_t stop);

#endif
