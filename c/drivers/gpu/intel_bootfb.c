/* Passive boot-display support for Intel graphics.
 *
 * Intel integrated graphics spans many generations and shares display class
 * 03.00 with discrete Arc devices, so an ID allowlist would age badly and
 * still would not establish which adapter owns the picture.  This driver uses
 * the narrower invariant firmware leaves behind: the complete physical LFB
 * reported by Multiboot2/GOP must be contained in a decoded memory BAR of the
 * same 8086 display function.  That admits old and new Intel boot displays
 * without claiming a secondary Intel adapter merely because a screen exists.
 *
 * This is deliberately an observer, not an i915/Xe substitute.  It never maps
 * GPU registers, enables bus mastering, submits DMA, requests an IRQ, changes
 * clocks or performs a modeset.  Waking a D3 GPU or guessing stolen-memory and
 * GGTT state can turn a working firmware display black, so those cases are
 * refused and fb.c continues to own the CPU-written boot framebuffer.
 */
#include <stddef.h>
#include <stdint.h>

#include "intel_bootfb.h"
#include "driver.h"
#include "fb.h"
#include "kprintf.h"
#include "pci.h"

#define INTEL_VENDOR_ID 0x8086u
#define PCI_CAP_PM       0x01u
#define PCI_PMCSR        0x04u

static const char intel_bootfb_name[] = "Intel firmware framebuffer";

static int memory_bar_sane(const struct dev_resource *r)
{
    if (!r || !(r->flags & DEV_RES_MEM) || (r->flags & DEV_RES_IO) ||
        !r->start || !r->size)
        return 0;
    return r->start + r->size >= r->start;
}

static int contains_lfb(const struct dev_resource *r,
                        uint64_t lfb, uint64_t lfb_bytes)
{
    if (!memory_bar_sane(r) || !lfb || !lfb_bytes) return 0;
#ifdef INTEL_BOOTFB_NEGCTL_SKIP_LFB_OWNER
    /* Watched mutation: any sane BAR is treated as proof that this PCI
     * function owns the LFB, recreating the unsafe secondary-GPU bind. */
    (void)lfb;
    (void)lfb_bytes;
    return 1;
#else
    uint64_t lfb_end = lfb + lfb_bytes;
    uint64_t bar_end = r->start + r->size;
    if (lfb_end < lfb) return 0;
    return lfb >= r->start && lfb_end <= bar_end;
#endif
}

int intel_bootfb_probe(struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI) return -1;
#ifndef INTEL_BOOTFB_NEGCTL_ACCEPT_FOREIGN
    if (dev->vendor != INTEL_VENDOR_ID) return -1;
#endif

    /* Only type-0 VGA and 3D display functions have the six-BAR endpoint
     * layout inspected below.  Other 8086 functions must remain available to
     * their real subsystem drivers. */
    if (dev->class_code != PCI_CLASS_DISPLAY ||
        (dev->subclass != 0x00 && dev->subclass != 0x02) ||
        dev->header_type != 0)
        return -1;

    uint16_t command = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                      PCI_CFG_COMMAND);
#ifndef INTEL_BOOTFB_NEGCTL_ACCEPT_UNAVAILABLE
    if (command == UINT16_MAX) {
        /* All-ones is PCI's absent/unreachable value.  Its MEM bit is also
         * one, so testing only PCI_CMD_MEM silently accepts a vanished
         * function as a decoded display. */
        kprintf("[intel-bootfb] %s %04x:%04x refused: PCI command unavailable\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }
#endif
    if (!(command & PCI_CMD_MEM)) {
        /* Enabling decode here would alter a GPU whose firmware state we do
         * not own.  A future native driver may recover it with generation-
         * specific sequencing; the passive observer cannot. */
        kprintf("[intel-bootfb] %s %04x:%04x refused: memory decode is off\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    uint8_t pm = pci_cap_find(dev->bus, dev->slot, dev->func, PCI_CAP_PM);
    unsigned power_state = 0;
    if (pm) {
        if (pm > 0xfbu) {
            kprintf("[intel-bootfb] %s %04x:%04x refused: malformed PM capability\n",
                    dev->name, dev->vendor, dev->device);
            return -1;
        }
        power_state = pci_cfg_read16(dev->bus, dev->slot, dev->func,
                                     (uint16_t)(pm + PCI_PMCSR)) & 3u;
#ifndef INTEL_BOOTFB_NEGCTL_ACCEPT_D3
        if (power_state != 0) {
            kprintf("[intel-bootfb] %s %04x:%04x refused: PCI power state D%u\n",
                    dev->name, dev->vendor, dev->device, power_state);
            return -1;
        }
#endif
    }

    uint32_t width = fb_width(), height = fb_height();
    uint64_t lfb = 0, lfb_bytes = 0;
    if (!width || !height || !fb_boot_lfb_range(&lfb, &lfb_bytes) ||
        !lfb || !lfb_bytes || lfb + lfb_bytes < lfb) {
        kprintf("[intel-bootfb] %s %04x:%04x refused: no valid Multiboot LFB\n",
                dev->name, dev->vendor, dev->device);
        return -1;
    }

    int lfb_bar = -1;
    for (int i = 0; i < DEV_NRES; i++) {
        if (contains_lfb(&dev->res[i], lfb, lfb_bytes)) {
            lfb_bar = i;
            break;
        }
    }
    if (lfb_bar < 0) {
        kprintf("[intel-bootfb] %s %04x:%04x refused: LFB %p/%p is outside its BARs\n",
                dev->name, dev->vendor, dev->device,
                (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes);
        return -1;
    }

    kprintf("[intel-bootfb] %s %04x:%04x mode=passive bootfb=%ux%u source=multiboot-lfb bar=%d lfb=%p/%p cmd=%04x",
            dev->name, dev->vendor, dev->device, width, height, lfb_bar,
            (void *)(uintptr_t)lfb, (void *)(uintptr_t)lfb_bytes, command);
    if (pm) kprintf(" pm=D%u", power_state);
    else    kprintf(" pm=none");
    kprintf("\n[intel-bootfb] framebuffer retained; no MMIO write, BME, DMA, IRQ, clocks, modeset or 3D\n");

    dev_set_drvdata(dev, (void *)intel_bootfb_name);
    return 0;
}

static void intel_bootfb_remove(struct device *dev)
{
    /* Probe borrowed firmware state and acquired no hardware resource. */
    if (dev) dev_set_drvdata(dev, NULL);
}

static const struct dev_match intel_bootfb_ids[] = {
    DEV_MATCH_VCLASS(INTEL_VENDOR_ID, PCI_CLASS_DISPLAY, 0x00),
    DEV_MATCH_VCLASS(INTEL_VENDOR_ID, PCI_CLASS_DISPLAY, 0x02),
    DEV_MATCH_END
};

#ifdef LOGIT_HOST_TEST
const struct dev_match *intel_bootfb_match_table(void)
{
    return intel_bootfb_ids;
}
#endif

static struct driver intel_bootfb_driver = {
    .name = "intel-bootfb",
    .bus_type = DEV_BUS_PCI,
    .match = intel_bootfb_ids,
    .probe = intel_bootfb_probe,
    .remove = intel_bootfb_remove,
};
DRIVER_DECLARE(intel_bootfb_driver);
