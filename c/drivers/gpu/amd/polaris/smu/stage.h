#ifndef LOGIT_POLARIS_SMU_STAGE_H
#define LOGIT_POLARIS_SMU_STAGE_H
#include <stddef.h>
#include <stdint.h>
#include "amd/polaris/smu/toc.h"
struct polaris_smu_stage_info {
    uint64_t proposed_gpu_base;
    size_t bytes_used;
    size_t image_offset[2];
    uint32_t image_bytes[2];
    struct polaris_smu_toc_info inventory;
};
/* Build a normal-PF SDMA-only staging image in ordinary RAM. fw0/fw1 must be
 * provenance-checked files for SDMA0/1 respectively: their common headers
 * cannot authenticate the file or identify the engine instance.
 * proposed_gpu_base describes future placement, NOT an allocated mapping.
 * No upload/SMU message follows success. The inventory remains partial and
 * cannot be submitted as the standard Polaris firmware load (RLC/CP absent).
 * Source buffers may alias each other; destination and info must be disjoint
 * from one another and both sources. Failure leaves destination/info intact. */
int polaris_smu_stage_sdma(uint8_t *destination, size_t capacity,
                           uint64_t proposed_gpu_base,
                           const void *fw0, size_t fw0_bytes,
                           const void *fw1, size_t fw1_bytes,
                           struct polaris_smu_stage_info *info);
#endif
