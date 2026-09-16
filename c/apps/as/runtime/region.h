/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_REGION_H
#define AS_NATIVE_REGION_H
#include <stdint.h>

typedef struct AtRegion AtRegion;
typedef struct AtRegionBorrow AtRegionBorrow;

/* These runtime primitives are the ownership backend, not a source-language
 * escape hatch. Generated owner/borrow slots start NULL and are never copied.
 * The compiler must additionally prove that a borrow cannot escape its owner.
 * All failures return an exception code and leave existing ownership intact. */
int at_region_new(AtRegion **out, int64_t length);
int at_region_move(AtRegion **destination, AtRegion **source);
int at_region_release(AtRegion **owner);
int at_region_length(int64_t *out, const AtRegion *owner);
int at_region_read(int64_t *out, const AtRegion *owner, int64_t index);
int at_region_write(AtRegion *owner, int64_t index, int64_t value);
/* Compiler-only address lease for one checked byte operation. Generated code
 * must not expose or retain this pointer beyond the immediate read/write. */
int at_region_address(unsigned char **out, AtRegion *owner, int64_t index, int32_t writable);

/* Multiple shared borrows or one mutable borrow may exist. A mutable parent's
 * access is suspended while its reborrows live; shared reborrows remain shared.
 * Ranges are explicit [start, stop), with no silent clipping. */
int at_region_borrow(AtRegionBorrow **out, AtRegion *owner, int64_t start, int64_t stop,
                     int32_t writable);
int at_region_reborrow(AtRegionBorrow **out, AtRegionBorrow *parent, int64_t start, int64_t stop,
                       int32_t writable);
int at_region_borrow_release(AtRegionBorrow **borrow);
/* Used only immediately after a successful borrow acquisition. The compiler
 * keeps the descriptor in its cleanup slot while the plain Slice is in use. */
unsigned char *at_region_borrow_data(AtRegionBorrow *borrow);
int at_region_borrow_length(int64_t *out, const AtRegionBorrow *borrow);
int at_region_borrow_read(int64_t *out, const AtRegionBorrow *borrow, int64_t index);
int at_region_borrow_write(AtRegionBorrow *borrow, int64_t index, int64_t value);

#endif
