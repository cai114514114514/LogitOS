#include "polaris_firmware.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void put16(uint8_t *p, uint16_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
}

static void put32(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (i * 8));
}

/* Synthetic metadata and marker words, NOT AMD executable firmware. Offsets
 * are independently encoded from the pinned upstream common/v1 structures,
 * never obtained from the parser's internal constants or serialization code. */
static void fixture(uint8_t *p, uint32_t total, uint32_t offset)
{
    memset(p, 0, total);
    put32(p, total); put32(p + 4, 48);
    put16(p + 8, 1); put16(p + 12, 3); put16(p + 14, 1);
    put32(p + 16, 0x01020304); put32(p + 20, 16);
    put32(p + 24, offset); put32(p + 28, 0x89abcdef);
    put32(p + 32, 20); put32(p + 36, 9);
    put32(p + 40, 2); put32(p + 44, 2);
    put32(p + offset, 0x12345678); put32(p + offset + 4, 0x90abcdef);
    put32(p + offset + 8, 0x55667788); put32(p + offset + 12, 0xfedcba98);
}

static int empty(const struct polaris_sdma_firmware *v)
{
    return !v->ucode && !v->ucode_bytes && !v->ucode_dwords &&
        !v->ucode_version && !v->feature_version && !v->change_version &&
        !v->jump_offset_dwords && !v->jump_size_dwords &&
        !v->declared_crc32 && !v->digest_size && !v->header_major &&
        !v->header_minor && !v->ip_major && !v->ip_minor;
}

static void reject(const uint8_t *p, size_t n, int expected)
{
    struct polaris_sdma_firmware v;
    memset(&v, 0xa5, sizeof(v));
    int rc = polaris_sdma_firmware_parse(p, n, &v);
    CHECK(rc == expected && empty(&v));
}

