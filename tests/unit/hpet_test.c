#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hpet.h"

static int checks, fails;
#define CHECK(x, msg) do { checks++; if (!(x)) { fails++; printf("FAIL: %s\n", msg); } } while (0)

static void put32(uint8_t *p, uint32_t v)
{ for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static void put64(uint8_t *p, uint64_t v)
{ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8)); }

static void valid_table(uint8_t t[56])
{
    memset(t, 0, 56);
    memcpy(t, "HPET", 4); put32(t + 4, 56);
    put32(t + 36, 0x8086a201u);
    t[40] = 0; t[41] = 64; t[42] = 0; t[43] = 4;
    put64(t + 44, 0xfed00000ull);
    t[52] = 0; t[53] = 0x80; t[54] = 0;
}

int main(void)
{
    uint8_t t[56]; struct hpet_desc d; struct hpet_caps c;
    valid_table(t);
    CHECK(hpet_parse_table(t, sizeof t, &d) == 0, "valid ACPI HPET rejected");
    CHECK(d.address == 0xfed00000ull && d.block_id == 0x8086a201u,
          "valid ACPI HPET fields decoded incorrectly");

    t[0] = 'X'; CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "wrong signature accepted");
    valid_table(t); CHECK(hpet_parse_table(t, 55, &d) != 0, "short buffer accepted");
    put32(t + 4, 55); CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "short declared length accepted");
    put32(t + 4, 57); CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "overlong declared length accepted");
    valid_table(t); t[40] = 1;
    CHECK(hpet_parse_table(t, sizeof t, &d) != 0,
          "system-I/O GAS accepted as an MMIO pointer");
    valid_table(t); t[42] = 8;
    CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "nonzero GAS bit offset accepted");
    valid_table(t); t[43] = 1;
    CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "byte GAS access accepted for 64-bit MMIO");
    valid_table(t); put64(t + 44, 0xfed00004ull);
    CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "unaligned MMIO address accepted");
    valid_table(t); put64(t + 44, 0);
    CHECK(hpet_parse_table(t, sizeof t, &d) != 0, "zero MMIO address accepted");

    uint64_t cap64 = (uint64_t)69841279u << 32 | 1ull << 13 | 2ull << 8 | 1u;
    CHECK(hpet_parse_caps(cap64, (uint32_t)cap64, &c) == 0,
          "valid 64-bit capabilities rejected");
    CHECK(c.hz >= 14318000ull && c.hz <= 14319000ull && c.mask == UINT64_MAX && c.timers == 3,
          "64-bit capabilities decoded incorrectly");
    uint64_t cap32 = (uint64_t)100000000u << 32 | 1u;
    CHECK(hpet_parse_caps(cap32, (uint32_t)cap32, &c) == 0 &&
          c.hz == 10000000ull && c.mask == UINT32_MAX,
          "valid 32-bit minimum-frequency capabilities rejected");
    CHECK(hpet_parse_caps(cap64, (uint32_t)cap64 ^ 1u, &c) != 0,
          "firmware/hardware block-ID mismatch accepted");
    uint64_t revision_zero = cap64 & ~0xffull;
    CHECK(hpet_parse_caps(revision_zero, (uint32_t)revision_zero, &c) != 0,
          "reserved HPET revision zero accepted");
    CHECK(hpet_parse_caps(0, 0, &c) != 0, "zero period accepted");
    CHECK(hpet_parse_caps((uint64_t)100000001u << 32, 0, &c) != 0,
          "sub-10MHz counter accepted");

    printf("hpet_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
