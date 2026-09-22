#ifndef EXECHOST_KHEAP_H
#define EXECHOST_KHEAP_H

/* aex.c snapshots signed file input with the kernel heap. Use real host
 * allocation here so its bounds and lifetime remain visible to sanitizers. */
#include <stdlib.h>
static inline void *kmalloc(size_t size) { return malloc(size); }
static inline void kfree(void *pointer) { free(pointer); }

#endif
