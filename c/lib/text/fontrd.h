#ifndef LOGIT_FONTRD_H
#define LOGIT_FONTRD_H

#include <stdint.h>

/* Bounds-checked big-endian readers over a font blob.
 *
 * Every offset in a font file is attacker-controlled the moment a page uses
 * @font-face, so the rule here is that a read which falls outside the blob
 * yields 0 rather than touching memory. Callers still range-check structural
 * boundaries (an array's whole extent, a subtable's declared length) so a
 * truncated table is rejected instead of silently read as a field of zeroes --
 * fr_* is the backstop, not the only check. */

struct fr { const uint8_t *d; uint32_t len; };

/* Does [off, off+n) lie inside the blob?  Written to be overflow-proof:
 * off + n could wrap, len - off cannot (off <= len is checked first). */
static inline int fr_ok(const struct fr *b, uint32_t off, uint32_t n)
{
    return b->d && off <= b->len && n <= b->len - off;
}

static inline uint32_t fr_u8(const struct fr *b, uint32_t o)
{ return fr_ok(b, o, 1) ? b->d[o] : 0u; }

static inline uint32_t fr_u16(const struct fr *b, uint32_t o)
{ return fr_ok(b, o, 2) ? (((uint32_t)b->d[o] << 8) | b->d[o + 1]) : 0u; }

static inline int32_t fr_s16(const struct fr *b, uint32_t o)
{ return (int32_t)(int16_t)(uint16_t)fr_u16(b, o); }

static inline uint32_t fr_u24(const struct fr *b, uint32_t o)
{
    return fr_ok(b, o, 3) ? (((uint32_t)b->d[o] << 16) | ((uint32_t)b->d[o + 1] << 8)
                             | b->d[o + 2]) : 0u;
}

static inline uint32_t fr_u32(const struct fr *b, uint32_t o)
{
    return fr_ok(b, o, 4) ? (((uint32_t)b->d[o] << 24) | ((uint32_t)b->d[o + 1] << 16)
                             | ((uint32_t)b->d[o + 2] << 8) | b->d[o + 3]) : 0u;
}

static inline int32_t fr_s32(const struct fr *b, uint32_t o)
{ return (int32_t)fr_u32(b, o); }

/* An offset field that must land inside the blob and be non-zero to be usable.
 * Returns 0 for "absent or bogus", which every caller already treats as absent. */
static inline uint32_t fr_off16(const struct fr *b, uint32_t base, uint32_t o)
{
    uint32_t v = fr_u16(b, o);
    if (!v) return 0;
    uint32_t a = base + v;
    return (a >= base && a < b->len) ? a : 0;
}

static inline uint32_t fr_off32(const struct fr *b, uint32_t base, uint32_t o)
{
    uint32_t v = fr_u32(b, o);
    if (!v) return 0;
    uint64_t a = (uint64_t)base + v;
    return (a < b->len) ? (uint32_t)a : 0;
}

/* 'abcd' -> a 4-byte tag, for comparing against table/script/feature tags. */
#define FONT_TAG(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8)  |  (uint32_t)(uint8_t)(d))

#endif /* LOGIT_FONTRD_H */
