/* SPDX-License-Identifier: MIT */
#include "native.h"
#include <string.h>

static void scan_data(void *slot)
{
    at_gc_mark((void *)*(const void **)slot);
}

AtBytes *at_bytes_copy(const void *data, int64_t length)
{
    if (length < 0 || length > AT_BUFFER_LIMIT) {
        return NULL;
    }
    /* Allocation is a safepoint. Root the source's interior data pointer so
     * copying a temporary text view or Buffer cannot read reclaimed storage. */
    AtRoot root;
    void *mark = at_gc_frame();
    at_gc_root(&root, &data, scan_data);
    AtBytes *bytes = at_buffer_new(length);
    if (bytes && length) {
        memcpy(bytes->data, data, (size_t)length);
    }
    at_gc_restore(mark);
    return bytes;
}

int32_t at_bytes_equal(const AtBytes *left, const AtBytes *right)
{
    return left->length == right->length &&
           (!left->length || !memcmp(left->data, right->data, (size_t)left->length));
}

AtBuffer *at_buffer_new(int64_t length)
{
    if (length < 0 || length > AT_BUFFER_LIMIT) {
        return NULL;
    }
    /* A fixed-size buffer is one nonmoving allocation. There is no backing
     * resize or embedded managed reference for its scanner to follow. */
    AtBuffer *buffer = at_gc_allocate(sizeof *buffer + (size_t)length, NULL);
    if (buffer) {
        buffer->length = length;
    }
    return buffer;
}

int64_t at_buffer_len(const AtBuffer *buffer)
{
    return buffer->length;
}

int32_t at_layout_store_bytes(AtBuffer *record, int64_t offset, int64_t width, const AtBytes *value)
{
    /* Validate before touching storage: an oversized assignment must leave the
     * previous record intact. The compiler also checks constant field bounds;
     * keeping this check makes the runtime contract independently testable. */
    if (offset < 0 || width < 0 || offset > record->length || width > record->length - offset ||
        value->length > width) {
        return AT_E_VALUE;
    }
    memcpy(record->data + offset, value->data, (size_t)value->length);
    memset(record->data + offset + value->length, 0, (size_t)(width - value->length));
    return 0;
}

unsigned char *at_buffer_data(AtBuffer *buffer)
{
    /* Unlike indexed access, an empty buffer still has a backing address.
     * The unsafe caller is responsible for never dereferencing that end. */
    return buffer->data;
}

unsigned char *at_buffer_at(AtBuffer *buffer, int64_t index)
{
    if (index < 0) {
        index += buffer->length;
    }
    if (index < 0 || index >= buffer->length) {
        return NULL;
    }
    return &buffer->data[index];
}

int32_t at_buffer_contains(const AtBuffer *buffer, int64_t value)
{
    if (value < 0 || value > 255) {
        return 0;
    }
    for (int64_t index = 0; index < buffer->length; index++) {
        if (buffer->data[index] == value) {
            return 1;
        }
    }
    return 0;
}
