#ifndef LOGIT_KHEAP_H
#define LOGIT_KHEAP_H
/* Host stub: the kernel heap is the host heap. Deliberately real malloc/free so
 * the fs tests can run under ASan and see a leak or an overflow. */
#include <stdlib.h>
static inline void *kmalloc(size_t n) { return malloc(n); }
static inline void  kfree(void *p)    { free(p); }
#endif
