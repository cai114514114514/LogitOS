/* Passive boot-display support for the GeForce GTX 1050 family.
 *
 * This driver intentionally does less than a native NVIDIA display driver.  A
 * GTX 1050 has already been initialised by its VBIOS/UEFI GOP before LogitOS
 * starts.  GRUB or our UEFI loader describes that existing linear framebuffer
 * with Multiboot2 tag 8, and fb.c maps it.  Reclocking GP107, loading firmware,
 * programming display heads, or creating graphics channels without NVIDIA's
 * full initialisation sequence can turn a working boot display black.  The
 * original probe therefore performed PCI CONFIGURATION READS only.  It still
 * verifies that a live
 * display-class function still has memory decoding, is in D0 when it exposes
 * PCI PM, and owns at least one sane memory BAR, then binds as an observer.
 *
 * It still does not call dev_enable(), request an IRQ, enable bus mastering or
 * write GPU MMIO.  After every passive check succeeds, nvidia_pascal_accel.c
 * may map BAR0 and read BOOT0/BOOT1/PMC_ENABLE, but only after all 22 pinned
 * firmware files pass size and SHA-256.  The CPU keeps writing the firmware
 * framebuffer fb.c already selected.  fb_boot_lfb_range() supplies the missing
 * provenance: the complete boot framebuffer must lie inside one of this PCI
 * function's decoded memory BARs.  This stops a secondary/3D-only NVIDIA GPU
 * from binding merely because another GPU happens to own a working screen.
 *
 * Supported IDs come from NVIDIA's own Linux driver supported-products table,
 * release 595.45.04.  Adjacent Pascal IDs are deliberately absent: 1c90 is an
 * MX150, for example, and accepting the whole 1cxx range would make dev_dump()
 * report a driver that has never even identified the hardware correctly.
 */
#include <stdint.h>
#include <stddef.h>
#include "nvidia_pascal.h"
#include "nvidia_pascal_accel.h"
#include "driver.h"
#include "pci.h"
#include "fb.h"
#include "kprintf.h"

#define NVIDIA_VENDOR_ID 0x10deu
#define PCI_CAP_PM        0x01u
#define PCI_PMCSR         0x04u

struct nv1050_id {
    uint16_t id;
    const char *name;
};

static const struct nv1050_id nv1050_ids[] = {
    { 0x1c21, "GeForce GTX 1050 Ti" },
    { 0x1c22, "GeForce GTX 1050" },
    { 0x1c61, "GeForce GTX 1050 Ti" },
    { 0x1c62, "GeForce GTX 1050" },
    { 0x1c81, "GeForce GTX 1050" },
    { 0x1c82, "GeForce GTX 1050 Ti" },
    { 0x1c83, "GeForce GTX 1050" },
    { 0x1c8c, "GeForce GTX 1050 Ti" },
    { 0x1c8d, "GeForce GTX 1050" },
    { 0x1c8f, "GeForce GTX 1050 Ti" },
    { 0x1c91, "GeForce GTX 1050" },
    { 0x1c92, "GeForce GTX 1050" },
};

const char *nvidia_pascal_model_name(uint16_t id)
{
#ifdef NVIDIA_PASCAL_NEGCTL_BROAD_ID
    /* The negative gate restores the tempting family-wide match and watches
     * an MX150 (1c90) get misidentified. */
    if (id == 0x1c90) return "GeForce GTX 1050 (unsafe wildcard)";
#endif
    for (unsigned i = 0; i < sizeof nv1050_ids / sizeof nv1050_ids[0]; i++)
        if (nv1050_ids[i].id == id) return nv1050_ids[i].name;
    return NULL;
}

static int memory_bar_sane(const struct dev_resource *r)
{
    if (!(r->flags & DEV_RES_MEM) || !r->start || !r->size) return 0;
    return r->start + r->size >= r->start; /* reject a wrapped aperture */
}

static int is_synthetic_qemu_vga(const struct device *dev)
{
#ifdef NVIDIA_PASCAL_QEMU_TEST
    /* 1234:1111 is QEMU stdvga.  This alias exists only in an isolated test
     * kernel and is logged as synthetic; it proves the declarative driver,
     * passive probe, GRUB LFB and UEFI GOP paths stay joined.  It is not
     * evidence that QEMU emulated a GP107. */
    return dev->vendor == 0x1234 && dev->device == 0x1111;
#else
    (void)dev;
    return 0;
#endif
}

