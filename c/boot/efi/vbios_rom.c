/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vbios_rom.h"

/* Why a local hex digit: this file is compiled into the freestanding loader,
 * which has no libc, and into a host test, which must not depend on one. */
static char vbios_hex_digit(UINTN value)
{
    return "0123456789abcdef"[value & 0xf];
}

/* The kernel finds the module by this exact string, so the format is fixed
 * here and in polaris_vbios_module_name(): segment 4 hex digits, bus 2,
 * device 2, function 1, all lowercase, the order GetLocation() reports them
 * (UEFI spec 2.10 section 14.4). */
void vbios_rom_name(char out[VBIOS_ROM_NAME_BYTES], UINTN segment, UINTN bus,
                    UINTN device, UINTN function)
{
#ifdef VBIOS_ROM_NEGCTL_NAME_DROPS_FUNCTION
    /* Watched mutation: two functions of one card would share a name. */
    function = 0;
#endif
    static const char prefix[] = "vbios-";
    UINTN position = 0;
    for (UINTN i = 0; i + 1 < sizeof prefix; i++) {
        out[position++] = prefix[i];
    }
    for (int shift = 12; shift >= 0; shift -= 4) {
        out[position++] = vbios_hex_digit(segment >> shift);
    }
    out[position++] = ':';
    out[position++] = vbios_hex_digit(bus >> 4);
    out[position++] = vbios_hex_digit(bus);
    out[position++] = ':';
    out[position++] = vbios_hex_digit(device >> 4);
    out[position++] = vbios_hex_digit(device);
    out[position++] = '.';
    out[position++] = vbios_hex_digit(function);
    out[position] = 0;
}

/* The boot log is the only evidence a real machine gives, so a skipped ROM must
 * say why in words rather than as a number. */
const char *vbios_rom_outcome_name(int outcome)
{
    switch (outcome) {
    case VBIOS_ROM_LOADED:          return "loaded";
    case VBIOS_ROM_NO_ROM:          return "firmware offers no ROM image";
    case VBIOS_ROM_BAD_LOCATION:    return "location unusable";
    case VBIOS_ROM_BAD_SIGNATURE:   return "ROM does not start 55 AA";
    case VBIOS_ROM_TOO_LARGE:       return "ROM larger than the module ceiling";
    case VBIOS_ROM_NO_SLOT:         return "module list full";
    case VBIOS_ROM_NO_MEMORY:       return "no pages for the copy";
    case VBIOS_ROM_NOT_LOADER_DATA: return "pages are not EfiLoaderData";
    default:                        return "unknown";
    }
}

/* By hand, and through a volatile destination: -ffreestanding does not stop
 * LLVM's loop-idiom pass turning a copy or clear loop into a call to
 * memcpy()/memset(), which does not exist in this binary. loader.c's mcopy()
 * and mzero() carry the longer explanation; they are static there and this
 * file also builds without loader.c, so the two loops are repeated. */
static void vbios_copy_bytes(void *destination, const void *source, UINT64 bytes)
{
    volatile UINT8 *to = (volatile UINT8 *)destination;
    const UINT8 *from = (const UINT8 *)source;
    while (bytes--) {
        *to++ = *from++;
    }
}

/* Same volatile loop as vbios_copy_bytes(), for the same reason: a plain
 * clear loop would be rewritten into a memset() call this binary lacks. */
static void vbios_clear_bytes(void *destination, UINT64 bytes)
{
    volatile UINT8 *to = (volatile UINT8 *)destination;
    while (bytes--) {
        *to++ = 0;
    }
}

/* Same reason: a struct this size cleared with `= {0}` becomes a memset call. */
static void vbios_clear_report(struct vbios_rom_report *report)
{
    vbios_clear_bytes(report, sizeof *report);
}

/* PCI addresses the kernel could name: a 16-bit segment, 8-bit bus, 5-bit
 * device, 3-bit function (PCI Firmware Specification 3.x section 2.2.4). Any
 * other value from GetLocation() means this is not a function we can address
 * in a module name, and a name with truncated digits could collide with a
 * different function's. */
static int vbios_location_fits(UINTN segment, UINTN bus, UINTN device,
                               UINTN function)
{
    return segment <= 0xffff && bus <= 0xff && device <= 0x1f && function <= 7;
}

/* Copy ROM_BYTES of ROM into fresh EfiLoaderData pages and describe them in
 * SLOT. The tail of the last page is cleared so the module never carries
 * whatever the page held before, and the pages are freed again when the memory
 * map does not call them LoaderData: unlike the firmware files, which are
 * required, a missing VBIOS module is harmless, so refusing to boot would be
 * the wrong response. */
static int vbios_copy_rom(EFI_BOOT_SERVICES *services, const void *rom,
                          UINT64 rom_bytes, vbios_rom_verify_fn verify,
                          struct vbios_rom_slot *slot)
{
    UINTN pages = (UINTN)((rom_bytes + 0xFFF) >> 12);
    EFI_PHYSICAL_ADDRESS at = 0;
    if (services->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &at)) {
        return VBIOS_ROM_NO_MEMORY;
    }
    UINT64 page_hi = at + ((UINT64)pages << 12);
    if (!verify(at, page_hi)) {
        services->FreePages(at, pages);
        return VBIOS_ROM_NOT_LOADER_DATA;
    }
    vbios_copy_bytes((void *)(UINTN)at, rom, rom_bytes);
    vbios_clear_bytes((UINT8 *)(UINTN)at + rom_bytes, page_hi - at - rom_bytes);
    slot->lo = at;
    slot->hi = at + rom_bytes;
