/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gop_pick_efi.h"
#include "gop_pick.h"

/* UEFI 2.10 sec 12.9:
 * https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html
 * QueryMode allocates Info for the caller to free; EDID is borrowed protocol
 * storage. LocateProtocol(EDID) would silently select another output on a
 * multi-GPU machine. Match the exact GOP interface on its child handle first.
 * Host model / OVMF evidence only: not verified on X79-VG4 hardware. */
static struct gop_edid *display_edid(EFI_BOOT_SERVICES *services,
                                    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID active = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    EFI_GUID discovered = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
    EFI_HANDLE *handles = 0;
    UINTN count = 0;
    struct gop_edid *edid = 0;
    if (!services->LocateHandleBuffer) return 0;
    /* EFI_LOCATE_SEARCH_TYPE.ByProtocol is 2 (UEFI sec 7.3.15). */
    gop_handles_fn locate = (gop_handles_fn)services->LocateHandleBuffer;
    if (locate(2, &guid, 0, &count, &handles) != EFI_SUCCESS) return 0;
    if (!handles) return 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *found = 0;
        if (services->HandleProtocol(handles[i], &guid, (void **)&found) ||
            found != gop) continue;
        if (services->HandleProtocol(handles[i], &active, (void **)&edid) ||
            !edid || !edid->Edid || !edid->SizeOfEdid) {
            edid = 0;
            if (services->HandleProtocol(handles[i], &discovered, (void **)&edid))
                edid = 0;
        }
        break;
    }
    services->FreePool(handles);
    return edid;
}

static int eight_bits(UINT32 mask)
{
    if (!mask) return 0;
    while (!(mask & 1)) mask >>= 1;
    return mask == 255;
}

static int linear32(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info)
{
    if (info->Version || !info->HorizontalResolution || !info->VerticalResolution ||
        info->PixelsPerScanLine < info->HorizontalResolution ||
        info->PixelsPerScanLine > UINT32_MAX / 4) return 0;
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
        info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) return 1;
    if (info->PixelFormat != PixelBitMask) return 0;
    EFI_PIXEL_BITMASK m = info->PixelInformation;
    UINT32 rgb = m.RedMask | m.GreenMask | m.BlueMask;
    /* Three 8-bit channels can still be a packed 24-bit pixel. The upper
     * storage byte must actually be described, and the fields cannot overlap.
     * Otherwise multiplying PixelsPerScanLine by four silently shears rows. */
    return eight_bits(m.RedMask) && eight_bits(m.GreenMask) && eight_bits(m.BlueMask) &&
           !(m.RedMask & m.GreenMask) && !(m.RedMask & m.BlueMask) &&
           !(m.GreenMask & m.BlueMask) && !(rgb & m.ReservedMask) &&
           ((rgb | m.ReservedMask) & 0xff000000) != 0;
}

void gop_select_mode(EFI_BOOT_SERVICES *services,
                     EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                     struct gop_selection *result)
{
    *result = (struct gop_selection){.why="keep-firmware"};
    if (!gop || !gop->Mode || !gop->QueryMode || !gop->SetMode) return;
    UINT32 count = gop->Mode->MaxMode;
    UINT32 original = gop->Mode->Mode;
    if (!count || count > INT32_MAX || original >= count) return;
    struct gop_mode_info *modes = 0;
    if (services->AllocatePool(EfiLoaderData, (UINTN)count * sizeof(*modes),
                               (void **)&modes) != EFI_SUCCESS || !modes) return;
    gop_query_fn query = (gop_query_fn)gop->QueryMode;
    for (UINT32 i = 0; i < count; i++) {
        modes[i] = (struct gop_mode_info){0};
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN size = 0;
        if (query(gop, i, &size, &info) != EFI_SUCCESS) continue;
        if (info) {
            if (size >= sizeof(*info) && linear32(info)) {
                modes[i] = (struct gop_mode_info){info->HorizontalResolution,
                                                 info->VerticalResolution, 32};
            }
            services->FreePool(info);
        }
    }
    struct gop_edid *edid = display_edid(services, gop);
    int pick = gop_pick_mode(modes, (int)count, edid ? edid->Edid : 0,
                            edid ? edid->SizeOfEdid : 0,
                            result->why, sizeof(result->why));
    services->FreePool(modes);
    if (pick < 0 || (UINT32)pick == original) return;
    gop_set_fn set = (gop_set_fn)gop->SetMode;
    EFI_STATUS status = set(gop, (UINT32)pick);
    if (status != EFI_SUCCESS) {
        /* Do not restore a saved Info pointer or framebuffer base: SetMode
         * owns them and can replace them even on error. Ask firmware to put
         * the original mode back, then let the caller inspect the LIVE Mode.
         * A failed restore is reported separately, never claimed successful. */
        *result = (struct gop_selection){.why="keep-firmware", .set_failed=1,
            .set_status=status, .restore_status=set(gop, original)};
    }
}
