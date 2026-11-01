#include "amd/polaris/resource/vbios.h"
#include "acpi.h"
#include "bootmod.h"

/* PCI Firmware Specification 3.x section 5.1.1 (expansion ROM header) and
 * Linux v6.12 drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c check_atom_bios():
 * 0x55 0xAA at the start, the image length in 512-byte units at offset 2, and
 * a 16-bit pointer to the ATOM ROM header at 0x48 whose "ATOM" signature sits
 * 4 bytes in. */
#define ROM_SIGNATURE_LOW 0x55u
#define ROM_SIGNATURE_HIGH 0xaau
#define ROM_LENGTH_OFFSET 2u
#define ROM_LENGTH_UNIT 512u
#define ROM_HEADER_POINTER 0x48u
#define ROM_HEADER_SIGNATURE_OFFSET 4u
#define ROM_HEADER_SIGNATURE_BYTES 4u

/* Linux v6.12 drivers/gpu/drm/amd/include/atombios.h, UEFI_ACPI_VFCT:
 * a 36-byte ACPI SDT header, TableUUID[16], VBIOSImageOffset (0x34),
 * Lib1ImageOffset (0x38) and Reserved[4], 0x4c bytes in all. Each image is a
 * VFCT_IMAGE_HEADER (28 bytes: PCIBus, PCIDevice, PCIFunction as u32, then
 * VendorID, DeviceID, SSVID, SSID as u16, Revision, ImageLength as u32)
 * followed by ImageLength bytes of ROM (GOP_VBIOS_CONTENT). */
#define VFCT_HEADER_BYTES 0x4cu
#define VFCT_IMAGE_OFFSET_FIELD 0x34u
#define VFCT_IMAGE_HEADER_BYTES 28u
#define VFCT_FIELD_BUS 0u
#define VFCT_FIELD_DEVICE 4u
#define VFCT_FIELD_FUNCTION 8u
#define VFCT_FIELD_VENDOR 12u
#define VFCT_FIELD_DEVICE_ID 14u
#define VFCT_FIELD_IMAGE_LENGTH 24u
#define ACPI_SDT_LENGTH_FIELD 4u

/* ROM and VFCT fields are unaligned little-endian, so read them bytewise. */
static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

/* Built on read_le16() so the endian-safe bytewise read lives in one place. */
static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)read_le16(bytes) | (uint32_t)read_le16(bytes + 2) << 16;
}

/* Overflow-proof "[offset, offset + length) lies inside bytes". Firmware
 * supplies every operand, so the sum is never formed. */
static int contains(size_t bytes, size_t offset, size_t length)
{
    return offset <= bytes && length <= bytes - offset;
}

/* The module name must match the loader's, so spell lowercase hex by table. */
static char hex_digit(unsigned value)
{
    return "0123456789abcdef"[value & 0xfu];
}

/* The name is the only key joining the loader's saved ROM to this PCI function,
 * so it is built here from the same fields the loader printed. */
void polaris_vbios_module_name(char out[POLARIS_VBIOS_MODULE_NAME_BYTES],
                               const struct polaris_vbios_location *location)
{
    unsigned function = location->function;
#ifdef POLARIS_VBIOS_NEGCTL_NO_BDF_MATCH
    /* Watched mutation: the lookup forgets which function it wants, so a ROM
     * the loader saved for function 0 is handed to function 1. */
    function = 0;
#endif
    unsigned position = 0;
    static const char prefix[] = "vbios-";
    for (unsigned i = 0; i + 1 < sizeof prefix; ++i) {
        out[position++] = prefix[i];
    }
    for (int shift = 12; shift >= 0; shift -= 4) {
        out[position++] = hex_digit(location->segment >> shift);
    }
    out[position++] = ':';
    out[position++] = hex_digit(location->bus >> 4);
    out[position++] = hex_digit(location->bus);
    out[position++] = ':';
    out[position++] = hex_digit(location->slot >> 4);
    out[position++] = hex_digit(location->slot);
    out[position++] = '.';
    out[position++] = hex_digit(function);
    out[position] = '\0';
}

/* Both sources hand over firmware bytes of unknown quality; this is the single
 * gate that keeps a wrong or cut-short image away from the ATOM parser. */
int polaris_vbios_check(const void *image, size_t bytes)
{
    const uint8_t *rom = image;
    /* Linux tests size < 0x49 and then reads rom[0x49]; one byte more is
     * needed for the two-byte header pointer to be inside the buffer. */
    if (!rom || bytes < ROM_HEADER_POINTER + 2u) {
        return POLARIS_VBIOS_BAD_LENGTH;
    }
    if (rom[0] != ROM_SIGNATURE_LOW || rom[1] != ROM_SIGNATURE_HIGH) {
        return POLARIS_VBIOS_BAD_SIGNATURE;
    }
    size_t legacy_bytes = (size_t)rom[ROM_LENGTH_OFFSET] * ROM_LENGTH_UNIT;
#ifndef POLARIS_VBIOS_NEGCTL_NO_LENGTH_CHECK
    /* The header may only claim bytes we actually hold. A module the loader
     * cut short, or a VFCT entry whose ImageLength is smaller than the ROM it
     * wraps, would otherwise let the ATOM walk run past the buffer. */
    if (!legacy_bytes || legacy_bytes > bytes) {
        return POLARIS_VBIOS_BAD_LENGTH;
    }
    bytes = legacy_bytes;
#else
    (void)legacy_bytes;
#endif
    size_t header = read_le16(rom + ROM_HEADER_POINTER);
    if (!header || !contains(bytes, header + ROM_HEADER_SIGNATURE_OFFSET,
                             ROM_HEADER_SIGNATURE_BYTES)) {
        return POLARIS_VBIOS_BAD_ATOM;
    }
    const uint8_t *signature = rom + header + ROM_HEADER_SIGNATURE_OFFSET;
    int atom = signature[0] == 'A' && signature[1] == 'T' &&
               signature[2] == 'O' && signature[3] == 'M';
    int swapped = signature[0] == 'M' && signature[1] == 'O' &&
                  signature[2] == 'T' && signature[3] == 'A';
    return atom || swapped ? POLARIS_VBIOS_OK : POLARIS_VBIOS_BAD_ATOM;
}

