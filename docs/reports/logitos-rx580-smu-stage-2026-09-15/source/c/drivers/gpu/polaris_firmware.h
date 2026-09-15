#ifndef LOGITOS_POLARIS_FIRMWARE_H
#define LOGITOS_POLARIS_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

enum polaris_fw_result {
    POLARIS_FW_OK = 0,
    POLARIS_FW_BAD_ARGUMENT = -1,
    POLARIS_FW_TRUNCATED = -2,
    POLARIS_FW_BAD_VERSION = -3,
    POLARIS_FW_BAD_LAYOUT = -4,
    POLARIS_FW_BAD_JUMP_TABLE = -5
};

/* Borrowed immutable view: keep the complete input alive until all consumers
 * finish. Parsing establishes structure only, NOT authenticity, GPU-family
 * compatibility, residency, execution, or even CRC integrity. The caller must
 * match the image's provenance and IP version before a future hardware loader
 * can use it; a valid common header does not identify an SDMA image by itself. */
struct polaris_sdma_firmware {
    const uint8_t *ucode;
    uint32_t ucode_bytes;
    uint32_t ucode_dwords;
    uint32_t ucode_version;
    uint32_t feature_version;
    uint32_t change_version;
    uint32_t jump_offset_dwords;
    uint32_t jump_size_dwords;
    uint32_t declared_crc32; /* Metadata only; no CRC convention is guessed. */
    uint32_t digest_size; /* Raw v1.1 metadata; no unit or digest range inferred. */
    uint16_t header_major;
    uint16_t header_minor;
    uint16_t ip_major;
    uint16_t ip_minor;
};

/* Linux SDMA headers v1.0 and v1.1. Output must not overlap input; it is cleared
 * on every failure. Padding between the header and payload is allowed because
 * upstream's firmware header union reserves 0x100 bytes. Trailing file bytes
 * are allowed only when included in the header's exact total size. */
int polaris_sdma_firmware_parse(const void *data, size_t bytes,
                               struct polaris_sdma_firmware *out);

/* Decode one little-endian word without requiring host or file alignment.
 * The view must come from a successful parse; failures leave *word unchanged. */
int polaris_sdma_firmware_word(const struct polaris_sdma_firmware *fw,
                              uint32_t index, uint32_t *word);

#endif
