#ifndef AQUA_KHEAP_H
#define AQUA_KHEAP_H

#include <stddef.h>

/* Kernel dynamic memory. Backed by contiguous physical frames from the PMM
 * (identity-mapped). kmalloc must not be called before pmm_init. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

#endif /* AQUA_KHEAP_H */
