/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_MALLOC_SIZE_H
#define LOGIT_MALLOC_SIZE_H
#include <stddef.h>
/* Internal allocator/accounting transaction. The reported capacity is the
 * same value as malloc_usable_size, obtained while the allocator lock is held.
 * Failure sets capacity to zero. free_size returns the allocation's capacity
 * BEFORE coalescing, or zero for NULL/an invalid pointer. No size supplied by
 * a caller is trusted, and all ordinary header checks remain enabled. */
void *__libc_malloc_size(size_t bytes, size_t *capacity);
void *__libc_realloc_size(void *ptr, size_t bytes, size_t *capacity);
size_t __libc_free_size(void *ptr);
#endif
