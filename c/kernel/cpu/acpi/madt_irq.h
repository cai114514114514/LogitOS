#ifndef LOGIT_MADT_IRQ_H
#define LOGIT_MADT_IRQ_H
#include <stdint.h>
#include <stddef.h>

#define ACPI_MAX_IOAPICS 8
struct acpi_ioapic_info {
    uint8_t id;
    uint32_t addr;
    uint32_t gsi_base;
};

/* Pure ACPI 6.5 section 5.2.12 type-1 decoder; rejects unsafe MMIO bases. */
int madt_irq_decode_ioapic(const uint8_t *rec, size_t avail,
                         struct acpi_ioapic_info *out);
/* Keep the first firmware record for an address or GSI base. */
int madt_irq_add_ioapic(struct acpi_ioapic_info *tab, size_t *count, size_t cap,
                      const struct acpi_ioapic_info *in);
#endif
