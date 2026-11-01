/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_VBIOS_ROM_H
#define LOGIT_VBIOS_ROM_H

#include "efi.h"

/* Video-BIOS hand-over: every display-class PCI function's option ROM is
 * copied into EfiLoaderData before ExitBootServices and named for the kernel
 * as a boot module (LOGIT_BOOT_TAG_MODULE), so the AMD driver can read the
 * ATOM tables without touching BAR0 (c/drivers/gpu/amd/polaris/resource/
 * vbios.c is the consumer and names the Linux sources).
 *
 * EFI_PCI_IO_PROTOCOL (UEFI spec 2.10 section 14.4) is the only place the
 * firmware promises the ROM bytes: the PCI bus driver reads the ROM BAR (or a
 * platform-specific location) into memory it owns and publishes the pointer as
 * RomImage and the length as RomSize. That memory is EfiBootServicesData and
 * disappears at ExitBootServices, hence the copy.
 *
 * The EFI_PCI_IO_PROTOCOL declarations are local, like gop_pick_efi.h's EDID
 * ones: this track does not own the shared EFI header, and only the members
 * up to RomImage are named, each offset asserted against the spec table. */

/* Section 14.4: EFI_PCI_IO_PROTOCOL_GUID, as EDK II
 * MdePkg/Include/Protocol/PciIo.h spells it. */
#define EFI_PCI_IO_PROTOCOL_GUID \
    {0x4cf5b200,0x68b8,0x4ca5,{0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}}

/* Section 14.4: EFI_PCI_IO_PROTOCOL_WIDTH.EfiPciIoWidthUint8 and the PCI
 * configuration header byte holding the base class (PCI Local Bus
 * Specification 3.0 section 6.2.1, offset 0x0b; 0x03 is Display Controller). */
#define VBIOS_PCI_IO_WIDTH_UINT8 0
#define VBIOS_PCI_CLASS_OFFSET 0x0bu
#define VBIOS_PCI_CLASS_DISPLAY 0x03u
/* Section 7.3.15: EFI_LOCATE_SEARCH_TYPE.ByProtocol. */
#define VBIOS_LOCATE_BY_PROTOCOL 2u

struct vbios_pci_io;
typedef EFI_STATUS (EFIAPI *vbios_pci_read_fn)(struct vbios_pci_io *This,
    UINT32 Width, UINT32 Offset, UINTN Count, void *Buffer);
typedef EFI_STATUS (EFIAPI *vbios_pci_location_fn)(struct vbios_pci_io *This,
    UINTN *Segment, UINTN *Bus, UINTN *Device, UINTN *Function);
typedef EFI_STATUS (EFIAPI *vbios_handles_fn)(UINT32 SearchType,
    EFI_GUID *Protocol, void *SearchKey, UINTN *NoHandles,
    EFI_HANDLE **Buffer);

struct vbios_pci_io {
    void *PollMem;                          /*   0 */
    void *PollIo;                           /*   8 */
    void *MemRead;                          /*  16 */
    void *MemWrite;                         /*  24 */
    void *IoRead;                           /*  32 */
    void *IoWrite;                          /*  40 */
    vbios_pci_read_fn PciRead;              /*  48 */
    void *PciWrite;                         /*  56 */
    void *CopyMem;                          /*  64 */
    void *Map;                              /*  72 */
    void *Unmap;                            /*  80 */
    void *AllocateBuffer;                   /*  88 */
    void *FreeBuffer;                       /*  96 */
    void *Flush;                            /* 104 */
    vbios_pci_location_fn GetLocation;      /* 112 */
    void *Attributes;                       /* 120 */
    void *GetBarAttributes;                 /* 128 */
    void *SetBarAttributes;                 /* 136 */
    UINT64 RomSize;                         /* 144 */
    void *RomImage;                         /* 152 */
};
_Static_assert(OFF(struct vbios_pci_io, PciRead)     ==  48, "sec 14.4");
_Static_assert(OFF(struct vbios_pci_io, GetLocation) == 112, "sec 14.4");
_Static_assert(OFF(struct vbios_pci_io, RomSize)     == 144, "sec 14.4");
_Static_assert(OFF(struct vbios_pci_io, RomImage)    == 152, "sec 14.4");

