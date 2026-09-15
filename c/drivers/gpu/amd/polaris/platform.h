#ifndef LOGIT_POLARIS_IO_H
#define LOGIT_POLARIS_IO_H
#include <stdint.h>
#include "amd/polaris/sdma/queue.h"

/* A single exclusive device lease backs every callback until shutdown/reset.
 * Bounded ordered device accesses, and a real GPU/CPU translation query, are
 * required. There is intentionally no 'ready' callback or address-cast default.
 * BAR mappings must be uncached/device memory; sync includes HDP visibility. */
struct polaris_platform {
    void *opaque;
    int (*read_identity)(void *, uint32_t *vendor_device);
    int (*read_reg)(void *, uint32_t byte_offset, uint32_t *);
    int (*write_reg)(void *, uint32_t byte_offset, uint32_t);
    int (*resolve_mapping)(void *, uint64_t gpu_address, uint64_t bytes,
                           volatile uint8_t **cpu);
    int (*sync)(void *, enum polaris_sdma_sync, uint64_t gpu_address, uint64_t bytes);
    uint64_t (*now_us)(void *);
};
#endif
