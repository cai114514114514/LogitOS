/* Boot-framebuffer ownership for AMD/ATI PCI display functions.
 *
 * UEFI GOP or a VGA option ROM can leave a usable linear framebuffer long
 * before LogitOS has enough of an AMDGPU stack to own display clocks, memory
 * controllers, rings, fences and reset.  The tempting shortcut is to bind any
 * 1002 display once fb.c has a Multiboot framebuffer.  That is wrong on a
 * multi-GPU machine: the framebuffer can belong to another PCI function.
 * This observer therefore binds only when the COMPLETE half-open LFB range is
 * contained by one sane memory BAR on this exact function.
 *
 * Ownership validation is deliberately read-only.  After it succeeds, the
 * exact RV100 1002:5159 path may run amd_accel's isolated fill/copy canary;
 * Polaris10 67df now also samples BAR5 registers without command submission;
 * every other AMD GPU remains passive.  In particular this driver never wakes
 * D3 hardware or enables BME, DMA, IRQs, clocks, modesetting or 3D.  Desktop
 * rendering remains fb.c's CPU copy into the established aperture.
 */
#include <stddef.h>
#include <stdint.h>
#include "amd/bootfb.h"
#include "amd/accel.h"
#include "amd/polaris/resource/device.h"
#include "driver.h"
#include "pci.h"
#include "fb.h"
#include "kprintf.h"

#define AMD_VENDOR_ID 0x1002u
#define PCI_CAP_PM     0x01u
#define PCI_PMCSR      0x04u

static const char amd_bootfb_name[] = "AMD/ATI firmware framebuffer";
static struct device *polaris_boot_device;
static int polaris_resources_probed;

static int memory_bar_sane(const struct dev_resource *r)
{
    if (!(r->flags & DEV_RES_MEM) ||
#ifndef AMD_BOOTFB_NEGCTL_ACCEPT_MIXED_BAR
        (r->flags & DEV_RES_IO) ||
#endif
        !r->start || !r->size)
        return 0;
    return r->size <= UINT64_MAX - r->start;
}

