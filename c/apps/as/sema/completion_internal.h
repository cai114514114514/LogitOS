/* SPDX-License-Identifier: MIT */
#ifndef AETHER_COMPLETION_INTERNAL_H
#define AETHER_COMPLETION_INTERNAL_H
#include "sema/internal.h"

void at_completion_item(AsTypedProject *project, FILE *out, const char *name, const char *kind,
                        int type, int module, Token declaration, const char *prefix, size_t bytes,
                        int *count);
int at_completion_object_type(AsTypedProject *project, AtNode *field);
void at_completion_object_items(AsTypedProject *project, AtNode *field, int type, FILE *out,
                                const char *prefix, size_t bytes, int *count);
#endif
