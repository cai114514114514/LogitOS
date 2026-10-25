/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <parserutils/utils/vector.h>
#include "sequence.h"
#include <stdlib.h>
#include <stdio.h>

struct parserutils_vector { parser_sequence items; };

parserutils_error parserutils_vector_create(size_t size, size_t chunk, parserutils_vector **result)
{
    if (!result) return PARSERUTILS_BADPARM;
    *result = NULL;
    parserutils_vector *v = malloc(sizeof(*v));
    if (!v) return PARSERUTILS_NOMEM;
    parserutils_error error = sequence_init(&v->items, size, chunk);
    if (error) { free(v); return error; }
    *result = v;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_vector_destroy(parserutils_vector *v)
{
    if (!v) return PARSERUTILS_BADPARM;
    sequence_free(&v->items);
    free(v);
    return PARSERUTILS_OK;
}

parserutils_error parserutils_vector_append(parserutils_vector *v, void *item)
{
    return v ? sequence_push(&v->items, item) : PARSERUTILS_BADPARM;
}

parserutils_error parserutils_vector_clear(parserutils_vector *v)
{
    if (!v) return PARSERUTILS_BADPARM;
    v->items.count = 0;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_vector_remove_last(parserutils_vector *v)
{
    if (!v) return PARSERUTILS_BADPARM;
    if (!v->items.count) return PARSERUTILS_INVALID;
    v->items.count--;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_vector_get_length(parserutils_vector *v, size_t *length)
{
    if (!v || !length) return PARSERUTILS_BADPARM;
    *length = v->items.count;
    return PARSERUTILS_OK;
}

const void *parserutils_vector_peek(const parserutils_vector *v, int32_t cursor)
{
    return v && cursor >= 0 ? sequence_at(&v->items, (size_t)cursor) : NULL;
}

const void *parserutils_vector_iterate(const parserutils_vector *v, int32_t *cursor)
{
    if (!cursor) return NULL;
    const void *item = parserutils_vector_peek(v, *cursor);
    if (item) (*cursor)++;
    return item;
}

void parserutils_vector_dump(parserutils_vector *v, const char *prefix, void (*printer)(void *))
{
    if (!v || !printer) return;
    if (prefix) fprintf(stdout, "%s\n", prefix);
    for (size_t i = 0; i < v->items.count; i++) printer(sequence_at(&v->items, i));
}
