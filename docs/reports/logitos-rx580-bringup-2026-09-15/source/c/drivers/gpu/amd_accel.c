/* AMD acceleration coverage.
 *
 * The vendor-wide display path lives in amd_bootfb.c.  Modern AMD GPUs need
 * firmware, GPUVM, rings, fences and reset handling. Previously all stopped
 * before BAR mapping; exact Polaris10 67df now gets a read-only BAR5 snapshot.
 * The one native command path is QEMU's auditable RV100 model
 * (1002:5159), whose legacy 2D engine presents dirty rectangles after an offscreen canary.
 * The compositor still renders through its CPU back buffer; ACTIVE means the
 * RV100 2D engine passed its destructive-state-free startup canary. Runtime
 * presents are counted separately after engine completion and first readback.
 */
#include <stddef.h>
#include <stdint.h>

#include "amd_accel.h"
#include "driver.h"
#include "kprintf.h"
#include "ktime.h"
#include "pci.h"
#include "rv100_accel.h"
#include "fb.h"

#define AMD_VENDOR_ID 0x1002u
#define RV100_DEVICE_ID 0x5159u
#define PCI_CAP_PM 0x01u
#define PCI_PMCSR  0x04u

static struct rv100_context active;
#ifndef AMD_PRESENT_DISABLE
static int native_present(const uint32_t *, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t);
#endif
static struct amd_accel_info active_info = {
    .stage = AMD_ACCEL_OFF,
    .software_fallback = 1,
};

static void zero_bytes(void *ptr, size_t bytes)
{
    uint8_t *p = ptr;
    while (bytes--) *p++ = 0;
}

enum amd_accel_family amd_accel_family_for_pci(uint16_t vendor,
                                               uint16_t device,
                                               uint8_t class_code,
                                               uint8_t subclass)
{
    if (vendor != AMD_VENDOR_ID || class_code != PCI_CLASS_DISPLAY ||
        (subclass != 0x00u && subclass != 0x02u && subclass != 0x80u))
        return AMD_FAMILY_UNKNOWN;
    switch (device) {
    case RV100_DEVICE_ID: return AMD_FAMILY_RV100;
    case 0x6780u: case 0x6818u: return AMD_FAMILY_GCN;
    case 0x67dfu: case 0x6987u: return AMD_FAMILY_POLARIS;
    case 0x6863u: case 0x69afu: return AMD_FAMILY_VEGA;
    case 0x731fu: case 0x73bfu: case 0x744cu: return AMD_FAMILY_RDNA;
    default: return AMD_FAMILY_UNKNOWN;
    }
}

const char *amd_accel_family_name(enum amd_accel_family family)
{
    switch (family) {
    case AMD_FAMILY_RV100: return "RV100";
    case AMD_FAMILY_GCN: return "GCN";
    case AMD_FAMILY_POLARIS: return "POLARIS";
    case AMD_FAMILY_VEGA: return "VEGA";
    case AMD_FAMILY_RDNA: return "RDNA";
    default: return "UNKNOWN";
    }
}

const char *amd_accel_blocker_name(enum amd_accel_blocker blocker)
{
    switch (blocker) {
    case AMD_BLOCK_NONE: return "none";
    case AMD_BLOCK_WRONG_DEVICE: return "wrong-device";
    case AMD_BLOCK_MODERN_STACK: return "firmware-gpuvm-ring-fence";
    case AMD_BLOCK_BAD_COMMAND: return "memory-decode";
    case AMD_BLOCK_BAD_POWER: return "power-state";
    case AMD_BLOCK_BAD_BAR: return "bad-bar";
    case AMD_BLOCK_BAD_SCANOUT: return "bad-scanout";
    case AMD_BLOCK_MAP_FAILED: return "map-failed";
    case AMD_BLOCK_BAD_VRAM_SIZE: return "vram-size";
    case AMD_BLOCK_OFFSCREEN_SPACE: return "offscreen-space";
    case AMD_BLOCK_ENGINE_TIMEOUT: return "engine-timeout";
    case AMD_BLOCK_CACHE_TIMEOUT: return "cache-timeout";
    case AMD_BLOCK_CANARY_FILL: return "fill-readback";
    case AMD_BLOCK_CANARY_COPY: return "copy-readback";
    case AMD_BLOCK_CANARY_RESTORE: return "restore-readback";
    case AMD_BLOCK_SURFACE_BOUNDS: return "surface-bounds";
    case AMD_BLOCK_BUSY: return "command-busy";
    case AMD_BLOCK_POLARIS_INIT: return "polaris-smu-gart-ring-fence-reset";
    default: return "unknown";
    }
}

