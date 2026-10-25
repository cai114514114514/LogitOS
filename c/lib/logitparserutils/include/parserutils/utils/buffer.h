/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_BUFFER_H
#define LOGIT_PARSER_BUFFER_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parserutils_buffer {
    /* randomise may move data away from the allocation base. Consumers read
     * data/length; only this library may free alloc or change these fields. */
    uint8_t *alloc;
    uint8_t *data;
    size_t length;
    size_t allocated;
} parserutils_buffer;

parserutils_error parserutils_buffer_create(parserutils_buffer **result);
parserutils_error parserutils_buffer_destroy(parserutils_buffer *buffer);
parserutils_error parserutils_buffer_append(parserutils_buffer *buffer,
                                          const uint8_t *bytes, size_t length);
parserutils_error parserutils_buffer_appendv(parserutils_buffer *buffer, size_t count, ...);
parserutils_error parserutils_buffer_insert(parserutils_buffer *buffer, size_t offset,
                                          const uint8_t *bytes, size_t length);
parserutils_error parserutils_buffer_discard(parserutils_buffer *buffer, size_t offset, size_t length);
parserutils_error parserutils_buffer_grow(parserutils_buffer *buffer);
parserutils_error parserutils_buffer_randomise(parserutils_buffer *buffer);
#ifdef __cplusplus
}
#endif
#endif
