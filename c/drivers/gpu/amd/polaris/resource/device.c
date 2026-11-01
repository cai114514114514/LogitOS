#include "amd/polaris/resource/device.h"
#include "driver.h"
#include "pci.h"

#define AMD_VENDOR 0x1002u
#define PM_CAPABILITY_ID 1u
#define PM_CONTROL_OFFSET 4u
#define MAX_PM_CAPABILITY 0xf8u
#define PCI_STATUS_CAPABILITIES (1u << 20)
#define PCI_CAPABILITIES_POINTER 0x34u
#define PCI_BAR_MEMORY_ADDRESS_MASK 0xfffffff0u

static uint32_t read_config(struct device *device, uint16_t offset)
{
    return pci_cfg_read(device->bus, device->slot, device->func, offset);
}

static int current_bars_match(struct device *device)
{
    uint32_t header = read_config(device, 0x0cu);
    uint32_t bar0 = read_config(device, PCI_CFG_BAR0);
    uint32_t bar4 = read_config(device, PCI_CFG_BAR0 + 16);
    uint32_t bar5 = read_config(device, PCI_CFG_BAR0 + 20);
    if (header == UINT32_MAX || ((header >> 16) & 0x7fu) ||
        bar0 == UINT32_MAX || bar4 == UINT32_MAX || bar5 == UINT32_MAX ||
        (bar0 & 1u) || (bar5 & 7u) || (bar4 & 7u) == 4u) {
        return 0;
    }
    unsigned memory_type = (bar0 >> 1) & 3u;
    if (memory_type != 0 && memory_type != 2) {
        return 0;
    }
    uint64_t aperture_address = bar0 & PCI_BAR_MEMORY_ADDRESS_MASK;
    if (memory_type == 2) {
        uint32_t upper = read_config(device, PCI_CFG_BAR0 + 4);
        if (upper == UINT32_MAX) {
            return 0;
        }
        aperture_address |= (uint64_t)upper << 32;
    }
    return aperture_address == device->res[0].start &&
           (bar5 & PCI_BAR_MEMORY_ADDRESS_MASK) == device->res[5].start &&
           !!(device->res[0].flags & DEV_RES_64) == (memory_type == 2);
}

static int read_power_state(struct device *device, uint32_t command_status,
                             uint16_t *power)
{
    *power = 0;
    if (!(command_status & PCI_STATUS_CAPABILITIES)) {
        return 0;
    }
    uint32_t pointer = read_config(device, PCI_CAPABILITIES_POINTER);
    if (pointer == UINT32_MAX) {
        return -1;
    }
    unsigned offset = pointer & 0xffu;
    uint64_t visited = 0;
    unsigned pm_seen = 0;
    while (offset) {
        if (offset < 0x40 || offset > 0xfc || (offset & 3u)) {
            return -1;
        }
        uint64_t bit = UINT64_C(1) << (offset / 4u);
        if (visited & bit) {
            return -1;
        }
        visited |= bit;
        uint32_t capability = read_config(device, (uint16_t)offset);
        if (capability == UINT32_MAX) {
            return -1;
        }
        if ((capability & 0xffu) == PM_CAPABILITY_ID) {
            if (pm_seen || offset > MAX_PM_CAPABILITY) {
                return -1;
            }
            pm_seen = 1;
            uint32_t control = read_config(device,
                (uint16_t)(offset + PM_CONTROL_OFFSET));
            if (control == UINT32_MAX) {
                return -1;
            }
            *power = (uint16_t)control;
        }
        offset = (capability >> 8) & 0xffu;
    }
    return 0;
}

static uint64_t map_register_bar(void *opaque, int bar)
{
    return dev_bar_map(opaque, bar);
}

static uint32_t read_register(void *opaque, uint64_t base, uint32_t offset)
{
    (void)opaque;
    return *(volatile uint32_t *)(uintptr_t)(base + offset);
}

/* Find the VBIOS of this function and parse its declared reservations.
 *
 * BAR0 is no longer read. The previous code copied the first 256 KiB of the
 * aperture, as Linux v6.12 amdgpu_read_bios_from_vram() does for an already
 * POSTed dGPU after amdgpu_get_bios_dgpu() has tried ATRM and VFCT
 * (drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c); it only finds a ROM if POST left
 * one there. On the X79-VG4 that range is also where the GOP scanout lives, so
 * the read needed a window and a skip rule (P12) to avoid remapping live
 * pages, and still could not say where the bytes came from. The boot module
 * and the VFCT table are firmware-provided copies with a known origin, so they
 * replace it entirely (resource/vbios.c has the policy and the Linux sources).
 *
 * The window and skip logic were removed rather than kept as dead code: with
 * no BAR0 access there is nothing for them to protect, and an unreachable
 * guard is a guard nobody can test. The P12 control that BAR0 is never mapped
 * from here stays alive as the NEGCTL below, now with a stronger assertion
 * (never mapped, wherever the framebuffer sits). */
