/* SPDX-License-Identifier: GPL-3.0-or-later
 * Contiguous bytes for a token under construction. Capacity changes are checked
 * before publishing any length change, including a multi-piece append.
 */
#include <parserutils/utils/buffer.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static parserutils_error reserve(parserutils_buffer *buffer, size_t needed)
{
    if (needed <= buffer->allocated) return PARSERUTILS_OK;
    size_t capacity = buffer->allocated ? buffer->allocated : 128;
    while (capacity < needed) {
        size_t step = capacity / 2 + 64;
        if (capacity > SIZE_MAX - step) { capacity = needed; break; }
        capacity += step;
    }
    uint8_t *data = malloc(capacity);
    if (!data) return PARSERUTILS_NOMEM;
    if (buffer->length) memcpy(data, buffer->data, buffer->length);
    free(buffer->alloc);
    buffer->alloc = data;
    buffer->data = data;
    buffer->allocated = capacity;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_buffer_create(parserutils_buffer **result)
{
    if (!result) return PARSERUTILS_BADPARM;
    *result = calloc(1, sizeof(**result));
    return *result ? PARSERUTILS_OK : PARSERUTILS_NOMEM;
}

parserutils_error parserutils_buffer_destroy(parserutils_buffer *buffer)
{
    if (!buffer) return PARSERUTILS_BADPARM;
    free(buffer->alloc);
    free(buffer);
    return PARSERUTILS_OK;
}

parserutils_error parserutils_buffer_insert(parserutils_buffer *buffer, size_t offset,
                                          const uint8_t *bytes, size_t length)
{
    if (!buffer || (!bytes && length) || offset > buffer->length) return PARSERUTILS_BADPARM;
    if (length > SIZE_MAX - buffer->length) return PARSERUTILS_NOMEM;
    if (!length) return PARSERUTILS_OK;
    /* An insertion may use a slice of this very buffer. Preserve that slice
     * before realloc or tail movement can invalidate/overwrite the source. */
    uint8_t *copy = NULL;
    uintptr_t source = (uintptr_t)bytes, start = (uintptr_t)buffer->data;
    if (buffer->data && source >= start && source - start < buffer->allocated) {
        size_t relative = (size_t)(source - start);
        if (relative > buffer->length || length > buffer->length - relative)
            return PARSERUTILS_BADPARM;
        copy = malloc(length);
        if (!copy) return PARSERUTILS_NOMEM;
        memcpy(copy, bytes, length);
        bytes = copy;
    }
    parserutils_error error = reserve(buffer, buffer->length + length);
    if (error == PARSERUTILS_OK) {
        memmove(buffer->data + offset + length, buffer->data + offset, buffer->length - offset);
        memcpy(buffer->data + offset, bytes, length);
        buffer->length += length;
    }
    free(copy);
    return error;
}

parserutils_error parserutils_buffer_append(parserutils_buffer *buffer,
                                          const uint8_t *bytes, size_t length)
{
    return buffer ? parserutils_buffer_insert(buffer, buffer->length, bytes, length) : PARSERUTILS_BADPARM;
}

parserutils_error parserutils_buffer_appendv(parserutils_buffer *buffer, size_t count, ...)
{
    if (!buffer) return PARSERUTILS_BADPARM;
    va_list args;
    size_t total = 0;
    va_start(args, count);
    for (size_t i = 0; i < count; i++) {
        const void *bytes = va_arg(args, const void *);
        size_t length = va_arg(args, size_t);
        if ((!bytes && length) || length > SIZE_MAX - total) {
            va_end(args);
            return bytes ? PARSERUTILS_NOMEM : PARSERUTILS_BADPARM;
        }
        total += length;
    }
    va_end(args);
    if (total > SIZE_MAX - buffer->length) return PARSERUTILS_NOMEM;
    if (!total) return PARSERUTILS_OK;
    /* One temporary transaction also handles arguments aliasing the target.
     * A later allocation failure must not leave half a calc instruction. */
    uint8_t *joined = malloc(total);
    if (!joined) return PARSERUTILS_NOMEM;
    size_t at = 0;
    va_start(args, count);
    for (size_t i = 0; i < count; i++) {
        const void *bytes = va_arg(args, const void *);
        size_t length = va_arg(args, size_t);
        if (length) memcpy(joined + at, bytes, length);
        at += length;
    }
    va_end(args);
    parserutils_error error = parserutils_buffer_append(buffer, joined, total);
    free(joined);
    return error;
}

parserutils_error parserutils_buffer_discard(parserutils_buffer *buffer, size_t offset, size_t length)
{
    if (!buffer || offset > buffer->length || length > buffer->length - offset)
        return PARSERUTILS_BADPARM;
    if (length) {
        memmove(buffer->data + offset, buffer->data + offset + length,
                buffer->length - offset - length);
        buffer->length -= length;
    }
    return PARSERUTILS_OK;
}

parserutils_error parserutils_buffer_grow(parserutils_buffer *buffer)
{
    if (!buffer) return PARSERUTILS_BADPARM;
    if (buffer->allocated == SIZE_MAX) return PARSERUTILS_NOMEM;
    return reserve(buffer, buffer->allocated + 1);
}

parserutils_error parserutils_buffer_randomise(parserutils_buffer *buffer)
{
    if (!buffer) return PARSERUTILS_BADPARM;
    if (!buffer->allocated) return PARSERUTILS_OK;
    static unsigned sequence;
    size_t padding = 1 + (__atomic_fetch_add(&sequence, 1, __ATOMIC_RELAXED) % 31);
    if (buffer->allocated > SIZE_MAX - padding) return PARSERUTILS_NOMEM;
    uint8_t *allocation = malloc(buffer->allocated + padding);
    if (!allocation) return PARSERUTILS_NOMEM;
    /* This API intentionally perturbs alignment as well as address. Merely
     * allocating another aligned block would not exercise its original role. */
    if (buffer->length) memcpy(allocation + padding, buffer->data, buffer->length);
    free(buffer->alloc);
    buffer->alloc = allocation;
    buffer->data = allocation + padding;
    return PARSERUTILS_OK;
}
