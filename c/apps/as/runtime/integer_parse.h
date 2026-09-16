/* SPDX-License-Identifier: MIT */
#ifndef AS_INTEGER_PARSE_H
#define AS_INTEGER_PARSE_H
#include <stdint.h>
#include <stddef.h>

/* Compiler literals and runtime parse_int share exact signed decimal/hex
 * parsing. Keeping this small implementation in one header avoids linking
 * source-version policy or the retiring VM into native executables. */
static inline int at_parse_i64_exact(const char *s, size_t n, int64_t *out)
{
    size_t p = 0;
    int negative = 0;
    unsigned radix = 10;
    uint64_t value = 0;
    if (p < n && (s[p] == '-' || s[p] == '+')) {
        negative = s[p++] == '-';
    }
    if (n - p >= 2 && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) {
        radix = 16;
        p += 2;
    }
    if (p == n) {
        return 0;
    }
    uint64_t limit = (uint64_t)INT64_MAX + (unsigned)negative;
    for (; p < n; p++) {
        unsigned char c = (unsigned char)s[p];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return 0;
        }
        if (d >= radix || value > limit / radix || (value == limit / radix && d > limit % radix)) {
            return 0;
        }
        value = value * radix + d;
    }
    if (negative) {
        *out = value == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)value;
    } else {
        *out = (int64_t)value;
    }
    return 1;
}

#endif
