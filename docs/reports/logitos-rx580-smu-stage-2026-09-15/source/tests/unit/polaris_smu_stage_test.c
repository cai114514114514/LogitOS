#include "polaris_smu_stage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

/* Independent synthetic v1.1 file layout. Nonzero header/padding and different
 * payload patterns expose copying the file header or the wrong engine image.
 * They are not executable AMD firmware and confer no hardware evidence. */
static uint8_t fw0[256 + 4100 + 12], fw1[256 + 60 + 12];
static uint8_t destination[16384 + 16];
static struct polaris_smu_stage_info info;
static void put16(uint8_t *p, uint16_t x)
{ p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (i * 8)); }
static int all_byte(const void *p, size_t size, uint8_t value)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < size; ++i) if (b[i] != value) return 0;
    return 1;
}
static void fixture(uint8_t *p, uint32_t size, uint32_t payload, uint16_t version,
                    uint8_t seed)
{
    memset(p, 0xcd, size);
    memset(p, 0, 52);
    put32(p, size); put32(p + 4, 52);
    put16(p + 8, 1); put16(p + 10, 1);
    put16(p + 12, 3); put16(p + 14, 1);
    put32(p + 16, version); put32(p + 20, payload); put32(p + 24, 256);
    put32(p + 32, 20); put32(p + 36, 9);
    put32(p + 40, payload / 4 - 5); put32(p + 44, 5); put32(p + 48, 5);
    for (uint32_t i = 0; i < payload; ++i) p[256 + i] = (uint8_t)(seed + i * 7);
}
static void reset(void)
{
    fixture(fw0, sizeof(fw0), 4100, 0x1234, 0x31);
    fixture(fw1, sizeof(fw1), 60, 0x5678, 0xb7);
    memset(destination, 0xa5, sizeof(destination));
    memset(&info, 0x5a, sizeof(info));
}
static void reject(const void *first, size_t n0, const void *second, size_t n1,
                    uint64_t base, size_t capacity)
{
    CHECK(polaris_smu_stage_sdma(destination, capacity, base,
                                 first, n0, second, n1, &info) != 0);
    CHECK(all_byte(destination, sizeof(destination), 0xa5));
    CHECK(all_byte(&info, sizeof(info), 0x5a));
}
#define BAD() reject(fw0, sizeof(fw0), fw1, sizeof(fw1), UINT64_C(0x1200200000), sizeof(destination))

