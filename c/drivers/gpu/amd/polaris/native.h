#ifndef LOGIT_POLARIS_NATIVE_H
#define LOGIT_POLARIS_NATIVE_H
#include <stddef.h>
#include "amd/polaris/platform.h"
#include "amd/polaris/memory/layout.h"

struct polaris_native_resources {
    void *opaque;
    int (*read_pci32)(void *, uint16_t byte_offset, uint32_t *);
    uint64_t (*now_us)(void *);
    volatile uint8_t *bar0, *bar5;
    uint64_t bar0_physical, bar0_bytes, bar5_physical, bar5_bytes;
    struct polaris_memory_range arena, scanout;
};
struct polaris_native {
    struct polaris_native_resources resource;
    unsigned bound, faulted;
    /* Register accesses still covered by the last full PCI identity check; see
     * POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK in native.c. Zero after bind,
     * so the first access always re-reads configuration space. */
    unsigned identity_credit;
    uint32_t location, hdp_base, hdp_info, hdp_misc;
    uint32_t system_low, system_high, l1_mode;
    uint64_t vram_base, vram_bytes;
};
/* The platform must already own the PCI PF, arena and scanout exclusively,
 * with a complete firmware/reservation inventory, for the entire context
 * lifetime. BAR0/BAR5 must be actual UC/device mappings of the physical BARs;
 * no WB alias may exist. Sizes come from the platform's BAR resource records.
 * Supplying guessed free space at the BAR's end is NOT allocation.
 * Binding performs only fresh PCI/MMIO reads; it does not acquire ownership,
 * map memory, reset/reconfigure GMC/HDP, or establish engine readiness. Only
 * the already configured flat VRAM aperture (VMID0, linear, little-endian) is
 * supported; the GMC system window must cover VRAM with direct system-access
 * mode enabled. Unknown layouts are refused, preserving the boot display.
 * Zero-init once. Binding and exported operations run under the caller's
 * exclusive GPU lease. Inputs/outputs must not race and must be disjoint.
 * Failure preserves out/context. Mapping/PCI drift afterwards latches fault;
 * retaining buffers is mandatory while runtime resolves outstanding DMA.
 *
 * PCI identity (ID, command, class, BARs, power state, capability walk) is
 * verified in full by bind, by every read_identity, resolve_mapping and sync,
 * and by the first register access after any of them has not covered it; a
 * run of plain register reads and writes shares one verification for at most
 * POLARIS_NATIVE_ACCESSES_PER_IDENTITY_CHECK accesses. Re-walking configuration
 * space for every one of the ~65000 writes of an SMC firmware upload made that
 * upload slower than the SMU loader's whole-load limit allows. */
int polaris_native_bind(struct polaris_native *,
                         const struct polaris_native_resources *,
                         struct polaris_platform *out);
#endif