#ifdef VBIOS_ROM_NEGCTL_SHORT_COPY
    /* Watched mutation: advertise one sector instead of the ROM's RomSize. */
    slot->hi = at + 512;
#endif
    slot->page_hi = page_hi;
    return VBIOS_ROM_LOADED;
}

/* Handle one PCI function that is already known to be display class. Filling
 * RESULT is the only way the caller learns what happened. */
static void vbios_collect_function(EFI_BOOT_SERVICES *services,
                                   struct vbios_pci_io *pci,
                                   struct vbios_rom_slot *slots,
                                   UINTN slot_capacity,
                                   struct vbios_rom_report *report,
                                   vbios_rom_verify_fn verify,
                                   struct vbios_rom_result *result)
{
    UINTN segment = 0;
    UINTN bus = 0;
    UINTN device = 0;
    UINTN function = 0;
    if (!pci->GetLocation ||
        pci->GetLocation(pci, &segment, &bus, &device, &function) ||
        !vbios_location_fits(segment, bus, device, function)) {
        result->outcome = VBIOS_ROM_BAD_LOCATION;
        return;
    }
    vbios_rom_name(result->name, segment, bus, device, function);

    const UINT8 *rom = (const UINT8 *)pci->RomImage;
    UINT64 rom_bytes = pci->RomSize;
    result->rom_bytes = rom_bytes;
    if (!rom || !rom_bytes) {
        result->outcome = VBIOS_ROM_NO_ROM;
    } else if (rom_bytes > VBIOS_ROM_BYTES_MAX) {
        result->outcome = VBIOS_ROM_TOO_LARGE;
    } else if (rom_bytes < 2 || rom[0] != 0x55 || rom[1] != 0xaa) {
        /* PCI Firmware Specification 3.x section 5.1.1: every expansion ROM
         * image starts 0x55 0xAA. Without it the kernel would reject the
         * module anyway; do not spend pages and a registry slot on it. */
        result->outcome = VBIOS_ROM_BAD_SIGNATURE;
    } else if (report->loaded >= slot_capacity) {
        result->outcome = VBIOS_ROM_NO_SLOT;
    } else {
        struct vbios_rom_slot *slot = &slots[report->loaded];
        result->outcome = vbios_copy_rom(services, rom, rom_bytes, verify, slot);
        if (result->outcome == VBIOS_ROM_LOADED) {
            for (UINTN i = 0; i < VBIOS_ROM_NAME_BYTES; i++) {
                slot->name[i] = result->name[i];
            }
            report->loaded++;
            report->loaded_bytes += rom_bytes;
        }
    }
}

/* The loader's one entry point: it runs before ExitBootServices, while the
 * RomImage buffers still exist, and it never fails the boot, because a missing
 * VBIOS module is harmless. */
void vbios_rom_collect(EFI_BOOT_SERVICES *services, struct vbios_rom_slot *slots,
                       UINTN slot_capacity, struct vbios_rom_report *report,
                       vbios_rom_verify_fn verify)
{
    EFI_GUID guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = 0;
    UINTN count = 0;
    vbios_clear_report(report);
    if (!services->LocateHandleBuffer) {
        return;
    }
    vbios_handles_fn locate = (vbios_handles_fn)services->LocateHandleBuffer;
    if (locate(VBIOS_LOCATE_BY_PROTOCOL, &guid, 0, &count, &handles) != EFI_SUCCESS ||
        !handles) {
        return;
    }
    for (UINTN index = 0; index < count; index++) {
        struct vbios_pci_io *pci = 0;
        UINT8 base_class = 0xff;
        report->handles++;
        /* Legacy HandleProtocol, as load_kernel() does: it needs no agent
         * handle and no CloseProtocol (UEFI 2.10 section 7.3). */
        if (services->HandleProtocol(handles[index], &guid, (void **)&pci) || !pci ||
            !pci->PciRead) {
            continue;
        }
        if (pci->PciRead(pci, VBIOS_PCI_IO_WIDTH_UINT8, VBIOS_PCI_CLASS_OFFSET, 1,
                         &base_class)) {
            continue;
        }
#ifdef VBIOS_ROM_NEGCTL_ANY_CLASS
        /* Watched mutation: copy the ROM of every PCI function, NICs included. */
        base_class = VBIOS_PCI_CLASS_DISPLAY;
#endif
        if (base_class != VBIOS_PCI_CLASS_DISPLAY) {
            continue;
        }
        report->display++;
        struct vbios_rom_result detail;
        vbios_clear_bytes(&detail, sizeof detail);
        vbios_collect_function(services, pci, slots, slot_capacity, report,
                               verify, &detail);
        if (report->recorded < VBIOS_ROM_RESULTS_MAX) {
            report->results[report->recorded++] = detail;
        }
    }
    services->FreePool(handles);
}
