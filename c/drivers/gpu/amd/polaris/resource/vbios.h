#ifndef LOGIT_POLARIS_RESOURCE_VBIOS_H
#define LOGIT_POLARIS_RESOURCE_VBIOS_H

#include <stddef.h>
#include <stdint.h>

/* Where the ATOM VBIOS image of one display function comes from.
 *
 * BAR0 is deliberately NOT a source any more. Reading its first 256 KiB only
 * finds a copy if the card's POST left one there, and on the X79-VG4 the same
 * BAR0 range holds the live GOP scanout, so the read both guessed and risked
 * remapping pages the display engine was fetching (P12 diagnosis, kept as the
 * dead-path control in resource/device.c). The two sources below are copies
 * the firmware made on purpose, before the OS existed:
 *
 *   MODULE  the loader copied EFI_PCI_IO_PROTOCOL.RomImage into EfiLoaderData
 *           before ExitBootServices (UEFI spec 2.10 section 14.4) and named
 *           it vbios-SSSS:BB:DD.F (c/boot/efi/vbios_rom.c).
 *   VFCT    the ACPI "VFCT" table (Vendor Fw Capabilities Table) AMD
 *           platform firmware fills with one image per GPU; Linux v6.12
 *           amdgpu_bios.c amdgpu_acpi_vfct_bios() reads it the same way. */
enum polaris_vbios_source {
    POLARIS_VBIOS_SOURCE_NONE = 0,
    POLARIS_VBIOS_SOURCE_MODULE,
    POLARIS_VBIOS_SOURCE_VFCT
};

/* Per-source outcome. ABSENT means "nothing to look at" (no module, no VFCT
 * table), the others mean something WAS there and was refused, which is the
 * difference between a firmware that offers nothing and one that offers
 * something broken. */
enum polaris_vbios_status {
    POLARIS_VBIOS_OK = 0,
    POLARIS_VBIOS_ABSENT = -1,
    POLARIS_VBIOS_NO_MATCH = -2,      /* VFCT holds no image for this BDF */
    POLARIS_VBIOS_TRUNCATED = -3,     /* a VFCT length runs past the table */
    POLARIS_VBIOS_BAD_SIGNATURE = -4, /* no 0x55 0xAA */
    POLARIS_VBIOS_BAD_LENGTH = -5,    /* header says more bytes than we hold */
    POLARIS_VBIOS_BAD_ATOM = -6       /* no ATOM header where 0x48 points */
};

/* The PCI function whose ROM is wanted. vendor/device are only used by the
 * VFCT walk, which (like Linux) matches them besides the bus address. */
struct polaris_vbios_location {
    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
};

struct polaris_vbios_image {
    const uint8_t *data; /* borrowed: loader pages or the firmware table alias */
    size_t bytes;
    enum polaris_vbios_source source;
    int module_status;
    int vfct_status;
};

/* "vbios-0000:00:02.0" plus its NUL: vbios-, 4 hex segment, 2 hex bus,
 * 2 hex device, 1 hex function. Must stay below LOGIT_BOOT_MODULE_NAME_MAX. */
#define POLARIS_VBIOS_MODULE_NAME_BYTES 19u

/* Spell the boot-module name the loader gives the ROM of this function. Both
 * ends format it by hand with lowercase hex, because the kernel's bootmod
 * registry is looked up by exact name and one side using %X would silently
 * never match. */
void polaris_vbios_module_name(char out[POLARIS_VBIOS_MODULE_NAME_BYTES],
                               const struct polaris_vbios_location *);

/* Accept an image only if it is an ATOM BIOS the parser can walk: 0x55 0xAA,
 * a nonzero legacy length that fits in what we hold, and the "ATOM" (or
 * byte-swapped "MOTA") signature where the ROM header pointer says it is.
 * Returns POLARIS_VBIOS_OK or the specific refusal. Reads only [0, bytes). */
int polaris_vbios_check(const void *image, size_t bytes);

/* Find the VFCT image for LOCATION inside a complete, already checksummed
 * table of TABLE_BYTES. On OK, *image points INTO the table. Every offset and
 * length is bounds-checked against TABLE_BYTES before it is followed. */
int polaris_vbios_vfct_find(const void *table, size_t table_bytes,
                            const struct polaris_vbios_location *,
                            const uint8_t **image, size_t *image_bytes);

/* Policy: the matching boot module first, then the matching VFCT entry,
 * never BAR0. Returns 0 with *out filled when an image passed
 * polaris_vbios_check, -1 otherwise (out->source NONE). The two per-source
 * statuses are filled either way so the boot log can say why. */
int polaris_vbios_select(const struct polaris_vbios_location *,
                         struct polaris_vbios_image *out);

static inline const char *polaris_vbios_source_name(enum polaris_vbios_source source)
{
    switch (source) {
    case POLARIS_VBIOS_SOURCE_MODULE: return "module";
    case POLARIS_VBIOS_SOURCE_VFCT: return "vfct";
    default: return "none";
    }
}

static inline const char *polaris_vbios_status_name(int status)
{
    switch (status) {
    case POLARIS_VBIOS_OK: return "ok";
    case POLARIS_VBIOS_ABSENT: return "absent";
    case POLARIS_VBIOS_NO_MATCH: return "no-match";
    case POLARIS_VBIOS_TRUNCATED: return "truncated";
    case POLARIS_VBIOS_BAD_SIGNATURE: return "bad-signature";
    case POLARIS_VBIOS_BAD_LENGTH: return "bad-length";
    case POLARIS_VBIOS_BAD_ATOM: return "bad-atom";
    default: return "unknown";
    }
}

#endif
