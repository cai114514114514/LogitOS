/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "acpi_integrity.h"

static int checks, failures;

static void check(int yes, const char *what)
{
    checks++;
    if (!yes) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void checksum(uint8_t *p, uint32_t n, uint32_t checksum_off)
{
    uint8_t sum = 0;
    p[checksum_off] = 0;
    for (uint32_t i = 0; i < n; i++) sum = (uint8_t)(sum + p[i]);
    p[checksum_off] = (uint8_t)(0u - sum);
}

static void make_sdt(uint8_t *p, uint32_t length)
{
    memset(p, 0, length);
    memcpy(p, "MCFG", 4);
    p[4] = (uint8_t)length;
    p[5] = (uint8_t)(length >> 8);
    p[6] = (uint8_t)(length >> 16);
    p[7] = (uint8_t)(length >> 24);
    p[8] = 1;
    memcpy(p + 10, "LOGITO", 6);
    checksum(p, length, 9);
}

static void make_rsdp(uint8_t p[36], uint8_t revision)
{
    memset(p, 0, 36);
    memcpy(p, "RSD PTR ", 8);
    memcpy(p + 9, "LOGITO", 6);
    p[15] = revision;
    if (revision >= 2) {
        p[20] = 36;
        p[24] = 0x00; p[25] = 0x20; /* a plausible XSDT address */
        checksum(p, 20, 8);
        checksum(p, 36, 32);
    } else {
        p[16] = 0x00; p[17] = 0x10; /* a plausible RSDT address */
        checksum(p, 20, 8);
    }
}

int main(void)
{
    uint8_t sdt[64];
    make_sdt(sdt, sizeof sdt);
    check(acpi_sdt_integrity_ok(sdt, sizeof sdt),
          "valid complete SDT is accepted");
    check(acpi_sdt_matches(sdt, sizeof sdt, "MCFG"),
          "valid SDT signature is accepted");
    check(!acpi_sdt_matches(sdt, sizeof sdt, "XSDT"),
          "wrong root SDT signature is rejected");

    sdt[40] ^= 1;
    check(!acpi_sdt_integrity_ok(sdt, sizeof sdt),
          "bad SDT checksum is rejected");
    sdt[40] ^= 1;

    check(!acpi_sdt_integrity_ok(sdt, sizeof sdt - 1),
          "SDT length beyond mapped bytes is rejected");
    sdt[4] = 35; sdt[5] = sdt[6] = sdt[7] = 0;
    check(!acpi_sdt_integrity_ok(sdt, sizeof sdt),
          "SDT shorter than its fixed header is rejected");
    sdt[4] = 1; sdt[5] = 0; sdt[6] = 0x10; sdt[7] = 0;
    check(!acpi_sdt_integrity_ok(sdt, UINT32_MAX),
          "SDT above the one-MiB mapper ceiling is rejected");
    check(!acpi_sdt_integrity_ok(NULL, sizeof sdt),
          "null SDT is rejected");

    uint8_t rsdp[36];
    make_rsdp(rsdp, 0);
    check(acpi_rsdp_integrity_ok(rsdp, 20),
          "valid ACPI 1.0 RSDP is accepted");
    rsdp[8] ^= 1;
    check(!acpi_rsdp_integrity_ok(rsdp, 20),
          "bad ACPI 1.0 checksum is rejected");

    make_rsdp(rsdp, 2);
    check(acpi_rsdp_integrity_ok(rsdp, sizeof rsdp),
          "valid ACPI 2.0 RSDP is accepted");
    rsdp[32] ^= 1;
    check(!acpi_rsdp_integrity_ok(rsdp, sizeof rsdp),
          "bad ACPI 2.0 extended checksum is rejected");
    rsdp[32] ^= 1;
    check(!acpi_rsdp_integrity_ok(rsdp, 35),
          "truncated ACPI 2.0 RSDP is rejected");
    rsdp[20] = 37;
    check(!acpi_rsdp_integrity_ok(rsdp, sizeof rsdp),
          "RSDP length beyond available bytes is rejected");
    make_rsdp(rsdp, 2);
    rsdp[20] = 1; rsdp[21] = 0x10;
    check(!acpi_rsdp_integrity_ok(rsdp, UINT32_MAX),
          "RSDP above the one-page validation ceiling is rejected");

    printf("ACPI_INTEGRITY: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
