#ifndef LOGIT_NVIDIA_PASCAL_ACCEL_H
#define LOGIT_NVIDIA_PASCAL_ACCEL_H

#include <stddef.h>
#include <stdint.h>

struct device;

/* GP107 acceleration is deliberately split into observable stages.  A caller
 * may use fill/copy only at ACTIVE; every earlier stage means the existing CPU
 * framebuffer remains authoritative.  In particular, VERIFIED_FIRMWARE and
 * IDENTIFIED are prerequisites, not acceleration claims. */
enum nv1050_accel_stage {
    NV1050_ACCEL_OFF = 0,
    NV1050_ACCEL_FW_VERIFIED,
    NV1050_ACCEL_PRI_MAPPED,
    NV1050_ACCEL_IDENTIFIED,
    NV1050_ACCEL_ACTIVE,
    NV1050_ACCEL_BLOCKED,
};

enum nv1050_accel_blocker {
    NV1050_BLOCK_NONE = 0,
    NV1050_BLOCK_WRONG_DEVICE,
    NV1050_BLOCK_BAD_BAR0,
    NV1050_BLOCK_FW_MISSING,
    NV1050_BLOCK_FW_SIZE,
    NV1050_BLOCK_FW_HASH,
    NV1050_BLOCK_FW_IO,
    NV1050_BLOCK_MAP_FAILED,
    NV1050_BLOCK_WRONG_CHIPSET,
    NV1050_BLOCK_CHANNEL_STACK,
};

struct nv1050_accel_info {
    enum nv1050_accel_stage stage;
    enum nv1050_accel_blocker blocker;
    uint32_t firmware_files;
    uint32_t firmware_verified;
    uint32_t boot0;
    uint32_t boot1;
    uint32_t pmc_enable;
    uint16_t pci_device;
    uint16_t chipset;
    uint8_t chiprev;
    uint8_t mmio_reads;
    uint8_t mmio_writes;
    uint8_t software_fallback;
};

/* Pinned linux-firmware entry.  sha256_hex is lower-case and NUL terminated.
 * Paths are relative to /lib/firmware. */
struct nv1050_fw_manifest_entry {
    const char *path;
    uint32_t size;
    const char *sha256_hex;
};

#define NV1050_FW_MANIFEST_COUNT 22u
#define NV1050_LINUX_FIRMWARE_COMMIT \
    "1522c78ab870b3c051d8a3a1d24ecbbc12b23be5"
#define NV1050_LINUX_NOUVEAU_COMMIT \
    "587858367581b9c55c3690f4e63382ad622719d4"

size_t nv1050_fw_manifest_count(void);
const struct nv1050_fw_manifest_entry *nv1050_fw_manifest_at(size_t index);

/* The reader returns 0 and the measured size/digest on success.  It returns
 * NV1050_BLOCK_FW_MISSING, _SIZE or _HASH on a fail-closed read error.  Tests
 * inject this seam so absence and corruption can be proved to precede mapping. */
struct nv1050_fw_reader {
    void *ctx;
    int (*measure)(void *ctx, const struct nv1050_fw_manifest_entry *entry,
                   uint32_t *measured_size, uint8_t sha256[32]);
};

/* The hardware seam is also testable: map_bar0 may create a CPU mapping, while
 * read32 is the only operation the bring-up stage is allowed to perform. */
struct nv1050_hw_reader {
    void *ctx;
    uint64_t (*map_bar0)(void *ctx, struct device *dev);
    uint32_t (*read32)(void *ctx, uint64_t base, uint32_t offset);
};

int nv1050_accel_prepare_with_ops(struct device *dev,
                                  const struct nv1050_fw_reader *firmware,
                                  const struct nv1050_hw_reader *hardware,
                                  struct nv1050_accel_info *out);
int nvidia_pascal_accel_prepare(struct device *dev);
int nvidia_pascal_accel_query(struct nv1050_accel_info *out);
void nvidia_pascal_accel_reset(void);

/* These return -1 until an off-screen CE canary has completed and the visible
 * scanout mapping is owned.  BAR1 CPU stores are not an implementation. */
int nvidia_pascal_accel_fill(uint64_t dst_gpu_va, uint32_t dst_pitch,
                             uint32_t width, uint32_t height, uint32_t color);
int nvidia_pascal_accel_copy(uint64_t dst_gpu_va, uint32_t dst_pitch,
                             uint64_t src_gpu_va, uint32_t src_pitch,
                             uint32_t width, uint32_t height);

const char *nvidia_pascal_accel_blocker_name(enum nv1050_accel_blocker blocker);

#endif
