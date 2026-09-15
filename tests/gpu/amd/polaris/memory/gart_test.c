#include "amd/polaris/memory/gart.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static void bad_page(uint64_t address, uint64_t flags)
{
    uint64_t out = UINT64_C(0x123456789abcdef0);
    CHECK(polaris_gart_encode(address, flags, &out) == -1);
    CHECK(out == UINT64_C(0x123456789abcdef0));
}
int main(void)
{
    uint64_t value;
    const uint64_t pages[] = {UINT64_C(0x1234567000), 0, UINT64_C(0xfffffff000)};
    const unsigned char oracle[] = {
        0x67, 0x70, 0x56, 0x34, 0x12, 0, 0, 0,
        0x67, 0, 0, 0, 0, 0, 0, 0,
        0x67, 0xf0, 0xff, 0xff, 0xff, 0, 0, 0
    };
    unsigned char table[32], before[32];
    struct polaris_gart_descriptor desc, saved;
    memset(table, 0xa6, sizeof(table));
    CHECK(polaris_gart_encode(pages[0], 0x67, &value) == 0);
    CHECK(value == UINT64_C(0x1234567067));
    CHECK(polaris_gart_build(table, sizeof(table), UINT64_C(0x800400000),
                           UINT64_C(0x100000000), pages, 3, 0x67, &desc) == 0);
    CHECK(memcmp(table, oracle, sizeof(oracle)) == 0);
    CHECK(table[24] == 0xa6 && table[31] == 0xa6);
    CHECK(desc.start_page == 0x100000 && desc.end_page == 0x100002);
    CHECK(desc.table_base_page == 0x800400 && desc.table_bytes == 24);
    for (unsigned b = 7; b < 64; ++b) bad_page(0x1000, UINT64_C(0x67) | (UINT64_C(1) << b));
    bad_page(1, 0x67); bad_page(UINT64_C(1) << 40, 0x67);
    bad_page(UINT64_MAX, 0x67); bad_page(0x1000, 0x66);
    bad_page(0x1000, 0x65); bad_page(0x1000, 7);
    bad_page(0x1000, 0x6f); bad_page(0x1000, 0x77);
    CHECK(polaris_gart_encode(0x1000, 0x67, 0) == -1);
    memcpy(before, table, sizeof(table)); saved = desc;
#define REJECT(dst, cap, tg, gb, pp, n, f, oo) do { \
    CHECK(polaris_gart_build(dst, cap, tg, gb, pp, n, f, oo) == -1); \
    CHECK(memcmp(table, before, sizeof(table)) == 0); \
    CHECK(memcmp(&desc, &saved, sizeof(desc)) == 0); \
} while (0)
    REJECT(table, 23, 0x400000, 0x10000000, pages, 3, 0x67, &desc);
    REJECT(table, 32, 0x400001, 0x10000000, pages, 3, 0x67, &desc);
    REJECT(table, 32, 0x400000, 0x10000001, pages, 3, 0x67, &desc);
    REJECT(table, 32, UINT64_C(1) << 40, 0x10000000, pages, 3, 0x67, &desc);
    REJECT(table, 32, 0x400000, UINT64_C(0xfffffff000), pages, 3, 0x67, &desc);
    REJECT(table, 32, 0x400000, 0x10000000, pages, 0, 0x67, &desc);
    REJECT(table, 32, 0x400000, 0x10000000, pages, SIZE_MAX, 0x67, &desc);
    REJECT(table, 32, 0x400000, 0x10000000, 0, 3, 0x67, &desc);
    REJECT(0, 32, 0x400000, 0x10000000, pages, 3, 0x67, &desc);
    REJECT(table, 32, 0x400000, 0x10000000, pages, 3, 0x67, 0);
    REJECT(table, 32, 0x400000, 0x10000000, (const uint64_t *)table, 3, 0x67, &desc);
    REJECT(table, 32, 0x400000, 0x10000000, pages, 3, 0x67, (struct polaris_gart_descriptor *)table);
    uint64_t invalid[] = {0x1000, 0x2000, 0x3001};
    REJECT(table, 32, 0x400000, 0x10000000, invalid, 3, 0x67, &desc);
    printf("POLARIS_GART: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
