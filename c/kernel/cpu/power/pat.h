#ifndef LOGIT_PAT_H
#define LOGIT_PAT_H
#include <stdint.h>
/* 4KiB PTE PAT:PCD:PWT = 001 selects PA1. Kept here because this track may
 * not edit vmm.h. Consumers of the WC mapping include this header too. */
#define VMM_PAT_WC (1ull << 3)
void pat_init(void);
/* 1 once pat_init() has programmed PA1 = WC (read back) on the boot CPU. The
 * framebuffer maps WC only then; every AP runs pat_init() before it starts
 * scheduling, so no CPU uses the old PA1 = WT for that mapping. */
int  pat_wc_available(void);
void *vmm_map_mmio_wc(uint64_t phys, uint64_t len);
#endif
