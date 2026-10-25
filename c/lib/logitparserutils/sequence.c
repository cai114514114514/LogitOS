/* SPDX-License-Identifier: GPL-3.0-or-later
 * A parser holds pointers to states while pushing another state. Paged storage
 * keeps those pointers stable: only the page index is reallocated on growth.
 * Popped pages are retained until destroy, avoiding allocator churn when the
 * parser repeatedly enters and leaves a block at the same maximum depth.
 */
#include "sequence.h"
#include <stdlib.h>
#include <string.h>

parserutils_error sequence_init(parser_sequence *s, size_t item_size, size_t chunk)
{
    if (!item_size || !chunk || chunk > SIZE_MAX / item_size) return PARSERUTILS_BADPARM;
    memset(s, 0, sizeof(*s));
    s->item_size = item_size;
    s->page_items = chunk;
    return PARSERUTILS_OK;
}

void sequence_free(parser_sequence *s)
{
    for (size_t i = 0; i < s->page_count; i++) free(s->pages[i]);
    free(s->pages);
}

void *sequence_at(const parser_sequence *s, size_t index)
{
    if (index >= s->count) return NULL;
    return s->pages[index / s->page_items] + (index % s->page_items) * s->item_size;
}

parserutils_error sequence_push(parser_sequence *s, const void *item)
{
    if (!item) return PARSERUTILS_BADPARM;
    if (s->count == INT32_MAX) return PARSERUTILS_NOMEM;
    size_t page = s->count / s->page_items;
    if (page == s->page_count) {
        if (page == s->slots) {
            size_t slots = s->slots ? s->slots * 2 : 8;
            if (slots < s->slots || slots > SIZE_MAX / sizeof(*s->pages)) return PARSERUTILS_NOMEM;
            unsigned char **pages = realloc(s->pages, slots * sizeof(*pages));
            if (!pages) return PARSERUTILS_NOMEM;
            s->pages = pages;
            s->slots = slots;
        }
        unsigned char *bytes = malloc(s->page_items * s->item_size);
        if (!bytes) return PARSERUTILS_NOMEM;
        s->pages[s->page_count++] = bytes;
    }
    unsigned char *dest = s->pages[page] + (s->count % s->page_items) * s->item_size;
    memmove(dest, item, s->item_size);
    s->count++;
    return PARSERUTILS_OK;
}
