/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_SEQUENCE_H
#define LOGIT_PARSER_SEQUENCE_H
#include <parserutils/errors.h>

typedef struct {
    unsigned char **pages;
    size_t page_count, slots, count;
    size_t item_size, page_items;
} parser_sequence;

parserutils_error sequence_init(parser_sequence *, size_t, size_t);
void sequence_free(parser_sequence *);
void *sequence_at(const parser_sequence *, size_t);
parserutils_error sequence_push(parser_sequence *, const void *);
#endif
