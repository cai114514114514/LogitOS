/* SPDX-License-Identifier: GPL-3.0-or-later
 * peek lends a pointer until advance passes its character. The first stream
 * implementation used a resizable byte buffer; growth freed borrowed pointers
 * and compaction moved their bytes. Keep each replaced allocation until all of
 * its unread characters have been consumed. Relative distances avoid a lifetime
 * byte counter that could overflow on a long-running stream.
 */
#include "window.h"
#include <stdlib.h>
#include <string.h>

struct window_snapshot {
    uint8_t *data;
    size_t remaining;
    window_snapshot *next;
};

void window_free(parser_window *window)
{
    free(window->data);
    while (window->retired) {
        window_snapshot *entry = window->retired;
        window->retired = entry->next;
        free(entry->data);
        free(entry);
    }
}

static parserutils_error replace(parser_window *window, size_t needed, size_t discard)
{
    size_t capacity = 128;
    while (capacity < needed) {
        size_t step = capacity / 2 + 64;
        if (capacity > SIZE_MAX - step) { capacity = needed; break; }
        capacity += step;
    }
    uint8_t *data = malloc(capacity);
    if (!data) return PARSERUTILS_NOMEM;
    window_snapshot *snapshot = NULL;
    size_t unread = window->length - window->cursor;
    if (unread) {
        snapshot = malloc(sizeof(*snapshot));
        if (!snapshot) { free(data); return PARSERUTILS_NOMEM; }
        snapshot->data = window->data;
        snapshot->remaining = unread;
        snapshot->next = window->retired;
    }
    size_t kept = window->length - discard;
    if (kept) memcpy(data, window->data + discard, kept);
    /* Publish only after both allocations succeed. A failed refill must keep
     * the old window, its cursor and every borrowed pointer intact. */
    if (snapshot) window->retired = snapshot;
    else free(window->data);
    window->data = data;
    window->capacity = capacity;
    window->length = kept;
    window->cursor -= discard;
    return PARSERUTILS_OK;
}

parserutils_error window_append(parser_window *window, const uint8_t *bytes, size_t length)
{
    if (length > SIZE_MAX - window->length) return PARSERUTILS_NOMEM;
    if (!length) return PARSERUTILS_OK;
    if (window->length + length > window->capacity) {
        parserutils_error error = replace(window, window->length + length, 0);
        if (error) return error;
    }
    memcpy(window->data + window->length, bytes, length);
    window->length += length;
    return PARSERUTILS_OK;
}

parserutils_error window_insert(parser_window *window, const uint8_t *bytes, size_t length)
{
    if (length > SIZE_MAX - window->length) return PARSERUTILS_NOMEM;
    if (!length) return PARSERUTILS_OK;
    /* Insertion shifts existing characters. Even if capacity is sufficient,
     * copy first so pointers to unread characters keep their original bytes.
     * The new prefix delays consumption of every retired window by length. */
    parserutils_error error = replace(window, window->length + length, 0);
    if (error) return error;
    for (window_snapshot *entry = window->retired; entry; entry = entry->next)
        entry->remaining += length;
    memmove(window->data + window->cursor + length, window->data + window->cursor,
            window->length - window->cursor);
    memcpy(window->data + window->cursor, bytes, length);
    window->length += length;
    return PARSERUTILS_OK;
}

parserutils_error window_compact(parser_window *window)
{
    if (window->cursor < 65536) return PARSERUTILS_OK;
    return replace(window, window->length - window->cursor, window->cursor);
}

void window_advance(parser_window *window, size_t length)
{
    size_t unread = window->length - window->cursor;
    if (length > unread) length = unread;
    window->cursor += length;
    window_snapshot **link = &window->retired;
    while (*link) {
        window_snapshot *entry = *link;
        if (length >= entry->remaining) {
            *link = entry->next;
            free(entry->data);
            free(entry);
        } else {
            entry->remaining -= length;
            link = &entry->next;
        }
    }
}
