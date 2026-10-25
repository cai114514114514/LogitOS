/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <parserutils/utils/stack.h>
#include "sequence.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct parserutils_stack { parser_sequence items; };

parserutils_error parserutils_stack_create(size_t size, size_t chunk, parserutils_stack **result)
{
    if (!result) return PARSERUTILS_BADPARM;
    *result = NULL;
    parserutils_stack *s = malloc(sizeof(*s));
    if (!s) return PARSERUTILS_NOMEM;
    parserutils_error error = sequence_init(&s->items, size, chunk);
    if (error) { free(s); return error; }
    *result = s;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_stack_destroy(parserutils_stack *s)
{
    if (!s) return PARSERUTILS_BADPARM;
    sequence_free(&s->items);
    free(s);
    return PARSERUTILS_OK;
}

parserutils_error parserutils_stack_push(parserutils_stack *s, const void *item)
{
    return s ? sequence_push(&s->items, item) : PARSERUTILS_BADPARM;
}

parserutils_error parserutils_stack_pop(parserutils_stack *s, void *item)
{
    if (!s) return PARSERUTILS_BADPARM;
    if (!s->items.count) return PARSERUTILS_INVALID;
    if (item) memcpy(item, sequence_at(&s->items, s->items.count - 1), s->items.item_size);
    s->items.count--;
    return PARSERUTILS_OK;
}

void *parserutils_stack_get_current(parserutils_stack *s)
{
    return s && s->items.count ? sequence_at(&s->items, s->items.count - 1) : NULL;
}

void parserutils_stack_dump(parserutils_stack *s, const char *prefix, void (*printer)(void *))
{
    if (!s || !printer) return;
    if (prefix) fprintf(stdout, "%s\n", prefix);
    for (size_t i = s->items.count; i; i--) printer(sequence_at(&s->items, i - 1));
}
