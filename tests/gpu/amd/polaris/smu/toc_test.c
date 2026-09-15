#include "amd/polaris/smu/toc.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #expr); } } while (0)

static uint8_t output[POLARIS_SMU_TOC_BYTES + 8];
static struct polaris_smu_toc_info result;
static const struct polaris_smu_image base[2] = {
    {1, 0x1234, UINT64_C(0x1200100000), 0x800, 0},
    {2, 0x5678, UINT64_C(0x2300200000), 0x400, 0},
};

static int all_byte(const void *p, size_t n, uint8_t value)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) if (b[i] != value) return 0;
    return 1;
}

static void rejected(struct polaris_smu_image *images, size_t count,
                     uint64_t toc, size_t cap, int expected)
{
    memset(output, 0xa5, sizeof(output));
    memset(&result, 0x5a, sizeof(result));
    CHECK(polaris_smu_toc_encode(output, cap, toc, images, count, &result) == expected);
    CHECK(all_byte(output, sizeof(output), 0xa5));
    CHECK(all_byte(&result, sizeof(result), 0x5a));
}

int main(void)
{
    /* Literal bytes deliberately do not derive field positions, ID values or
     * masks from the encoder: a HIGH/LOW address swap must fail this oracle. */
    static const uint8_t oracle[] = {
        1,0,0,0, 2,0,0,0,
        1,0,0x34,0x12, 0x12,0,0,0, 0,0,0x10,0,
        0,0,0,0, 0,0,0,0, 0,8,0,0, 0,0,0,0,
        2,0,0x78,0x56, 0x23,0,0,0, 0,0,0x20,0,
        0,0,0,0, 0,0,0,0, 0,4,0,0, 0,0,0,0,
    };
    memset(output, 0xa5, sizeof(output));
    CHECK(polaris_smu_toc_encode(output, sizeof(output), 0x800000,
                                 base, 2, &result) == 0);
    CHECK(memcmp(output, oracle, sizeof(oracle)) == 0);
    CHECK(all_byte(output + sizeof(oracle), 344 - sizeof(oracle), 0));
    CHECK(all_byte(output + 344, 8, 0xa5));
    CHECK(result.present_mask == 6 && result.missing_mask == 0x478 && result.entry_count == 2);

    /* Wire output can be unaligned RAM; the GPU directory allocation cannot. */
    CHECK(polaris_smu_toc_encode(output + 1, 344, 0x800000, base, 2, &result) == 0);
    CHECK(memcmp(output + 1, oracle, sizeof(oracle)) == 0);

    struct polaris_smu_image a[13];
#define RESET() memcpy(a, base, sizeof(base))
#define BAD() rejected(a, 2, 0x800000, sizeof(output), POLARIS_SMU_TOC_INVALID)
    RESET(); a[1].id = 1; BAD();
    for (uint32_t id = 0; id < 40; ++id) {
        if ((id >= 1 && id <= 8) || id == 10) continue;
        RESET(); a[1].id = id; BAD();
    }
    RESET(); a[1].id = UINT32_MAX; BAD();
    RESET(); a[1].version = 0x10000; BAD();
    RESET(); a[1].flags = 1; BAD();
    RESET(); a[1].flags = 0x10000; BAD();
    RESET(); a[1].bytes = 0; BAD();
    RESET(); a[1].bytes = 3; BAD();
    RESET(); a[1].gpu_addr += 1; BAD();
    RESET(); a[1].gpu_addr = UINT64_C(0x10000000000); BAD();
    RESET(); a[1].gpu_addr = UINT64_C(0xfffffffffffc); BAD();
    RESET(); a[1].gpu_addr = UINT64_C(0xfffffffffc); a[1].bytes = 8; BAD();
    RESET(); a[1].gpu_addr = a[0].gpu_addr; BAD();
    RESET(); a[1].gpu_addr = a[0].gpu_addr + 4; BAD();
    RESET(); a[1].gpu_addr = a[0].gpu_addr - 4; BAD();
    RESET(); a[1].gpu_addr = 0x800ffc; a[1].bytes = 4; BAD();
    RESET(); a[1].gpu_addr = 0x7ffffc; a[1].bytes = 8; BAD();
    RESET(); rejected(a, 2, 0x800001, sizeof(output), -1);
    RESET(); rejected(a, 2, UINT64_C(0x10000000000), sizeof(output), -1);
    RESET(); rejected(a, 2, 0x800000, 343, -2);
    RESET(); rejected(a, 13, 0x800000, sizeof(output), -1);
    rejected(NULL, 1, 0x800000, sizeof(output), -1);

    /* Adjacent ranges and the last representable DWORD are valid. */
    RESET(); a[1].gpu_addr = a[0].gpu_addr + a[0].bytes;
    CHECK(polaris_smu_toc_encode(output, sizeof(output), 0x800000, a, 2, &result) == 0);
    RESET(); a[1].gpu_addr = UINT64_C(0xfffffffffc); a[1].bytes = 4;
    CHECK(polaris_smu_toc_encode(output, sizeof(output), 0x800000, a, 2, &result) == 0);
    RESET(); a[1].version = UINT16_MAX;
    CHECK(polaris_smu_toc_encode(output, sizeof(output), 0x800000, a, 2, &result) == 0);
    CHECK(output[38] == 0xff && output[39] == 0xff);

    size_t n = 0;
    for (uint32_t id = 1; id <= 10; ++id) {
        if (id == 9) continue;
        a[n] = (struct polaris_smu_image){id, 1, 0x100000 + n * 4096, 256,
                                       (id == 6 || id == 10) ? 1u : 0u};
        ++n;
    }
    CHECK(polaris_smu_toc_encode(output, sizeof(output), 0x800000, a, n, &result) == 0);
    CHECK(result.present_mask == 0x5fe && result.missing_mask == 0 && result.entry_count == 9);
    CHECK(output[8 + 5 * 28 + 24] == 1 && output[8 + 8 * 28 + 24] == 1);
    a[5].flags = 0; rejected(a, n, 0x800000, sizeof(output), -1); a[5].flags = 1;
    a[8].flags = 0; rejected(a, n, 0x800000, sizeof(output), -1);

    CHECK(polaris_smu_toc_encode(output, sizeof(output), 0, NULL, 0, &result) == 0);
    CHECK(result.present_mask == 0 && result.missing_mask == 0x47e && result.entry_count == 0);
    CHECK(output[0] == 1 && all_byte(output + 1, 343, 0));

    /* Aliased host inputs cannot be mutated indirectly by the success writer. */
    memset(output, 0xa5, sizeof(output));
    memset(&result, 0x5a, sizeof(result));
    CHECK(polaris_smu_toc_encode(NULL, 344, 0x800000, base, 2, &result) == -1);
    CHECK(all_byte(&result, sizeof(result), 0x5a));
    CHECK(polaris_smu_toc_encode(output, 344, 0x800000, base, 2, NULL) == -1);
    CHECK(polaris_smu_toc_encode(output, 344, 0x800000, base, 2,
                                 (struct polaris_smu_toc_info *)(void *)output) == -1);
    CHECK(polaris_smu_toc_encode(output, 344, 0x800000,
                                 (const struct polaris_smu_image *)(const void *)output,
                                 2, &result) == -1);
    CHECK(all_byte(output, sizeof(output), 0xa5));
    CHECK(all_byte(&result, sizeof(result), 0x5a));

    printf("POLARIS_SMU_TOC: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
