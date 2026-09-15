#include "amd/polaris/resource/atom.h"

/* Linux v6.12 include/atombios.h: ATOM_ROM_HEADER,
 * ATOM_MASTER_LIST_OF_DATA_TABLES, ATOM_VRAM_USAGE_BY_FIRMWARE[_V1_5]. */
#define ROM_HEADER_POINTER 0x48u
#define PCIR_POINTER 0x18u
#define ROM_MASTER_DATA_POINTER 0x20u
#define MASTER_USAGE_POINTER (4u + 11u * 2u)
#define COMMON_HEADER_BYTES 4u
#define USAGE_TABLE_BYTES 12u
#define KIB_SHIFT 10u
#define BLOCK_POLICY_SHIFT 30u
#define BLOCK_OFFSET_MASK 0x3fffffffu
#define BLOCK_RESERVED 0u
#define BLOCK_NOT_RESERVED 1u
#define TWO_GIB (UINT64_C(2) << 30)

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)read_le16(bytes) |
           (uint32_t)read_le16(bytes + 2) << 16;
}

static int contains(size_t bytes, size_t offset, size_t length)
{
    return offset <= bytes && length <= bytes - offset;
}

static int signature(const uint8_t *bytes, const char *expected)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (bytes[i] != (uint8_t)expected[i]) {
            return 0;
        }
    }
    return 1;
}

static int table_at(const uint8_t *image, size_t bytes, size_t offset,
                    size_t minimum)
{
    if (!offset || !contains(bytes, offset, COMMON_HEADER_BYTES)) {
        return 0;
    }
    size_t table_bytes = read_le16(image + offset);
    return table_bytes >= minimum && contains(bytes, offset, table_bytes);
}

static int locate_usage(const uint8_t *image, size_t bytes, size_t *usage)
{
    if (bytes < ROM_HEADER_POINTER + 2 || image[0] != 0x55 ||
        image[1] != 0xaa || !image[2]) {
        return POLARIS_ATOM_BAD_IMAGE;
    }
    size_t legacy_bytes = (size_t)image[2] * 512u;
    if (legacy_bytes > bytes) {
        return POLARIS_ATOM_BAD_IMAGE;
    }
    bytes = legacy_bytes;

    size_t pcir = read_le16(image + PCIR_POINTER);
    if (!contains(bytes, pcir, 24) || !signature(image + pcir, "PCIR") ||
        read_le16(image + pcir + 10) < 24 ||
        !contains(bytes, pcir, read_le16(image + pcir + 10))) {
        return POLARIS_ATOM_BAD_IMAGE;
    }
    if (read_le16(image + pcir + 4) != 0x1002 ||
        read_le16(image + pcir + 6) != 0x67df) {
        return POLARIS_ATOM_WRONG_DEVICE;
    }

    size_t rom = read_le16(image + ROM_HEADER_POINTER);
    if (!table_at(image, bytes, rom, 36) ||
        (!signature(image + rom + 4, "ATOM") &&
         !signature(image + rom + 4, "MOTA"))) {
        return POLARIS_ATOM_BAD_IMAGE;
    }
    if ((image[rom + 2] != 1 && image[rom + 2] != 2) || image[rom + 3] != 1) {
        return POLARIS_ATOM_UNSUPPORTED_TABLE;
    }
    if (image[rom + 2] == 2 && read_le16(image + rom) < 40) {
        return POLARIS_ATOM_BAD_IMAGE;
    }

    size_t master = read_le16(image + rom + ROM_MASTER_DATA_POINTER);
    if (!table_at(image, bytes, master, MASTER_USAGE_POINTER + 2)) {
        return POLARIS_ATOM_BAD_IMAGE;
    }
    if (image[master + 2] != 1 || image[master + 3] != 1) {
        return POLARIS_ATOM_UNSUPPORTED_TABLE;
    }
    *usage = read_le16(image + master + MASTER_USAGE_POINTER);
    if (!*usage) {
        return POLARIS_ATOM_NO_USAGE_TABLE;
    }
    if (!table_at(image, bytes, *usage, USAGE_TABLE_BYTES)) {
        return POLARIS_ATOM_BAD_IMAGE;
    }
    if (read_le16(image + *usage) != USAGE_TABLE_BYTES ||
        image[*usage + 2] != 1 ||
        (image[*usage + 3] != 4 && image[*usage + 3] != 5)) {
        return POLARIS_ATOM_UNSUPPORTED_TABLE;
    }
    return POLARIS_ATOM_OK;
}

