/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "integer_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_formatted(AtNativeText *out, const char *text, size_t length)
{
    char *copy = at_gc_allocate(length + 1, NULL);
    if (!copy) {
        return 0;
    }
    memcpy(copy, text, length);
    copy[length] = 0;
    *out = (AtNativeText){copy, (int64_t)length};
    return 1;
}

int at_format_i64(AtNativeText *out, int64_t value)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%lld", (long long)value);
    return length >= 0 && length < (int)sizeof text && copy_formatted(out, text, (size_t)length);
}

int at_format_u64(AtNativeText *out, uint64_t value)
{
    char text[32];
    int length = snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return length >= 0 && length < (int)sizeof text && copy_formatted(out, text, (size_t)length);
}

int at_format_f64(AtNativeText *out, double value)
{
    /* Seventeen significant digits round-trip f64. The buffer also has room
     * for sign, decimal point, exponent and the platform's inf/nan spelling. */
    char text[128];
    int length = snprintf(text, sizeof text, "%.17g", value);
    return length >= 0 && length < (int)sizeof text && copy_formatted(out, text, (size_t)length);
}

void at_format_bool(AtNativeText *out, int value)
{
    *out = value ? (AtNativeText){"true", 4} : (AtNativeText){"false", 5};
}

int at_text_chr(AtNativeText *out, int64_t value)
{
    char byte = (char)(unsigned char)value;
    return copy_formatted(out, &byte, 1);
}

int at_parse_int(int64_t *out, const char *text, int64_t length)
{
    return length >= 0 && at_parse_i64_exact(text, (size_t)length, out);
}

int at_parse_float(double *out, const char *text, int64_t length)
{
    if (length <= 0 || (uint64_t)length >= SIZE_MAX || memchr(text, 0, (size_t)length)) {
        return 0;
    }
    /* Substrings are length-delimited views, not C strings. Copy before
     * strtod so it cannot consume bytes beyond the language value's end.
     * This temporary is native malloc storage, never a GC safepoint. */
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) {
        return -1;
    }
    memcpy(buffer, text, (size_t)length);
    buffer[length] = 0;
    char *end = NULL;
    double value = strtod(buffer, &end);
    int valid = end != buffer && end == buffer + length;
    free(buffer);
    if (valid) {
        *out = value;
    }
    return valid;
}