int amd_bootfb_probe(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI ||
        dev->vendor != AMD_VENDOR_ID ||
        dev->class_code != PCI_CLASS_DISPLAY || dev->header_type != 0)
        return -1;

    uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                      PCI_CFG_COMMAND);
    if (command == UINT16_MAX) {
        kprintf("[amd-bootfb] %s %04x:%04x refused: PCI command unavailable\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }
#ifndef AMD_BOOTFB_NEGCTL_ACCEPT_MEM_OFF
    if (!(command & PCI_CMD_MEM)) {
        /* Re-enabling decode is not passive recovery: the BAR assignment or
         * firmware display state may already be gone. */
        kprintf("[amd-bootfb] %s %04x:%04x refused: PCI memory decode is off\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }
#endif

    uint8_t pm = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_PM);
    unsigned power_state = 0;
    if (pm) {
#ifndef AMD_BOOTFB_NEGCTL_ACCEPT_BAD_PM
        if (pm > 0xfbu) {
            kprintf("[amd-bootfb] %s %04x:%04x refused: malformed PM capability\n",
                    dev->name, dev->vendor, dev->device);
            return -1;
        }
#endif
        power_state = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                     (uint16_t)(pm + PCI_PMCSR)) & 3u;
        if (power_state != 0) {
            kprintf("[amd-bootfb] %s %04x:%04x refused: PCI power state D%u\n",
                    dev->name, dev->vendor, dev->device, power_state);
            return -1;
        }
    }

    uint32_t width = fb_width(), height = fb_height();
    uint64_t lfb = 0, lfb_bytes = 0;
    if (!width || !height || !fb_boot_lfb_range(&lfb, &lfb_bytes) ||
        !lfb_bytes || lfb_bytes > UINT64_MAX - lfb) {
        kprintf("[amd-bootfb] %s %04x:%04x refused: no valid Multiboot LFB\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    int owner_bar = -1;
    uint64_t lfb_end = lfb + lfb_bytes;
#ifdef AMD_BOOTFB_NEGCTL_START_ONLY
    (void)lfb_end;
#endif
    for (int i = 0; i < DEV_NRES; i++) {
        const struct dev_resource *r = &dev->res[i];
        if (!memory_bar_sane(r)) continue;
        uint64_t bar_end = r->start + r->size;
#ifdef AMD_BOOTFB_NEGCTL_START_ONLY
        /* Watched mutation: reproduces the unsafe start-address-only test. */
        if (lfb >= r->start && lfb < bar_end) {
#else
        if (lfb >= r->start && lfb_end <= bar_end) {
#endif
            owner_bar = i;
            break;
        }
    }
    if (owner_bar < 0) {
        kprintf("[amd-bootfb] %s %04x:%04x refused: LFB %p/%p is outside its BARs\n",
                dev->name, dev->vendor, dev->device,
                (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes);
        return -1;
    }

    kprintf("[amd-bootfb] %s %04x:%04x mode=passive bootfb=%ux%u "
            "source=multiboot-lfb bar=%d lfb=%p/%p cmd=%04x",
            dev->name, dev->vendor, dev->device, width, height, owner_bar,
            (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes, command);
    if (pm) kprintf(" pm=D%u", power_state);
    else    kprintf(" pm=none");
    kprintf("\n");
    int accel = amd_accel_prepare(dev, lfb, lfb_bytes, width, height);
    if (accel == 0)
        kprintf("[amd-bootfb] framebuffer retained; RV100 2D canary passed, "
                "desktop remains CPU-composited\n");
    else if (dev->device == 0x5159u)
        kprintf("[amd-bootfb] framebuffer retained; RV100 canary blocked, "
                "desktop remains CPU-composited\n");
    else if (dev->device == POLARIS10_PCI_DEVICE)
        kprintf("[amd-bootfb] framebuffer retained; Polaris read-only probe, "
                "SMU/GART/ring bring-up pending; desktop remains CPU-composited\n");
    else
        kprintf("[amd-bootfb] framebuffer retained; no modeset, GPU command, "
                "BAR map, DMA, IRQ or MMIO write on this device\n");
    dev_set_drvdata(dev, (void *)amd_bootfb_name);
    if (dev->device == POLARIS10_PCI_DEVICE) {
        polaris_boot_device = dev;
        polaris_resources_probed = 0;
    }
    return 0;
}

void amd_bootfb_probe_resources(void)
{
    if (!polaris_boot_device || polaris_resources_probed)
        return;
    uint64_t lfb, bytes;
    if (!fb_boot_lfb_range(&lfb, &bytes))
        return;

    /* Keep VFS resource loading in the boot phase that guarantees both root
     * mount and completed driver binding. Boot is still single-threaded, so
     * the device cannot be unplugged during these reads. The resource layer
     * rechecks PCI/BAR identity before accessing the device. */
    polaris_resources_probed = 1;
    struct polaris_resource_report report;
    int status = polaris_resources_probe_device(polaris_boot_device, lfb, bytes,
                                                fb_width(), fb_height(), &report);
    kprintf("[amd-resources] pci=%04x status=%d vbios-bytes=%u atom=%d "
            "usage-revision=%u firmware-key0=%x firmware-key1=%x required=%x "
            "ownership-missing=%u security-key-unknown=%u\n",
            polaris_boot_device->device, status, report.vbios_bytes_read,
            report.atom_result, report.reservations.table_revision,
            report.firmware_mask[0], report.firmware_mask[1], POLARIS_RESOURCE_ALL_FILES,
            report.ownership_missing, report.security_key_unknown);
    if (report.atom_result == POLARIS_ATOM_OK) {
        kprintf("[amd-resources] firmware-reserved=%llx/%llu driver-scratch=%llx/%llu\n",
                (unsigned long long)report.reservations.firmware.base,
                (unsigned long long)report.reservations.firmware.bytes,
                (unsigned long long)report.reservations.driver_scratch.base,
                (unsigned long long)report.reservations.driver_scratch.bytes);
    }
}

static void amd_bootfb_remove(struct device *dev)
{
    if (dev == polaris_boot_device) {
        polaris_boot_device = NULL;
        polaris_resources_probed = 0;
    }
    amd_accel_reset();
    if (dev) dev_set_drvdata(dev, NULL);
}

static const struct dev_match amd_bootfb_ids[] = {
    { AMD_VENDOR_ID, DEV_ANY, PCI_CLASS_DISPLAY, DEV_ANYC, DEV_ANYC, 0 },
    DEV_MATCH_END
};

#ifdef LOGIT_HOST_TEST
const struct dev_match *amd_bootfb_match_table(void)
{
    return amd_bootfb_ids;
}
#endif

static struct driver amd_bootfb_driver = {
    .name = "amd-bootfb",
    .bus_type = DEV_BUS_PCI,
    .match = amd_bootfb_ids,
    .probe = amd_bootfb_probe,
    .remove = amd_bootfb_remove,
};
DRIVER_DECLARE(amd_bootfb_driver);
