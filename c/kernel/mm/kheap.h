#ifndef LOGIT_KHEAP_H
#define LOGIT_KHEAP_H

#include <stddef.h>

/* Kernel dynamic memory. Backed by contiguous physical frames from the PMM
 * (identity-mapped). kmalloc must not be called before pmm_init. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

#endif /* LOGIT_KHEAP_H */
