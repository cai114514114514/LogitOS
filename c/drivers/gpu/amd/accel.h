#ifndef LOGIT_AMD_ACCEL_H
#define LOGIT_AMD_ACCEL_H

#include <stdint.h>
#include "amd/polaris/device.h"

struct device;

enum amd_accel_family {
    AMD_FAMILY_UNKNOWN = 0,
    AMD_FAMILY_RV100,
    AMD_FAMILY_GCN,
    AMD_FAMILY_POLARIS,
    AMD_FAMILY_VEGA,
    AMD_FAMILY_RDNA,
};

enum amd_accel_stage {
    AMD_ACCEL_OFF = 0,
    AMD_ACCEL_IDENTIFIED,
    AMD_ACCEL_MMIO_READ,
    AMD_ACCEL_CANARY_FILL,
    AMD_ACCEL_CANARY_COPY,
    AMD_ACCEL_ACTIVE,
    AMD_ACCEL_BLOCKED,
};

enum amd_accel_blocker {
    AMD_BLOCK_NONE = 0,
    AMD_BLOCK_WRONG_DEVICE,
    AMD_BLOCK_MODERN_STACK,
    AMD_BLOCK_BAD_COMMAND,
    AMD_BLOCK_BAD_POWER,
    AMD_BLOCK_BAD_BAR,
    AMD_BLOCK_BAD_SCANOUT,
    AMD_BLOCK_MAP_FAILED,
    AMD_BLOCK_BAD_VRAM_SIZE,
    AMD_BLOCK_OFFSCREEN_SPACE,
    AMD_BLOCK_ENGINE_TIMEOUT,
    AMD_BLOCK_CACHE_TIMEOUT,
    AMD_BLOCK_CANARY_FILL,
    AMD_BLOCK_CANARY_COPY,
    AMD_BLOCK_CANARY_RESTORE,
    AMD_BLOCK_SURFACE_BOUNDS,
    AMD_BLOCK_BUSY,
    AMD_BLOCK_POLARIS_INIT,
};

struct amd_accel_info {
    enum amd_accel_stage stage;
    enum amd_accel_blocker blocker;
    enum amd_accel_family family;
    uint16_t pci_device;
    uint32_t vram_bytes;
    uint32_t scanout_offset;
    uint32_t scanout_bytes;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t canary_offset;
    uint32_t mmio_reads;
    uint32_t mmio_writes;
    uint32_t vram_reads;
    uint32_t vram_writes;
    uint32_t commands;
    uint8_t canary_fill_ok;
    uint8_t canary_copy_ok;
    uint8_t canary_restore_ok;
    uint8_t canary_ready;
    uint8_t software_fallback;
    uint8_t quarantined;
    uint64_t presents;
    uint64_t uploaded_bytes;
    uint64_t gpu_pixels;
    struct polaris_info polaris; /* Read-only snapshot, not acceleration. */
};

#define AMD_ACCEL_LINUX_COMMIT \
    "587858367581b9c55c3690f4e63382ad622719d4"
#define AMD_ACCEL_QEMU_11_COMMIT \
    "98b060da3a4f92b2a994ead5b16a87e783baf77c"
#define AMD_ACCEL_CANARY_WIDTH   13u
#define AMD_ACCEL_CANARY_HEIGHT  7u
#define AMD_ACCEL_CANARY_PITCH   64u
#define AMD_ACCEL_CANARY_TRIGGER 0x0007000du

enum amd_accel_family amd_accel_family_for_pci(uint16_t vendor,
                                               uint16_t device,
                                               uint8_t class_code,
                                               uint8_t subclass);
const char *amd_accel_family_name(enum amd_accel_family family);
const char *amd_accel_blocker_name(enum amd_accel_blocker blocker);

/* Product wrapper. The former RV100-only mapping policy is extended with an
 * exact 1002:67df read-only BAR5 snapshot. This automatic entry only submits
 * RV100 commands; polaris/desktop.h exposes the separate owned-resource entry.
 * Lifecycle/query and synchronous present all share fb's graphics mutex.
 * A quarantined engine cannot be rearmed by a software reset. */
int amd_accel_prepare(struct device *dev, uint64_t lfb_phys,
                      uint64_t lfb_bytes, uint32_t width, uint32_t height);
int amd_accel_query(struct amd_accel_info *out);
void amd_accel_reset(void);

#endif
