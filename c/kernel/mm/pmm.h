#ifndef LOGIT_PMM_H
#define LOGIT_PMM_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_SIZE 4096

/* Initialise the physical frame allocator from the Multiboot2 info block. */
void pmm_init(uint64_t mb_info_addr);

/* Allocate / free a single 4 KiB physical frame. Returns 0 on failure. */
uint64_t pmm_alloc(void);
void     pmm_free(uint64_t phys_addr);

/* Allocate `n` contiguous frames; returns the base physical address or 0. */
uint64_t pmm_alloc_contig(size_t n);

uint64_t pmm_total_bytes(void);   /* usable RAM reported by firmware */
uint64_t pmm_free_bytes(void);    /* currently free */

#endif /* LOGIT_PMM_H */
