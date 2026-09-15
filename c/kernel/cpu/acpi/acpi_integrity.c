/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "acpi_integrity.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int bytes_equal(const uint8_t *p, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}

static int checksum_zero(const uint8_t *p, uint32_t n)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

int acpi_sdt_integrity_ok(const void *table, uint32_t available)
{
    if (!table || available < ACPI_SDT_HEADER_BYTES) return 0;
    const uint8_t *p = table;
    uint32_t length = le32(p + 4);
    if (length < ACPI_SDT_HEADER_BYTES || length > ACPI_SDT_MAX_BYTES ||
        length > available) return 0;
#ifdef ACPI_INTEGRITY_NEGCTL_PRECHECKSUM
    /* Reproduce the old mapper for the observable negative control: it bounded
     * the length but treated an SDT with a bad checksum as trusted firmware. */
    return 1;
#else
    return checksum_zero(p, length);
#endif
}

int acpi_sdt_matches(const void *table, uint32_t available,
                     const char expected[4])
{
    if (!expected || !acpi_sdt_integrity_ok(table, available)) return 0;
#ifdef ACPI_INTEGRITY_NEGCTL_SKIP_SIGNATURE
    (void)table;
    return 1;
#else
    return bytes_equal(table, expected, 4);
#endif
}

int acpi_rsdp_integrity_ok(const void *rsdp, uint32_t available)
{
    if (!rsdp || available < 20) return 0;
    const uint8_t *p = rsdp;
    if (!bytes_equal(p, "RSD PTR ", 8) || !checksum_zero(p, 20)) return 0;
    if (p[15] < 2) return 1;                 /* ACPI 1.0 ends at byte 20 */

    if (available < 36) return 0;
    uint32_t length = le32(p + 20);
    /* Current RSDPs are 36 bytes.  Permit forward extensions within one page,
     * but never let a corrupt firmware length turn checksum validation into an
     * unbounded read through the BIOS scan window. */
    if (length < 36 || length > available || length > ACPI_RSDP_MAX_BYTES) return 0;
#ifdef ACPI_INTEGRITY_NEGCTL_PRECHECKSUM
    /* The legacy path checked only the ACPI-1.0 prefix even when revision 2
     * selected the XSDT pointer from the extended half. */
    return 1;
#else
    return checksum_zero(p, length);
#endif
}
