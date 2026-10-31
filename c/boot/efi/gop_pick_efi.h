/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_GOP_PICK_EFI_H
#define LOGIT_GOP_PICK_EFI_H

#include "efi.h"

/* UEFI 2.10 sec 12.9.2.4/5; definitions cross-checked against EDK II:
 * https://github.com/tianocore/edk2/blob/master/MdePkg/Include/Protocol/EdidActive.h
 * https://github.com/tianocore/edk2/blob/master/MdePkg/Include/Protocol/EdidDiscovered.h
 * Keep these additions local: this track does not own the shared EFI header. */
#define EFI_EDID_ACTIVE_PROTOCOL_GUID \
    {0xbd8c1056,0x9f36,0x44ec,{0x92,0xa8,0xa6,0x33,0x7f,0x81,0x79,0x86}}
#define EFI_EDID_DISCOVERED_PROTOCOL_GUID \
    {0x1c0c34f6,0xd380,0x41fa,{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}}
struct gop_edid {
    UINT32 SizeOfEdid;
    UINT8 *Edid;
};
_Static_assert(OFF(struct gop_edid, Edid) == 8, "UEFI x64 EDID pointer offset");

typedef EFI_STATUS (EFIAPI *gop_query_fn)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32,
    UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
typedef EFI_STATUS (EFIAPI *gop_set_fn)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, UINT32);
typedef EFI_STATUS (EFIAPI *gop_handles_fn)(UINT32, EFI_GUID *, void *,
    UINTN *, EFI_HANDLE **);

struct gop_selection {
    char why[40];
    int set_failed;
    EFI_STATUS set_status, restore_status;
    /* what the GOP offered, for the boot report (LOGIT_BOOT_TAG_DISPLAY) */
    UINT32 fw_width, fw_height, mode_count, listed;
    struct { UINT16 width, height; } mode[48];
    UINT32 edid_len;
    UINT8 edid[256];
};

void gop_select_mode(EFI_BOOT_SERVICES *services,
                     EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                     struct gop_selection *result);

#endif
