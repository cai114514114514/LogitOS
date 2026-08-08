#ifndef EXECHOST_PMM_H
#define EXECHOST_PMM_H
#include <stdint.h>
uint64_t pmm_alloc(void);
void     pmm_free(uint64_t phys);
#endif