/* Linux compares bus, device, function, vendor and device ID, and so do we.
 * VFCT carries no PCI segment, so a function outside segment 0 is refused
 * rather than matched against the wrong domain's bus number. */
static int vfct_entry_matches(const uint8_t *entry,
                              const struct polaris_vbios_location *location)
{
#ifndef POLARIS_VBIOS_NEGCTL_NO_BDF_MATCH
    if (read_le32(entry + VFCT_FIELD_BUS) != location->bus ||
        read_le32(entry + VFCT_FIELD_DEVICE) != location->slot ||
        read_le32(entry + VFCT_FIELD_FUNCTION) != location->function) {
        return 0;
    }
#endif
    return read_le16(entry + VFCT_FIELD_VENDOR) == location->vendor &&
           read_le16(entry + VFCT_FIELD_DEVICE_ID) == location->device;
}

/* VFCT packs several GPUs' ROMs back to back, so the walk must find this
 * function's image without ever following a length past the table. */
int polaris_vbios_vfct_find(const void *table, size_t table_bytes,
                            const struct polaris_vbios_location *location,
                            const uint8_t **image, size_t *image_bytes)
{
    const uint8_t *base = table;
    if (!base || location->segment != 0) {
        return POLARIS_VBIOS_NO_MATCH;
    }
    if (table_bytes < VFCT_HEADER_BYTES) {
        return POLARIS_VBIOS_TRUNCATED;
    }
    size_t offset = read_le32(base + VFCT_IMAGE_OFFSET_FIELD);
    /* Linux does not check this. An offset inside the fixed header would make
     * the walk read the table's own GUID and reserved words as an entry. */
    if (offset < VFCT_HEADER_BYTES) {
        return POLARIS_VBIOS_TRUNCATED;
    }
    while (offset < table_bytes) {
#ifndef POLARIS_VBIOS_NEGCTL_NO_LENGTH_CHECK
        if (!contains(table_bytes, offset, VFCT_IMAGE_HEADER_BYTES)) {
            return POLARIS_VBIOS_TRUNCATED;
        }
#endif
        const uint8_t *entry = base + offset;
        size_t length = read_le32(entry + VFCT_FIELD_IMAGE_LENGTH);
        size_t body = offset + VFCT_IMAGE_HEADER_BYTES;
#ifndef POLARIS_VBIOS_NEGCTL_NO_LENGTH_CHECK
        if (!contains(table_bytes, body, length)) {
            return POLARIS_VBIOS_TRUNCATED;
        }
#endif
        if (length && vfct_entry_matches(entry, location)) {
            *image = base + body;
            *image_bytes = length;
            return POLARIS_VBIOS_OK;
        }
        offset = body + length;
    }
    return POLARIS_VBIOS_NO_MATCH;
}

/* One place decides the order (module, then VFCT, never BAR0) and records why
 * each source was refused, so the boot log can explain a missing VBIOS. */
int polaris_vbios_select(const struct polaris_vbios_location *location,
                         struct polaris_vbios_image *out)
{
    *out = (struct polaris_vbios_image){
        .module_status = POLARIS_VBIOS_ABSENT,
        .vfct_status = POLARIS_VBIOS_ABSENT
    };

    char name[POLARIS_VBIOS_MODULE_NAME_BYTES];
    polaris_vbios_module_name(name, location);
    uint64_t module_bytes = 0;
    const uint8_t *module = bootmod_find(name, &module_bytes);
    if (module) {
        out->module_status = polaris_vbios_check(module, (size_t)module_bytes);
        if (out->module_status == POLARIS_VBIOS_OK) {
            out->data = module;
            out->bytes = (size_t)module_bytes;
            out->source = POLARIS_VBIOS_SOURCE_MODULE;
            return 0;
        }
    }

    /* acpi_find_table() (c/kernel/cpu/acpi/acpi.c) and not uACPI: boot-time
     * probing runs before the acpi-init thread calls uacpi_initialize(), so
     * uacpi_table_find_by_signature() has no table subsystem yet. The kernel's
     * own walker already bounds the table to 1 MiB and verifies its checksum,
     * which is why the length at SDT offset 4 can be trusted here. A VFCT
     * holding several large ROMs and exceeding 1 MiB is therefore invisible;
     * the boot module is the path for such machines. */
    const uint8_t *table = acpi_find_table("VFCT");
    if (table) {
        const uint8_t *image = NULL;
        size_t image_bytes = 0;
        int status = polaris_vbios_vfct_find(table,
            read_le32(table + ACPI_SDT_LENGTH_FIELD), location,
            &image, &image_bytes);
        if (status == POLARIS_VBIOS_OK) {
            status = polaris_vbios_check(image, image_bytes);
        }
        out->vfct_status = status;
        if (status == POLARIS_VBIOS_OK) {
            out->data = image;
            out->bytes = image_bytes;
            out->source = POLARIS_VBIOS_SOURCE_VFCT;
            return 0;
        }
    }
    return -1;
}
