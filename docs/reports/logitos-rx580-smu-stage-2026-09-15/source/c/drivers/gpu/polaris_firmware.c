#include "polaris_firmware.h"

/* Wire layout reference (field offsets, not a copied packed host struct):
 * Linux v6.12 drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h,
 * common_firmware_header and sdma_firmware_header_v1_0/v1_1:
 * https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h
 * sdma_v3_0.c requests polaris10_sdma[1].bin using the v1 common fields.
 * Correction: the initial parser accepted only v1.0. Real linux-firmware
 * commit 1522c78ab870b3c051d8a3a1d24ecbbc12b23be5 Polaris10 SDMA files use
 * v1.1 (52-byte header, digest_size=5); a synthetic v1.0 fixture alone missed
 * this. v1.1 adds one metadata word, not a new microcode-array layout. Linux's
 * amdgpu_ucode_print_sdma_hdr() prints it but specifies neither units nor a
 * digest location: retain the raw word without pretending to verify a digest.
 * Do not accept v2 as v1: v2 changes payload layout and needs its own contract.
 * CRC32 is retained but not checked: that header names CRC32 without specifying
 * polynomial, initial state or final xor, and amdgpu_ucode_validate() checks
 * only file size. Inventing a checksum contract here would reject valid files
 * or falsely certify bad ones. No firmware bytes are installed by this module. */
enum {
    FW_SIZE = 0, FW_HEADER_SIZE = 4, FW_MAJOR = 8, FW_MINOR = 10,
    FW_IP_MAJOR = 12, FW_IP_MINOR = 14, FW_UCODE_VERSION = 16,
    FW_UCODE_SIZE = 20, FW_UCODE_OFFSET = 24, FW_CRC32 = 28,
    FW_FEATURE = 32, FW_CHANGE = 36, FW_JUMP_OFFSET = 40,
    FW_JUMP_SIZE = 44, FW_DIGEST_SIZE = 48, FW_V1_BYTES = 48, FW_V11_BYTES = 52
};

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int polaris_sdma_firmware_parse(const void *data, size_t bytes,
                               struct polaris_sdma_firmware *out)
{
    if (!out) return POLARIS_FW_BAD_ARGUMENT;
    *out = (struct polaris_sdma_firmware){0};
    if (!data) return POLARIS_FW_BAD_ARGUMENT;
    if (bytes < FW_V1_BYTES) return POLARIS_FW_TRUNCATED;

    const uint8_t *p = data;
    uint16_t major = read16(p + FW_MAJOR);
    uint16_t minor = read16(p + FW_MINOR);
#ifdef POLARIS_FW_NEGCTL_ALLOW_V2
    if (major == 2) major = 1;
#endif
    if (major != 1 || minor > 1)
        return POLARIS_FW_BAD_VERSION;
    uint32_t required = minor == 1 ? FW_V11_BYTES : FW_V1_BYTES;
    if (bytes < required) return POLARIS_FW_TRUNCATED;

    uint32_t total = read32(p + FW_SIZE);
    uint32_t header = read32(p + FW_HEADER_SIZE);
    uint32_t offset = read32(p + FW_UCODE_OFFSET);
    uint32_t length = read32(p + FW_UCODE_SIZE);
    /* Subtraction bounds avoid wraparound of offset+length near UINT32_MAX.
     * Read bytes rather than casting headers: an archive's starting pointer
     * need not be aligned even when firmware-relative offsets are aligned. */
#ifdef POLARIS_FW_NEGCTL_MISSING_EXTENSION
    required = FW_V1_BYTES;
#endif
    if ((size_t)total != bytes || header < required || header > total ||
        (header & 3u) || (offset & 3u) || (length & 3u) || !length ||
        offset < header || offset > total || length > total - offset)
        return POLARIS_FW_BAD_LAYOUT;

    uint32_t count = length / 4;
    uint32_t jump = read32(p + FW_JUMP_OFFSET);
    uint32_t jump_size = read32(p + FW_JUMP_SIZE);
    /* AMD v1 jump-table offsets/sizes count DWORDs relative to microcode, not
     * file bytes. A zero-size table may point one past the last word, but a
     * nonempty table must fit entirely inside the declared microcode range. */
    if (jump > count || jump_size > count - jump)
        return POLARIS_FW_BAD_JUMP_TABLE;

    *out = (struct polaris_sdma_firmware){
        .ucode = p + offset, .ucode_bytes = length, .ucode_dwords = count,
        .ucode_version = read32(p + FW_UCODE_VERSION),
        .feature_version = read32(p + FW_FEATURE),
        .change_version = read32(p + FW_CHANGE),
        .jump_offset_dwords = jump, .jump_size_dwords = jump_size,
        .declared_crc32 = read32(p + FW_CRC32),
        .digest_size = minor == 1 ? read32(p + FW_DIGEST_SIZE) : 0,
        .header_major = major, .header_minor = minor,
        .ip_major = read16(p + FW_IP_MAJOR),
        .ip_minor = read16(p + FW_IP_MINOR)
    };
    return POLARIS_FW_OK;
}

int polaris_sdma_firmware_word(const struct polaris_sdma_firmware *fw,
                              uint32_t index, uint32_t *word)
{
    if (!fw || !word || !fw->ucode || (fw->ucode_bytes & 3u) ||
        fw->ucode_dwords != fw->ucode_bytes / 4 || index >= fw->ucode_dwords)
        return POLARIS_FW_BAD_ARGUMENT;
    *word = read32(fw->ucode + (size_t)index * 4);
    return POLARIS_FW_OK;
}
