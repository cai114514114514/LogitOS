/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_BUFFER_H
#define AS_NATIVE_BUFFER_H
#include <stdint.h>
#include "exception.h"

/* buffer(n) keeps the existing 64 MiB per-allocation ceiling. The language
 * reads and writes i64 byte values, but storage occupies exactly n bytes. */
#define AT_BUFFER_LIMIT (INT64_C(1) << 26)

typedef struct {
    int64_t length;
    unsigned char data[];
} AtBuffer;

/* Bytes has the same fixed storage layout as Buffer, but is a distinct
 * language type. Read helpers are shared; the checker never permits a Bytes
 * store or a conversion back to Buffer. Freezing a Buffer always copies. */
typedef AtBuffer AtBytes;

AtBytes *at_bytes_copy(const void *data, int64_t length);
int32_t at_bytes_equal(const AtBytes *left, const AtBytes *right);
int at_bytes_decode(AtNativeText *out, const AtBytes *bytes);

AtBuffer *at_buffer_new(int64_t length);
int32_t at_layout_store_bytes(AtBuffer *record, int64_t offset, int64_t width,
                              const AtBytes *value);
int64_t at_buffer_len(const AtBuffer *buffer);
unsigned char *at_buffer_at(AtBuffer *buffer, int64_t index);
unsigned char *at_buffer_data(AtBuffer *buffer);
int32_t at_buffer_contains(const AtBuffer *buffer, int64_t value);
#endif