/* The same ceiling bootmod.c puts on any module (BOOTMOD_BYTES_MAX); a larger
 * ROM would be refused by the kernel anyway, so it is not worth the pages. */
#define VBIOS_ROM_BYTES_MAX (16ull * 1024 * 1024)
/* "vbios-0000:00:02.0" and its NUL. */
#define VBIOS_ROM_NAME_BYTES 19u
/* loader.c asserts this equals LOGIT_BOOT_MODULE_NAME_MAX. */
#define VBIOS_ROM_SLOT_NAME_BYTES 64u
/* Display functions listed individually in the report; more are counted. */
#define VBIOS_ROM_RESULTS_MAX 16u

enum vbios_rom_outcome {
    VBIOS_ROM_LOADED = 0,
    VBIOS_ROM_NO_ROM,          /* RomImage null or RomSize zero */
    VBIOS_ROM_BAD_LOCATION,    /* GetLocation failed or returned a non-PCI value */
    VBIOS_ROM_BAD_SIGNATURE,   /* image does not start 0x55 0xAA */
    VBIOS_ROM_TOO_LARGE,       /* RomSize above VBIOS_ROM_BYTES_MAX */
    VBIOS_ROM_NO_SLOT,         /* module registry already full */
    VBIOS_ROM_NO_MEMORY,       /* AllocatePages refused */
    VBIOS_ROM_NOT_LOADER_DATA  /* firmware did not hand out EfiLoaderData */
};

/* One finished module: the same fields loader.c keeps for firmware files. */
struct vbios_rom_slot {
    UINT64 lo;        /* first byte of the ROM copy */
    UINT64 hi;        /* one past its last byte */
    UINT64 page_hi;   /* end of the page allocation */
    char name[VBIOS_ROM_SLOT_NAME_BYTES];
};

/* What happened to one display-class function, for the boot log. */
struct vbios_rom_result {
    char name[VBIOS_ROM_NAME_BYTES]; /* empty when the location was unusable */
    UINT64 rom_bytes;                /* RomSize as the firmware reported it */
    int outcome;                     /* enum vbios_rom_outcome */
};

struct vbios_rom_report {
    UINTN handles;      /* EFI_PCI_IO_PROTOCOL handles the firmware listed */
    UINTN display;      /* of those, base class 0x03 */
    UINTN loaded;       /* modules produced == slots filled */
    UINT64 loaded_bytes;
    UINTN recorded;     /* valid entries in results (<= VBIOS_ROM_RESULTS_MAX) */
    struct vbios_rom_result results[VBIOS_ROM_RESULTS_MAX];
};

/* Called with the page range of a fresh allocation; nonzero when the memory
 * map really lists it as EfiLoaderData (loader.c: allocated_range_is_loader_data). */
typedef int (*vbios_rom_verify_fn)(UINT64 lo, UINT64 hi);

/* Spell "vbios-SSSS:BB:DD.F" (lowercase hex) into OUT. The kernel looks
 * modules up by exact name, so this and polaris_vbios_module_name() must agree
 * byte for byte; both gates pin the same literal. */
void vbios_rom_name(char out[VBIOS_ROM_NAME_BYTES], UINTN segment, UINTN bus,
                    UINTN device, UINTN function);

const char *vbios_rom_outcome_name(int outcome);

/* Copy the ROM of every display-class PCI function into EfiLoaderData and fill
 * SLOTS (at most SLOT_CAPACITY) with the result. Allocation only; the caller
 * appends the slots to its module list and still performs the final-map
 * LoaderData check before it jumps. A function that offers no usable ROM is
 * reported and skipped, never fatal: the desktop does not need a VBIOS. */
void vbios_rom_collect(EFI_BOOT_SERVICES *services, struct vbios_rom_slot *slots,
                       UINTN slot_capacity, struct vbios_rom_report *report,
                       vbios_rom_verify_fn verify);

#endif
