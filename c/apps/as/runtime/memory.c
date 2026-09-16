/* SPDX-License-Identifier: MIT */
#include "native.h"
#include "system.h"
#include <string.h>

int at_memory_text(AtNativeText *out, const void *memory, int64_t capacity, int64_t length,
                   int32_t terminated)
{
    *out = (AtNativeText){0};
    if (!at_caps_have(AS_CAP_RAW)) {
        return AT_E_PERMISSION;
    }
    if (!memory || capacity < -1 || length < 0 || length > AT_BUFFER_LIMIT) {
        return AT_E_VALUE;
    }
    if (terminated) {
        int64_t limit = capacity >= 0 && capacity < length ? capacity : length;
        const char *end = memchr(memory, 0, (size_t)limit);
        if (!end) {
            return AT_E_VALUE;
        }
        length = end - (const char *)memory;
    } else if (capacity >= 0 && length > capacity) {
        return AT_E_INDEX;
    }
    /* A3 str promises UTF-8. Snapshot mutable/raw memory before decoding; the
     * returned interior pointer retains this independent immutable GC owner.
     * Binary data must stay Bytes instead of masquerading as malformed text. */
    AtBytes *bytes = at_bytes_copy(memory, length);
    if (!bytes) {
        return AT_E_MEMORY;
    }
    return at_bytes_decode(out, bytes);
}
