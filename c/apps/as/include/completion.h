/* SPDX-License-Identifier: MIT */
#ifndef AETHER_SEMANTIC_COMPLETION_H
#define AETHER_SEMANTIC_COMPLETION_H
#include "project.h"

/* Member queries use actual module declarations or checked struct/class
 * receivers. Locations refer to the immutable source snapshot, including
 * incomplete member tokens. Parsed but unsupported receiver types explicitly
 * return unhandled; missing syntax never invents candidate declarations. */
int as_typed_complete_report(AsTypedProject *project, size_t caret, FILE *out);
#endif
