/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_STACK_H
#define LOGIT_PARSER_STACK_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct parserutils_stack parserutils_stack;
parserutils_error parserutils_stack_create(size_t size, size_t chunk, parserutils_stack **result);
parserutils_error parserutils_stack_destroy(parserutils_stack *s);
parserutils_error parserutils_stack_push(parserutils_stack *s, const void *item);
parserutils_error parserutils_stack_pop(parserutils_stack *s, void *item);
void *parserutils_stack_get_current(parserutils_stack *s);
#ifdef __cplusplus
}
#endif
#endif
