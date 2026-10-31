#include "madt_irq.h"

/* MADT fields are little-endian and need not be naturally aligned. Reading
 * bytes avoids host alignment UB when testing malformed firmware records. */
static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* ACPI 6.5 section 5.2.12.3: a type-1 entry has a 12-byte fixed prefix.
 * Intel 82093AA section 3 places IOWIN at base+0x10; a misaligned base cannot
 * describe that register window and must never become an MMIO access. */
int madt_irq_decode_ioapic(const uint8_t *rec, size_t avail,
                         struct acpi_ioapic_info *out)
{
    if (!rec || !out || avail < 12 || rec[0] != 1 || rec[1] < 12 || rec[1] > avail) {
        return -1;
    }
    uint32_t address = read_u32(rec + 4);
    if (!address || (address & 0xf)) {
        return -1;
    }
    *out = (struct acpi_ioapic_info){rec[2], address, read_u32(rec + 8)};
    return 1;
}

/* Do not let a duplicate MADT entry mask/program one controller twice. Full
 * GSI interval checks require the live VER register and belong to ioapic_map.
 * Compare Linux v6.12 arch/x86/kernel/apic/io_apic.c: mp_register_ioapic(). */
int madt_irq_add_ioapic(struct acpi_ioapic_info *tab, size_t *count, size_t cap,
                      const struct acpi_ioapic_info *in)
{
    if (!tab || !count || !in || *count > cap) {
        return -1;
    }
    for (size_t i = 0; i < *count; i++) {
        if (tab[i].addr == in->addr || tab[i].gsi_base == in->gsi_base) {
            return 0;
        }
    }
    if (*count == cap) {
        return -1;
    }
    tab[(*count)++] = *in;
    return 1;
}