static enum amd_accel_blocker translate_blocker(enum rv100_blocker blocker)
{
    switch (blocker) {
    case RV100_OK: return AMD_BLOCK_NONE;
    case RV100_WRONG_DEVICE: return AMD_BLOCK_WRONG_DEVICE;
    case RV100_BAD_COMMAND: return AMD_BLOCK_BAD_COMMAND;
    case RV100_BAD_PM: return AMD_BLOCK_BAD_POWER;
    case RV100_BAD_BAR0: case RV100_BAD_BAR2: return AMD_BLOCK_BAD_BAR;
    case RV100_BAD_SCANOUT: return AMD_BLOCK_BAD_SCANOUT;
    case RV100_MAP_FAILED: return AMD_BLOCK_MAP_FAILED;
    case RV100_BAD_VRAM_SIZE: return AMD_BLOCK_BAD_VRAM_SIZE;
    case RV100_NO_OFFSCREEN_SPACE: return AMD_BLOCK_OFFSCREEN_SPACE;
    case RV100_ENGINE_TIMEOUT: return AMD_BLOCK_ENGINE_TIMEOUT;
    case RV100_CACHE_TIMEOUT: return AMD_BLOCK_CACHE_TIMEOUT;
    case RV100_FILL_MISMATCH: return AMD_BLOCK_CANARY_FILL;
    case RV100_COPY_MISMATCH: return AMD_BLOCK_CANARY_COPY;
    case RV100_RESTORE_MISMATCH: return AMD_BLOCK_CANARY_RESTORE;
    case RV100_BAD_SURFACE: return AMD_BLOCK_SURFACE_BOUNDS;
    case RV100_BUSY: return AMD_BLOCK_BUSY;
    default: return AMD_BLOCK_ENGINE_TIMEOUT;
    }
}

static enum amd_accel_stage translate_stage(enum rv100_stage stage)
{
    switch (stage) {
    case RV100_IDENTIFIED: return AMD_ACCEL_IDENTIFIED;
    case RV100_MMIO_MAPPED: case RV100_VRAM_MAPPED: return AMD_ACCEL_MMIO_READ;
    case RV100_CANARY_FILL: return AMD_ACCEL_CANARY_FILL;
    case RV100_CANARY_COPY: return AMD_ACCEL_CANARY_COPY;
    case RV100_ACTIVE: return AMD_ACCEL_ACTIVE;
    case RV100_BLOCKED: return AMD_ACCEL_BLOCKED;
    default: return AMD_ACCEL_OFF;
    }
}

static void publish_info(enum amd_accel_family family, uint16_t pci_device)
{
    const struct rv100_info *r = &active.info;
    zero_bytes(&active_info, sizeof active_info);
    active_info.stage = translate_stage(r->stage);
    active_info.blocker = translate_blocker(r->blocker);
    active_info.family = family;
    active_info.pci_device = pci_device;
    active_info.vram_bytes = r->vram_bytes;
    active_info.scanout_offset = r->scanout_offset;
    active_info.scanout_bytes = r->scanout_bytes;
    active_info.pitch = r->pitch;
    active_info.width = r->width;
    active_info.height = r->height;
    active_info.canary_offset = r->canary_src;
    active_info.mmio_reads = r->mmio_reads;
    active_info.mmio_writes = r->mmio_writes;
    active_info.vram_reads = r->vram_reads;
    active_info.vram_writes = r->vram_writes;
    active_info.commands = r->commands;
    active_info.canary_fill_ok = r->fill_ok;
    active_info.canary_copy_ok = r->copy_ok;
    active_info.canary_restore_ok = r->restore_ok;
    active_info.canary_ready = r->canary_ready;
    active_info.quarantined = r->quarantined;
    active_info.presents = r->presents;
    active_info.uploaded_bytes = r->uploaded_bytes;
    active_info.gpu_pixels = r->gpu_pixels;
    /* The startup canary is real; the desktop still uses its CPU back buffer. */
    /* CPU rasterization remains in use; successful presents use the GPU hook. */
    active_info.software_fallback = 1;
}