int nvidia_pascal_bootfb_probe(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI) return -1;
    int synthetic = is_synthetic_qemu_vga(dev);
    const char *model = synthetic ? "QEMU stdvga (synthetic test alias)" :
                                    nvidia_pascal_model_name(dev->device);
    if ((!synthetic && dev->vendor != NVIDIA_VENDOR_ID) || !model) return -1;

    /* 03.00 is VGA-compatible; 03.02 is the 3D-controller form used by some
     * switchable-graphics machines.  A matching numeric ID on another PCI
     * function is not a display and must remain unclaimed. */
    if (dev->class_code != PCI_CLASS_DISPLAY ||
        (dev->subclass != 0x00 && dev->subclass != 0x02) ||
        dev->header_type != 0) {
        kprintf("[nv-bootfb] %s %04x:%04x refused: not a type-0 VGA/3D display\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                      PCI_CFG_COMMAND);
    if (!(command & PCI_CMD_MEM)) {
        /* Turning decode back on is not recovery: firmware may have powered
         * the board down or moved its apertures.  Stay passive and leave the
         * function free for a future native driver. */
        kprintf("[nv-bootfb] %s %04x:%04x refused: PCI memory decode is off\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    uint8_t pm = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_PM);
    unsigned power_state = 0;
    if (pm) {
        power_state = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                     (uint16_t)(pm + PCI_PMCSR)) & 3u;
#ifndef NVIDIA_PASCAL_NEGCTL_ACCEPT_D3
        if (power_state != 0) {
            /* A config write to D0 needs device-specific restore delays and
             * state recovery.  Reading a firmware framebuffer through a GPU
             * still in D3 is not a supported configuration. */
            kprintf("[nv-bootfb] %s %04x:%04x refused: PCI power state D%u\n",
                    dev->name, dev->vendor, dev->device, power_state);
            return -1;
        }
#endif
    }

    unsigned memory_bars = 0;
    for (int i = 0; i < DEV_NRES; i++)
        if (memory_bar_sane(&dev->res[i])) memory_bars++;
    if (!memory_bars) {
        kprintf("[nv-bootfb] %s %04x:%04x refused: no sane memory BAR\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    uint32_t width = fb_width(), height = fb_height();
    uint64_t lfb = 0, lfb_bytes = 0;
    if (!width || !height || !fb_boot_lfb_range(&lfb, &lfb_bytes) || !lfb_bytes ||
        lfb + lfb_bytes < lfb) {
        kprintf("[nv-bootfb] %s %04x:%04x refused: no valid Multiboot LFB\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    int lfb_bar = -1;
    uint64_t lfb_end = lfb + lfb_bytes;
    for (int i = 0; i < DEV_NRES; i++) {
        const struct dev_resource *r = &dev->res[i];
        if (!memory_bar_sane(r)) continue;
        uint64_t bar_end = r->start + r->size;
        if (lfb >= r->start && lfb_end <= bar_end) {
            lfb_bar = i;
            break;
        }
    }
    if (lfb_bar < 0) {
        kprintf("[nv-bootfb] %s %04x:%04x refused: LFB %p/%p is outside its BARs\n",
                dev->name, dev->vendor, dev->device,
                (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes);
        return -1;
    }

    kprintf("[nv-bootfb] %s %04x:%04x %s%s mode=passive bootfb=%ux%u source=multiboot-lfb bar=%d lfb=%p/%p cmd=%04x",
            dev->name, dev->vendor, dev->device, model,
            synthetic ? " TEST-ONLY" : "", width, height, lfb_bar,
            (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes, command);
    if (pm) kprintf(" pm=D%u", power_state);
    else    kprintf(" pm=none");
    for (int i = 0; i < DEV_NRES; i++) {
        const struct dev_resource *r = &dev->res[i];
        if (!memory_bar_sane(r)) continue;
        kprintf(" bar%d=%p/%p%s", i, (void *)(uintptr_t)r->start,
                (void *)(uintptr_t)r->size,
                (r->flags & DEV_RES_PREFETCH) ? "/pref" : "");
    }
    kprintf("\n[nv-bootfb] framebuffer retained; no GPU command, modeset, clocks, DMA, IRQ or 3D\n");
    /* Acceleration failure is not passive display failure.  prepare() is a
     * staged, read-only capability probe and leaves software_fallback set on
     * every current exit, so missing firmware never makes a visible GOP mode
     * disappear. */
    (void)nvidia_pascal_accel_prepare(dev);
    dev_set_drvdata(dev, (void *)model);
    return 0;
}

static void nvidia_pascal_bootfb_remove(struct device *dev)
{
    /* Probe acquired no hardware or memory resource. */
    nvidia_pascal_accel_reset();
    if (dev) dev_set_drvdata(dev, NULL);
}

#define NV1050_MATCH(id) DEV_MATCH_VD(NVIDIA_VENDOR_ID, (id))
static const struct dev_match nvidia_pascal_bootfb_ids[] = {
    NV1050_MATCH(0x1c21), NV1050_MATCH(0x1c22),
    NV1050_MATCH(0x1c61), NV1050_MATCH(0x1c62),
    NV1050_MATCH(0x1c81), NV1050_MATCH(0x1c82),
    NV1050_MATCH(0x1c83), NV1050_MATCH(0x1c8c),
    NV1050_MATCH(0x1c8d), NV1050_MATCH(0x1c8f),
    NV1050_MATCH(0x1c91), NV1050_MATCH(0x1c92),
#ifdef NVIDIA_PASCAL_QEMU_TEST
    DEV_MATCH_VD(0x1234, 0x1111),
#endif
    DEV_MATCH_END
};
#ifdef LOGIT_HOST_TEST
const struct dev_match *nvidia_pascal_bootfb_match_table(void)
{
    return nvidia_pascal_bootfb_ids;
}
#endif
static struct driver nvidia_pascal_bootfb_driver = {
    .name = "nv-bootfb",
    .bus_type = DEV_BUS_PCI,
    .match = nvidia_pascal_bootfb_ids,
    .probe = nvidia_pascal_bootfb_probe,
    .remove = nvidia_pascal_bootfb_remove,
};
DRIVER_DECLARE(nvidia_pascal_bootfb_driver);