static int decode_reservations(const uint8_t *table,
                               struct polaris_memory_range vram,
                               uint64_t aperture_bytes,
                               struct polaris_atom_reservations *out)
{
    uint32_t encoded_start = read_le32(table + 4);
    uint64_t offset = (uint64_t)(encoded_start & BLOCK_OFFSET_MASK) << KIB_SHIFT;
    uint64_t firmware_bytes = (uint64_t)read_le16(table + 8) << KIB_SHIFT;
    uint64_t driver_bytes = (uint64_t)read_le16(table + 10) << KIB_SHIFT;
    out->table_revision = table[3];
    out->block_policy = encoded_start >> BLOCK_POLICY_SHIFT;
    if (out->block_policy > BLOCK_NOT_RESERVED ||
        (out->table_revision == 4 && driver_bytes)) {
        return POLARIS_ATOM_UNSUPPORTED_TABLE;
    }
    if (offset > vram.bytes || firmware_bytes > vram.bytes - offset) {
        return POLARIS_ATOM_BAD_RESERVATION;
    }

    if (out->block_policy == BLOCK_RESERVED && firmware_bytes) {
#ifndef POLARIS_RESOURCE_NEGCTL_DROP_RESERVATION
        out->firmware = (struct polaris_memory_range){vram.base + offset,
                                                     firmware_bytes};
#endif
    }
    if (!driver_bytes) {
        return POLARIS_ATOM_OK;
    }
    if (out->block_policy != BLOCK_RESERVED) {
        return POLARIS_ATOM_UNSUPPORTED_TABLE;
    }

    /* v1.5 places the interpreter scratch area before the firmware block.
     * With no firmware start, the table instead asks the OS to allocate it;
     * reporting that requested address is not an allocation operation. */
    uint64_t driver_end = offset;
    if (!driver_end) {
        driver_end = vram.bytes <= TWO_GIB ? vram.bytes : aperture_bytes;
        out->driver_allocation_required = 1;
    }
    if (driver_bytes > driver_end) {
        return POLARIS_ATOM_BAD_RESERVATION;
    }
    out->driver_scratch = (struct polaris_memory_range){
        vram.base + driver_end - driver_bytes, driver_bytes};
    return POLARIS_ATOM_OK;
}

int polaris_atom_reservations_parse(const void *data, size_t bytes,
                                    struct polaris_memory_range vram,
                                    uint64_t aperture_bytes,
                                    struct polaris_atom_reservations *out)
{
    if (!data || !out || !bytes || bytes > UINTPTR_MAX - (uintptr_t)data ||
        sizeof *out > UINTPTR_MAX - (uintptr_t)out ||
        ((uintptr_t)data < (uintptr_t)out + sizeof *out &&
         (uintptr_t)out < (uintptr_t)data + bytes) ||
        !vram.bytes || vram.base >= POLARIS_MEMORY_LIMIT ||
        vram.bytes > POLARIS_MEMORY_LIMIT - vram.base ||
        !aperture_bytes || aperture_bytes > vram.bytes) {
        return POLARIS_ATOM_BAD_IMAGE;
    }

    size_t usage = 0;
    int result = locate_usage(data, bytes, &usage);
    if (result) {
        return result;
    }
    struct polaris_atom_reservations parsed = {0};
    result = decode_reservations((const uint8_t *)data + usage, vram,
                                  aperture_bytes, &parsed);
    if (!result) {
        *out = parsed;
    }
    return result;
}

const char *polaris_atom_result_name(int result)
{
    switch (result) {
    case POLARIS_ATOM_OK:
        return "declared-reservations";
    case POLARIS_ATOM_WRONG_DEVICE:
        return "pcir-device-mismatch";
    case POLARIS_ATOM_NO_USAGE_TABLE:
        return "usage-table-absent";
    case POLARIS_ATOM_UNSUPPORTED_TABLE:
        return "usage-version-or-policy";
    case POLARIS_ATOM_BAD_RESERVATION:
        return "reservation-range";
    default:
        return "vbios-image-unavailable";
    }
}