static uint64_t product_map(void *ctx, const struct rv100_device *unused, int bar)
{
    (void)unused;
    return dev_bar_map((struct device *)ctx, bar);
}
static uint32_t product_mmio_read(void *ctx, uint64_t base, uint32_t off)
{ (void)ctx; return *(volatile uint32_t *)(uintptr_t)(base + off); }
static uint64_t polaris_map(void *ctx, int bar)
{ return dev_bar_map((struct device *)ctx, bar); }
static void product_mmio_write(void *ctx, uint64_t base, uint32_t off, uint32_t val)
{ (void)ctx; *(volatile uint32_t *)(uintptr_t)(base + off) = val; }
static uint32_t product_vram_read(void *ctx, uint64_t base, uint32_t off)
{ (void)ctx; return *(volatile uint32_t *)(uintptr_t)(base + off); }
static void product_vram_write(void *ctx, uint64_t base, uint32_t off, uint32_t val)
{ (void)ctx; *(volatile uint32_t *)(uintptr_t)(base + off) = val; }
static uint64_t product_now(void *ctx)
{ (void)ctx; return time_mono_ns(); }
static void product_relax(void *ctx)
{
    (void)ctx;
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
static void product_barrier(void *ctx)
{ (void)ctx; __sync_synchronize(); }

static void log_state(void)
{
    if (active_info.stage == AMD_ACCEL_ACTIVE) {
        kprintf("[amd-accel] family=%s pci=%04x stage=active "
                "engine=rv100-2d-canary-passed "
                "desktop=cpu commands=%u vram=%u scanout=%u canary=%u "
                "reads=%u writes=%u fill=ok copy=ok restore=ok\n",
                amd_accel_family_name(active_info.family), active_info.pci_device,
                active_info.commands, active_info.vram_bytes,
                active_info.scanout_offset, active_info.canary_offset,
                active_info.vram_reads, active_info.vram_writes);
    } else {
        kprintf("[amd-accel] family=%s pci=%04x stage=blocked blocker=%s "
                "commands=%u desktop=cpu\n",
                amd_accel_family_name(active_info.family), active_info.pci_device,
                amd_accel_blocker_name(active_info.blocker), active_info.commands);
    }
}

static int prepare_locked(struct device *dev, uint64_t lfb_phys,
                      uint64_t lfb_bytes, uint32_t width, uint32_t height)
{
    /* A device remove/probe must not erase evidence of an in-flight command.
     * Only a real hardware reset may recover that state (not implemented). */
    if (active.info.quarantined) return -1;
    fb_set_native_present(NULL);
    zero_bytes(&active, sizeof active);
    zero_bytes(&active_info, sizeof active_info);
    enum amd_accel_family family = dev ?
        amd_accel_family_for_pci(dev->vendor, dev->device,
                                 dev->class_code, dev->subclass) :
        AMD_FAMILY_UNKNOWN;
    active_info.family = family;
    active_info.pci_device = dev ? dev->device : 0;
    active_info.stage = AMD_ACCEL_BLOCKED;
    active_info.blocker = AMD_BLOCK_WRONG_DEVICE;
    /* CPU rasterization remains in use; successful presents use the GPU hook. */
    active_info.software_fallback = 1;

    if (dev && dev->bus_type == DEV_BUS_PCI && dev->vendor == AMD_VENDOR_ID &&
        dev->device == POLARIS10_PCI_DEVICE && dev->header_type == 0 &&
        dev->class_code == PCI_CLASS_DISPLAY &&
        (dev->subclass == 0 || dev->subclass == 2)) {
        uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                         PCI_CFG_COMMAND);
        uint8_t pm = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_PM);
        uint16_t power = !pm ? 0 : pm > 0xfbu ? 3 :
            pci_cfg_read16(dev->bus, dev->slot, dev->func, (uint16_t)(pm + 4));
        struct polaris_probe_ops ops = {
            .ctx = dev, .map_bar = polaris_map, .read32 = product_mmio_read,
        };
        int rc = polaris_probe_readonly(dev, command, power, lfb_phys, lfb_bytes,
                                       width, height, &ops, &active_info.polaris);
#if defined(POLARIS_INTEGRATION_NEGCTL_REGISTER_PRESENTER) && !defined(AMD_PRESENT_DISABLE)
        /* Watched mutation: a register snapshot must not enable rendering. */
        if (!rc) fb_set_native_present(native_present);
#endif
        const struct polaris_info *p = &active_info.polaris;
        active_info.stage = rc ? AMD_ACCEL_BLOCKED : AMD_ACCEL_MMIO_READ;
        active_info.blocker = AMD_BLOCK_POLARIS_INIT;
        active_info.mmio_reads = p->mmio_reads;
        kprintf("[amd-polaris] pci=%04x revision=%02x stage=%s "
                "vram_mib=%llu aperture_mib=%llu mc=%08x layout=%s "
                "srbm=%08x vm0=%08x f32=%08x/%08x ring=%08x/%08x "
                "reads=%u commands=0 acceleration=unavailable\n",
                dev->device, dev->revision, polaris_probe_result_name(p->result),
                (unsigned long long)(p->vram_bytes >> 20),
                (unsigned long long)(p->cpu_aperture_bytes >> 20),
                p->mc_location, p->layout_covers_vram ? "covers-vram" : "unverified",
                p->srbm_status2, p->vm_context0,
                p->sdma_f32[0], p->sdma_f32[1], p->sdma_ring[0], p->sdma_ring[1],
                p->mmio_reads);
        /* Observation succeeds independently from engine bring-up. Returning
         * success here would make callers announce a passed 2D canary. */
        return -1;
    }

    /* The exact identity check is deliberately before BAR mapping or config
     * mutation.  Modern GCN/RDNA devices need a different complete driver. */
    if (!dev || dev->bus_type != DEV_BUS_PCI || dev->vendor != AMD_VENDOR_ID ||
        dev->class_code != PCI_CLASS_DISPLAY ||
        (dev->subclass != 0x00u && dev->subclass != 0x02u) ||
        dev->header_type != 0 || dev->device != RV100_DEVICE_ID) {
        if (family == AMD_FAMILY_GCN || family == AMD_FAMILY_POLARIS ||
            family == AMD_FAMILY_VEGA || family == AMD_FAMILY_RDNA)
            active_info.blocker = AMD_BLOCK_MODERN_STACK;
        log_state();
        return -1;
    }

    struct rv100_device card;
    zero_bytes(&card, sizeof card);
    card.vendor = dev->vendor;
    card.device = dev->device;
    card.class_code = dev->class_code;
    card.subclass = dev->subclass;
    card.header_type = dev->header_type;
    card.command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                  PCI_CFG_COMMAND);
    card.pm_cap = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_PM);
    if (card.pm_cap && card.pm_cap <= 0xfbu)
        card.pmcsr = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                    (uint16_t)(card.pm_cap + PCI_PMCSR));
    else if (card.pm_cap)
        card.pmcsr = 3u;
    for (int i = 0; i < DEV_NRES; i++) {
        card.bar[i].start = dev->res[i].start;
        card.bar[i].size = dev->res[i].size;
        card.bar[i].flags = dev->res[i].flags;
    }

    struct rv100_ops ops = {
        .ctx = dev,
        .map_bar = product_map,
        .mmio_read32 = product_mmio_read,
        .mmio_write32 = product_mmio_write,
        .vram_read32 = product_vram_read,
        .vram_write32 = product_vram_write,
        .now_ns = product_now,
        .relax = product_relax,
        .barrier = product_barrier,
    };
    int rc = rv100_prepare(&active, &card, lfb_phys, lfb_bytes,
                           width, height, &ops);
    publish_info(family, dev->device);
    log_state();
