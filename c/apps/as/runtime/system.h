/* SPDX-License-Identifier: MIT */
#ifndef AS_NATIVE_SYSTEM_H
#define AS_NATIVE_SYSTEM_H
#include "exception.h"
#include <stdint.h>

/* Status is a language exception code; the output preserves the kernel ABI's
 * raw signed return value. Hosted programs never interpret LogitOS numbers. */
int at_system_call(int64_t *out, int64_t number, uint64_t a, uint64_t b, uint64_t c);
/* capacity == -1 denotes an unsafe raw address. Known storage bounds are
 * enforced before reading; terminated mode requires a NUL within the limit. */
int at_memory_text(AtNativeText *out, const void *memory, int64_t capacity, int64_t length,
                   int32_t terminated);
#endif