static void discover_vbios(struct device *device,
                           struct polaris_resource_report *report)
{
    struct polaris_vbios_location location = {
        .segment = (uint16_t)device->seg,
        .bus = (uint8_t)device->bus,
        .slot = (uint8_t)device->slot,
        .function = (uint8_t)device->func,
        .vendor = device->vendor,
        .device = device->device
    };
    struct polaris_vbios_image image;
    int missing = polaris_vbios_select(&location, &image);
    report->vbios_source = image.source;
    report->vbios_module_status = image.module_status;
    report->vbios_vfct_status = image.vfct_status;
#ifdef POLARIS_RESOURCE_NEGCTL_MAP_WHOLE_BAR
    /* Watched mutation: bring back the whole-aperture UC remap that
     * overwrote fb_init's WC PTEs on RX 580. */
    (void)dev_bar_map(device, 0);
#endif
    if (missing) {
        return;
    }
    report->vbios_bytes = (unsigned)image.bytes;
    if (!report->device.layout_covers_vram) {
        /* Skipped on purpose, not failed: the declared ranges are MC addresses
         * and cannot be checked against VRAM this layout does not cover. Say
         * so, so the log does not read like a rejected image. */
        report->atom_result = POLARIS_ATOM_SKIPPED_LAYOUT_UNVERIFIED;
        return;
    }
    /* The image is parsed in place: both sources are stable read-only pages
     * (held boot-module frames, the permanent ACPI alias), so no scratch
     * copy or allocation is needed and none can fail. */
    report->atom_result = polaris_atom_reservations_parse(image.data,
        image.bytes,
        (struct polaris_memory_range){report->device.mc_base,
                                      report->device.vram_bytes},
        report->device.cpu_aperture_bytes, &report->reservations);
}

static void discover_files(struct polaris_resource_report *report)
{
    /* Security selection is behind an indirect register port. Probing it
     * would write INDEX without having the GPU lease. Check both candidate
     * sets instead and retain that uncertainty explicitly. */
    for (unsigned key = 0; key <= 1; ++key) {
        struct polaris_resource_files files = {0};
        (void)polaris_resource_files_load(report->pci_revision, key, &files);
        report->firmware_mask[key] = files.valid_mask;
        for (unsigned kind = 0; kind < POLARIS_RESOURCE_FILE_SLOTS; ++kind) {
            report->file_result[key][kind] = files.result[kind];
            if (files.result[kind] == POLARIS_RESOURCE_FILE_PIN) {
                ++report->firmware_pin_mismatch;
            }
            /* key=1 repeats the shared slots, so count only its SMC file. */
            if (key == 1 && kind != POLARIS_FW_SMC) {
                continue;
            }
            if (files.source[kind] == POLARIS_RESOURCE_SOURCE_ROOTFS) {
                ++report->firmware_from_rootfs;
            } else if (files.source[kind] == POLARIS_RESOURCE_SOURCE_BOOTMOD) {
                ++report->firmware_from_bootmod;
            }
        }
        polaris_resource_files_release(&files);
    }
}

int polaris_resources_probe_device(struct device *device, uint64_t lfb,
                                   uint64_t lfb_bytes, uint32_t width,
                                   uint32_t height,
                                   struct polaris_resource_report *out)
{
    if (!out) {
        return -1;
    }
    *out = (struct polaris_resource_report){
        .atom_result = POLARIS_ATOM_BAD_IMAGE,
        .ownership_missing = 1,
        .security_key_unknown = 1
    };
    if (!device || device->bus_type != DEV_BUS_PCI || device->seg != 0 ||
        device->vendor != AMD_VENDOR || device->device != POLARIS10_PCI_DEVICE) {
        return -1;
    }
    uint32_t identity = read_config(device, PCI_CFG_VENDOR);
    uint32_t class_revision = read_config(device, PCI_CFG_REVISION);
    if (identity != 0x67df1002u || class_revision == UINT32_MAX ||
        class_revision >> 24 != PCI_CLASS_DISPLAY ||
        ((class_revision >> 16) & 0xffu) != device->subclass ||
        !current_bars_match(device)) {
        return -1;
    }
    uint32_t command_status = read_config(device, PCI_CFG_COMMAND);
    if (command_status == UINT32_MAX) {
        return -1;
    }
    uint16_t command = (uint16_t)command_status;
    uint16_t power = 0;
    if (read_power_state(device, command_status, &power)) {
        return -1;
    }
    struct polaris_probe_ops ops = {device, map_register_bar, read_register};
    if (polaris_probe_readonly(device, command, power, lfb, lfb_bytes,
                                width, height, &ops, &out->device)) {
        return -1;
    }
    out->pci_revision = (uint8_t)class_revision;
    discover_vbios(device, out);
    discover_files(out);
    return 0;
}
