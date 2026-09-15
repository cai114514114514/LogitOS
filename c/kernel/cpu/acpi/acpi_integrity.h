/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_ACPI_INTEGRITY_H
#define LOGIT_ACPI_INTEGRITY_H

#include <stdint.h>

/* Firmware controls the length fields consumed here.  Keep the same one-MiB
 * ceiling as acpi.c's mapper in this shared header so the validator and the
 * mapper cannot silently disagree about how much untrusted firmware is safe
 * to walk. */
#define ACPI_SDT_HEADER_BYTES 36u
#define ACPI_SDT_MAX_BYTES    (1u << 20)
#define ACPI_RSDP_MAX_BYTES   4096u

/* Validate one complete System Description Table already mapped for
 * `available` bytes.  This checks the in-table length as well as the ACPI
 * checksum; callers must still check the four-byte signature they asked for. */
int acpi_sdt_integrity_ok(const void *table, uint32_t available);
/* The root table's entry width comes from its kind.  Require the exact four
 * bytes before walking payload as RSDT (32-bit) or XSDT (64-bit) entries. */
int acpi_sdt_matches(const void *table, uint32_t available,
                     const char expected[4]);

/* Validate an RSDP copied by a bootloader or found in the BIOS scan window.
 * ACPI 1.0 has only the first 20-byte checksum.  ACPI 2.0+ also has a length
 * and an extended checksum covering the whole RSDP; accepting only the first
 * checksum can turn a single corrupted XSDT pointer into an arbitrary MMIO
 * mapping request. */
int acpi_rsdp_integrity_ok(const void *rsdp, uint32_t available);

#endif
