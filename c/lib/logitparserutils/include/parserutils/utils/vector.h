/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_VECTOR_H
#define LOGIT_PARSER_VECTOR_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct parserutils_vector parserutils_vector;
parserutils_error parserutils_vector_create(size_t size, size_t chunk, parserutils_vector **result);
parserutils_error parserutils_vector_destroy(parserutils_vector *v);
parserutils_error parserutils_vector_append(parserutils_vector *v, void *item);
parserutils_error parserutils_vector_clear(parserutils_vector *v);
parserutils_error parserutils_vector_remove_last(parserutils_vector *v);
parserutils_error parserutils_vector_get_length(parserutils_vector *v, size_t *length);
const void *parserutils_vector_iterate(const parserutils_vector *v, int32_t *cursor);
const void *parserutils_vector_peek(const parserutils_vector *v, int32_t cursor);
#ifdef __cplusplus
}
#endif
#endif
