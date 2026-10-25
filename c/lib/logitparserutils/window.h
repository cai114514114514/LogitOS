/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_WINDOW_H
#define LOGIT_PARSER_WINDOW_H
#include <parserutils/errors.h>

typedef struct window_snapshot window_snapshot;
typedef struct {
    uint8_t *data;
    size_t length, capacity, cursor;
    window_snapshot *retired;
} parser_window;

void window_free(parser_window *window);
parserutils_error window_append(parser_window *window, const uint8_t *bytes, size_t length);
parserutils_error window_insert(parser_window *window, const uint8_t *bytes, size_t length);
parserutils_error window_compact(parser_window *window);
void window_advance(parser_window *window, size_t length);
#endif