int main(void)
{
    static const uint8_t toc_oracle[344] = {
        1,0,0,0, 2,0,0,0,
        1,0,0x34,0x12, 0x12,0,0,0, 0,0x10,0x20,0,
        0,0,0,0, 0,0,0,0, 4,0x10,0,0, 0,0,0,0,
        2,0,0x78,0x56, 0x12,0,0,0, 0,0x30,0x20,0,
        0,0,0,0, 0,0,0,0, 60,0,0,0, 0,0,0,0,
    };
    reset();
    CHECK(polaris_smu_stage_sdma(destination, sizeof(destination),
                                 UINT64_C(0x1200200000), fw0, sizeof(fw0),
                                 fw1, sizeof(fw1), &info) == 0);
    CHECK(memcmp(destination, toc_oracle, sizeof(toc_oracle)) == 0);
    CHECK(info.proposed_gpu_base == UINT64_C(0x1200200000) && info.bytes_used == 16384);
    CHECK(info.image_offset[0] == 4096 && info.image_offset[1] == 12288);
    CHECK(info.image_bytes[0] == 4100 && info.image_bytes[1] == 60);
    CHECK(info.inventory.entry_count == 2 && info.inventory.present_mask == 6 &&
          info.inventory.missing_mask == 0x478);
    CHECK(memcmp(destination + 4096, fw0 + 256, 4100) == 0);
    CHECK(memcmp(destination + 12288, fw1 + 256, 60) == 0);
    /* These trailing 20 payload bytes must survive normal-PF handling despite
     * v1.1 digest metadata. File trailer and reserved header bytes stay out. */
    CHECK(memcmp(destination + 4096 + 4080, fw0 + 256 + 4080, 20) == 0);
    CHECK(memcmp(destination + 12288 + 40, fw1 + 256 + 40, 20) == 0);
    CHECK(all_byte(destination + 344, 4096 - 344, 0));
    CHECK(all_byte(destination + 8196, 12288 - 8196, 0));
    CHECK(all_byte(destination + 12348, 16384 - 12348, 0));
    CHECK(all_byte(destination + 16384, 16, 0xa5));
    CHECK(fw0[52] == 0xcd && fw0[sizeof(fw0) - 1] == 0xcd &&
          fw1[52] == 0xcd && fw1[sizeof(fw1) - 1] == 0xcd);

    reset(); put16(fw0 + 12, 4); BAD();
    reset(); put16(fw1 + 14, 0); BAD();
    reset(); put16(fw0 + 8, 2); BAD();
    reset(); put16(fw1 + 10, 2); BAD();
    reset(); put32(fw0 + 16, 0x10000); BAD();
    reset(); put32(fw1 + 16, UINT32_MAX); BAD();
    reset(); put32(fw1 + 20, 59); BAD();
    reset(); put32(fw0 + 24, UINT32_MAX); BAD();
    reset(); reject(fw0, sizeof(fw0), NULL, 0, 0x100000, sizeof(destination));
    reset(); reject(NULL, 0, fw1, sizeof(fw1), 0x100000, sizeof(destination));
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), 0x100001, sizeof(destination));
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), 0x100000, 16383);
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), 0x100000, 0);
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), UINT64_C(0x10000000000), sizeof(destination));
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), UINT64_C(0xfffffff000), sizeof(destination));
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), UINT64_MAX, sizeof(destination));
    reset(); reject(fw0, sizeof(fw0), fw1, sizeof(fw1), 0x100000, SIZE_MAX);

    const size_t truncations[] = {1, 31, 47, 48, 51, 256, sizeof(fw1) - 1};
    for (size_t i = 0; i < sizeof(truncations) / sizeof(truncations[0]); ++i) {
        size_t n = truncations[i];
        uint8_t *short_file = malloc(n);
        if (!short_file) return 2;
        reset(); memcpy(short_file, fw1, n);
        reject(fw0, sizeof(fw0), short_file, n, 0x100000, sizeof(destination));
        free(short_file);
    }

    reset(); reject(destination, sizeof(fw0), fw1, sizeof(fw1), 0x100000, sizeof(destination));
    reset(); reject(fw0, sizeof(fw0), destination + sizeof(destination) - 1, 2,
                    0x100000, sizeof(destination));
    reset(); reject(&info, sizeof(info), fw1, sizeof(fw1), 0x100000, sizeof(destination));
    reset();
    CHECK(polaris_smu_stage_sdma(destination, sizeof(destination), 0x100000,
                                 fw0, sizeof(fw0), fw1, sizeof(fw1),
                                 (struct polaris_smu_stage_info *)(void *)destination) != 0);
    CHECK(all_byte(destination, sizeof(destination), 0xa5));
    CHECK(polaris_smu_stage_sdma(destination, sizeof(destination), 0x100000,
                                 fw0, sizeof(fw0), fw1, sizeof(fw1), NULL) != 0);
    CHECK(all_byte(destination, sizeof(destination), 0xa5));
    CHECK(polaris_smu_stage_sdma(NULL, sizeof(destination), 0x100000,
                                 fw0, sizeof(fw0), fw1, sizeof(fw1), &info) != 0);
    CHECK(all_byte(&info, sizeof(info), 0x5a));

    /* Source aliasing is explicitly allowed: one provenance-checked file may
     * intentionally populate both engine slots without writing its input. */
    reset();
    CHECK(polaris_smu_stage_sdma(destination, sizeof(destination), 0x100000,
                                 fw1, sizeof(fw1), fw1, sizeof(fw1), &info) == 0);
    CHECK(info.bytes_used == 12288 && info.image_offset[1] == 8192);
    CHECK(memcmp(destination + 4096, destination + 8192, 60) == 0);

    printf("POLARIS_SMU_STAGE: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
