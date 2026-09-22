#ifndef LOGIT_RENAME_TEST_HEAP_H
#define LOGIT_RENAME_TEST_HEAP_H

#include <stddef.h>

/* The fixture supplies a counted host heap so it can fail each allocation in
 * the real rename operation without replacing any filesystem operation. */
void *kmalloc(size_t size);
void kfree(void *pointer);

#endif
