#ifndef LOGITOS_RV100_BACKUP_ACCEL_H
#define LOGITOS_RV100_BACKUP_ACCEL_H

#include <stdint.h>

#define RV100_VENDOR_ID 0x1002u
#define RV100_DEVICE_ID 0x5159u
#define RV100_CLASS_DISPLAY 0x03u
#define RV100_PCI_CMD_MEM 0x0002u
#define RV100_RES_MEM 0x01u
#define RV100_RES_IO  0x02u

struct rv100_resource {
    uint64_t start;
    uint64_t size;
    uint32_t flags;
};

struct rv100_device {
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t header_type;
    uint8_t pm_cap;       /* 0 means absent. */
    uint16_t command;
    uint16_t pmcsr;
    struct rv100_resource bar[6];
};

enum rv100_stage {
    RV100_OFF = 0,
    RV100_IDENTIFIED,
    RV100_MMIO_MAPPED,
    RV100_VRAM_MAPPED,
    RV100_CANARY_FILL,
    RV100_CANARY_COPY,
    RV100_ACTIVE,
    RV100_BLOCKED,
};

enum rv100_blocker {
    RV100_OK = 0,
    RV100_WRONG_DEVICE,
    RV100_BAD_COMMAND,
    RV100_BAD_PM,
    RV100_BAD_BAR0,
    RV100_BAD_BAR2,
    RV100_BAD_SCANOUT,
    RV100_MAP_FAILED,
    RV100_BAD_VRAM_SIZE,
    RV100_NO_OFFSCREEN_SPACE,
    RV100_ENGINE_TIMEOUT,
    RV100_CACHE_TIMEOUT,
    RV100_FILL_MISMATCH,
    RV100_COPY_MISMATCH,
    RV100_RESTORE_MISMATCH,
    RV100_BAD_SURFACE,
    RV100_BUSY,
};

struct rv100_ops {
    void *ctx;
    uint64_t (*map_bar)(void *ctx, const struct rv100_device *dev, int bar);
    uint32_t (*mmio_read32)(void *ctx, uint64_t base, uint32_t off);
    void (*mmio_write32)(void *ctx, uint64_t base, uint32_t off,
                         uint32_t value);
    uint32_t (*vram_read32)(void *ctx, uint64_t base, uint32_t off);
    void (*vram_write32)(void *ctx, uint64_t base, uint32_t off,
                         uint32_t value);
    uint64_t (*now_ns)(void *ctx);
    void (*relax)(void *ctx);
    void (*barrier)(void *ctx);
};

struct rv100_info {
    enum rv100_stage stage;
    enum rv100_blocker blocker;
    uint32_t vram_bytes;
    uint32_t scanout_offset;
    uint32_t scanout_bytes;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t canary_src;
    uint32_t canary_dst;
    uint32_t staging_offset;
    uint32_t staging_bytes;
    uint64_t presents;
    uint64_t uploaded_bytes;
    uint64_t gpu_pixels;
    uint64_t solid_presents;
    uint64_t copy_presents;
    uint32_t mmio_reads;
    uint32_t mmio_writes;
    uint32_t vram_reads;
    uint32_t vram_writes;
    uint32_t commands;
    uint8_t fill_ok;
    uint8_t copy_ok;
    uint8_t restore_ok;
    uint8_t canary_ready;
    uint8_t present_verified;
    uint8_t software_fallback;
    uint8_t quarantined; /* A submitted command timed out; never CPU-touch it. */
};

struct rv100_context {
    struct rv100_ops ops;
    struct rv100_info info;
    uint64_t mmio_base;
    uint64_t vram_base;
    unsigned lock;
};

int rv100_prepare(struct rv100_context *ctx, const struct rv100_device *dev,
                  uint64_t lfb_phys, uint64_t lfb_bytes,
                  uint32_t width, uint32_t height,
                  const struct rv100_ops *ops);
int rv100_fill(struct rv100_context *ctx, uint32_t x, uint32_t y,
               uint32_t width, uint32_t height, uint32_t color);
int rv100_copy(struct rv100_context *ctx, uint32_t dst_x, uint32_t dst_y,
               uint32_t src_x, uint32_t src_y,
               uint32_t width, uint32_t height);
/* pixels describes the complete CPU back buffer; x/y select damage in it.
 * 0: synchronous GPU completion, -1: no pending GPU write (CPU fallback safe),
 * -2: busy or quarantined (caller must not write the front buffer).
 * The caller owns the source RAM for the duration; stride is in uint32_t words. */
int rv100_present(struct rv100_context *ctx, const uint32_t *pixels,
                  uint32_t stride_pixels, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height);
const char *rv100_blocker_name(enum rv100_blocker blocker);

#endif
