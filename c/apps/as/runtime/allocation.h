/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_ALLOCATION_H
#define AS_NATIVE_ALLOCATION_H
#include <stdint.h>

/* Manual memory is outside the tracing heap. These functions return exception
 * codes so generated code attaches the allocation/release expression's source
 * location. A raw address carries neither bounds nor a lifetime token. */
int at_manual_alloc(uint64_t *out, int64_t bytes);
int at_manual_dealloc(uint64_t address);

#endif
