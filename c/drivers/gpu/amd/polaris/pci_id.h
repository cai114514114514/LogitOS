#ifndef LOGIT_POLARIS_PCI_ID_H
#define LOGIT_POLARIS_PCI_ID_H
#include <stdint.h>

/* Which PCI identities are Polaris10 silicon, and which SMU firmware a given
 * device/revision pair needs. One place, copied from Linux v6.12, so that
 * every Polaris10 path accepts the same boards.
 *
 * The real X79-VG4 card reads 1002:6fdf rev ef (RX 580 2048SP), not the
 * common 67df: the first version of this driver hard-coded 67df and refused
 * the very card it was written for (HUD photo, 2026-11-01). */

#define POLARIS_AMD_VENDOR 0x1002u

/* Linux v6.12 drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c pciidlist, the entries
 * mapped to CHIP_POLARIS10 (lines 1992-2004). The PCI ID names the chip, not
 * the board: shader count, VRAM size and marketing name still have to come
 * from the VBIOS and the hardware itself. */
static inline int polaris10_device_known(uint16_t device)
{
    switch (device) {
    case 0x67c0u:
    case 0x67c1u:
    case 0x67c2u:
    case 0x67c4u:
    case 0x67c7u:
    case 0x67d0u:
    case 0x67dfu:
    case 0x67c8u:
    case 0x67c9u:
    case 0x67cau:
    case 0x67ccu:
    case 0x67cfu:
    case 0x6fdfu:
        return 1;
    default:
        return 0;
    }
}

/* The 32-bit PCI config dword 0 (device << 16 | vendor) is what the hardware
 * paths read back to confirm they are still talking to the probed function;
 * accept exactly the Polaris10 set above, from AMD. */
static inline int polaris10_identity_known(uint32_t identity)
{
    return (identity & 0xffffu) == POLARIS_AMD_VENDOR &&
           polaris10_device_known((uint16_t)(identity >> 16));
}

/* Linux v6.12 drivers/gpu/drm/amd/amdgpu/amdgpu.h:765 ASICID_IS_P20: the
 * "kicker" Polaris20 parts that need polaris10_k_smc.bin. 6fdf counts with
 * revisions e7, ef and ff; revision alone is not enough, because the same
 * revision byte on another device ID means something else. */
static inline int polaris10_is_p20(uint16_t device, uint8_t revision)
{
    if (device == 0x67dfu) {
        return revision == 0xe3 || revision == 0xe4 || revision == 0xe5 ||
               revision == 0xe7 || revision == 0xef;
    }
    if (device == 0x6fdfu) {
        return revision == 0xe7 || revision == 0xef || revision == 0xff;
    }
    return 0;
}

/* Linux v6.12 amdgpu.h:776 ASICID_IS_P30: the Polaris30 parts that need
 * polaris10_k2_smc.bin; only 67df revisions e1 and f7. */
static inline int polaris10_is_p30(uint16_t device, uint8_t revision)
{
    return device == 0x67dfu && (revision == 0xe1 || revision == 0xf7);
}

#endif
