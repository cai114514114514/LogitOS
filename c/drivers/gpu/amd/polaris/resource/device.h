#ifndef LOGIT_POLARIS_RESOURCE_DEVICE_H
#define LOGIT_POLARIS_RESOURCE_DEVICE_H

#include "amd/polaris/device.h"
#include "amd/polaris/resource/atom.h"
#include "amd/polaris/resource/files.h"
#include "amd/polaris/resource/vbios.h"

struct polaris_resource_report {
    struct polaris_info device;
    struct polaris_atom_reservations reservations;
    /* enum polaris_atom_result. SKIPPED_LAYOUT_UNVERIFIED: an image was
     * accepted but deliberately not parsed. BAD_IMAGE (the default): no image
     * passed, or the parser rejected it. */
    int atom_result;
    unsigned vbios_bytes; /* The accepted image; 0 when no source passed. */
    enum polaris_vbios_source vbios_source;
    int vbios_module_status; /* enum polaris_vbios_status, per source, so the */
    int vbios_vfct_status;   /* log can tell "absent" from "present but bad". */
    uint8_t pci_revision;
    uint16_t pci_device;             /* selects the SMU file with the revision */
    unsigned firmware_mask[2]; /* Independently checked key=0/key=1 candidates. */
    int file_result[2][POLARIS_RESOURCE_FILE_SLOTS];
    /* Where the distinct accepted files came from: every valid key=0 slot plus
     * the key=1 SMC slot, the only one the two candidate sets do not share. */
    unsigned firmware_from_rootfs;
    unsigned firmware_from_bootmod;
    unsigned firmware_pin_mismatch; /* slots, in either set, whose digest differed */
    /* polaris_fw_bundle_plan().bytes_used of the seven engine files, the input
     * the arena size depends on (runtime.c plans memory from the same value).
     * Zero when any of the seven failed: the size is then not known. The SMC
     * file is not part of the bundle, so key=0 and key=1 give the same number. */
    uint64_t firmware_bundle_bytes;
    unsigned ownership_missing;
    unsigned security_key_unknown;
};

/* Production discovery for the already POSTed boot-display PF. Call once
 * during single-threaded boot after VFS mounts, outside the graphics lock.
 * The device must remain bound throughout the call. Actual PCI identity,
 * power, BARs and the complete LFB are rechecked before any MMIO mapping.
 *
 * Return 0 means the read-only device snapshot succeeded, not acceleration.
 * ATOM and every named file have independent results even when absent. No
 * MMIO or PCI register writes, ring submissions, engine reset or presenter
 * installation occur. The VBIOS comes from the boot module the loader saved
 * for this PCI function or from the ACPI VFCT table, never from BAR0, so the
 * live scanout is not touched. A valid image still proves nothing about which
 * VRAM is free: ownership_missing remains set even with a valid table and
 * all files. The current boot ABI supplies no complete VRAM allocation lease.
 */
int polaris_resources_probe_device(struct device *, uint64_t lfb,
                                   uint64_t lfb_bytes, uint32_t width,
                                   uint32_t height,
                                   struct polaris_resource_report *);

#endif