#ifndef AMD_PRESENT_DISABLE
    if (!rc) fb_set_native_present(native_present);
#endif
    return rc;
}

int amd_accel_prepare(struct device *dev, uint64_t lfb_phys,
                      uint64_t lfb_bytes, uint32_t width, uint32_t height)
{
    fb_graphics_lock();
    int rc = prepare_locked(dev, lfb_phys, lfb_bytes, width, height);
    fb_graphics_unlock();
    return rc;
}

#ifndef AMD_PRESENT_DISABLE
static int native_present(const uint32_t *pixels, uint32_t stride,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* fb holds the same mutex for the callback and excludes AP front writers.
     * Keep this private: exposing commands outside that lease would allow a
     * CPU cursor write to race an outstanding GPU blit. */
    int rc = rv100_present(&active, pixels, stride, x, y, w, h);
    publish_info(active_info.family, active_info.pci_device);
    if (!rc) {
        uint64_t n = active.info.presents;
        if (!(n & (n - 1)))
            kprintf("[amd-present] completed=%llu pixels=%llu commands=%u "
                    "upload_bytes=%llu scanout=%u scratch=%u pitch=%u "
                    "rects=%llu verified=%u\n",
                    (unsigned long long)n,
                    (unsigned long long)active.info.gpu_pixels,
                    active.info.commands,
                    (unsigned long long)active.info.uploaded_bytes,
                    active.info.scanout_offset, active.info.staging_offset,
                    active.info.pitch, (unsigned long long)n,
                    active.info.present_verified);
    } else if (rc == -2) {
        kprintf("[amd-present] quarantined=1 blocker=%s CPU-front-writes=stopped\n",
                rv100_blocker_name(active.info.blocker));
    }
    return rc;
}
#endif

int amd_accel_query(struct amd_accel_info *out)
{
    if (!out) return -1;
    fb_graphics_lock();
    *out = active_info;
    fb_graphics_unlock();
    return 0;
}

void amd_accel_reset(void)
{
    fb_graphics_lock();
    if (active.info.quarantined) {
        fb_graphics_unlock();
        return;
    }
    fb_set_native_present(NULL);
    zero_bytes(&active, sizeof active);
    zero_bytes(&active_info, sizeof active_info);
    active_info.stage = AMD_ACCEL_OFF;
    /* CPU rasterization remains in use; successful presents use the GPU hook. */
    active_info.software_fallback = 1;
    fb_graphics_unlock();
}
