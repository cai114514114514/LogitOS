/* SPDX-License-Identifier: MIT */
#include "common/numeric.h"
#include "runtime/integer_parse.h"
#include <string.h>

int64_t as_i64_bits(uint64_t bits)
{
    /* Unlike a cast of an out-of-range uint64_t, this has no implementation-
     * defined conversion. The subtraction is representable, including MIN. */
    return bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
}

int as_parse_i64(const char *text, size_t bytes, int64_t *out)
{
    return at_parse_i64_exact(text, bytes, out);
}
