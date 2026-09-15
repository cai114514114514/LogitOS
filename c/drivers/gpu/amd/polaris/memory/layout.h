#ifndef LOGIT_POLARIS_MEMORY_LAYOUT_H
#define LOGIT_POLARIS_MEMORY_LAYOUT_H
#include <stddef.h>
#include <stdint.h>
#define POLARIS_MEMORY_PAGE 4096u
#define POLARIS_MEMORY_LIMIT (UINT64_C(1) << 40)
#define POLARIS_MEMORY_MAX_RESERVED 64u
#define POLARIS_SMU_SCRATCH_BYTES 819200u
struct polaris_memory_range { uint64_t base, bytes; };
enum polaris_memory_object {
    POLARIS_MEMORY_TOC, POLARIS_MEMORY_SMU_SCRATCH, POLARIS_MEMORY_FIRMWARE,
    POLARIS_MEMORY_RING, POLARIS_MEMORY_FENCE, POLARIS_MEMORY_STAGING,
    POLARIS_MEMORY_OBJECTS
};
struct polaris_memory_request {
    struct polaris_memory_range vram, aperture, arena, scanout;
    uint64_t aperture_cpu_base;
    const struct polaris_memory_range *reserved;
    size_t reserved_count;
    uint64_t firmware_bytes, staging_bytes;
};
struct polaris_memory_allocation {
    uint64_t gpu_address, cpu_physical, aperture_offset, bytes;
};
struct polaris_memory_layout {
    struct polaris_memory_allocation object[POLARIS_MEMORY_OBJECTS];
    uint64_t bytes_used;
};
/* All ranges are half-open GPU MC addresses, not PCI BAR offsets. The caller
 * must own arena exclusively and provide a complete inventory of firmware/
 * driver reservations. Discovering BAR size or MC_FB_LOCATION does NOT grant
 * this ownership: an unknown firmware reservation means do not call this API.
 * We use a CPU-visible, VRAM-only arena to avoid bootstrapping GART with its
 * own unestablished mapping. scanout is always protected separately.
 * Planning performs no allocation/MMIO; publish the result only while holding
 * the GPU ownership lock. Failure preserves output; input/output must not race.
 */
int polaris_memory_plan(const struct polaris_memory_request *,
                        struct polaris_memory_layout *);
#endif
