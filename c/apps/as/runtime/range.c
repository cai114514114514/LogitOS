/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <string.h>

struct AtRange {
    int64_t start;
    int64_t stop;
    int64_t step;
    uint64_t count;
};

static uint64_t magnitude(int64_t negative)
{
    /* Negating INT64_MIN is undefined; unsigned subtraction represents its
     * magnitude exactly, including the otherwise unrepresentable 2**63. */
    return UINT64_C(0) - (uint64_t)negative;
}

AtRange *at_range_new(int64_t start, int64_t stop, int64_t step)
{
    if (!step) {
        return NULL;
    }
    uint64_t count = 0;
    if ((step > 0 && start < stop) || (step < 0 && start > stop)) {
        uint64_t span =
            step > 0 ? (uint64_t)stop - (uint64_t)start : (uint64_t)start - (uint64_t)stop;
        uint64_t stride = step > 0 ? (uint64_t)step : magnitude(step);
        /* Divide first. The traditional (span + stride - 1) formula wraps
         * for legal ranges crossing almost the whole signed integer domain. */
        count = span / stride + (span % stride != 0);
    }
    AtRange *range = at_gc_allocate(sizeof *range, NULL);
    if (range) {
        *range = (AtRange){start, stop, step, count};
    }
    return range;
}

int64_t at_range_bound(AtRange *range, int bound)
{
    return bound == 0 ? range->start : bound == 1 ? range->stop : range->step;
}

int64_t at_range_len(AtRange *range)
{
    return range->count > INT64_MAX ? -1 : (int64_t)range->count;
}

int at_range_at(int64_t *out, AtRange *range, int64_t index)
{
    uint64_t offset;
    if (index < 0) {
        uint64_t distance = magnitude(index);
        if (distance > range->count) {
            return 0;
        }
        offset = range->count - distance;
    } else {
        offset = (uint64_t)index;
        if (offset >= range->count) {
            return 0;
        }
    }
    /* The selected mathematical element lies between start and stop and is
     * representable as i64. Compute its bits using defined unsigned wrapping,
     * then copy them instead of relying on an implementation-defined cast. */
    uint64_t bits = (uint64_t)range->start + offset * (uint64_t)range->step;
    memcpy(out, &bits, sizeof bits);
    return 1;
}

int at_range_contains(AtRange *range, int64_t item)
{
    if ((range->step > 0 && (item < range->start || item >= range->stop)) ||
        (range->step < 0 && (item > range->start || item <= range->stop))) {
        return 0;
    }
    uint64_t distance = range->step > 0 ? (uint64_t)item - (uint64_t)range->start
                                        : (uint64_t)range->start - (uint64_t)item;
    uint64_t stride = range->step > 0 ? (uint64_t)range->step : magnitude(range->step);
    return distance % stride == 0;
}
