#ifndef LOGIT_ACPI_H
#define LOGIT_ACPI_H
#include <stdint.h>

#define ACPI_MAX_CPUS 32

/* Find the RSDP and cache the (X)SDT. Idempotent; 0 on success, -1 if there is
 * no usable ACPI. Callable before acpi_init() -- the PCI bus driver needs the
 * MCFG table at enumeration time, which is long before SMP comes up. */
int         acpi_tables_init(void);

/* Look up a table by its 4-character signature ("APIC", "MCFG", ...). Returns a
 * pointer to its SDT header, or NULL. */
const void *acpi_find_table(const char *sig);

/* MCFG allocation entry `idx` (one PCIe ECAM window). 0 on success, -1 when
 * there is no MCFG or idx is past the end. Any out pointer may be NULL. */
int         acpi_mcfg_entry(int idx, uint64_t *base, uint16_t *seg,
                            uint8_t *bus_lo, uint8_t *bus_hi);

/* Parse ACPI tables (RSDP -> MADT). Returns the number of enabled CPUs, or -1. */
int      acpi_init(void);
int      acpi_cpu_count(void);
uint8_t  acpi_cpu_apic_id(int i);
uint32_t acpi_lapic_base(void);
uint32_t acpi_ioapic_addr(void);          /* IOAPIC MMIO base (0 if none) */
uint32_t acpi_ioapic_gsibase(void);
uint32_t acpi_gsi_for_irq(int isa_irq);   /* ISA IRQ -> GSI (handles overrides) */
uint16_t acpi_gsi_flags(int isa_irq);     /* MPS INTI flags for the override */

#endif