int main(void)
{
    uint8_t b[288], original[288];
    struct polaris_sdma_firmware v;
    uint32_t word;
    fixture(b, 64, 48); memcpy(original, b, 64);
    CHECK(polaris_sdma_firmware_parse(b, 64, &v) == POLARIS_FW_OK);
    CHECK(v.ucode == b + 48 && v.ucode_bytes == 16 && v.ucode_dwords == 4);
    CHECK(v.ucode_version == 0x01020304 && v.feature_version == 20 &&
          v.change_version == 9 && v.ip_major == 3 && v.ip_minor == 1);
    CHECK(v.declared_crc32 == 0x89abcdef && v.jump_offset_dwords == 2 &&
          v.jump_size_dwords == 2);
    CHECK(v.header_major == 1 && v.header_minor == 0 && v.digest_size == 0);
    const uint32_t expected[] = {0x12345678, 0x90abcdef, 0x55667788, 0xfedcba98};
    for (unsigned i = 0; i < 4; ++i) {
        word = 0;
        CHECK(polaris_sdma_firmware_word(&v, i, &word) == 0 && word == expected[i]);
    }
    CHECK(!memcmp(b, original, 64));
    word = 0xcafebabe;
    CHECK(polaris_sdma_firmware_word(&v, 4, &word) == POLARIS_FW_BAD_ARGUMENT &&
          word == 0xcafebabe);
    CHECK(polaris_sdma_firmware_word(&v, UINT32_MAX, &word) == POLARIS_FW_BAD_ARGUMENT &&
          word == 0xcafebabe);
    CHECK(polaris_sdma_firmware_word(NULL, 0, &word) == POLARIS_FW_BAD_ARGUMENT);
    CHECK(polaris_sdma_firmware_word(&v, 0, NULL) == POLARIS_FW_BAD_ARGUMENT);
    v.ucode_dwords++;
    CHECK(polaris_sdma_firmware_word(&v, 0, &word) == POLARIS_FW_BAD_ARGUMENT);
    CHECK(polaris_sdma_firmware_parse(b, 64, NULL) == POLARIS_FW_BAD_ARGUMENT);
    reject(NULL, 64, POLARIS_FW_BAD_ARGUMENT);

    /* Exact-size allocations let ASan detect reads beyond each truncation,
     * instead of a generous backing fixture accidentally hiding them. */
    for (size_t n = 0; n < 64; ++n) {
        uint8_t *short_blob = malloc(n ? n : 1);
        if (!short_blob) return 2;
        memcpy(short_blob, original, n);
        reject(short_blob, n, n < 48 ? POLARIS_FW_TRUNCATED : POLARIS_FW_BAD_LAYOUT);
        free(short_blob);
    }
    const struct { unsigned offset; uint32_t value; int result; } bad[] = {
        {0, 63, POLARIS_FW_BAD_LAYOUT}, {0, 65, POLARIS_FW_BAD_LAYOUT},
        {0, UINT32_MAX, POLARIS_FW_BAD_LAYOUT},
        {4, 44, POLARIS_FW_BAD_LAYOUT}, {4, 49, POLARIS_FW_BAD_LAYOUT},
        {4, 68, POLARIS_FW_BAD_LAYOUT}, {4, UINT32_MAX, POLARIS_FW_BAD_LAYOUT},
        {8, 2, POLARIS_FW_BAD_VERSION}, {8, 0, POLARIS_FW_BAD_VERSION},
        {8, 3, POLARIS_FW_BAD_VERSION}, {8, 0x00020001, POLARIS_FW_BAD_VERSION},
        {20, 0, POLARIS_FW_BAD_LAYOUT}, {20, 15, POLARIS_FW_BAD_LAYOUT},
        {20, 20, POLARIS_FW_BAD_LAYOUT}, {20, 0xfffffffc, POLARIS_FW_BAD_LAYOUT},
        {24, 44, POLARIS_FW_BAD_LAYOUT}, {24, 49, POLARIS_FW_BAD_LAYOUT},
        {24, 64, POLARIS_FW_BAD_LAYOUT}, {24, 68, POLARIS_FW_BAD_LAYOUT},
        {24, 0xfffffffc, POLARIS_FW_BAD_LAYOUT},
        {40, 3, POLARIS_FW_BAD_JUMP_TABLE}, {40, UINT32_MAX, POLARIS_FW_BAD_JUMP_TABLE},
        {44, 3, POLARIS_FW_BAD_JUMP_TABLE}, {44, UINT32_MAX, POLARIS_FW_BAD_JUMP_TABLE}
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memcpy(b, original, 64); put32(b + bad[i].offset, bad[i].value);
        reject(b, 64, bad[i].result);
    }

    /* Firmware padding and unaligned archive members are distinct: the former
     * affects relative offsets, the latter must never trigger typed loads. */
    for (unsigned align = 0; align < 4; ++align) {
        fixture(b + align, 280, 256);
        CHECK(polaris_sdma_firmware_parse(b + align, 280, &v) == 0);
        CHECK(v.ucode == b + align + 256);
        CHECK(polaris_sdma_firmware_word(&v, 3, &word) == 0 && word == 0xfedcba98);
    }
    fixture(b, 272, 256); put32(b + 4, 256);
    CHECK(polaris_sdma_firmware_parse(b, 272, &v) == 0);
    fixture(b, 64, 48); put32(b + 40, 4); put32(b + 44, 0);
    CHECK(polaris_sdma_firmware_parse(b, 64, &v) == 0 && v.jump_size_dwords == 0);
    put32(b + 40, 5);
    reject(b, 64, POLARIS_FW_BAD_JUMP_TABLE);
    fixture(b, 64, 48); put32(b + 40, 0); put32(b + 44, 4);
    CHECK(polaris_sdma_firmware_parse(b, 64, &v) == 0 && v.jump_size_dwords == 4);

    /* v1.1 metadata follows the same payload layout. The real Polaris10 pair
     * has header_size=52, payload_offset=256, IP=3.1, digest_size=5. Reproduce
     * that structure with marker words, keeping executable bytes out of the
     * unit fixture. Merely checking the old 48-byte prefix is insufficient. */
    fixture(b, 272, 256); put16(b + 10, 1); put32(b + 4, 52); put32(b + 48, 5);
    CHECK(polaris_sdma_firmware_parse(b, 272, &v) == 0 &&
          v.header_major == 1 && v.header_minor == 1 && v.digest_size == 5);
    CHECK(v.ucode == b + 256 && v.ucode_dwords == 4 && v.ip_major == 3 && v.ip_minor == 1);
    CHECK(polaris_sdma_firmware_word(&v, 3, &word) == 0 && word == 0xfedcba98);
    for (size_t n = 48; n < 52; ++n) {
        uint8_t *short_blob = malloc(n);
        if (!short_blob) return 2;
        memcpy(short_blob, b, n);
        reject(short_blob, n, POLARIS_FW_TRUNCATED);
        free(short_blob);
    }
    put32(b + 4, 48);
    reject(b, 272, POLARIS_FW_BAD_LAYOUT);
    put32(b + 4, 52); put32(b + 24, 48);
    reject(b, 272, POLARIS_FW_BAD_LAYOUT);
    put32(b + 24, 256); put16(b + 10, 2);
    reject(b, 272, POLARIS_FW_BAD_VERSION);
    put16(b + 10, 1); put32(b + 48, UINT32_MAX);
    CHECK(polaris_sdma_firmware_parse(b, 272, &v) == 0 && v.digest_size == UINT32_MAX);
    /* Unknown digest metadata cannot authorize an address calculation. It is
     * retained verbatim, never used to read a fabricated digest byte range. */
    printf("POLARIS_FIRMWARE: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
