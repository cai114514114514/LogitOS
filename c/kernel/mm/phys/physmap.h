#ifndef LOGIT_PHYSMAP_H
#define LOGIT_PHYSMAP_H

#include <stdint.h>

/* Keep the existing kernel/driver ABI below 1 GiB. Only callers that explicitly
 * request ANY memory may receive a high frame. The 64 TiB direct-map window is
 * an address-layout reservation, not a claim that the flat PMM metadata can
 * manage that much RAM; pmm_init validates the metadata's actual low-RAM fit. */
#define PMM_LOW_LIMIT       0x40000000ull
#define PHYSMAP_BASE        0xffff800000000000ull
#define PHYSMAP_SIZE        0x0000400000000000ull
#define MM_PHYS_INVALID     UINT64_MAX

/* The high window contains only whole AVAILABLE RAM pages from the firmware
 * map, never PCI holes. Low physical addresses keep their original identity
 * alias. pmm_init installs all high mappings before releasing any high frame. */
/* DMA extension: the same window now aliases low AVAILABLE RAM too, while
 * mm_p2v retains its legacy low identity alias. Map pages counts both zones;
 * pmm_high_free_frames remains the separate high-RAM capacity diagnostic. */
/* Low RAM aliases can be ready even if the later high mapping fails. Panic
 * uses this immutable state without taking a lock; RAM provenance alone is
 * available earlier than the mapping and cannot prove a CPU alias is live. */
int      pmm_physmap_low_ready(void);
int      pmm_physmap_ready(void);
uint64_t pmm_physmap_pages(void);
uint64_t pmm_physmap_table_pages(void);

#endif
