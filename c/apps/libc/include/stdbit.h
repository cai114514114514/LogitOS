#ifndef _STDBIT_H
#define _STDBIT_H

/* C23 bit utilities for every standard unsigned integer type.  The small loop
 * helpers deliberately define the zero cases before doing any shift, avoiding
 * the undefined __builtin_clz(0) trap that otherwise hides in wrappers. */
#include <limits.h>
#include <stdbool.h>

#define __STDBIT_GEN(suffix, type) \
static inline unsigned int stdc_leading_zeros_##suffix(type x) { \
    unsigned int n = 0, w = (unsigned int)(sizeof(type) * CHAR_BIT); \
    if (!x) return w; for (type m = (type)1 << (w - 1); !(x & m); m >>= 1) n++; return n; } \
static inline unsigned int stdc_leading_ones_##suffix(type x) \
{ return stdc_leading_zeros_##suffix((type)~x); } \
static inline unsigned int stdc_trailing_zeros_##suffix(type x) { \
    unsigned int n = 0, w = (unsigned int)(sizeof(type) * CHAR_BIT); \
    if (!x) return w; while (!(x & 1)) { x >>= 1; n++; } return n; } \
static inline unsigned int stdc_trailing_ones_##suffix(type x) \
{ return stdc_trailing_zeros_##suffix((type)~x); } \
static inline unsigned int stdc_first_leading_zero_##suffix(type x) \
{ return x == (type)~(type)0 ? 0 : stdc_leading_ones_##suffix(x) + 1; } \
static inline unsigned int stdc_first_leading_one_##suffix(type x) \
{ return !x ? 0 : stdc_leading_zeros_##suffix(x) + 1; } \
static inline unsigned int stdc_first_trailing_zero_##suffix(type x) \
{ return x == (type)~(type)0 ? 0 : stdc_trailing_ones_##suffix(x) + 1; } \
static inline unsigned int stdc_first_trailing_one_##suffix(type x) \
{ return !x ? 0 : stdc_trailing_zeros_##suffix(x) + 1; } \
static inline unsigned int stdc_count_ones_##suffix(type x) \
{ unsigned int n = 0; while (x) { x &= (type)(x - 1); n++; } return n; } \
static inline unsigned int stdc_count_zeros_##suffix(type x) \
{ return (unsigned int)(sizeof(type) * CHAR_BIT) - stdc_count_ones_##suffix(x); } \
static inline bool stdc_has_single_bit_##suffix(type x) \
{ return x && !(x & (type)(x - 1)); } \
static inline unsigned int stdc_bit_width_##suffix(type x) \
{ return (unsigned int)(sizeof(type) * CHAR_BIT) - stdc_leading_zeros_##suffix(x); } \
static inline type stdc_bit_floor_##suffix(type x) \
{ return x ? (type)((type)1 << (stdc_bit_width_##suffix(x) - 1)) : (type)0; } \
static inline type stdc_bit_ceil_##suffix(type x) { \
    unsigned int w; \
    if (x <= 1) return (type)1; \
    w = stdc_bit_width_##suffix((type)(x - 1)); \
    return w >= sizeof(type) * CHAR_BIT ? (type)0 : (type)((type)1 << w); }

__STDBIT_GEN(uc, unsigned char)
__STDBIT_GEN(us, unsigned short)
__STDBIT_GEN(ui, unsigned int)
__STDBIT_GEN(ul, unsigned long)
__STDBIT_GEN(ull, unsigned long long)
#undef __STDBIT_GEN

#define __STDBIT_GENERIC(name, value) _Generic((value), \
    unsigned char: name##_uc, unsigned short: name##_us, \
    unsigned int: name##_ui, unsigned long: name##_ul, \
    unsigned long long: name##_ull)(value)

#define stdc_leading_zeros(v)       __STDBIT_GENERIC(stdc_leading_zeros, (v))
#define stdc_leading_ones(v)        __STDBIT_GENERIC(stdc_leading_ones, (v))
#define stdc_trailing_zeros(v)      __STDBIT_GENERIC(stdc_trailing_zeros, (v))
#define stdc_trailing_ones(v)       __STDBIT_GENERIC(stdc_trailing_ones, (v))
#define stdc_first_leading_zero(v)  __STDBIT_GENERIC(stdc_first_leading_zero, (v))
#define stdc_first_leading_one(v)   __STDBIT_GENERIC(stdc_first_leading_one, (v))
#define stdc_first_trailing_zero(v) __STDBIT_GENERIC(stdc_first_trailing_zero, (v))
#define stdc_first_trailing_one(v)  __STDBIT_GENERIC(stdc_first_trailing_one, (v))
#define stdc_count_zeros(v)         __STDBIT_GENERIC(stdc_count_zeros, (v))
#define stdc_count_ones(v)          __STDBIT_GENERIC(stdc_count_ones, (v))
#define stdc_has_single_bit(v)      __STDBIT_GENERIC(stdc_has_single_bit, (v))
#define stdc_bit_width(v)           __STDBIT_GENERIC(stdc_bit_width, (v))
#define stdc_bit_floor(v)           __STDBIT_GENERIC(stdc_bit_floor, (v))
#define stdc_bit_ceil(v)            __STDBIT_GENERIC(stdc_bit_ceil, (v))

#endif /* _STDBIT_H */
