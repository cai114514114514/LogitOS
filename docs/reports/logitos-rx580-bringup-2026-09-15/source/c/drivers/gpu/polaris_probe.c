/* RX 580-class bring-up. Register facts follow Linux v6.12:
 * drivers/gpu/drm/amd/amdgpu/{amdgpu_device.c,gmc_v8_0.c,sdma_v3_0.c}
 * and include/asic_reg/{gmc/gmc_8_1_d.h,oss/oss_3_0_d.h}.
 * AMD's generated offsets are DWORD indices; constants below are byte offsets.
 * BAR0 is the CPU VRAM aperture, BAR2 doorbells, BAR5 register MMIO. A 256 MiB
 * aperture is not a 256 MiB card: CONFIG_MEMSIZE is a separate MiB quantity.
 * Firmware/SMU loading, MC/GART ownership, queues, fences and reset are absent;
 * sampling idle/enable bits cannot stand in for any of those transitions. */
#include "polaris_probe.h"
#include "driver.h"
#include "pci.h"
#include <stddef.h>

#define CONFIG_MEMSIZE 0x5428u
#define MC_VM_FB_LOCATION 0x2024u
#define VM_CONTEXT0_CNTL 0x1410u
#define SRBM_STATUS2 0x0e4cu
#define SDMA0_F32_CNTL 0xd048u
#define SDMA1_F32_CNTL 0xd848u
#define SDMA0_GFX_RB_CNTL 0xd200u
#define SDMA1_GFX_RB_CNTL 0xda00u

static int bar_ok(const struct dev_resource *r)
{
    return (r->flags & DEV_RES_MEM) && !(r->flags & DEV_RES_IO) &&
           r->start && r->size && !(r->start & 4095u) &&
           !(r->size & 4095u) && r->size <= UINT64_MAX - r->start &&
           ((r->flags & DEV_RES_64) || r->start + r->size <= (1ull << 32));
}
static int reject(struct polaris_info *out, enum polaris_probe_result reason)
{ out->result = reason; return -1; }

int polaris_probe_readonly(const struct device *d, uint16_t command,
                          uint16_t pmcsr, uint64_t lfb, uint64_t bytes,
                          uint32_t width, uint32_t height,
                          const struct polaris_probe_ops *ops,
                          struct polaris_info *out)
{
    if (!out) return -1;
    *out = (struct polaris_info){0};
    if (!d || d->bus_type != DEV_BUS_PCI || d->header_type ||
        d->vendor != 0x1002u || d->device != POLARIS10_PCI_DEVICE ||
        d->class_code != PCI_CLASS_DISPLAY ||
        (d->subclass != 0 && d->subclass != 2))
        return reject(out, POLARIS_PROBE_ID);
    if (command == UINT16_MAX || !(command & PCI_CMD_MEM))
        return reject(out, POLARIS_PROBE_DECODE);
    if (pmcsr & 3u) return reject(out, POLARIS_PROBE_POWER);
    const struct dev_resource *v = &d->res[0], *m = &d->res[5];
    /* Mapping an arbitrary huge BAR consumes page tables before any read.
     * This bring-up path deliberately caps the register window at 1 MiB. */
    if (!bar_ok(v) || !bar_ok(m) || (m->flags & DEV_RES_64) ||
        (d->res[4].flags & DEV_RES_64) || m->size < SDMA1_GFX_RB_CNTL + 4u ||
        m->size > (1u << 20) ||
        (v->start < m->start + m->size && m->start < v->start + v->size))
        return reject(out, POLARIS_PROBE_BAR);
    /* GOP may pad each scanline. No pixel upload happens here, so preserve
     * that valid boot surface instead of imposing RV100's tight-pitch limit. */
    if (!width || !height || width > 16384u || height > 16384u ||
        bytes % height || bytes / height < (uint64_t)width * 4u ||
        (bytes / height) % 4u || bytes > v->size ||
        lfb < v->start || lfb > UINT64_MAX - bytes ||
        lfb - v->start > v->size - bytes)
        return reject(out, POLARIS_PROBE_SURFACE);
    if (!ops || !ops->map_bar || !ops->read32)
        return reject(out, POLARIS_PROBE_MAP);
    uint64_t base = ops->map_bar(ops->ctx, 5);
    if (!base || (base & 3u) || base > UINTPTR_MAX - m->size)
        return reject(out, POLARIS_PROBE_MAP);
    const uint32_t regs[] = {CONFIG_MEMSIZE, MC_VM_FB_LOCATION, SRBM_STATUS2,
        VM_CONTEXT0_CNTL, SDMA0_F32_CNTL, SDMA1_F32_CNTL,
        SDMA0_GFX_RB_CNTL, SDMA1_GFX_RB_CNTL};
    uint32_t values[8];
    for (unsigned i = 0; i < 8; i++) {
        values[i] = ops->read32(ops->ctx, base, regs[i]);
        out->mmio_reads++;
        if (values[i] == UINT32_MAX) return reject(out, POLARIS_PROBE_READ);
    }
    /* Upper bound is an initial Polaris policy, not a vendor-wide capacity
     * limit. Keep all byte quantities 64-bit so an 8 GiB board stays 8 GiB. */
    if (values[0] < 128u || values[0] > 16384u)
        return reject(out, POLARIS_PROBE_READ);
#ifdef POLARIS_NEGCTL_VRAM32
    out->vram_bytes = values[0] << 20; /* Watched truncation on 4/8 GiB cards. */
#else
    out->vram_bytes = (uint64_t)values[0] << 20;
#endif
    out->cpu_aperture_bytes = v->size;
    out->mc_location = values[1];
    out->mc_base = (uint64_t)(values[1] & 0xffffu) << 24;
    out->mc_end = ((uint64_t)(values[1] >> 16) + 1u) << 24;
    /* BIOS layout can be partial; report disagreement, retain bootfb, and do
     * not manufacture a CPU-aperture-to-GPU address translation from it. */
    out->layout_covers_vram = out->mc_end > out->mc_base &&
                             out->mc_end - out->mc_base >= out->vram_bytes;
    out->srbm_status2 = values[2];
    out->vm_context0 = values[3];
    out->sdma_f32[0] = values[4]; out->sdma_f32[1] = values[5];
    out->sdma_ring[0] = values[6]; out->sdma_ring[1] = values[7];
    out->result = POLARIS_PROBE_OBSERVED;
    return 0;
}

const char *polaris_probe_result_name(enum polaris_probe_result r)
{
    switch (r) {
    case POLARIS_PROBE_NONE: return "not-probed";
    case POLARIS_PROBE_OBSERVED: return "registers-observed";
    case POLARIS_PROBE_ID: return "identity";
    case POLARIS_PROBE_POWER: return "power-state";
    case POLARIS_PROBE_DECODE: return "memory-decode";
    case POLARIS_PROBE_BAR: return "bar-layout";
    case POLARIS_PROBE_SURFACE: return "bootfb-ownership";
    case POLARIS_PROBE_MAP: return "mmio-map";
    case POLARIS_PROBE_READ: return "register-read";
    default: return "unknown";
    }
}
